#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "highlight_formats.h"
#include "kobo.h"
#include "kobo_highlights.h"
#include "sqlite_db.h"
#include "test_support.h"

using HighlightFormats::Format;

namespace {

// kbextract2's own fixture, as a Kobo database.
bool kbextractDatabase(const QString &path, QString *error)
{
    SqliteDb db;
    return writeAll(path, {}) && db.open(path, SqliteDb::Mode::ReadWrite, error)
        && db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-kbextract.sql"))), error);
}

QList<KoboHighlights::Book> readDatabase(const QString &path, QString *error)
{
    SqliteDb db;
    if (!db.open(path, SqliteDb::Mode::Immutable, error))
        return {};
    return KoboHighlights::read(db, error);
}

const KoboHighlights::Book *bookOf(const QList<KoboHighlights::Book> &books, const QString &volumeId)
{
    for (const KoboHighlights::Book &book : books) {
        if (book.volumeId == volumeId)
            return &book;
    }
    return nullptr;
}

const KoboHighlights::Highlight *highlightOf(const KoboHighlights::Book &book, const QString &id)
{
    for (const KoboHighlights::Highlight &highlight : book.highlights) {
        if (highlight.id == id)
            return &highlight;
    }
    return nullptr;
}

// What a book's highlights show: those not hidden.
QString rendered(const KoboHighlights::Book &book, Format format)
{
    QList<HighlightFormats::Entry> entries;
    for (const KoboHighlights::Highlight &highlight : book.highlights) {
        if (!highlight.hidden)
            entries.append(KoboHighlights::entry(highlight));
    }
    return HighlightFormats::render(entries, format);
}

} // namespace

class KoboHighlightsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void formatsAsKbextract();
    void groupsSplitFilesUnderTheirChapter();
    void cleansText();
    void readsWhereTheyAre();
    void keepsSnapshots();
    void tellsWhoseDatabase();
};

// kbextract2's TestFormatsSelectedBookAsMarkdown, its strings as they are.
void KoboHighlightsTest::formatsAsKbextract()
{
    QTemporaryDir folder;
    QString error;
    const QString path = folder.filePath(QStringLiteral("KoboReader.sqlite"));
    QVERIFY2(kbextractDatabase(path, &error), qPrintable(error));
    const QList<KoboHighlights::Book> books = readDatabase(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Blank rows and corners left out; the book without a row of its own
    // goes by its id.
    QStringList titles;
    for (const KoboHighlights::Book &book : books)
        titles.append(book.title);
    QCOMPARE(titles, (QStringList{ QStringLiteral("Alpha"), QStringLiteral("beta"), QStringLiteral("Le labyrinthe des égarés"),
                                   QStringLiteral("missing-volume") }));

    const KoboHighlights::Book *alpha = bookOf(books, QStringLiteral("book-alpha"));
    QVERIFY(alpha);
    QCOMPARE(alpha->authors, QStringList{ QStringLiteral("Author A") });
    QCOMPARE(rendered(*alpha, Format::Markdown),
             QStringLiteral("## Chapter One\n\n"
                            "> Stars *stay* & <tag> second line\n"
                            ">\n"
                            "> Another paragraph continues here\n\n\n"
                            "> Selected note text\n\n"
                            "Written note\n"
                            "second note line\n\n\n"
                            "## Chapter Two\n\n"
                            "> Later text"));
    QCOMPARE(rendered(*alpha, Format::Obsidian),
             QStringLiteral("## Chapter One\n\n"
                            "> [!quote]\n"
                            "> Stars *stay* & <tag> second line\n"
                            ">\n"
                            "> Another paragraph continues here\n\n\n"
                            "> [!quote]\n"
                            "> Selected note text\n\n"
                            "Written note\n"
                            "second note line\n\n\n"
                            "## Chapter Two\n\n"
                            "> [!quote]\n"
                            "> Later text"));
    QCOMPARE(rendered(*alpha, Format::Plain),
             QStringLiteral("Chapter One\n\n"
                            "Stars *stay* & <tag> second line\n\n"
                            "Another paragraph continues here\n\n\n"
                            "Selected note text\n\n"
                            "Written note\n"
                            "second note line\n\n\n"
                            "Chapter Two\n\n"
                            "Later text"));

    const KoboHighlights::Book *beta = bookOf(books, QStringLiteral("book-beta"));
    QVERIFY(beta);
    QCOMPARE(rendered(*beta, Format::Markdown), QStringLiteral("## Beta Chapter\n\nMargin note"));
    QCOMPARE(rendered(*beta, Format::Obsidian), QStringLiteral("## Beta Chapter\n\nMargin note"));
    QCOMPARE(rendered(*beta, Format::Plain), QStringLiteral("Beta Chapter\n\nMargin note"));

    const KoboHighlights::Book *missing = bookOf(books, QStringLiteral("missing-volume"));
    QVERIFY(missing);
    QCOMPARE(rendered(*missing, Format::Markdown), QStringLiteral("## Untitled chapter\n\n> Orphaned highlight"));
    QCOMPARE(rendered(*missing, Format::Obsidian),
             QStringLiteral("## Untitled chapter\n\n> [!quote]\n> Orphaned highlight"));
    QCOMPARE(rendered(*missing, Format::Plain), QStringLiteral("Untitled chapter\n\nOrphaned highlight"));

    // Several books, each under its title.
    QCOMPARE(HighlightFormats::render({ { QStringLiteral("Beta"), { KoboHighlights::entry(beta->highlights.first()) } },
                                        { QStringLiteral("Lost"), { KoboHighlights::entry(missing->highlights.first()) } } },
                                      Format::Markdown),
             QStringLiteral("# Beta\n\n## Beta Chapter\n\nMargin note\n\n\n"
                            "# Lost\n\n## Untitled chapter\n\n> Orphaned highlight"));
}

// kbextract2's TestGroupsSplitContentFilesUnderNavigationChapters.
void KoboHighlightsTest::groupsSplitFilesUnderTheirChapter()
{
    QTemporaryDir folder;
    QString error;
    const QString path = folder.filePath(QStringLiteral("KoboReader.sqlite"));
    QVERIFY2(kbextractDatabase(path, &error), qPrintable(error));
    const QList<KoboHighlights::Book> books = readDatabase(path, &error);
    const KoboHighlights::Book *split = bookOf(books, QStringLiteral("book-split"));
    QVERIFY(split);
    const QString markdown = rendered(*split, Format::Markdown);
    QCOMPARE(markdown, QStringLiteral("## I. Les étincelles japonaises\n\n"
                                      "> First part-one highlight\n\n\n"
                                      "> Second part-one highlight\n\n\n"
                                      "## III. Une si longue marche\n\n"
                                      "> Part-three highlight\n\n\n"
                                      "## Épilogue. Un monde à reconstruire\n\n"
                                      "> First epilogue highlight\n\n\n"
                                      "> Second epilogue highlight"));
    QVERIFY(!markdown.contains(QLatin1String(".xhtml")));
    QCOMPARE(rendered(*split, Format::Plain), QStringLiteral("I. Les étincelles japonaises\n\n"
                                                             "First part-one highlight\n\n\n"
                                                             "Second part-one highlight\n\n\n"
                                                             "III. Une si longue marche\n\n"
                                                             "Part-three highlight\n\n\n"
                                                             "Épilogue. Un monde à reconstruire\n\n"
                                                             "First epilogue highlight\n\n\n"
                                                             "Second epilogue highlight"));
}

// kbextract2's text_test.go.
void KoboHighlightsTest::cleansText()
{
    QCOMPARE(HighlightFormats::normalizedText(QStringLiteral("  a\r\nb\rc \t")), QStringLiteral("a\nb\nc"));
    QCOMPARE(HighlightFormats::singleLineText(QStringLiteral("I. Les  étincelles\n\njaponaises ")),
             QStringLiteral("I. Les étincelles japonaises"));
    QCOMPARE(HighlightFormats::singleLineText(QStringLiteral("I.") + QChar(0x2002) + QStringLiteral("Les")),
             QStringLiteral("I. Les"));
    QCOMPARE(HighlightFormats::reflowedHighlight(QStringLiteral("  first  line\nsecond\n\n\n  third\r\nfourth ")),
             QStringLiteral("first  line second\n\nthird fourth"));
    QCOMPARE(HighlightFormats::render({ { QStringLiteral("a"), QStringLiteral("one\n\ntwo"), {}, QStringLiteral("k"), QStringLiteral("K"), 1 } },
                                      Format::Markdown),
             QStringLiteral("## K\n\n> one\n>\n> two"));
    // A note keeps its own lines and indentation.
    QCOMPARE(HighlightFormats::render({ { QStringLiteral("a"), {}, QStringLiteral(" a\n  indented\n\nb "), QStringLiteral("k"), QStringLiteral("K"), 1 } },
                                      Format::Markdown),
             QStringLiteral("## K\n\na\n  indented\n\nb"));
    QCOMPARE(KoboHighlights::chapterIdOf(QStringLiteral("book-split!OPS!p1.xhtml-1")), QStringLiteral("book-split!OPS!p1.xhtml"));
    QCOMPARE(KoboHighlights::chapterIdOf(QStringLiteral("book-beta!opf!chapter-2-1")), QStringLiteral("book-beta!opf!chapter-2"));
    QCOMPARE(KoboHighlights::chapterIdOf(QStringLiteral("book-alpha")), QStringLiteral("book"));
    QCOMPARE(KoboHighlights::chapterIdOf(QStringLiteral("book!OPS!file.xhtml")), QStringLiteral("book!OPS!file.xhtml"));
    QCOMPARE(KoboHighlights::chapterIdOf(QStringLiteral("plain")), QStringLiteral("plain"));
    // Chapters without an order last, the others by it.
    QCOMPARE(HighlightFormats::render({ { QStringLiteral("a"), QStringLiteral("unordered"), {}, QStringLiteral("x"), QStringLiteral("X"), std::nullopt },
                                        { QStringLiteral("b"), QStringLiteral("second"), {}, QStringLiteral("y"), QStringLiteral("Y"), 2 },
                                        { QStringLiteral("c"), QStringLiteral("first"), {}, QStringLiteral("z"), QStringLiteral("Z"), 1 } },
                                      Format::Markdown),
             QStringLiteral("## Z\n\n> first\n\n\n## Y\n\n> second\n\n\n## X\n\n> unordered"));
    QCOMPARE(KoboHighlights::calibreColor(-1), QStringLiteral("yellow"));
    QCOMPARE(KoboHighlights::calibreColor(1), QStringLiteral("red"));
    QCOMPARE(KoboHighlights::calibreColor(3), QStringLiteral("green"));
}

// Cousoir's own Kobo: where each highlight starts and ends, when it was
// made, hidden ones kept, corners and blank rows left out, and the books it
// still has.
void KoboHighlightsTest::readsWhereTheyAre()
{
    QTemporaryDir kobo;
    QString error;
    QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
    {
        SqliteDb db;
        QVERIFY2(db.open(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")), SqliteDb::Mode::ReadWrite,
                         &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-reading.sql"))), &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QStringLiteral("UPDATE Bookmark SET DateModified = '2026-10-19T08:00:00Z' WHERE BookmarkID = 'e2'"),
                         &error),
                 qPrintable(error));
    }
    QList<KoboHighlights::Book> books =
        readDatabase(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    KoboHighlights::attach(&books, Kobo::books(Kobo::describe(kobo.path(), {}), &error));

    const KoboHighlights::Book *ecume = bookOf(books, QStringLiteral("file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub"));
    QVERIFY(ecume);
    QCOMPARE(ecume->title, QStringLiteral("L'Écume des jours"));
    QCOMPARE(ecume->authors, QStringList{ QStringLiteral("Boris Vian") });
    QVERIFY(ecume->onDevice);
    QCOMPARE(ecume->format, QStringLiteral("KEPUB"));
    QCOMPARE(ecume->lpath, QStringLiteral("Vian, Boris/Ecume.kepub.epub"));
    QStringList ids;
    for (const KoboHighlights::Highlight &highlight : ecume->highlights)
        ids.append(highlight.id);
    // e4 a corner, e7 blank; how far into their chapter, as kbextract2 has
    // them, the chapters put in order when written out.
    QCOMPARE(ids, (QStringList{ QStringLiteral("e8"), QStringLiteral("e3"), QStringLiteral("e1"), QStringLiteral("e5"),
                                QStringLiteral("e6"), QStringLiteral("e2") }));
    const KoboHighlights::Highlight &first = *highlightOf(*ecume, QStringLiteral("e1"));
    QCOMPARE(first.contentId, QStringLiteral("file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub!OEBPS!Text/ch01.xhtml"));
    QCOMPARE(first.startPath, QStringLiteral("span#kobo\\.3\\.1"));
    QCOMPARE(first.startOffset, 10);
    QCOMPARE(first.endPath, QStringLiteral("span#kobo\\.3\\.2"));
    QCOMPARE(first.endOffset, 40);
    QCOMPARE(first.text, QStringLiteral("Colin achevait sa toilette."));
    QCOMPARE(first.created, QStringLiteral("2026-10-18T20:01:00.000"));
    QCOMPARE(first.chapterTitle, QStringLiteral("Un"));
    QCOMPARE(first.chapterOrder, std::optional<qint64>(0));
    QCOMPARE(first.color, -1);
    QVERIFY(!first.hidden);
    const KoboHighlights::Highlight &noted = *highlightOf(*ecume, QStringLiteral("e2"));
    QCOMPARE(noted.note, QStringLiteral("Le nénuphar, déjà ?"));
    QCOMPARE(noted.modified, QStringLiteral("2026-10-19T08:00:00Z"));
    QVERIFY(highlightOf(*ecume, QStringLiteral("e5"))->hidden);
    const KoboHighlights::Highlight &note = *highlightOf(*ecume, QStringLiteral("e6"));
    QCOMPARE(note.text, QString());
    QCOMPARE(note.note, QStringLiteral("Seulement une note"));
    QCOMPARE(note.chapterTitle, QStringLiteral("Trois"));
    QCOMPARE(highlightOf(*ecume, QStringLiteral("e8"))->chapterTitle, QString());

    // A plain EPUB's chapters, the Kobo's rows of type 9.
    const KoboHighlights::Book *mort = bookOf(books, QStringLiteral("file:///mnt/onboard/Pratchett, Terry/Mort.epub"));
    QVERIFY(mort);
    QCOMPARE(mort->format, QStringLiteral("EPUB"));
    QCOMPARE(highlightOf(*mort, QStringLiteral("m2"))->startPath, QStringLiteral("/1/4/1"));
    QCOMPARE(highlightOf(*mort, QStringLiteral("m2"))->chapterTitle, QStringLiteral("Chapter 1"));
    // Bought from Kobo: its file in .kobo/kepub.
    const KoboHighlights::Book *bought = bookOf(books, QStringLiteral("5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d"));
    QVERIFY(bought);
    QVERIFY(bought->purchased);
    QCOMPARE(bought->highlights.first().chapterTitle, QStringLiteral("Opening"));

    // A database that is no Kobo's.
    QTemporaryDir other;
    SqliteDb db;
    QVERIFY(writeAll(other.filePath(QStringLiteral("other.db")), {}));
    QVERIFY2(db.open(other.filePath(QStringLiteral("other.db")), SqliteDb::Mode::ReadWrite, &error), qPrintable(error));
    QVERIFY(db.exec(QStringLiteral("CREATE TABLE notes (text TEXT)"), &error));
    error.clear();
    QVERIFY(KoboHighlights::read(db, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void KoboHighlightsTest::keepsSnapshots()
{
    QTemporaryDir folder;
    QString error;
    const QString path = folder.filePath(QStringLiteral("KoboReader.sqlite"));
    QVERIFY2(kbextractDatabase(path, &error), qPrintable(error));
    KoboHighlights::Snapshot snapshot;
    snapshot.device = QStringLiteral("N418000000000");
    snapshot.name = QStringLiteral("Kobo Libra 2");
    snapshot.source = QStringLiteral("usb");
    snapshot.seen = QDateTime(QDate(2026, 10, 24), QTime(9, 30), QTimeZone::UTC);
    snapshot.books = readDatabase(path, &error);
    snapshot.synced.insert(QStringLiteral("0123456789abcdef"), QStringLiteral("digest"));
    const std::optional<KoboHighlights::Snapshot> back = KoboHighlights::fromJson(KoboHighlights::toJson(snapshot));
    QVERIFY(back);
    QCOMPARE(back->device, snapshot.device);
    QCOMPARE(back->name, snapshot.name);
    QCOMPARE(back->seen, snapshot.seen);
    QCOMPARE(back->synced, snapshot.synced);
    QCOMPARE(back->books.size(), snapshot.books.size());
    QCOMPARE(back->digest(), snapshot.digest());
    QCOMPARE(KoboHighlights::toJson(*back), KoboHighlights::toJson(snapshot));

    // Kept per Kobo, found again.
    QVERIFY2(KoboHighlights::writeSnapshot(snapshot, &error), qPrintable(error));
    QCOMPARE(KoboHighlights::readSnapshot(snapshot.device)->digest(), snapshot.digest());
    QVERIFY(!KoboHighlights::readSnapshot(QStringLiteral("another")));
    QCOMPARE(KoboHighlights::snapshots().size(), 1);
    // What is synced is no part of the books' digest.
    KoboHighlights::Snapshot changed = snapshot;
    changed.synced.clear();
    QCOMPARE(changed.digest(), snapshot.digest());
    changed.books.first().highlights.first().note = QStringLiteral("A new note");
    QVERIFY(changed.digest() != snapshot.digest());
}

void KoboHighlightsTest::tellsWhoseDatabase()
{
    QTemporaryDir kobo;
    QString error;
    QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
    const KoboHighlights::Identity identity =
        KoboHighlights::identify(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")));
    QCOMPARE(identity.serial, QStringLiteral("N418000000000"));
    QCOMPARE(identity.name, QDir(kobo.path()).dirName());

    // A copy kept elsewhere: no serial, the folder's name.
    QTemporaryDir backups;
    const QString copy = QDir(backups.path()).filePath(QStringLiteral("Kobo backup/KoboReader.sqlite"));
    QVERIFY(writeAll(copy, {}));
    QCOMPARE(KoboHighlights::identify(copy).serial, QString());
    QCOMPARE(KoboHighlights::identify(copy).name, QStringLiteral("Kobo backup"));

    // kbextract2's TestReadSerialNumber.
    const QString conf = backups.filePath(QStringLiteral("Kobo eReader.conf"));
    QVERIFY(writeAll(conf, "# comment\n[PowerOptions]\nSerialNumber=\n[DeviceInfo]\n  SerialNumber = N123 \nSerialNumber=N456\n"));
    QCOMPARE(KoboHighlights::readSerialNumber(conf), QStringLiteral("N123"));
    QCOMPARE(KoboHighlights::readSerialNumber({}), QString());
    // Without a version file, Kobo eReader.conf's.
    QVERIFY(QFile::remove(QDir(kobo.path()).filePath(QStringLiteral(".kobo/version"))));
    QVERIFY(writeAll(QDir(kobo.path()).filePath(QStringLiteral(".kobo/Kobo/Kobo eReader.conf")), "[DeviceInfo]\nSerialNumber=N999\n"));
    QCOMPARE(KoboHighlights::identify(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite"))).serial,
             QStringLiteral("N999"));
}

QTEST_GUILESS_MAIN(KoboHighlightsTest)
#include "kobo_highlights_test.moc"
