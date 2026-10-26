#include <QtTest>

#include <QJsonArray>
#include <QTemporaryDir>

#include "epub_book.h"
#include "epub_cfi.h"
#include "kepub.h"
#include "kobo_highlights.h"
#include "kobo_placement.h"
#include "sqlite_db.h"
#include "test_support.h"
#include "xml_tree.h"

using XmlTree::Node;

namespace {

const QString voyage = QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub");

// The text of each of Kobo's spans of a page, by id.
QHash<QString, QString> spanTexts(Node &root)
{
    QHash<QString, QString> texts;
    for (const Node *span : XmlTree::findAll(&root, [](const Node &n) {
             return n.isElement("span") && n.attribute(QStringLiteral("id")).startsWith(QLatin1String("kobo."));
         })) {
        if (span->children.size() == 1 && span->children.front()->kind == Node::Kind::Text)
            texts.insert(span->attribute(QStringLiteral("id")), span->children.front()->text);
    }
    return texts;
}

// The Voyage's highlights on a Kobo, from tests/data/kobo-highlights.sql.
QList<KoboHighlights::Highlight> voyageHighlights(QString *error)
{
    QTemporaryDir kobo;
    if (!makeKobo(kobo.path(), error))
        return {};
    SqliteDb db;
    const QString database = QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite"));
    if (!db.open(database, SqliteDb::Mode::ReadWrite, error)
        || !db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-highlights.sql"))), error))
        return {};
    QList<KoboHighlights::Highlight> highlights;
    for (const KoboHighlights::Book &book : KoboHighlights::read(db, error)) {
        if (book.title == QLatin1String("The Voyage"))
            highlights += book.highlights;
    }
    return highlights;
}

} // namespace

class KoboPlacementTest : public QObject
{
    Q_OBJECT

private slots:
    void replaysKepubifySpans_data();
    void replaysKepubifySpans();
    void replaysCalibresSpans();
    void placesHighlights();
    void placesInAKepub();
    void writesCalibresHighlight();
    void namesFiles();
    void placesTheKobosPlace();
    void findsSpansAgain();
};

void KoboPlacementTest::replaysKepubifySpans_data()
{
    QTest::addColumn<QString>("book");
    QTest::newRow("voyage") << voyage;
    QTest::newRow("rich") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub");
    QTest::newRow("odd pages") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/cfi.epub");
}

// Every span kepubify writes into a page is found in the page it was made
// of, with the same text.
void KoboPlacementTest::replaysKepubifySpans()
{
    QFETCH(QString, book);
    QString error;
    const std::unique_ptr<EpubBook> epub = EpubBook::open(book, &error);
    QVERIFY2(epub, qPrintable(error));
    int spans = 0;
    for (const EpubBook::SpineItem &item : epub->spine()) {
        const QByteArray page = epub->read(item.name);
        const std::unique_ptr<Node> root = XmlTree::parse(page);
        QVERIFY(root);
        const std::optional<QByteArray> kepubified = Kepub::kepubifyPage(page, QStringLiteral("kobo.js"));
        QVERIFY(kepubified);
        const std::unique_ptr<Node> written = XmlTree::parse(*kepubified);
        const QHash<QString, QString> texts = spanTexts(*written);
        const QHash<QString, Kepub::SpanStart> starts = Kepub::spanStarts(*root);
        QCOMPARE(starts.size(), texts.size());
        for (auto it = texts.cbegin(); it != texts.cend(); ++it) {
            const Kepub::SpanStart start = starts.value(it.key());
            QVERIFY2(start.text, qPrintable(item.name + u' ' + it.key()));
            QCOMPARE(start.text->text.mid(start.offset, start.length), it.value());
            ++spans;
        }
    }
    QVERIFY(spans > 0);
}

// And with Calibre's own numbering, on the chapter kepubify is checked by.
void KoboPlacementTest::replaysCalibresSpans()
{
    QString error;
    const std::unique_ptr<EpubBook> epub = EpubBook::open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub"), &error);
    QVERIFY2(epub, qPrintable(error));
    const std::unique_ptr<Node> root = XmlTree::parse(epub->read(QStringLiteral("OEBPS/text/chapter 1.xhtml")));
    const std::unique_ptr<Node> calibre =
        XmlTree::parse(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/chapter 1 by calibre.xhtml")));
    QVERIFY(root && calibre);
    const QHash<QString, QString> texts = spanTexts(*calibre);
    const QHash<QString, Kepub::SpanStart> starts = Kepub::spanStarts(*root);
    QVERIFY(!texts.isEmpty());
    for (auto it = texts.cbegin(); it != texts.cend(); ++it) {
        const Kepub::SpanStart start = starts.value(it.key());
        QVERIFY2(start.text, qPrintable(it.key()));
        QCOMPARE(start.text->text.mid(start.offset, start.length), it.value());
    }
}

// Each highlight of the fixture where it was made, and as it was found.
void KoboPlacementTest::placesHighlights()
{
    QString error;
    const QList<KoboHighlights::Highlight> highlights = voyageHighlights(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const std::unique_ptr<KoboPlacement::PlacementBook> book = KoboPlacement::PlacementBook::open(voyage, QStringLiteral("EPUB"), &error);
    QVERIFY2(book, qPrintable(error));
    const std::unique_ptr<EpubBook> epub = EpubBook::open(voyage, &error);
    QHash<QString, KoboPlacement::Placement> placed;
    for (const KoboHighlights::Highlight &highlight : highlights) {
        const KoboPlacement::Placement placement = book->place(highlight);
        placed.insert(highlight.id, placement);
        if (placement.method == KoboPlacement::Method::None)
            continue;
        // What the CFIs hold is what the Kobo highlighted.
        const std::unique_ptr<Node> root = XmlTree::parse(epub->read(placement.spineName));
        const EpubCfi::Document document(*root);
        QCOMPARE(KoboPlacement::folded(document.textBetween(document.decode(placement.startCfi), document.decode(placement.endCfi))),
                 KoboPlacement::folded(highlight.text));
    }
    const auto method = [&placed](const char *id) { return KoboPlacement::methodName(placed.value(QLatin1String(id)).method); };
    for (const char *id : { "v1", "v2", "v3", "v4", "v5", "v6" })
        QCOMPARE(method(id), QStringLiteral("spans"));
    QCOMPARE(method("v9"), QStringLiteral("text"));
    QCOMPARE(method("w1"), QStringLiteral("text"));
    QCOMPARE(method("v10"), QStringLiteral("book"));
    QCOMPARE(method("v8"), QStringLiteral("none"));
    QCOMPARE(placed.value(QStringLiteral("v8")).reason, QStringLiteral("note without highlighted text"));
    QCOMPARE(method("v11"), QStringLiteral("none"));
    QCOMPARE(placed.value(QStringLiteral("v11")).reason, QStringLiteral("text not found"));

    // The first paragraph of the first chapter: body, then its third element.
    const KoboPlacement::Placement first = placed.value(QStringLiteral("v1"));
    QCOMPARE(first.spineIndex, 1);
    QCOMPARE(first.spineName, QStringLiteral("OEBPS/text/chapter1.xhtml"));
    QCOMPARE(first.startCfi, QStringLiteral("/2/4/6/1:24"));
    QCOMPARE(first.endCfi, QStringLiteral("/2/4/6/1:53"));
    // In the chapter's first section, not its second.
    QCOMPARE(first.tocFamily, (QStringList{ QStringLiteral("The Harbour"), QStringLiteral("Arrival") }));
    // Into the link that follows the paragraph's text.
    QCOMPARE(placed.value(QStringLiteral("v2")).endCfi, QStringLiteral("/2/4/10/2[noteref]/1:7"));
    // From one paragraph into the next.
    QCOMPARE(placed.value(QStringLiteral("v3")).endCfi, QStringLiteral("/2/4/8/1:22"));
    // Where the third chapter's note is.
    QCOMPARE(placed.value(QStringLiteral("v10")).spineIndex, 3);
    QCOMPARE(placed.value(QStringLiteral("v10")).startCfi, QStringLiteral("/2/4/34[note1]/1:0"));
    QCOMPARE(placed.value(QStringLiteral("v10")).tocFamily, (QStringList{ QStringLiteral("The North"), QStringLiteral("Winter") }));
    // The same words, found from a plain EPUB's highlight.
    QCOMPARE(placed.value(QStringLiteral("w1")).startCfi, placed.value(QStringLiteral("v5")).startCfi);
}

// A library with the book as a KEPUB only: places in that KEPUB.
void KoboPlacementTest::placesInAKepub()
{
    QTemporaryDir folder;
    QString error;
    const QString kepub = folder.filePath(QStringLiteral("voyage.kepub"));
    QVERIFY2(Kepub::kepubify(voyage, kepub, {}, &error), qPrintable(error));
    const QList<KoboHighlights::Highlight> highlights = voyageHighlights(&error);
    const std::unique_ptr<KoboPlacement::PlacementBook> book = KoboPlacement::PlacementBook::open(kepub, QStringLiteral("KEPUB"), &error);
    QVERIFY2(book, qPrintable(error));
    const std::unique_ptr<EpubBook> epub = EpubBook::open(kepub, &error);
    int spans = 0;
    for (const KoboHighlights::Highlight &highlight : highlights) {
        const KoboPlacement::Placement placement = book->place(highlight);
        if (placement.method == KoboPlacement::Method::None)
            continue;
        spans += placement.method == KoboPlacement::Method::Spans;
        const std::unique_ptr<Node> root = XmlTree::parse(epub->read(placement.spineName));
        const EpubCfi::Document document(*root);
        QCOMPARE(KoboPlacement::folded(document.textBetween(document.decode(placement.startCfi), document.decode(placement.endCfi))),
                 KoboPlacement::folded(highlight.text));
    }
    QCOMPARE(spans, 6);
}

void KoboPlacementTest::writesCalibresHighlight()
{
    QString error;
    const QList<KoboHighlights::Highlight> highlights = voyageHighlights(&error);
    const std::unique_ptr<KoboPlacement::PlacementBook> book = KoboPlacement::PlacementBook::open(voyage, QStringLiteral("EPUB"), &error);
    for (const KoboHighlights::Highlight &highlight : highlights) {
        if (highlight.id != QLatin1String("v4"))
            continue;
        const QJsonObject json = KoboPlacement::annotation(highlight, book->place(highlight));
        QCOMPARE(json.value(QLatin1String("type")).toString(), QStringLiteral("highlight"));
        QCOMPARE(json.value(QLatin1String("uuid")).toString(), QStringLiteral("v4"));
        QCOMPARE(json.value(QLatin1String("highlighted_text")).toString(), QStringLiteral("See the note."));
        QCOMPARE(json.value(QLatin1String("notes")).toString(), QStringLiteral("Check it"));
        QCOMPARE(json.value(QLatin1String("start_cfi")).toString(), QStringLiteral("/2/4/10/2[noteref]/1:0"));
        QCOMPARE(json.value(QLatin1String("spine_name")).toString(), QStringLiteral("OEBPS/text/chapter1.xhtml"));
        QCOMPARE(json.value(QLatin1String("spine_index")).toInt(), 1);
        QCOMPARE(json.value(QLatin1String("style")).toObject().value(QLatin1String("which")).toString(), QStringLiteral("blue"));
        // Changed since made: when, its zone none, read as UTC.
        QCOMPARE(json.value(QLatin1String("timestamp")).toString(), QStringLiteral("2026-10-21T08:00:00.000Z"));
        QCOMPARE(json.value(QLatin1String("toc_family_titles")).toArray().size(), 2);
        return;
    }
    QFAIL("No v4 in the fixture");
}

// The file a Kobo's id names, as firmware 4.38 and older ones write them.
void KoboPlacementTest::namesFiles()
{
    QCOMPARE(KoboPlacement::fileOf(QStringLiteral("/mnt/onboard/A/B.kepub.epub!!OEBPS/Text/c1.xhtml")), QStringLiteral("OEBPS/Text/c1.xhtml"));
    // The section of the file a highlight is in.
    QCOMPARE(KoboPlacement::fileOf(QStringLiteral("/mnt/onboard/A/B.kepub.epub!!OEBPS/XHTML/c09.xhtml#d14e4446")),
             QStringLiteral("OEBPS/XHTML/c09.xhtml"));
    QCOMPARE(KoboPlacement::fileOf(QStringLiteral("file:///mnt/onboard/A/B.kepub.epub!OEBPS!text/c1.xhtml")), QStringLiteral("OEBPS/text/c1.xhtml"));
    QCOMPARE(KoboPlacement::fileOf(QStringLiteral("file:///mnt/onboard/A/B.epub#(2)OEBPS/text/c2.xhtml")), QStringLiteral("OEBPS/text/c2.xhtml"));
    QCOMPARE(KoboPlacement::spanPath(QStringLiteral("kobo.12.3")), QStringLiteral("span#kobo\\.12\\.3"));
}

// Where a Kobo stopped, by its span, in the EPUB and in a KEPUB; without its
// span, as far into the file; in a file of no text, at its start.
void KoboPlacementTest::placesTheKobosPlace()
{
    QTemporaryDir folder;
    QString error;
    const QString kepub = folder.filePath(QStringLiteral("voyage.kepub"));
    QVERIFY2(Kepub::kepubify(voyage, kepub, {}, &error), qPrintable(error));
    for (const auto &[path, format] : { std::pair(voyage, QStringLiteral("EPUB")), std::pair(kepub, QStringLiteral("KEPUB")) }) {
        const std::unique_ptr<KoboPlacement::PlacementBook> book = KoboPlacement::PlacementBook::open(path, format, &error);
        QVERIFY2(book, qPrintable(error));
        const KoboPlacement::Spot spot = book->placePoint(QStringLiteral("OEBPS/text/chapter1.xhtml#kobo.5.2"), 0);
        QCOMPARE(KoboPlacement::methodName(spot.method), QStringLiteral("spans"));
        QCOMPARE(spot.spineIndex, 1);
        QCOMPARE(spot.spineName, QStringLiteral("OEBPS/text/chapter1.xhtml"));
        if (format == QLatin1String("EPUB"))
            QCOMPARE(spot.cfi, QStringLiteral("/2/4/10/2[noteref]/1:0"));
        // And back, by the text there, to the span.
        const std::optional<KoboPlacement::TextSpot> text = book->textAt(spot.spineIndex, spot.cfi, QString());
        QVERIFY(text);
        QCOMPARE(text->text.left(13), QStringLiteral("See the note."));
    }
    const std::unique_ptr<KoboPlacement::PlacementBook> book = KoboPlacement::PlacementBook::open(voyage, QStringLiteral("EPUB"), &error);
    const KoboPlacement::Spot far = book->placePoint(QStringLiteral("OEBPS/text/chapter2.xhtml#kobo.99.1"), 0.5);
    QCOMPARE(KoboPlacement::methodName(far.method), QStringLiteral("text"));
    QCOMPARE(far.spineIndex, 2);
    QCOMPARE(KoboPlacement::methodName(book->placePoint(QStringLiteral("OEBPS/text/nowhere.xhtml#kobo.1.1"), 0).method),
             QStringLiteral("none"));
}

// The library's highlights found again by Kobo's spans in the KEPUB a Kobo
// reads: each where the Kobo would have it.
void KoboPlacementTest::findsSpansAgain()
{
    QTemporaryDir folder;
    QString error;
    const QString kepub = folder.filePath(QStringLiteral("voyage.kepub"));
    QVERIFY2(Kepub::kepubify(voyage, kepub, {}, &error), qPrintable(error));
    const QList<KoboHighlights::Highlight> highlights = voyageHighlights(&error);
    const std::unique_ptr<KoboPlacement::PlacementBook> library = KoboPlacement::PlacementBook::open(voyage, QStringLiteral("EPUB"), &error);
    const std::unique_ptr<KoboPlacement::PlacementBook> kobo = KoboPlacement::PlacementBook::open(kepub, QStringLiteral("KEPUB"), &error);
    QVERIFY(library && kobo);
    int found = 0;
    for (const KoboHighlights::Highlight &highlight : highlights) {
        const KoboPlacement::Placement placement = library->place(highlight);
        if (placement.method != KoboPlacement::Method::Spans)
            continue;
        const std::optional<KoboPlacement::TextSpot> text = library->textAt(placement.spineIndex, placement.startCfi, placement.endCfi);
        QVERIFY(text);
        const std::optional<KoboPlacement::SpanRange> range = kobo->spansAt(*text);
        QVERIFY2(range, qPrintable(highlight.id));
        // The same text between the spans found as between the Kobo's.
        KoboHighlights::Highlight again = highlight;
        again.startPath = KoboPlacement::spanPath(range->startSpan);
        again.startOffset = range->startOffset;
        again.endPath = KoboPlacement::spanPath(range->endSpan);
        again.endOffset = range->endOffset;
        const KoboPlacement::Placement placed = kobo->place(again);
        QCOMPARE(KoboPlacement::methodName(placed.method), QStringLiteral("spans"));
        QCOMPARE(range->spineName, placement.spineName);
        if (highlight.id == QLatin1String("v1")) {
            QCOMPARE(range->startSpan, QStringLiteral("kobo.3.1"));
            QCOMPARE(range->startOffset, 24);
            QCOMPARE(range->endOffset, 52);
        }
        ++found;
    }
    QCOMPARE(found, 6);
    // Text the Kobo's copy has not.
    QVERIFY(!kobo->spansAt({ QStringLiteral("OEBPS/text/chapter1.xhtml"), 0, 100, QStringLiteral("Words this book has not") }));
}

QTEST_GUILESS_MAIN(KoboPlacementTest)
#include "kobo_placement_test.moc"
