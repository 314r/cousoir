#include <QtTest>

#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <tuple>

#include "cousoir_paths.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "highlight_json.h"
#include "kepub.h"
#include "kobo_highlights.h"
#include "kobo_placement.h"
#include "kobo_sync.h"
#include "kobo_write.h"
#include "sqlite_db.h"
#include "test_support.h"
#include "xml_tree.h"

namespace {

const QString voyage = QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub");
const QString serial = QStringLiteral("N418000000000");
const QString volume = QStringLiteral("file:///mnt/onboard/Ann Example/The Voyage.kepub.epub");
const QString lpath = QStringLiteral("Ann Example/The Voyage.kepub.epub");
const QString chapter1 = QStringLiteral("OEBPS/text/chapter1.xhtml");
const QString koboOwn = QStringLiteral("c6fb154b-bc62-497f-be73-a332d2368434");

double epochOf(const char *iso)
{
    return double(QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate).toMSecsSinceEpoch()) / 1000;
}

// A place in one of Kobo's spans of a file of The Voyage, as a CFI, and the
// text between two.
struct VoyagePage
{
    std::unique_ptr<EpubBook> book;
    std::unique_ptr<XmlTree::Node> root;
    std::unique_ptr<EpubCfi::Document> document;
    QHash<QString, Kepub::SpanStart> spans;

    explicit VoyagePage(const QString &name)
    {
        QString error;
        book = EpubBook::open(voyage, &error);
        root = XmlTree::parse(book->read(name));
        document = std::make_unique<EpubCfi::Document>(*root);
        spans = Kepub::spanStarts(*root);
    }
    EpubCfi::Point point(const QString &span, qsizetype offset) const
    {
        const Kepub::SpanStart start = spans.value(span);
        return { start.text, start.offset + offset };
    }
    QString cfi(const QString &span, qsizetype offset) const
    {
        const EpubCfi::Point at = point(span, offset);
        return document->encode(*at.node, at.offset);
    }
    QString text(const QString &startSpan, qsizetype startOffset, const QString &endSpan, qsizetype endOffset) const
    {
        return document->textBetween(point(startSpan, startOffset), point(endSpan, endOffset));
    }
};

// A Kobo with The Voyage as Cousoir sends it, a library with it as an
// EPUB, and what a sync of the two takes.
struct Fixture
{
    QTemporaryDir kobo;
    QTemporaryDir library;
    LibraryData data;
    QList<DeviceBook> books;

    QString database() const { return QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")); }
    QString snapshotDb() const { return QDir(library.path()).filePath(QStringLiteral("snapshot.db")); }

    bool make(QString *error)
    {
        if (!makeKobo(kobo.path(), error))
            return false;
        SqliteDb db;
        if (!db.open(database(), SqliteDb::Mode::ReadWrite, error)
            || !db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-sync.sql"))), error))
            return false;
        const QString kepub = QDir(kobo.path()).filePath(lpath);
        QDir().mkpath(QFileInfo(kepub).path());
        if (!Kepub::kepubify(voyage, kepub, {}, error))
            return false;
        SqliteDb snapshot;
        if (!writeAll(snapshotDb(), {}) || !snapshot.open(snapshotDb(), SqliteDb::Mode::ReadWrite, error)
            || !snapshot.exec(QStringLiteral("CREATE TABLE annotations (id INTEGER PRIMARY KEY, book INTEGER, format TEXT, "
                                             "user_type TEXT, user TEXT, timestamp REAL, annot_id TEXT, annot_type TEXT, "
                                             "annot_data TEXT, searchable_text TEXT)"),
                              error))
            return false;
        Book book;
        book.id = 1;
        book.title = QStringLiteral("The Voyage");
        book.authors = { QStringLiteral("Ann Example") };
        book.uuid = QStringLiteral("voyage-uuid");
        book.formats = { { QStringLiteral("EPUB"), QStringLiteral("The Voyage"), QFileInfo(voyage).size(), voyage } };
        data.books = { book };
        DeviceBook deviceBook;
        deviceBook.lpath = lpath;
        deviceBook.title = book.title;
        deviceBook.authors = book.authors;
        deviceBook.format = QStringLiteral("KEPUB");
        deviceBook.contentId = volume;
        deviceBook.imported = true;
        deviceBook.bookId = 1;
        books = { deviceBook };
        return true;
    }

    // A highlight of the library: the viewer's, or the Kobo's own.
    bool annotate(const QString &userType, const QString &user, const QString &id, double timestamp, const QJsonObject &json)
    {
        SqliteDb db;
        QString error;
        return db.open(snapshotDb(), SqliteDb::Mode::ReadWrite, &error)
            && db.run(QStringLiteral("DELETE FROM annotations WHERE annot_id = ? AND user_type = ?"), { id, userType }, &error)
            && db.run(QStringLiteral("INSERT INTO annotations (book, format, user_type, user, timestamp, annot_id, annot_type, "
                                     "annot_data, searchable_text) VALUES (1, 'EPUB', ?, ?, ?, ?, 'highlight', ?, '')"),
                      { userType, user, timestamp, id, QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)) },
                      &error);
    }

    KoboWrite::Input input() const
    {
        KoboWrite::Input input;
        input.root = kobo.path();
        input.device = serial;
        input.books = books;
        input.data = data;
        input.snapshotDb = snapshotDb();
        input.sent = KoboHighlights::readSent(serial);
        return input;
    }

    // The Kobo read, as a Kobo plugged in is.
    KoboHighlights::Snapshot read() const
    {
        KoboHighlights::Snapshot snapshot;
        snapshot.device = serial;
        snapshot.source = QStringLiteral("usb");
        snapshot.seen = QDateTime::currentDateTimeUtc();
        SqliteDb db;
        QString error;
        if (db.open(database(), SqliteDb::Mode::Immutable, &error))
            snapshot.books = KoboHighlights::read(db, &error);
        for (KoboHighlights::Book &book : snapshot.books) {
            book.onDevice = book.volumeId == volume;
            if (book.onDevice) {
                book.lpath = lpath;
                book.format = QStringLiteral("KEPUB");
            }
        }
        return snapshot;
    }

    KoboSync::Plan syncIn() const
    {
        KoboSync::Input input;
        input.data = data;
        input.snapshotDb = snapshotDb();
        input.snapshot = read();
        input.sent = KoboHighlights::readSent(serial);
        return KoboSync::plan(input);
    }

    QVariantList row(const QString &sql, const QVariantList &values = {}) const
    {
        SqliteDb db;
        QString error;
        if (!db.open(database(), SqliteDb::Mode::Immutable, &error))
            return {};
        const QList<QVariantList> rows = db.rows(sql, values);
        return rows.isEmpty() ? QVariantList() : rows.first();
    }
};

} // namespace

class KoboWriteTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void takesTheKobosPlace();
    void writesWhereReadingStopped();
    void writesHighlights();
    void bringsBackWhatTheKoboDid();
    void refusesAKoboItCannotWrite();
    void keepsCopies();
};

void KoboWriteTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void KoboWriteTest::init()
{
    QVERIFY(QDir(CousoirPaths::koboFolder()).removeRecursively());
}

// Read on the Kobo since the library last heard of it: where it stopped
// goes into the library, the Kobo's user's, and only then.
void KoboWriteTest::takesTheKobosPlace()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    const KoboHighlights::Snapshot snapshot = fixture.read();
    const auto book = std::find_if(snapshot.books.cbegin(), snapshot.books.cend(),
                                   [](const KoboHighlights::Book &b) { return b.volumeId == volume; });
    QVERIFY(book != snapshot.books.cend());
    QVERIFY(book->place);
    QCOMPARE(book->place->bookmark, QStringLiteral("OEBPS/text/chapter2.xhtml#kobo.3.1"));
    QCOMPARE(book->place->percent, 30);
    // A book bought from Kobo keeps its place in Kobo's cloud.
    QVERIFY(std::none_of(snapshot.books.cbegin(), snapshot.books.cend(),
                         [](const KoboHighlights::Book &b) { return b.volumeId.startsWith(QLatin1String("9c1d")); }));
    // Kept with the snapshot.
    const std::optional<KoboHighlights::Snapshot> again = KoboHighlights::fromJson(KoboHighlights::toJson(snapshot));
    QVERIFY(again);
    QCOMPARE(again->digest(), snapshot.digest());

    fixture.data.readPositions[1] = { { QStringLiteral("EPUB"), QStringLiteral("local"), QStringLiteral("cousoir"),
                                        QStringLiteral("epubcfi(/4/2)"), epochOf("2026-10-24T08:00:00Z"), 0.1 } };
    KoboSync::Plan plan = fixture.syncIn();
    QCOMPARE(plan.positions.size(), 1);
    const ReadPosition position = plan.positions.first().second;
    QCOMPARE(plan.positions.first().first, 1);
    QCOMPARE(position.format, QStringLiteral("EPUB"));
    QCOMPARE(position.user, QStringLiteral("kobo"));
    QCOMPARE(position.device, serial);
    // The third file of the spine, at the span the Kobo stopped at.
    const VoyagePage page(QStringLiteral("OEBPS/text/chapter2.xhtml"));
    QCOMPARE(position.cfi, QStringLiteral("epubcfi(/6%1)").arg(page.cfi(QStringLiteral("kobo.3.1"), 0)));
    QCOMPARE(position.epoch, epochOf("2026-10-25T21:00:00Z"));
    QCOMPARE(position.fraction, 0.3);

    // Read here since: the library's stays.
    fixture.data.readPositions[1].first().epoch = epochOf("2026-10-25T21:00:00.400Z");
    QVERIFY(fixture.syncIn().positions.isEmpty());
}

void KoboWriteTest::writesWhereReadingStopped()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    const VoyagePage page(chapter1);
    // Older than the Kobo's: nothing to write.
    const QString cfi = QStringLiteral("epubcfi(/4%1)").arg(page.cfi(QStringLiteral("kobo.5.2"), 3));
    fixture.data.readPositions[1] = { { QStringLiteral("EPUB"), QStringLiteral("local"), QStringLiteral("viewer"), cfi,
                                        epochOf("2026-10-25T20:00:00Z"), 0.2 } };
    KoboWrite::Changes changes = KoboWrite::plan(fixture.input(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(changes.places.isEmpty());

    // Newer: the span it is in, in the KEPUB on the Kobo.
    fixture.data.readPositions[1].first().epoch = epochOf("2026-10-26T10:00:00.750Z");
    changes = KoboWrite::plan(fixture.input(), &error);
    QCOMPARE(changes.places.size(), 1);
    const KoboWrite::Place place = changes.places.first();
    QCOMPARE(place.contentId, volume);
    QCOMPARE(place.bookmark, chapter1 + QStringLiteral("#kobo.5.2"));
    QCOMPARE(place.percent, 20);
    QCOMPARE(place.lastRead, QStringLiteral("2026-10-26T10:00:00Z"));
    QVERIFY(!place.begin);
    QVERIFY(place.chapterProgress > 0 && place.chapterProgress < 1);
    QVERIFY(changes.added.isEmpty() && changes.changed.isEmpty() && changes.removed.isEmpty());
    QVERIFY2(KoboWrite::apply(fixture.kobo.path(), changes, KoboWrite::backupFolder(serial), &error), qPrintable(error));
    QCOMPARE(fixture.row(QStringLiteral("SELECT ChapterIDBookmarked, ___PercentRead, DateLastRead, ReadStatus, "
                                        "FirstTimeReading FROM content WHERE ContentID = ?"),
                         { volume }),
             (QVariantList{ chapter1 + QStringLiteral("#kobo.5.2"), 20, QStringLiteral("2026-10-26T10:00:00Z"), 1,
                            QStringLiteral("false") }));
    // The others as they were.
    QCOMPARE(fixture.row(QStringLiteral("SELECT DateLastRead FROM content WHERE ContentID LIKE '9c1d%'")).value(0).toString(),
             QStringLiteral("2026-10-23T21:00:00Z"));

    // Once there, nothing more; and the Kobo's place, as old, not taken back.
    QVERIFY(KoboWrite::plan(fixture.input(), &error).isEmpty());
    QVERIFY(fixture.syncIn().positions.isEmpty());

    // The reader's own place, newer still while the book is open in it.
    const QString positions = QDir(fixture.library.path()).filePath(QStringLiteral("positions.json"));
    QVERIFY(writeAll(positions, QJsonDocument(QJsonObject{ { QStringLiteral("voyage-uuid"),
                                                             QJsonObject{ { QStringLiteral("format"), QStringLiteral("EPUB") },
                                                                          { QStringLiteral("cfi"), QStringLiteral("epubcfi(/6%1)").arg(VoyagePage(QStringLiteral("OEBPS/text/chapter2.xhtml")).cfi(QStringLiteral("kobo.4.1"), 0)) },
                                                                          { QStringLiteral("fraction"), 0.5 },
                                                                          { QStringLiteral("epoch"), epochOf("2026-10-26T11:00:00Z") } } } })
                                         .toJson()));
    KoboWrite::Input input = fixture.input();
    input.positionsFile = positions;
    changes = KoboWrite::plan(input, &error);
    QCOMPARE(changes.places.size(), 1);
    QCOMPARE(changes.places.first().bookmark, QStringLiteral("OEBPS/text/chapter2.xhtml#kobo.4.1"));
}

// The library's highlights put on the Kobo as the Kobo keeps its own: read
// back as the Kobo's, they are where they were made; the Kobo's own changed
// here are changed there, those removed here removed.
void KoboWriteTest::writesHighlights()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    const VoyagePage page(chapter1);
    const QString text = page.text(QStringLiteral("kobo.4.1"), 2, QStringLiteral("kobo.4.1"), 30);
    const QJsonObject mine = HighlightJson::highlight(QStringLiteral("mine"), QStringLiteral("2026-10-26T09:00:00.000Z"), text,
                                                      page.cfi(QStringLiteral("kobo.4.1"), 2), page.cfi(QStringLiteral("kobo.4.1"), 30),
                                                      QStringLiteral("green"), chapter1, 1, {}, QStringLiteral("A note"));
    QVERIFY(fixture.annotate(QStringLiteral("local"), QStringLiteral("viewer"), QStringLiteral("mine"), epochOf("2026-10-26T09:00:00Z"), mine));
    // The Kobo's own, its note changed here since.
    KoboSync::Plan in = fixture.syncIn();
    QCOMPARE(in.placed, 1);
    QJsonObject own = QJsonDocument::fromJson(in.changes.first().annotations.first().data).object();
    own.insert(QStringLiteral("notes"), QStringLiteral("Written here"));
    QVERIFY(fixture.annotate(QStringLiteral("kobo"), serial, koboOwn, epochOf("2026-10-26T09:30:00Z"), own));

    KoboWrite::Changes changes = KoboWrite::plan(fixture.input(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(changes.skipped.isEmpty(), qPrintable(changes.skipped.join(u'\n')));
    QCOMPARE(changes.added.size(), 1);
    QCOMPARE(changes.changed.size(), 1);
    const KoboWrite::Highlight added = changes.added.first();
    QCOMPARE(added.id, KoboHighlights::bookmarkIdFor(QStringLiteral("mine")));
    QCOMPARE(added.contentId, QStringLiteral("/mnt/onboard/Ann Example/The Voyage.kepub.epub!!OEBPS/text/chapter1.xhtml"));
    QCOMPARE(added.startPath, QStringLiteral("span#kobo\\.4\\.1"));
    QCOMPARE(added.startOffset, 2);
    QCOMPARE(added.endPath, QStringLiteral("span#kobo\\.4\\.1"));
    QCOMPARE(added.endOffset, 30);
    QCOMPARE(added.created, QStringLiteral("2026-10-26T09:00:00.000"));
    QCOMPARE(added.modified, QStringLiteral("2026-10-26T09:00:00Z"));
    QCOMPARE(changes.changed.first().id, koboOwn);
    QCOMPARE(changes.changed.first().note, QStringLiteral("Written here"));
    QVERIFY2(KoboWrite::apply(fixture.kobo.path(), changes, KoboWrite::backupFolder(serial), &error), qPrintable(error));

    // As the Kobo writes its own.
    QCOMPARE(fixture.row(QStringLiteral("SELECT StartContainerChildIndex, Annotation, Hidden, Type, UserID, Published "
                                        "FROM Bookmark WHERE BookmarkID = ?"),
                         { added.id }),
             (QVariantList{ -99, QStringLiteral("A note"), QStringLiteral("false"), QStringLiteral("note"),
                            QStringLiteral("0956f31c-0000-4000-8000-000000000001"), QStringLiteral("false") }));
    QCOMPARE(fixture.row(QStringLiteral("SELECT Annotation, Type, DateModified FROM Bookmark WHERE BookmarkID = ?"), { koboOwn }),
             (QVariantList{ QStringLiteral("Written here"), QStringLiteral("note"), QStringLiteral("2026-10-26T09:30:00Z") }));
    // Read back as the Kobo's, where it was made.
    const KoboHighlights::Snapshot after = fixture.read();
    std::optional<KoboHighlights::Highlight> found;
    for (const KoboHighlights::Book &book : after.books) {
        for (const KoboHighlights::Highlight &highlight : book.highlights) {
            if (highlight.id == added.id)
                found = highlight;
        }
    }
    QVERIFY(found);
    QCOMPARE(found->note, QStringLiteral("A note"));
    const std::unique_ptr<KoboPlacement::PlacementBook> placing = KoboPlacement::PlacementBook::open(voyage, QStringLiteral("EPUB"), &error);
    const KoboPlacement::Placement placement = placing->place(*found);
    QCOMPARE(KoboPlacement::methodName(placement.method), QStringLiteral("spans"));
    QCOMPARE(placement.startCfi, mine.value(QLatin1String("start_cfi")).toString());
    QCOMPARE(placement.endCfi, mine.value(QLatin1String("end_cfi")).toString());

    // Once there, nothing more; and none of it comes back as the Kobo's own.
    QVERIFY(KoboWrite::plan(fixture.input(), &error).isEmpty());
    KoboHighlights::writeSent(serial, { { added.id, QDateTime::currentDateTimeUtc().addSecs(-1) } }, &error);
    in = fixture.syncIn();
    for (const DeviceAnnotations &set : std::as_const(in.changes)) {
        for (const Annotation &annotation : set.annotations)
            QVERIFY2(annotation.id != added.id && annotation.id != QLatin1String("mine"), qPrintable(annotation.id));
    }

    // Removed here: removed there.
    QVERIFY(fixture.annotate(QStringLiteral("local"), QStringLiteral("viewer"), QStringLiteral("mine"), epochOf("2026-10-26T10:00:00Z"),
                             HighlightJson::removed(QStringLiteral("mine"), QStringLiteral("2026-10-26T10:00:00.000Z"))));
    changes = KoboWrite::plan(fixture.input(), &error);
    QCOMPARE(changes.removed, QStringList{ added.id });
    QVERIFY(KoboWrite::apply(fixture.kobo.path(), changes, KoboWrite::backupFolder(serial), &error));
    QVERIFY(fixture.row(QStringLiteral("SELECT 1 FROM Bookmark WHERE BookmarkID = ?"), { added.id }).isEmpty());
}

// What became on the Kobo of the library's highlights put there: a note
// changed there comes back, one removed there is removed here; never one
// gone before the Kobo was read again, nor all of a book's, which the Kobo
// lost taking the book in again: those go back.
void KoboWriteTest::bringsBackWhatTheKoboDid()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    const VoyagePage page(chapter1);
    for (const auto &[id, start, end] : { std::tuple(QStringLiteral("mine"), 2, 30), std::tuple(QStringLiteral("other"), 40, 60) }) {
        QVERIFY(fixture.annotate(QStringLiteral("local"), QStringLiteral("viewer"), id, epochOf("2026-10-26T09:00:00Z"),
                                 HighlightJson::highlight(id, QStringLiteral("2026-10-26T09:00:00.000Z"),
                                                          page.text(QStringLiteral("kobo.4.1"), start, QStringLiteral("kobo.4.1"), end),
                                                          page.cfi(QStringLiteral("kobo.4.1"), start), page.cfi(QStringLiteral("kobo.4.1"), end),
                                                          QStringLiteral("purple"), chapter1, 1, {})));
    }
    const KoboWrite::Result result = KoboWrite::sync(fixture.input());
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.highlights, 2);
    const QString id = KoboHighlights::bookmarkIdFor(QStringLiteral("mine"));
    const QString otherId = KoboHighlights::bookmarkIdFor(QStringLiteral("other"));
    QVERIFY(KoboHighlights::readSent(serial).contains(id));
    // As it went: nothing to bring back, its purple aside, which the Kobo
    // has not.
    for (const DeviceAnnotations &set : fixture.syncIn().changes)
        QVERIFY(set.userType != QLatin1String("local"));

    // A note written on the Kobo.
    const auto onKobo = [&](const QString &sql, const QString &bookmark) {
        SqliteDb db;
        QString error;
        return db.open(fixture.database(), SqliteDb::Mode::ReadWrite, &error) && db.run(sql, { bookmark }, &error);
    };
    QVERIFY(onKobo(QStringLiteral("UPDATE Bookmark SET Annotation = 'From the Kobo', Type = 'note', "
                                  "DateModified = '2026-10-27T08:00:00Z' WHERE BookmarkID = ?"),
                   id));
    const auto viewerChanges = [](const KoboSync::Plan &plan) {
        QList<Annotation> found;
        for (const DeviceAnnotations &set : plan.changes) {
            if (set.userType == QLatin1String("local") && set.user == QLatin1String("viewer"))
                found += set.annotations;
        }
        return found;
    };
    QList<Annotation> changes = viewerChanges(fixture.syncIn());
    QCOMPARE(changes.size(), 1);
    QJsonObject json = QJsonDocument::fromJson(changes.first().data).object();
    QCOMPARE(changes.first().id, QStringLiteral("mine"));
    QCOMPARE(json.value(QLatin1String("notes")).toString(), QStringLiteral("From the Kobo"));
    QCOMPARE(json.value(QLatin1String("style")).toObject().value(QLatin1String("which")).toString(), QStringLiteral("purple"));
    QCOMPARE(changes.first().timestamp, epochOf("2026-10-27T08:00:00Z"));

    // One removed on the Kobo, which deletes it: removed here, not put
    // there again.
    QVERIFY(onKobo(QStringLiteral("DELETE FROM Bookmark WHERE BookmarkID = ?"), id));
    changes = viewerChanges(fixture.syncIn());
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.first().id, QStringLiteral("mine"));
    QVERIFY(QJsonDocument::fromJson(changes.first().data).object().value(QLatin1String("removed")).toBool());
    QVERIFY(KoboWrite::plan(fixture.input(), &error).added.isEmpty());
    // Not when put there after the Kobo was read: it was not there yet.
    const QHash<QString, QDateTime> sent = KoboHighlights::readSent(serial);
    KoboHighlights::writeSent(serial, { { id, QDateTime::currentDateTimeUtc().addSecs(60) }, { otherId, sent.value(otherId) } }, &error);
    QVERIFY(viewerChanges(fixture.syncIn()).isEmpty());
    KoboHighlights::writeSent(serial, sent, &error);

    // All gone: the book taken in again. Nothing removed here, all put back.
    QVERIFY(onKobo(QStringLiteral("DELETE FROM Bookmark WHERE BookmarkID = ?"), otherId));
    QVERIFY(viewerChanges(fixture.syncIn()).isEmpty());
    QCOMPARE(KoboWrite::plan(fixture.input(), &error).added.size(), 2);
}

void KoboWriteTest::refusesAKoboItCannotWrite()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    fixture.data.readPositions[1] = { { QStringLiteral("EPUB"), QStringLiteral("local"), QStringLiteral("viewer"),
                                        QStringLiteral("epubcfi(/4%1)").arg(VoyagePage(chapter1).cfi(QStringLiteral("kobo.5.2"), 0)),
                                        epochOf("2026-10-26T10:00:00Z"), 0.2 } };
    KoboWrite::Changes changes = KoboWrite::plan(fixture.input(), &error);
    QCOMPARE(changes.places.size(), 1);
    // Changed since it was read: not written.
    {
        SqliteDb db;
        QVERIFY(db.open(fixture.database(), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(db.exec(QStringLiteral("UPDATE content SET ___PercentRead = 31 WHERE ContentID LIKE 'file:///mnt/onboard/Ann%'"), &error));
    }
    {
        QFile file(fixture.database());
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
    }
    QVERIFY(!KoboWrite::apply(fixture.kobo.path(), changes, KoboWrite::backupFolder(serial), &error));
    QCOMPARE(error, QStringLiteral("The Kobo changed since it was read: sync again."));
    QCOMPARE(fixture.row(QStringLiteral("SELECT ChapterIDBookmarked FROM content WHERE ContentID = ?"), { volume }).value(0).toString(),
             QStringLiteral("OEBPS/text/chapter2.xhtml#kobo.3.1"));

    // A database newer than Calibre writes to.
    {
        SqliteDb db;
        QVERIFY(db.open(fixture.database(), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(db.exec(QStringLiteral("UPDATE DbVersion SET version = 999"), &error));
    }
    error.clear();
    QVERIFY(KoboWrite::plan(fixture.input(), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("Cousoir writes to no Kobo with a database newer than Calibre writes to."));
    // Or keeping its highlights otherwise.
    {
        SqliteDb db;
        QVERIFY(db.open(fixture.database(), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(db.exec(QStringLiteral("UPDATE DbVersion SET version = 174; ALTER TABLE Bookmark DROP COLUMN Type"), &error));
    }
    error.clear();
    KoboWrite::plan(fixture.input(), &error);
    QCOMPARE(error, QStringLiteral("This Kobo keeps its highlights otherwise: Cousoir cannot write to it."));
    // No Kobo there any more.
    QVERIFY(QFile::remove(QDir(fixture.kobo.path()).filePath(QStringLiteral(".kobo/version"))));
    QVERIFY(QDir(QDir(fixture.kobo.path()).filePath(QStringLiteral(".kobo"))).removeRecursively());
    error.clear();
    KoboWrite::plan(fixture.input(), &error);
    QCOMPARE(error, QStringLiteral("The Kobo is no longer there."));
}

// A copy of the Kobo's database before each write, the last five kept.
void KoboWriteTest::keepsCopies()
{
    Fixture fixture;
    QString error;
    QVERIFY2(fixture.make(&error), qPrintable(error));
    const QString original = QString::fromLatin1(readAll(fixture.database()).toHex());
    const QString cfi = QStringLiteral("epubcfi(/4%1)").arg(VoyagePage(chapter1).cfi(QStringLiteral("kobo.5.2"), 0));
    for (int i = 0; i < 7; ++i) {
        fixture.data.readPositions[1] = { { QStringLiteral("EPUB"), QStringLiteral("local"), QStringLiteral("viewer"), cfi,
                                            epochOf("2026-10-26T10:00:00Z") + 10 * i, 0.2 } };
        const KoboWrite::Changes changes = KoboWrite::plan(fixture.input(), &error);
        QCOMPARE(changes.places.size(), 1);
        QVERIFY2(KoboWrite::apply(fixture.kobo.path(), changes, KoboWrite::backupFolder(serial), &error), qPrintable(error));
        if (i == 0) {
            const QStringList copies = QDir(KoboWrite::backupFolder(serial)).entryList({ QStringLiteral("*.sqlite") });
            QCOMPARE(copies.size(), 1);
            QCOMPARE(QString::fromLatin1(readAll(QDir(KoboWrite::backupFolder(serial)).filePath(copies.first())).toHex()), original);
        }
        // A copy a second.
        QTest::qWait(1001);
    }
    QCOMPARE(QDir(KoboWrite::backupFolder(serial)).entryList({ QStringLiteral("*.sqlite") }).size(), 5);
}

QTEST_GUILESS_MAIN(KoboWriteTest)
#include "kobo_write_test.moc"
