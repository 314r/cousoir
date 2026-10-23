#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "calibre_job.h"
#include "kobo.h"
#include "kobo_send.h"
#include "reading_log.h"
#include "sqlite_db.h"
#include "test_support.h"

namespace {

// A row of the Kobo's database, as the Kobo writes it for a book it took in.
bool addRow(const QString &root, const QString &contentId, const QString &mimeType, const QString &title,
            const QString &attribution, int accessibility = -1, const QString &downloaded = QStringLiteral("true"),
            const QString &bookId = {})
{
    SqliteDb db;
    QString error;
    if (!db.open(QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite")), SqliteDb::Mode::ReadWrite, &error))
        return false;
    SqliteStatement insert(db, QStringLiteral("INSERT INTO content (ContentID, ContentType, MimeType, BookID, Title, "
                                              "Attribution, ___UserID, Accessibility, IsDownloaded, ImageId) "
                                              "VALUES (?, '6', ?, ?, ?, ?, '', ?, ?, ?)"));
    insert.bind(1, contentId);
    insert.bind(2, mimeType);
    if (bookId.isEmpty())
        insert.bindNull(3);
    else
        insert.bind(3, bookId);
    insert.bind(4, title);
    if (attribution.isEmpty())
        insert.bindNull(5);
    else
        insert.bind(5, attribution);
    insert.bind(6, qint64(accessibility));
    insert.bind(7, downloaded);
    insert.bind(8, QString(contentId).replace(u'/', u'_').replace(u' ', u'_').replace(u':', u'_').replace(u'.', u'_'));
    insert.step();
    return insert.error().isEmpty();
}

bool writeMetadata(const QString &root, const QJsonArray &books)
{
    return writeAll(QDir(root).filePath(QStringLiteral("metadata.calibre")), QJsonDocument(books).toJson());
}

QJsonObject entry(const QString &lpath, const QString &title, const QStringList &authors, const QString &uuid,
                  int applicationId)
{
    return {
        { QStringLiteral("lpath"), lpath },
        { QStringLiteral("title"), title },
        { QStringLiteral("authors"), QJsonArray::fromStringList(authors) },
        { QStringLiteral("uuid"), uuid },
        { QStringLiteral("application_id"), applicationId },
        { QStringLiteral("size"), 4 },
        { QStringLiteral("series_index"), 1.0 },
        { QStringLiteral("thumbnail"), QJsonValue() },
    };
}

QStringList lpaths(const QList<DeviceBook> &books)
{
    QStringList list;
    for (const DeviceBook &book : books)
        list.append(book.lpath);
    list.sort();
    return list;
}

const DeviceBook *withPath(const QList<DeviceBook> &books, const QString &lpath)
{
    for (const DeviceBook &book : books) {
        if (book.lpath == lpath)
            return &book;
    }
    return nullptr;
}

const QString vian = QStringLiteral("Vian, Boris/Ecume des jours, L_ - Boris Vian.kepub.epub");

} // namespace

class KoboTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_snapshotFolder;
    LibraryData m_data;

private slots:
    void initTestCase()
    {
        CalibreConfig config;
        config.interfaceLanguage = QStringLiteral("fr");
        QString error;
        QVERIFY2(CalibreDb::loadLibrary(fixtureLibrary(), m_snapshotFolder.filePath(QStringLiteral("snapshot.db")), config,
                                        &m_data, &error),
                 qPrintable(error));
    }

    // The Kobo's model from its USB product id, which sysfs gives above its
    // block device; the name Calibre gave it, once there is one.
    void describesKobos()
    {
        QTemporaryDir root;
        QString error;
        QVERIFY(!Kobo::isKobo(root.path()));
        QVERIFY2(makeKobo(root.path(), &error), qPrintable(error));
        QVERIFY(Kobo::isKobo(root.path()));

        QTemporaryDir sys;
        const QString usb = sys.filePath(QStringLiteral("devices/pci0000:00/usb1/1-2"));
        const QString block = usb + QStringLiteral("/1-2:1.0/host6/target6:0:0/6:0:0:0/block/sdz");
        QVERIFY(QDir().mkpath(block));
        QVERIFY(writeAll(usb + QStringLiteral("/idProduct"), "4234\n"));
        QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("class/block"))));
        QVERIFY(QFile::link(block, sys.filePath(QStringLiteral("class/block/sdz"))));
        QCOMPARE(Kobo::usbProductId(QStringLiteral("/dev/sdz"), sys.path()), 0x4234);
        QCOMPARE(Kobo::usbProductId(QStringLiteral("/dev/sdy"), sys.path()), 0);

        Kobo::Device device = Kobo::describe(root.path(), QStringLiteral("/dev/sdz"), sys.path());
        QCOMPARE(device.root, QDir::cleanPath(root.path()));
        QCOMPARE(device.name, QStringLiteral("Kobo Libra 2"));
        QCOMPARE(device.serial, QStringLiteral("N418000000000"));
        QCOMPARE(device.firmware, QStringLiteral("4.38.21908"));
        QCOMPARE(device.modelId, QStringLiteral("00000000-0000-0000-0000-000000000390"));
        QCOMPARE(Kobo::firmwareVersion(device), (QList<int>{ 4, 38, 21908 }));

        QVERIFY(writeAll(QDir(root.path()).filePath(QStringLiteral("driveinfo.calibre")),
                         "{\"device_name\": \"Liseuse\", \"location_code\": \"main\"}"));
        QCOMPARE(Kobo::describe(root.path(), QStringLiteral("/dev/sdz"), sys.path()).name, QStringLiteral("Liseuse"));

        // The Libra Colour's product id, shared with other models.
        QCOMPARE(Kobo::modelName(0x4237, QStringLiteral("00000000-0000-0000-0000-000000000391")), QStringLiteral("Kobo Clara BW"));
        QCOMPARE(Kobo::modelName(0x4237, QStringLiteral("00000000-0000-0000-0000-000000000390")),
                 QStringLiteral("Kobo Libra Colour"));
        QCOMPARE(Kobo::modelName(0, {}), QStringLiteral("Kobo eReader"));
    }

    // KoboTouch's books(): the database's, with metadata.calibre's for those
    // Calibre sent, then those sent since.
    // What the Kobo knows of each book's reading: begun, finished or not,
    // how far, when, and the highlights, notes and bookmarks it shows, in
    // the order and with the chapters Calibre gives them.
    void readsReading()
    {
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        {
            SqliteDb db;
            QVERIFY2(db.open(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")),
                             SqliteDb::Mode::ReadWrite, &error),
                     qPrintable(error));
            QVERIFY2(db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-reading.sql"))),
                             &error),
                     qPrintable(error));
        }
        const QList<DeviceBook> books = Kobo::books(Kobo::describe(kobo.path(), {}), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto find = [&books](const QString &contentId) -> const ReadingState * {
            for (const DeviceBook &book : books) {
                if (book.contentId == contentId)
                    return &book.reading;
            }
            return nullptr;
        };

        const ReadingState *ecume = find(QStringLiteral("file:///mnt/onboard/Vian, Boris/Ecume.kepub.epub"));
        QVERIFY(ecume);
        QCOMPARE(ecume->status, 1);
        QCOMPARE(ecume->percent, std::optional<int>(43));
        QCOMPARE(ecume->lastRead, QStringLiteral("2026-10-20T21:14:03Z"));
        QCOMPARE(ecume->secondsSpent, 5400);
        QStringList kinds;
        for (const ReaderNote &note : ecume->notes)
            kinds.append(QStringLiteral("%1 %2 %3").arg(note.location).arg(note.type, note.chapterTitle));
        // The hidden highlight is left out; the note whose id has no "!" has
        // no chapter.
        QCOMPARE(kinds, QStringList({ QStringLiteral("0 Highlight "), QStringLiteral("1 Highlight Un"),
                                      QStringLiteral("2 Highlight Un"), QStringLiteral("3 Annotation Un"),
                                      QStringLiteral("1000 Bookmark Deux"),
                                      QStringLiteral("2000 Unknown annotation type Trois"),
                                      QStringLiteral("2001 Highlight Trois") }));
        QCOMPARE(ecume->notes.at(1).chapterProgress, 12.35);
        QCOMPARE(ecume->notes.at(3).annotation, QStringLiteral("Le nénuphar, déjà ?"));
        QCOMPARE(ecume->notes.at(3).created, QStringLiteral("2026-10-18T20:15:00.000"));

        // Finished: 100 whatever its percent; a chapter sharing another's
        // index takes the place of one of its notes.
        const ReadingState *mort = find(QStringLiteral("file:///mnt/onboard/Pratchett, Terry/Mort.epub"));
        QVERIFY(mort);
        QCOMPARE(mort->status, 2);
        QCOMPARE(mort->percent, std::optional<int>(100));
        QCOMPARE(mort->notes.size(), 4);
        QCOMPARE(mort->notes.at(1).text, QStringLiteral("Binky"));
        // As Markdown: a heading per chapter that has a title.
        QCOMPARE(DeviceBooks::notesMarkdown(QStringLiteral("Mort"), { QStringLiteral("Terry Pratchett") }, mort->notes),
                 QStringLiteral("# Mort\n\nTerry Pratchett\n\n> Ysabell\n\n## Chapter 3\n\n> Binky\n\n"
                                "## Chapter 1\n\n> THERE IS NO JUSTICE.\n\n## Chapter 2\n\n> Mort\n"));

        const ReadingState *bought = find(QStringLiteral("5b0e1c2d-3e4f-4a5b-8c6d-7e8f9a0b1c2d"));
        QVERIFY(bought);
        QCOMPARE(bought->status, 0);
        QCOMPARE(bought->percent, std::optional<int>(0));
        QVERIFY(bought->lastRead.isEmpty());
        QCOMPARE(bought->notes.size(), 2);
        QCOMPARE(bought->notes.first().chapterTitle, QStringLiteral("Opening"));
        // round(3.125, 2) is 3.12, a tie going to the even digit.
        QCOMPARE(bought->notes.first().chapterProgress, 3.12);

        const ReadingState *notes = find(QStringLiteral("file:///mnt/onboard/Notes.pdf"));
        QVERIFY(notes);
        QCOMPARE(notes->status, 1);
        QVERIFY(!notes->percent);
    }

    // What readers told, kept per library book: written and read back, a
    // reader's entries replaced as a whole when it tells again.
    void keepsReadingLog()
    {
        QTemporaryDir folder;
        const QString file = folder.filePath(QStringLiteral("reading/log.json"));
        ReadingLog::Entry entry{ QStringLiteral("N418"), QStringLiteral("Kobo Clara"), QStringLiteral("a.kepub.epub"),
                                 QStringLiteral("KEPUB"), QDateTime(QDate(2026, 10, 20), QTime(8, 0), QTimeZone::UTC),
                                 {} };
        entry.reading.status = 1;
        entry.reading.percent = 43;
        entry.reading.notes.append(ReaderNote{ QStringLiteral("Annotation"), QStringLiteral("Chloé"),
                                               QStringLiteral("Le nénuphar"), 1, QStringLiteral("Un"), 60, 1001,
                                               QStringLiteral("2026-10-18T20:15:00.000") });
        ReadingLog::Entry other = entry;
        other.device = QStringLiteral("N999");
        ReadingLog::Log log;
        ReadingLog::replaceDevice(&log, entry.device, { { QStringLiteral("uuid-1"), { entry } } });
        ReadingLog::replaceDevice(&log, other.device, { { QStringLiteral("uuid-1"), { other } } });
        QVERIFY(ReadingLog::write(file, log));
        log = ReadingLog::read(file);
        QCOMPARE(log.value(QStringLiteral("uuid-1")).size(), 2);
        const ReadingLog::Entry read = log.value(QStringLiteral("uuid-1")).first();
        QCOMPARE(read.deviceName, QStringLiteral("Kobo Clara"));
        QCOMPARE(read.seen, entry.seen);
        QCOMPARE(read.reading.percent, std::optional<int>(43));
        QCOMPARE(read.reading.notes.first().annotation, QStringLiteral("Le nénuphar"));
        QCOMPARE(read.reading.notes.first().location, 1001);
        // The first reader no longer has the book.
        ReadingLog::replaceDevice(&log, entry.device, {});
        QCOMPARE(log.value(QStringLiteral("uuid-1")).size(), 1);
        QCOMPARE(log.value(QStringLiteral("uuid-1")).first().device, QStringLiteral("N999"));
        ReadingLog::replaceDevice(&log, other.device, {});
        QVERIFY(log.isEmpty());
    }

    void listsBooks()
    {
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        const QDir root(kobo.path());
        const Book &ecume = *m_data.book(1);
        QVERIFY(writeAll(root.filePath(vian), "kepub"));
        QVERIFY(addRow(kobo.path(), QStringLiteral("file:///mnt/onboard/") + vian, QStringLiteral("application/x-kobo-epub+zip"),
                       QStringLiteral("L'Ecume"), QStringLiteral("B. Vian")));
        QVERIFY(writeAll(root.filePath(QStringLiteral("Other/Some Book.epub")), "epub"));
        QVERIFY(addRow(kobo.path(), QStringLiteral("file:///mnt/onboard/Other/Some Book.epub"), QStringLiteral("application/epub+zip"),
                       QStringLiteral("Some Book"), QStringLiteral("Ann Author & Bob Author")));
        QVERIFY(writeAll(root.filePath(QStringLiteral(".kobo/kepub/abc-123")), "bought"));
        QVERIFY(addRow(kobo.path(), QStringLiteral("abc-123"), QStringLiteral("application/x-kobo-epub+zip"),
                       QStringLiteral("Bought"), QStringLiteral("Store Author"), 1));
        // Not books one sees: the manual, an archived purchase, a book
        // deleted on the Kobo, a chapter.
        QVERIFY(addRow(kobo.path(), QStringLiteral("file:///usr/local/kobo/help/userguide.pdf"), QStringLiteral("application/pdf"),
                       QStringLiteral("Guide"), {}));
        QVERIFY(addRow(kobo.path(), QStringLiteral("def-456"), QStringLiteral("application/x-kobo-epub+zip"),
                       QStringLiteral("Archived"), {}, 1, QStringLiteral("false")));
        QVERIFY(addRow(kobo.path(), QStringLiteral("file:///mnt/onboard/Gone.epub"), QStringLiteral("application/epub+zip"),
                       QStringLiteral("Gone"), {}, -1, QStringLiteral("false")));
        QVERIFY(addRow(kobo.path(), QStringLiteral("abc-123!chapter1"), QStringLiteral("application/xhtml+xml"),
                       QStringLiteral("Chapter"), {}, -1, QStringLiteral("true"), QStringLiteral("abc-123")));
        // Sent and not yet taken in; sent and since deleted.
        QVERIFY(writeAll(root.filePath(QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub")), "kepub"));
        const QJsonArray sent{
            entry(vian, ecume.title, ecume.authors, ecume.uuid, ecume.id),
            entry(QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub"), QStringLiteral("Shadows Linger"),
                  { QStringLiteral("Glen Cook") }, QStringLiteral("not-a-library-uuid"), 0),
            entry(QStringLiteral("Cook, Glen/Missing - Glen Cook.kepub.epub"), QStringLiteral("Missing"),
                  { QStringLiteral("Glen Cook") }, {}, 0),
        };
        QVERIFY(writeMetadata(kobo.path(), sent));

        const Kobo::Device device = Kobo::describe(kobo.path(), {});
        QList<DeviceBook> books = Kobo::books(device, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(lpaths(books), QStringList({ QStringLiteral(".kobo/kepub/abc-123"),
                                              QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub"),
                                              QStringLiteral("Other/Some Book.epub"), vian }));
        const DeviceBook *taken = withPath(books, vian);
        QCOMPARE(taken->title, ecume.title);
        QCOMPARE(taken->uuid, ecume.uuid);
        QVERIFY(taken->sent && taken->imported && !taken->purchased);
        QCOMPARE(taken->format, QStringLiteral("KEPUB"));
        QCOMPARE(taken->size, 5);
        const DeviceBook *copied = withPath(books, QStringLiteral("Other/Some Book.epub"));
        QCOMPARE(copied->authors, QStringList({ QStringLiteral("Ann Author"), QStringLiteral("Bob Author") }));
        QVERIFY(!copied->sent && copied->imported);
        QCOMPARE(copied->format, QStringLiteral("EPUB"));
        const DeviceBook *bought = withPath(books, QStringLiteral(".kobo/kepub/abc-123"));
        QVERIFY(bought->purchased);
        QCOMPARE(bought->format, QStringLiteral("KEPUB"));
        const DeviceBook *waiting = withPath(books, QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub"));
        QVERIFY(waiting->sent && !waiting->imported);

        // Without its database, the books Calibre sent.
        QVERIFY(QFile::remove(root.filePath(QStringLiteral(".kobo/KoboReader.sqlite"))));
        books = Kobo::books(device, &error);
        QCOMPARE(lpaths(books), QStringList({ QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub"), vian }));
    }

    // set_books_in_library(): the uuid first, then the title with the
    // authors, the author sort or one of the authors.
    void matchesBooks()
    {
        const Book &ecume = *m_data.book(1);
        const Book &omens = *m_data.book(4);
        QList<DeviceBook> books(7);
        books[0].title = QStringLiteral("Anything");
        books[0].uuid = ecume.uuid;
        books[1].title = QStringLiteral("l'écume DES jours !");
        books[1].authors = { QStringLiteral("boris  vian") };
        books[2].title = QStringLiteral("Shadows Linger");
        books[2].authors = { QStringLiteral("Cook, Glen") };
        books[3].title = omens.title;
        books[3].authors = { omens.authorSort };
        books[4].title = QStringLiteral("The Black Company");
        books[4].authors = { QStringLiteral("Somebody Else") };
        books[4].applicationId = 2;
        books[5].title = QStringLiteral("Not in the library");
        books[5].authors = { QStringLiteral("Boris Vian") };
        // One author matches a book with that author alone.
        books[6].title = QStringLiteral("Shadows Linger");
        books[6].authors = { QStringLiteral("Someone"), QStringLiteral("Glen Cook") };
        DeviceBooks::match(&books, m_data);
        QCOMPARE(books[0].bookId, 1);
        QCOMPARE(books[1].bookId, 1);
        QCOMPARE(books[2].bookId, 3);
        QCOMPARE(books[3].bookId, 4);
        QCOMPARE(books[4].bookId, 2);
        QCOMPARE(books[5].bookId, 0);
        QCOMPARE(books[6].bookId, 3);
        books[6].authors = { QStringLiteral("Someone"), QStringLiteral("Neil Gaiman") };
        books[6].title = omens.title;
        DeviceBooks::match(&books, m_data);
        QCOMPARE(books[6].bookId, 0);
    }

    // The first of KoboTouch's formats the book has, unless Calibre's
    // device settings order them otherwise.
    void choosesFormats()
    {
        QTemporaryDir config;
        const QStringList defaults = Kobo::formatMap(config.path());
        QCOMPARE(defaults, QStringList({ QStringLiteral("kepub"), QStringLiteral("epub"), QStringLiteral("pdf"),
                                         QStringLiteral("txt"), QStringLiteral("cbz"), QStringLiteral("cbr") }));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("AZW3"), QStringLiteral("EPUB") }, defaults), QStringLiteral("EPUB"));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }, defaults), QStringLiteral("KEPUB"));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("PDF") }, defaults), QStringLiteral("PDF"));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("AZW3") }, defaults), QString());
        QVERIFY(writeAll(QDir(config.path()).filePath(QStringLiteral("device_drivers_KOBOTOUCH.py.json")),
                         "{\"format_map\": [\"pdf\", \"epub\"], \"use_subdirs\": true}"));
        const QStringList changed = Kobo::formatMap(config.path());
        QCOMPARE(changed, QStringList({ QStringLiteral("pdf"), QStringLiteral("epub") }));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("EPUB"), QStringLiteral("PDF") }, changed), QStringLiteral("PDF"));
    }

    // images_path() and cover_file_endings() of KoboTouch, as Calibre
    // computes them for a Libra 2.
    void findsCoverCache()
    {
        Kobo::Device device;
        device.root = QStringLiteral("/K");
        device.firmware = QStringLiteral("4.38.21908");
        device.productId = 0x4234;
        const QString base = QStringLiteral("/K/.kobo-images/210/171/"
                                            "file____mnt_onboard_Vian,_Boris_Ecume_des_jours,_L__-_Boris_Vian_kepub_epub");
        QCOMPARE(Kobo::coverCache(device, vian),
                 QStringList({ base + QStringLiteral(" - N3_LIBRARY_FULL.parsed"), base + QStringLiteral(" - N3_LIBRARY_GRID.parsed"),
                               base + QStringLiteral(" - N3_LIBRARY_LIST.parsed"),
                               base + QStringLiteral(" - AndroidBookLoadTablet_Aspect.parsed"),
                               base + QStringLiteral(" - N3_FULL.parsed") }));
        device.firmware = QStringLiteral("2.6.1");
        QCOMPARE(Kobo::coverCache(device, vian, QStringLiteral("image")).first(),
                 QStringLiteral("/K/.kobo/images/image - N3_LIBRARY_FULL.parsed"));
    }

    // KOBO.delete_books() but for the Kobo's database, then
    // remove_books_from_metadata().
    void removesBooks()
    {
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        const QDir root(kobo.path());
        const Book &ecume = *m_data.book(1);
        QVERIFY(writeAll(root.filePath(vian), "kepub"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("Cook, Glen/Shadows Linger - Glen Cook.kepub.epub")), "kepub"));
        QVERIFY(writeAll(root.filePath(QStringLiteral(".kobo/kepub/abc-123")), "bought"));
        QVERIFY(addRow(kobo.path(), QStringLiteral("abc-123"), QStringLiteral("application/x-kobo-epub+zip"),
                       QStringLiteral("Bought"), {}, 1));
        const Kobo::Device device = Kobo::describe(kobo.path(), {});
        for (const QString &file : Kobo::coverCache(device, vian))
            QVERIFY(writeAll(file, "cover"));
        // As JsonCodec writes it, through Python's json.dumps(indent=2).
        const QByteArray shadows = "{\n"
                                   "    \"lpath\": \"Cook, Glen/Shadows Linger - Glen Cook.kepub.epub\",\n"
                                   "    \"title\": \"Shadows Linger\",\n"
                                   "    \"authors\": [\n"
                                   "      \"Glen Cook\"\n"
                                   "    ],\n"
                                   "    \"series_index\": 1.0,\n"
                                   "    \"rating\": null,\n"
                                   "    \"size\": 4\n"
                                   "  }";
        const QByteArray ecumeEntry = "{\n"
                                      "    \"lpath\": \"" + vian.toUtf8() + "\",\n"
                                      "    \"title\": \"L'\\u00c9cume des jours, \\\"[x]\\\" }\",\n"
                                      "    \"uuid\": \"" + ecume.uuid.toUtf8() + "\",\n"
                                      "    \"series_index\": 1.0\n"
                                      "  }";
        QVERIFY(writeAll(root.filePath(QStringLiteral("metadata.calibre")), "[\n  " + ecumeEntry + ",\n  " + shadows + "\n]"));

        QList<DeviceBook> books = Kobo::books(device, &error);
        books.removeIf([](const DeviceBook &book) { return book.lpath != vian && !book.purchased; });
        QCOMPARE(books.size(), 2);
        QVERIFY2(Kobo::removeBooks(device, books, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root.filePath(vian)));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("Vian, Boris"))));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral(".kobo-images/210"))));
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral(".kobo/kepub/abc-123"))));
        // The other entry as it was written, whole.
        QCOMPARE(readAll(root.filePath(QStringLiteral("metadata.calibre"))), "[\n  " + shadows + "\n]");
        QCOMPARE(filesUnder(kobo.path()).filter(QStringLiteral(".~")), QStringList());
        books = Kobo::books(device, &error);
        books.removeIf([](const DeviceBook &book) { return book.purchased; });
        QVERIFY2(Kobo::removeBooks(device, books, &error), qPrintable(error));
        QCOMPARE(readAll(root.filePath(QStringLiteral("metadata.calibre"))), QByteArray("[]"));
    }

    void writesSendJobs()
    {
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        QVERIFY(writeMetadata(kobo.path(), QJsonArray{}));
        Kobo::Device device = Kobo::describe(kobo.path(), {});
        device.productId = 0x4234;
        QTemporaryDir folder;
        const QStringList formatMap = Kobo::formatMap(folder.path());
        QVERIFY2(KoboSend::prepare(m_data, { 2, 5 }, device, formatMap, folder.path(), &error), qPrintable(error));
        const QJsonObject job = QJsonDocument::fromJson(readAll(folder.filePath(QStringLiteral("job.json")))).object();
        QCOMPARE(job.value(QLatin1String("device")).toString(), device.root);
        QCOMPARE(job.value(QLatin1String("product")).toInt(), 0x4234);
        QCOMPARE(job.value(QLatin1String("books")).toArray(), (QJsonArray{ 2, 5 }));
        const QJsonObject identities = job.value(QLatin1String("identities")).toObject();
        QCOMPARE(identities.value(QLatin1String("metadata.calibre")).toArray().at(0).toInteger(),
                 QFileInfo(QDir(kobo.path()).filePath(QStringLiteral("metadata.calibre"))).size());
        QVERIFY(identities.value(QLatin1String("driveinfo.calibre")).toArray().isEmpty());
        // Of the library, the database and what Calibre reads of each book:
        // the file it sends and the cover.
        const QDir library(folder.filePath(QStringLiteral("library")));
        QVERIFY(QFileInfo(library.filePath(QStringLiteral("metadata.db"))).size() > 0);
        const Book &company = *m_data.book(2);
        QCOMPARE(filesUnder(library.filePath(company.path)),
                 QStringList({ QStringLiteral("The Black Company - Glen Cook.epub"), QStringLiteral("cover.jpg") }));
        QCOMPARE(readAll(library.filePath(company.path + QStringLiteral("/cover.jpg"))), readAll(company.coverFile));
        QVERIFY(QFileInfo::exists(library.filePath(m_data.book(5)->path + QStringLiteral("/Manuel technique - Jean-Pierre Dupont.pdf"))));
        // Of the Kobo, copies of what Calibre reads there.
        QCOMPARE(filesUnder(folder.filePath(QStringLiteral("device"))),
                 QStringList({ QStringLiteral(".kobo/KoboReader.sqlite"), QStringLiteral(".kobo/version"),
                               QStringLiteral("metadata.calibre") }));
        QVERIFY(readAll(folder.filePath(QStringLiteral("send.py"))).contains("sync_to_device"));
        QCOMPARE(SendJob::arguments(folder.path()),
                 QStringList({ QStringLiteral("-e"), folder.filePath(QStringLiteral("send.py")), QStringLiteral("--"),
                               folder.path() }));

        // A book without a file the Kobo reads, or whose file is missing.
        QTemporaryDir other;
        QVERIFY(!KoboSend::prepare(m_data, { 6 }, device, formatMap, other.path(), &error));
        QVERIFY2(error.contains(QStringLiteral("none of the formats")), qPrintable(error));
        LibraryData missing = m_data;
        for (Book &book : missing.books) {
            if (book.id == 5)
                book.formats.first().file.clear();
        }
        QTemporaryDir third;
        QVERIFY(!KoboSend::prepare(missing, { 5 }, device, formatMap, third.path(), &error));
        QVERIFY2(error.contains(QStringLiteral("PDF file")), qPrintable(error));
    }

    void readsSendOutcomes()
    {
        QCOMPARE(CalibreJob::progress("{\"progress\": 0.25}\n"), 0.25);
        QCOMPARE(CalibreJob::progress("DEBUG: something\n"), -1.0);
        QTemporaryDir folder;
        const QString result = folder.filePath(QStringLiteral("result.json"));
        QVERIFY(writeAll(result, "{\"sent\": [{\"book\": 1, \"lpath\": \"A/B.kepub.epub\"}], \"unsendable\": [\"T\"],"
                                 " \"files\": [\"A/B.kepub.epub\", \"metadata.calibre\"], \"clear_covers\": false}"));
        KoboSend::Outcome outcome = KoboSend::outcome(folder.path(), 0);
        QVERIFY(outcome.ok);
        QCOMPARE(outcome.sent.size(), 1);
        QCOMPARE(outcome.sent.first().bookId, 1);
        QCOMPARE(outcome.sent.first().lpath, QStringLiteral("A/B.kepub.epub"));
        QCOMPARE(outcome.unsendable, QStringList({ QStringLiteral("T") }));
        QCOMPARE(outcome.files, QStringList({ QStringLiteral("A/B.kepub.epub"), QStringLiteral("metadata.calibre") }));
        QVERIFY(!outcome.clearCovers);

        QVERIFY(writeAll(result, "{\"error\": \"space\", \"detail\": \"There is insufficient free space in main memory\"}"));
        outcome = KoboSend::outcome(folder.path(), 1);
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains(QStringLiteral("not enough room")), qPrintable(outcome.error));
        QVERIFY(writeAll(result, "{\"error\": \"failed\", \"detail\": \"Could not kepubify the book\"}"));
        outcome = KoboSend::outcome(folder.path(), 1);
        QVERIFY2(outcome.error.contains(QStringLiteral("Could not kepubify")), qPrintable(outcome.error));

        QFile::remove(result);
        QVERIFY(writeAll(folder.filePath(QStringLiteral("log.txt")), "Sending...\nMemoryError\n"));
        outcome = KoboSend::outcome(folder.path(), -1);
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains(QStringLiteral("MemoryError")), qPrintable(outcome.error));
    }

    // The job's files go to the Kobo, metadata.calibre last, and the Kobo's
    // covers of the books sent go; not if metadata.calibre changed since.
    void appliesSends()
    {
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        QVERIFY(writeMetadata(kobo.path(), QJsonArray{}));
        Kobo::Device device = Kobo::describe(kobo.path(), {});
        for (const QString &file : Kobo::coverCache(device, vian))
            QVERIFY(writeAll(file, "cover"));
        QTemporaryDir folder;
        QVERIFY2(KoboSend::prepare(m_data, { 1 }, device, Kobo::formatMap(folder.path()), folder.path(), &error),
                 qPrintable(error));
        const QDir staged(folder.filePath(QStringLiteral("device")));
        QVERIFY(writeAll(staged.filePath(vian), "kepub"));
        QVERIFY(writeAll(staged.filePath(QStringLiteral("metadata.calibre")), "[{\"lpath\": \"x\"}]"));
        QVERIFY(writeAll(staged.filePath(QStringLiteral("driveinfo.calibre")), "{}"));
        KoboSend::Outcome outcome;
        outcome.ok = true;
        outcome.sent = { { 1, vian } };
        outcome.files = { vian, QStringLiteral("driveinfo.calibre"), QStringLiteral("metadata.calibre") };
        QList<double> progress;
        QVERIFY2(KoboSend::apply(folder.path(), outcome, device, &error, [&progress](double f) { progress.append(f); }),
                 qPrintable(error));
        const QDir root(kobo.path());
        QCOMPARE(readAll(root.filePath(vian)), QByteArray("kepub"));
        QCOMPARE(readAll(root.filePath(QStringLiteral("metadata.calibre"))), QByteArray("[{\"lpath\": \"x\"}]"));
        QCOMPARE(readAll(root.filePath(QStringLiteral("driveinfo.calibre"))), QByteArray("{}"));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral(".kobo-images"))));
        QCOMPARE(progress.last(), 1.0);
        QCOMPARE(filesUnder(kobo.path()).filter(QStringLiteral(".~")), QStringList());

        // metadata.calibre changed under the job: nothing goes.
        QTemporaryDir second;
        QVERIFY2(KoboSend::prepare(m_data, { 1 }, device, Kobo::formatMap(second.path()), second.path(), &error),
                 qPrintable(error));
        QVERIFY(writeAll(QDir(second.path()).filePath(QStringLiteral("device/") + vian), "other"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("metadata.calibre")), "[{\"lpath\": \"changed\"}]"));
        QVERIFY(!KoboSend::apply(second.path(), outcome, device, &error));
        QVERIFY2(error.contains(QStringLiteral("changed")), qPrintable(error));
        QCOMPARE(readAll(root.filePath(vian)), QByteArray("kepub"));

        // Unplugged.
        QVERIFY(QDir(root.filePath(QStringLiteral(".kobo"))).removeRecursively());
        QVERIFY(!KoboSend::apply(second.path(), outcome, device, &error));
        QVERIFY2(error.contains(QStringLiteral("no longer connected")), qPrintable(error));
    }

    // Calibre itself, when it is installed, with its default settings: an
    // EPUB arrives as a KEPUB with the library's metadata, which Cousoir
    // then finds on the Kobo as the library's book.
    void sendsWithCalibre()
    {
        if (SendJob::program().isEmpty())
            QSKIP("Calibre is not installed");
        QTemporaryDir config;
        qputenv("CALIBRE_CONFIG_DIRECTORY", QFile::encodeName(config.path()));
        QTemporaryDir kobo;
        QString error;
        QVERIFY2(makeKobo(kobo.path(), &error), qPrintable(error));
        Kobo::Device device = Kobo::describe(kobo.path(), {});
        device.productId = 0x4234;
        QTemporaryDir folder;
        QVERIFY2(KoboSend::prepare(m_data, { 1 }, device, Kobo::formatMap(config.path()), folder.path(), &error),
                 qPrintable(error));
        QList<double> progress;
        const KoboSend::Outcome outcome
            = KoboSend::run(folder.path(), QStringLiteral("fr"), [&progress](double f) { progress.append(f); });
        qunsetenv("CALIBRE_CONFIG_DIRECTORY");
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.sent.size(), 1);
        QCOMPARE(outcome.sent.first().lpath, vian);
        QVERIFY(!progress.isEmpty());
        QVERIFY2(KoboSend::apply(folder.path(), outcome, device, &error), qPrintable(error));

        const QDir root(kobo.path());
        const QByteArray text = epubText(root.filePath(vian));
        QVERIFY(text.contains("koboSpan"));
        const QJsonArray metadata = Kobo::readMetadata(kobo.path());
        QCOMPARE(metadata.size(), 1);
        const QJsonObject book = metadata.first().toObject();
        QCOMPARE(book.value(QLatin1String("lpath")).toString(), vian);
        QCOMPARE(book.value(QLatin1String("uuid")).toString(), m_data.book(1)->uuid);
        QCOMPARE(book.value(QLatin1String("title")).toString(), m_data.book(1)->title);
        const QJsonObject driveInfo = Kobo::readDriveInfo(kobo.path());
        QCOMPARE(driveInfo.value(QLatin1String("device_name")).toString(), QStringLiteral("Kobo Libra 2"));
        QCOMPARE(driveInfo.value(QLatin1String("last_library_uuid")).toString(), m_data.uuid);
        QCOMPARE(driveInfo.value(QLatin1String("prefix")).toString(), device.root + u'/');
        // Nothing of the job, and not the Kobo's database, changed there.
        QCOMPARE(filesUnder(kobo.path()), QStringList({ QStringLiteral(".kobo/KoboReader.sqlite"), QStringLiteral(".kobo/version"),
                                                        vian, QStringLiteral("driveinfo.calibre"),
                                                        QStringLiteral("metadata.calibre") }));

        QList<DeviceBook> books = Kobo::books(Kobo::describe(kobo.path(), {}), &error);
        DeviceBooks::match(&books, m_data);
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().bookId, 1);
    }
};

QTEST_GUILESS_MAIN(KoboTest)

#include "kobo_test.moc"
