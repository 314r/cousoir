#include <QtTest>

#include <QRandomGenerator>
#include <QTemporaryDir>

#include <zip.h>

#include "book_search.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "html_soup.h"
#include "mobi_book.h"
#include "test_support.h"
#include "xml_tree.h"

namespace {

// An EPUB of `files`, by their names in the archive.
bool writeEpub(const QString &path, const QList<QPair<QString, QByteArray>> &files)
{
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!archive)
        return false;
    QList<QByteArray> kept;
    for (const auto &[name, data] : files) {
        kept.append(data);
        zip_source_t *source = zip_source_buffer(archive, kept.last().constData(), zip_uint64_t(kept.last().size()), 0);
        if (!source || zip_file_add(archive, name.toUtf8().constData(), source, ZIP_FL_OVERWRITE) < 0)
            return false;
    }
    return zip_close(archive) == 0;
}
} // namespace

class EpubBookTest : public QObject
{
    Q_OBJECT

private slots:
    void readsAnEpub3();
    void readsAnEpub2();
    void resolvesPaths();
    void refusesWhatIsNoEpub();
    void searchesTheText();
    void findsTheSentences();
    void readsKindleBooks_data();
    void readsKindleBooks();
    void refusesKindleBooksItCannotRead();
    void survivesDamagedKindleBooks();
    void mendsTagSoup();
    void readsFixedLayouts();
};

namespace {
// A book of two chapters for the search and the sentences: accents, a
// ligature, italics, white space, a soft hyphen, headings without white
// space before the paragraph after them, a style. After the test's class:
// moc misreads the raw strings before it.
bool writeTextEpub(const QString &path)
{
    const QByteArray chapter1 = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Un</title><style>p { color: red }</style></head>
<body><h1 id="un">L'Écume</h1>
<p>Les <i>jours</i> passent, deux
     mots et une lan&#173;terne.</p>
<h2 id="deux">Suite</h2><p>Une Œuvre, l'écume des JOURS.</p></body></html>)";
    const QByteArray chapter2 = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Deux</title></head>
<body><p>Encore l’écume, écume, écume.</p></body></html>)";
    return writeEpub(path, {
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"), R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)" },
        { QStringLiteral("content.opf"), R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Recherche</dc:title><dc:identifier id="id">x</dc:identifier></metadata><manifest><item id="c1" href="c1.xhtml" media-type="application/xhtml+xml"/><item id="c2" href="c2.xhtml" media-type="application/xhtml+xml"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="c1"/><itemref idref="c2"/></spine></package>)" },
        { QStringLiteral("toc.ncx"), R"(<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap><navPoint id="n1"><navLabel><text>Premier</text></navLabel><content src="c1.xhtml#un"/><navPoint id="n2"><navLabel><text>Suite</text></navLabel><content src="c1.xhtml#deux"/></navPoint></navPoint><navPoint id="n3"><navLabel><text>Second</text></navLabel><content src="c2.xhtml"/></navPoint></navMap></ncx>)" },
        { QStringLiteral("c1.xhtml"), chapter1 },
        { QStringLiteral("c2.xhtml"), chapter2 },
    });
}

} // namespace

void EpubBookTest::readsAnEpub3()
{
    QString error;
    const auto book = EpubBook::open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub"), &error);
    QVERIFY2(book, qPrintable(error));
    QCOMPARE(book->title(), QStringLiteral("L'Écume des jours"));
    QCOMPARE(book->language(), QStringLiteral("fr"));
    QCOMPARE(book->spine().size(), 2);
    QCOMPARE(book->spine().at(1).name, QStringLiteral("OEBPS/text/chapter 1.xhtml"));
    QCOMPARE(book->spine().at(1).size, 1336);
    QCOMPARE(book->toc().size(), 1);
    QCOMPARE(book->toc().first().title, QStringLiteral("Chapitre premier"));
    QCOMPARE(book->toc().first().name, QStringLiteral("OEBPS/text/chapter 1.xhtml"));
    QCOMPARE(book->spineIndex(QStringLiteral("OEBPS/text/chapter 1.xhtml")), 1);
    QVERIFY(book->read(QStringLiteral("OEBPS/text/chapter 1.xhtml")).contains("<body"));
    QCOMPARE(book->mediaType(QStringLiteral("OEBPS/style.css")), QStringLiteral("text/css"));
    QCOMPARE(book->mediaType(QStringLiteral("OEBPS/images/plate.png")), QStringLiteral("image/png"));
    QVERIFY(book->read(QStringLiteral("OEBPS/missing.xhtml")).isEmpty());
}

void EpubBookTest::readsAnEpub2()
{
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("book.epub"));
    const QByteArray chapter = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Text</p></body></html>";
    const QList<QPair<QString, QByteArray>> files{
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"),
          R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
             <rootfiles><rootfile full-path="content/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
             </container>)" },
        { QStringLiteral("content/package.opf"),
          R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">
             <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title> The   Black
             Company </dc:title><dc:language>en</dc:language></metadata>
             <manifest>
               <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
               <item id="c1" href="../text/one.html" media-type="application/xhtml+xml"/>
               <item id="c2" href="../text/two.html" media-type="application/xhtml+xml"/>
               <item id="notes" href="../text/notes.html" media-type="application/xhtml+xml"/>
               <item id="lost" href="../text/lost.html" media-type="application/xhtml+xml"/>
             </manifest>
             <spine toc="ncx" page-progression-direction="ltr"><itemref idref="c1"/><itemref idref="lost"/>
             <itemref idref="c2"/><itemref idref="notes" linear="no"/></spine></package>)" },
        { QStringLiteral("content/toc.ncx"),
          R"(<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>
             <navPoint id="a"><navLabel><text>Book One</text></navLabel><content src="../text/one.html"/>
               <navPoint id="b"><navLabel><text>The Company</text></navLabel><content src="../text/one.html#company"/></navPoint>
               <navPoint id="c"><navLabel><text>Croaker</text></navLabel><content src="../text/two.html"/></navPoint>
             </navPoint>
             <navPoint id="d"><navLabel><text>Notes</text></navLabel><content src="../text/notes.html"/></navPoint>
             </navMap></ncx>)" },
        { QStringLiteral("text/one.html"), chapter },
        { QStringLiteral("text/two.html"), chapter },
        { QStringLiteral("text/notes.html"), chapter },
    };
    QVERIFY(writeEpub(path, files));
    QString error;
    const auto book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    QCOMPARE(book->title(), QStringLiteral("The Black Company"));
    QCOMPARE(book->direction(), QStringLiteral("ltr"));
    // The item missing from the archive is left out.
    QCOMPARE(book->spine().size(), 3);
    QCOMPARE(book->spine().at(0).name, QStringLiteral("text/one.html"));
    QVERIFY(!book->spine().at(2).linear);
    QCOMPARE(book->toc().size(), 4);
    QCOMPARE(book->toc().at(1).title, QStringLiteral("The Company"));
    QCOMPARE(book->toc().at(1).name, QStringLiteral("text/one.html"));
    QCOMPARE(book->toc().at(1).fragment, QStringLiteral("company"));
    QCOMPARE(book->toc().at(1).depth, 1);
    QCOMPARE(book->toc().at(3).depth, 0);
    QCOMPARE(book->read(QStringLiteral("text/two.html")), chapter);
}

void EpubBookTest::resolvesPaths()
{
    QCOMPARE(EpubBook::resolve(QStringLiteral("OEBPS/Text/c.xhtml"), QStringLiteral("../Images/a%20b.png")),
             QStringLiteral("OEBPS/Images/a b.png"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("OEBPS/Text/c.xhtml"), QStringLiteral("#note")),
             QStringLiteral("OEBPS/Text/c.xhtml"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("c.xhtml"), QStringLiteral("d.xhtml#x")), QStringLiteral("d.xhtml"));
    QCOMPARE(EpubBook::resolve(QStringLiteral("a/b.xhtml"), QStringLiteral("../../../x.css")), QStringLiteral("x.css"));
}

void EpubBookTest::refusesWhatIsNoEpub()
{
    QTemporaryDir folder;
    QString error;
    QVERIFY(!EpubBook::open(folder.filePath(QStringLiteral("none.epub")), &error));
    QVERIFY(!error.isEmpty());
    const QString empty = folder.filePath(QStringLiteral("empty.epub"));
    QVERIFY(writeEpub(empty, { { QStringLiteral("mimetype"), "application/epub+zip" } }));
    QVERIFY(!EpubBook::open(empty, &error));
}

QTEST_GUILESS_MAIN(EpubBookTest)

// A search of the book's text: case, accents and ligatures ignored, across
// elements, white space and soft hyphens; each place by the CFIs that name
// its text, under its chapter.
void EpubBookTest::searchesTheText()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("search.epub"));
    QVERIFY(writeTextEpub(path));
    QString error;
    std::shared_ptr<const EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    BookSearch search(book);

    // Each place's CFIs name the text found, in its page.
    const auto textOf = [&](const BookSearch::Match &match) {
        const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(book->spine().at(match.spineIndex).name));
        const EpubCfi::Document document(*root);
        return document.textBetween(document.decode(match.start), document.decode(match.end));
    };

    bool more = true;
    QList<BookSearch::Match> found = search.find(QStringLiteral("ecume"), 1000, &more);
    QVERIFY(!more);
    QCOMPARE(found.size(), 5);
    QCOMPARE(found.at(0).spineIndex, 0);
    QCOMPARE(found.at(0).text, QStringLiteral("Écume"));
    QCOMPARE(textOf(found.at(0)), QStringLiteral("Écume"));
    QCOMPARE(found.at(0).chapter, QStringLiteral("Premier"));
    QCOMPARE(found.at(1).text, QStringLiteral("écume"));
    QCOMPARE(found.at(1).chapter, QStringLiteral("Suite"));
    QCOMPARE(found.at(1).before, QStringLiteral("…deux mots et une lanterne. Suite Une Œuvre, l'"));
    QCOMPARE(found.at(1).after, QStringLiteral(" des JOURS."));
    QCOMPARE(found.at(2).spineIndex, 1);
    QCOMPARE(found.at(2).chapter, QStringLiteral("Second"));
    for (const BookSearch::Match &match : std::as_const(found))
        QCOMPARE(textOf(match).toCaseFolded(), match.text.toCaseFolded());

    // Across elements, blocks, white space and a soft hyphen; a ligature.
    found = search.find(QStringLiteral("suite une"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("SuiteUne"));
    found = search.find(QStringLiteral("les jours"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("Les jours"));
    found = search.find(QStringLiteral(" deux  mots "));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("deux\n     mots"));
    found = search.find(QStringLiteral("lanterne"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("lan­terne"));
    QCOMPARE(found.first().text, QStringLiteral("lanterne"));
    found = search.find(QStringLiteral("oeuvre"));
    QCOMPARE(found.size(), 1);
    QCOMPARE(textOf(found.first()), QStringLiteral("Œuvre"));
    // Not the style's text.
    QVERIFY(search.find(QStringLiteral("color")).isEmpty());

    // At most so many, and told so.
    found = search.find(QStringLiteral("écume"), 2, &more);
    QCOMPARE(found.size(), 2);
    QVERIFY(more);
}

// The sentences read aloud: each block on its own, those with no letter
// left out, each with the CFIs naming its text; the one a place is in.
void EpubBookTest::findsTheSentences()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("sentences.epub"));
    QVERIFY(writeTextEpub(path));
    QString error;
    const std::unique_ptr<EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    const BookSentences sentences(*book, 0);
    QStringList texts;
    for (const BookSentences::Sentence &sentence : sentences.sentences())
        texts.append(sentence.text);
    QCOMPARE(texts, (QStringList{ QStringLiteral("L'Écume"), QStringLiteral("Les jours passent, deux mots et une lanterne."),
                                  QStringLiteral("Suite"), QStringLiteral("Une Œuvre, l'écume des JOURS.") }));
    const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(book->spine().at(0).name));
    const EpubCfi::Document document(*root);
    for (const BookSentences::Sentence &sentence : sentences.sentences()) {
        QString text = document.textBetween(document.decode(sentence.start), document.decode(sentence.end));
        text.remove(QChar(0x00ad));
        QCOMPARE(text.simplified(), sentence.text);
    }
    // A place in the second sentence, and before all of them.
    const BookSentences::Sentence &second = sentences.sentences().at(1);
    QCOMPARE(sentences.sentenceAt(sentences.cfiAt(second.at + 10)), 1);
    QCOMPARE(sentences.sentenceAt(sentences.cfiAt(sentences.sentences().at(3).at)), 3);
    QCOMPARE(sentences.sentenceAt(QStringLiteral("/2")), 0);
    QCOMPARE(sentences.sentenceAt(QString()), 0);
    // The second chapter: one block, three sentences; a file that is not.
    QCOMPARE(BookSentences(*book, 1).sentences().size(), 1);
    QVERIFY(BookSentences(*book, 7).sentences().isEmpty());
}

void EpubBookTest::readsKindleBooks_data()
{
    QTest::addColumn<QString>("file");
    QTest::newRow("azw3") << QStringLiteral("voyage.azw3");
    QTest::newRow("mobi, both kinds") << QStringLiteral("voyage.mobi");
    QTest::newRow("mobi, old kind") << QStringLiteral("voyage-old.mobi");
}

// A Kindle book Calibre made of an EPUB, made an EPUB again: its text in
// the same order, its contents, its picture, its links.
void EpubBookTest::readsKindleBooks()
{
    QFETCH(QString, file);
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("book.epub"));
    QString error;
    QVERIFY2(MobiBook::toEpub(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/") + file, path, &error), qPrintable(error));
    const std::unique_ptr<EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    const std::unique_ptr<EpubBook> source = EpubBook::open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub"), &error);
    QVERIFY2(source, qPrintable(error));
    QCOMPARE(book->title(), source->title());
    QCOMPARE(book->language(), QStringLiteral("en"));

    // Every paragraph, in the book's order.
    const auto paragraphs = [](const EpubBook &epub) {
        QStringList texts;
        for (int i = 0; i < epub.spine().size(); ++i) {
            const BookSentences sentences(epub, i);
            for (const BookSentences::Sentence &sentence : sentences.sentences()) {
                if (sentence.text.startsWith(QLatin1String("Chapter ")))
                    texts.append(sentence.text);
            }
        }
        return texts;
    };
    const QStringList expected = paragraphs(*source);
    QVERIFY(expected.size() > 50);
    QCOMPARE(paragraphs(*book), expected);
    for (int i = 0; i < book->spine().size(); ++i)
        QCOMPARE(book->mediaType(book->spine().at(i).name), QStringLiteral("application/xhtml+xml"));

    // The contents' titles, and where they go.
    QStringList titles;
    for (const EpubBook::TocEntry &entry : book->toc()) {
        titles.append(entry.title);
        QVERIFY2(book->spineIndex(entry.name) >= 0, qPrintable(entry.name));
    }
    for (const char *title : { "The Harbour", "Arrival", "The Lantern", "The Valley", "Thunder", "The North" })
        QVERIFY2(titles.contains(QLatin1String(title)), qPrintable(titles.join(u'|')));
    // A section's entry goes to its heading, not to another in its file.
    for (const EpubBook::TocEntry &entry : book->toc()) {
        if (entry.fragment.isEmpty() || (entry.title != QLatin1String("Arrival") && entry.title != QLatin1String("The Lantern")
                                         && entry.title != QLatin1String("Thunder")))
            continue;
        const std::unique_ptr<XmlTree::Node> page = XmlTree::parse(book->read(entry.name));
        QVERIFY(page);
        const EpubCfi::Document document(*page);
        const XmlTree::Node *anchor = document.element(entry.fragment);
        QVERIFY2(anchor, qPrintable(entry.fragment));
        // The heading itself, or an empty anchor of the old kind before it.
        const EpubCfi::PageText text = EpubCfi::PageText::of(*page);
        qsizetype at = -1;
        for (qsizetype i = 0; i < text.starts.size() && at < 0; ++i) {
            if (text.order.value(text.starts.at(i).node, -1) > text.order.value(anchor, -1))
                at = i;
        }
        QVERIFY2(at >= 0 && text.text.mid(at).startsWith(entry.title), qPrintable(entry.title + u" → " + text.text.mid(at, 40)));
    }

    // The picture of the second chapter, and the link to the note of the
    // third, to its paragraph.
    bool picture = false;
    QString note;
    for (int i = 0; i < book->spine().size(); ++i) {
        const QString name = book->spine().at(i).name;
        const QByteArray page = book->read(name);
        static const QRegularExpression image(QStringLiteral("<img[^>]+src=\"([^\"]+)\""));
        for (QRegularExpressionMatchIterator it = image.globalMatch(QString::fromUtf8(page)); it.hasNext();) {
            const QString target = EpubBook::resolve(name, it.next().captured(1));
            picture = picture || (book->contains(target) && !book->read(target).isEmpty());
        }
        static const QRegularExpression link(QStringLiteral("<a[^>]+href=\"([^\"]+)\"[^>]*>(?:<[^>]+>)*See the note"));
        const QRegularExpressionMatch match = link.match(QString::fromUtf8(page));
        if (match.hasMatch())
            note = EpubBook::resolve(name, match.captured(1).section(u'#', 0, 0)) + u'#' + match.captured(1).section(u'#', 1);
    }
    QVERIFY(picture);
    QVERIFY2(note.contains(u'#'), qPrintable(note));
    const QString noteFile = note.section(u'#', 0, 0);
    const QString noteId = note.section(u'#', 1);
    QVERIFY(book->spineIndex(noteFile) >= 0);
    const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(noteFile));
    QVERIFY(root);
    const EpubCfi::Document document(*root);
    const XmlTree::Node *target = document.element(noteId);
    QVERIFY2(target, qPrintable(noteId));
    QString text;
    const std::function<void(const XmlTree::Node &)> walk = [&](const XmlTree::Node &node) {
        for (const auto &child : node.children) {
            if (child->kind == XmlTree::Node::Kind::Text)
                text += child->text;
            walk(*child);
        }
    };
    // The note's paragraph, or the paragraph of the old kind's empty anchor.
    walk(target->children.empty() && target->parent ? *target->parent : *target);
    QVERIFY2(text.contains(QLatin1String("The note: rivers remember every stone.")), qPrintable(text));
}

// What Cousoir cannot read, said.
void EpubBookTest::refusesKindleBooksItCannotRead()
{
    QTemporaryDir dir;
    QString error;
    QVERIFY(!MobiBook::toEpub(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub"), dir.filePath(QStringLiteral("b.epub")),
                              &error));
    QVERIFY(!error.isEmpty());
    // A Kindle book with DRM: its first record's encryption set.
    QFile mobi(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.azw3"));
    QVERIFY(mobi.open(QIODevice::ReadOnly));
    QByteArray data = mobi.readAll();
    const quint32 record0 = qFromBigEndian<quint32>(data.constData() + 78);
    data[record0 + 13] = 2;
    const QString locked = dir.filePath(QStringLiteral("locked.azw3"));
    QFile out(locked);
    QVERIFY(out.open(QIODevice::WriteOnly) && out.write(data) == data.size());
    out.close();
    error.clear();
    QVERIFY(!MobiBook::toEpub(locked, dir.filePath(QStringLiteral("c.epub")), &error));
    QVERIFY2(error.contains(QLatin1String("DRM")), qPrintable(error));
}

// Kindle books damaged every which way, as a file from anywhere may be:
// bytes changed, the file cut short, the headers' counts and offsets at
// their extremes. Read or refused, never a crash (run it under
// AddressSanitizer too); a page made is XML.
void EpubBookTest::survivesDamagedKindleBooks()
{
    QTemporaryDir dir;
    // COUSOIR_FUZZ_ROUNDS and COUSOIR_FUZZ_SEED make longer, other runs.
    const int rounds = qEnvironmentVariableIntValue("COUSOIR_FUZZ_ROUNDS") > 0 ? qEnvironmentVariableIntValue("COUSOIR_FUZZ_ROUNDS") : 150;
    QRandomGenerator random(quint32(qEnvironmentVariableIntValue("COUSOIR_FUZZ_SEED")) + 20261025);
    int read = 0;
    for (const char *name : { "voyage.azw3", "voyage.mobi", "voyage-old.mobi" }) {
        QFile file(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/") + QLatin1String(name));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        const quint32 record0 = qFromBigEndian<quint32>(original.constData() + 78);
        for (int round = 0; round < rounds; ++round) {
            QByteArray data = original;
            switch (round % 5) {
            case 0: // Bytes changed anywhere.
                for (int n = 0, count = 1 + random.bounded(40); n < count; ++n)
                    data[random.bounded(int(data.size()))] = char(random.bounded(256));
                break;
            case 1: // Cut short.
                data.truncate(random.bounded(int(data.size())));
                break;
            case 2: // A field of the first record's headers at an extreme.
            {
                const qsizetype at = record0 + 4 * random.bounded(80);
                const quint32 value = std::array<quint32, 4>{ 0xFFFFFFFF, 0x7FFFFFFF, 0, 1 }[random.bounded(4)];
                if (at + 4 <= data.size())
                    qToBigEndian(value, data.data() + at);
                break;
            }
            case 3: // The record table's offsets.
                for (int n = 0, count = 1 + random.bounded(4); n < count; ++n) {
                    const qsizetype at = 78 + 8 * random.bounded(int(qFromBigEndian<quint16>(original.constData() + 76)));
                    qToBigEndian(random.bounded(2) ? quint32(0xFFFFFFF0) : quint32(random.bounded(int(data.size()))),
                                 data.data() + at);
                }
                break;
            default: // Bytes changed in the records after the text: the
                     // indices, the pictures, the fonts.
                for (int n = 0, count = 1 + random.bounded(200); n < count; ++n) {
                    const qsizetype at = data.size() / 2 + random.bounded(int(data.size() / 2));
                    data[at] = char(random.bounded(256));
                }
            }
            const QString damaged = dir.filePath(QStringLiteral("damaged.azw3"));
            QFile out(damaged);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(data);
            out.close();
            QString error;
            const QString epub = dir.filePath(QStringLiteral("damaged.epub"));
            if (!MobiBook::toEpub(damaged, epub, &error))
                continue;
            ++read;
            const std::unique_ptr<EpubBook> book = EpubBook::open(epub, &error);
            QVERIFY2(book, qPrintable(error));
        }
    }
    // Most damages still leave a book to read.
    QVERIFY2(read > rounds * 2 / 3, qPrintable(QString::number(read)));
}

// Old HTML read as XHTML: blocks closing paragraphs, items closing items,
// what is left open closed; and what a hostile page could do bounded.
void EpubBookTest::mendsTagSoup()
{
    const auto bodyOf = [](const QString &html) {
        const std::unique_ptr<XmlTree::Node> root = HtmlSoup::parse(html);
        QByteArray xml = XmlTree::serialize(*root);
        // A page made is XML.
        if (!XmlTree::parse(xml))
            return QByteArray("not XML");
        const qsizetype start = xml.indexOf("<body");
        return xml.mid(start, xml.indexOf("</body>") + 7 - start);
    };
    QCOMPARE(bodyOf(QStringLiteral("<p>one<p>two<ul><li>a<li>b</ul><b>bold<i>both</b>after")),
             QByteArray("<body><p>one</p><p>two</p><ul><li>a</li><li>b</li></ul><b>bold<i>both</i></b>after</body>"));
    QCOMPARE(bodyOf(QStringLiteral("<body xmlns=\"\" class=a>x &amp; y &eacute;<script>bad()</script><mbp:pagebreak/>z</body>")),
             QByteArray("<body class=\"a\">x &amp; y \xc3\xa9z</body>"));
    // Thousands deep, and "<" without ">": quick, and bounded.
    QElapsedTimer timer;
    timer.start();
    QString deep;
    for (int i = 0; i < 100000; ++i)
        deep += QStringLiteral("<b>");
    QVERIFY(bodyOf(deep + QStringLiteral("text")).contains("text"));
    QString open;
    for (int i = 0; i < 100000; ++i)
        open += QStringLiteral("<a");
    QVERIFY(bodyOf(open).contains("&lt;a"));
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
}

// A picture book's pages laid out once, at their own size, and the side of
// a spread each goes on: as its spine says, else across from the page
// before, the first on the side the book opens to.
void EpubBookTest::readsFixedLayouts()
{
    QString error;
    const auto book = EpubBook::open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/fixed.epub"), &error);
    QVERIFY2(book, qPrintable(error));
    QStringList sides;
    for (const EpubBook::SpineItem &item : book->spine())
        sides.append(item.fixed ? item.side : QStringLiteral("flows"));
    QCOMPARE(sides, (QStringList{ QStringLiteral("right"), QStringLiteral("left"), QStringLiteral("right"),
                                  QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("center"),
                                  QStringLiteral("flows") }));

    // EPUB 2's way, Apple's, a book read from right to left, one whose
    // pages go one by one, and a fixed page in a book that flows.
    QTemporaryDir dir;
    const auto sidesOf = [&](const QString &metadata, const QString &spine, const QString &options = QString()) {
        QString manifest;
        QString itemrefs;
        for (int i = 1; i <= 3; ++i) {
            manifest += QStringLiteral("<item id=\"p%1\" href=\"p%1.xhtml\" media-type=\"application/xhtml+xml\"/>").arg(i);
            itemrefs += QStringLiteral("<itemref idref=\"p%1\"%2/>").arg(i).arg(i == 2 ? spine : QString());
        }
        QList<QPair<QString, QByteArray>> files{
            { QStringLiteral("mimetype"), "application/epub+zip" },
            { QStringLiteral("META-INF/container.xml"), "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>" },
            { QStringLiteral("content.opf"),
              QStringLiteral("<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Pages</dc:title><dc:identifier id=\"id\">x</dc:identifier>%1</metadata><manifest>%2</manifest><spine%3>%4</spine></package>")
                  .arg(metadata, manifest, metadata.contains(QLatin1String("rtl")) ? QStringLiteral(" page-progression-direction=\"rtl\"") : QString(), itemrefs)
                  .toUtf8() },
        };
        if (!options.isEmpty())
            files.append({ QStringLiteral("META-INF/com.apple.ibooks.display-options.xml"), options.toUtf8() });
        for (int i = 1; i <= 3; ++i)
            files.append({ QStringLiteral("p%1.xhtml").arg(i), "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>A page.</p></body></html>" });
        const QString path = dir.filePath(QStringLiteral("pages.epub"));
        QStringList found;
        QString error;
        const auto opened = writeEpub(path, files) ? EpubBook::open(path, &error) : nullptr;
        for (const EpubBook::SpineItem &item : opened ? opened->spine() : QList<EpubBook::SpineItem>())
            found.append(item.fixed ? item.side : QStringLiteral("flows"));
        return found.join(u' ');
    };
    QCOMPARE(sidesOf(QStringLiteral("<meta name=\"fixed-layout\" content=\"true\"/>"), QString()),
             QStringLiteral("right left right"));
    QCOMPARE(sidesOf(QString(), QString(),
                     QStringLiteral("<?xml version=\"1.0\"?><display_options><platform name=\"*\"><option name=\"fixed-layout\">true</option></platform></display_options>")),
             QStringLiteral("right left right"));
    QCOMPARE(sidesOf(QStringLiteral("<meta property=\"rendition:layout\">pre-paginated</meta><!-- rtl -->"), QString()),
             QStringLiteral("left right left"));
    QCOMPARE(sidesOf(QStringLiteral("<meta property=\"rendition:layout\">pre-paginated</meta><meta property=\"rendition:spread\">none</meta>"),
                     QString()),
             QStringLiteral("center center center"));
    QCOMPARE(sidesOf(QString(), QStringLiteral(" properties=\"rendition:layout-pre-paginated page-spread-left\"")),
             QStringLiteral("flows left flows"));
    QCOMPARE(sidesOf(QString(), QString()), QStringLiteral("flows flows flows"));
}

#include "epub_book_test.moc"
