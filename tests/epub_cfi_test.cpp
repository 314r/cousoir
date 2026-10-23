#include <QtTest>

#include "epub_book.h"
#include "epub_cfi.h"
#include "xml_tree.h"

using XmlTree::Node;

namespace {

void textsUnder(const Node &node, QList<const Node *> *texts)
{
    if (node.kind == Node::Kind::Text || node.kind == Node::Kind::Cdata)
        texts->append(&node);
    for (const auto &child : node.children)
        textsUnder(*child, texts);
}

// The element whose id is `id`.
const Node *byId(const Node &node, const QString &id)
{
    if (node.kind == Node::Kind::Element && node.attribute(QStringLiteral("id")) == id)
        return &node;
    for (const auto &child : node.children) {
        if (const Node *found = byId(*child, id))
            return found;
    }
    return nullptr;
}

const char page[] =
    "<?xml version=\"1.0\"?><!DOCTYPE html><!--top--><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>T</title></head>"
    "<body><p id=\"a\">One <em>two</em> three</p><p>x<!--c-->y</p><div id=\"d\">t<span id=\"d\">u</span></div></body></html>";

} // namespace

class EpubCfiTest : public QObject
{
    Q_OBJECT

private slots:
    void encodesAsCalibre();
    void decodesWhatItEncodes_data();
    void decodesWhatItEncodes();
    void escapes();
};

// Worked out by hand from cfi.pyj: elements even, the texts and the rest
// between them odd, a text's offset counted from the first of its siblings.
void EpubCfiTest::encodesAsCalibre()
{
    const std::unique_ptr<Node> root = XmlTree::parse(QByteArray(page));
    QVERIFY(root);
    const EpubCfi::Document document(*root);
    const Node *first = byId(*root, QStringLiteral("a"));
    QVERIFY(first);
    const Node &one = *first->children[0];
    const Node &two = *first->children[1]->children[0];
    const Node &three = *first->children[2];
    QCOMPARE(document.encode(one, 0), QStringLiteral("/2/4/2[a]/1:0"));
    QCOMPARE(document.encode(one, 2), QStringLiteral("/2/4/2[a]/1:2"));
    QCOMPARE(document.encode(two, 1), QStringLiteral("/2/4/2[a]/2/1:1"));
    // From the first text, the emphasis's own text not counted.
    QCOMPARE(document.encode(three, 3), QStringLiteral("/2/4/2[a]/1:7"));
    const Node &second = *root->children[2]->children[1]->children[1];
    QCOMPARE(document.encode(*second.children[2], 0), QStringLiteral("/2/4/4/1:1"));
    // A repeated id is no assertion.
    const Node &division = *root->children[2]->children[1]->children[2];
    QCOMPARE(document.encode(*division.children[0], 0), QStringLiteral("/2/4/6/1:0"));
    QCOMPARE(document.encode(*division.children[1]->children[0], 1), QStringLiteral("/2/4/6/2/1:1"));
    // An element, and an element's child element.
    QCOMPARE(document.encode(*first), QStringLiteral("/2/4/2[a]"));
    QCOMPARE(document.encode(*first, 1), QStringLiteral("/2/4/2[a]/2"));

    QCOMPARE(document.textBetween(document.decode(QStringLiteral("/2/4/2[a]/1:2")),
                                  document.decode(QStringLiteral("/2/4/2[a]/1:7"))),
             QStringLiteral("e two th"));
    // A place by its id alone, wherever the path says.
    QCOMPARE(document.decode(QStringLiteral("/2/4/99[a]/1:0")).node, &one);
    QVERIFY(!document.decode(QStringLiteral("/2/4/99/1:0")).node);
}

void EpubCfiTest::decodesWhatItEncodes_data()
{
    QTest::addColumn<QString>("book");
    QTest::newRow("odd pages") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/cfi.epub");
    QTest::newRow("voyage") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub");
    QTest::newRow("rich") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub");
}

// Every place of every page: decoded, the same place, or the end of the
// text before when it starts the next, as Calibre decodes it too. An id
// with a "]" stops Calibre's decoding, as below.
void EpubCfiTest::decodesWhatItEncodes()
{
    QFETCH(QString, book);
    QString error;
    const std::unique_ptr<EpubBook> epub = EpubBook::open(book, &error);
    QVERIFY2(epub, qPrintable(error));
    int places = 0;
    for (const EpubBook::SpineItem &item : epub->spine()) {
        const std::unique_ptr<Node> root = XmlTree::parse(epub->read(item.name));
        QVERIFY2(root, qPrintable(item.name));
        const EpubCfi::Document document(*root);
        QList<const Node *> texts;
        textsUnder(*root, &texts);
        for (const Node *text : std::as_const(texts)) {
            for (qsizetype offset = 0; offset <= text->text.size(); ++offset) {
                const QString cfi = document.encode(*text, offset);
                if (cfi.contains(QLatin1String("^]")))
                    continue;
                const EpubCfi::Point point = document.decode(cfi);
                QVERIFY2(point.node, qPrintable(item.name + u' ' + cfi));
                const bool same = point.node == text && point.offset == offset;
                const bool endBefore = offset == 0 && point.node != text && point.offset == point.node->text.size();
                QVERIFY2(same || endBefore, qPrintable(item.name + u' ' + cfi));
                QCOMPARE(document.encode(*point.node, point.offset), cfi);
                ++places;
            }
        }
    }
    QVERIFY(places > 100);
}

void EpubCfiTest::escapes()
{
    QCOMPARE(EpubCfi::escape(QStringLiteral("a[b]")), QStringLiteral("a^[b^]"));
    QCOMPARE(EpubCfi::escape(QStringLiteral("x,y;(p)^c~t@a!b")), QStringLiteral("x^,y^;^(p^)^^c^~t^@a^!b"));
    QCOMPARE(EpubCfi::unescape(QStringLiteral("x^,y^;^(p^)^^c^~t^@a^!b")), QStringLiteral("x,y;(p)^c~t@a!b"));

    const std::unique_ptr<Node> root = XmlTree::parse(
        QByteArrayLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p id=\"a[b]\">text</p></body></html>"));
    const EpubCfi::Document document(*root);
    const Node *paragraph = byId(*root, QStringLiteral("a[b]"));
    const QString cfi = document.encode(*paragraph->children[0], 2);
    QCOMPARE(cfi, QStringLiteral("/2/2/2[a^[b^]]/1:2"));
    // Calibre's decoding stops at the escaped "]": the paragraph, found by
    // its index, and no text.
    QCOMPARE(document.decode(cfi).node, paragraph);
}

QTEST_GUILESS_MAIN(EpubCfiTest)
#include "epub_cfi_test.moc"
