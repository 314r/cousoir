#include <QtTest>

#include <QCryptographicHash>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "conversion.h"
#include "device_files.h"
#include "kindle.h"
#include "kindle_clippings.h"
#include "kindle_send.h"
#include "mobi_metadata.h"
#include "test_support.h"

namespace {

QStringList paths(const QList<DeviceFiles::Entry> &entries)
{
    QStringList list;
    for (const DeviceFiles::Entry &entry : entries)
        list.append(entry.path);
    return list;
}

// A folder made to look like a Kindle's storage over MTP.
bool makeKindle(const QString &root)
{
    return QDir().mkpath(QDir(root).filePath(QStringLiteral("documents")))
        && QDir().mkpath(QDir(root).filePath(QStringLiteral("system/thumbnails")));
}

QByteArray index(const QString &lpath)
{
    return QCryptographicHash::hash(lpath.toLower().toUtf8(), QCryptographicHash::Sha1).toHex();
}

const QString company = QStringLiteral("documents/Black Company, The - Glen Cook.azw3");

} // namespace

class KindleTest : public QObject
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

    // Without Calibre: the AZW3 with the library's metadata, its cover
    // thumbnail kept against Amazon's bug, its page numbers, a PDF as it is,
    // and both in metadata.calibre; sent again, in place.
    void sendsWithoutCalibre()
    {
        QTemporaryDir root;
        QTemporaryDir config;
        QVERIFY(makeKindle(root.path()));
        const QStringList formats = Kindle::formatMap(config.path(), QStringLiteral("G000000000000000"));
        for (int round = 0; round < 2; ++round) {
            FolderFiles files(root.path(), true);
            QString error;
            QVERIFY2(files.scan(&error), qPrintable(error));
            QTemporaryDir job;
            QVERIFY2(KindleSend::prepareNatively(m_data, { 2, 5 }, files, KindleSend::standIn(), formats, config.path(),
                                                 QStringLiteral("fr"), job.path(), &error),
                     qPrintable(error));
            const KindleSend::Outcome outcome = KindleSend::outcome(job.path(), 0);
            QVERIFY2(outcome.ok, qPrintable(outcome.error));
            QCOMPARE(outcome.sent.size(), 2);
            QCOMPARE(outcome.sent.first().lpath, company);
            QVERIFY2(KindleSend::apply(job.path(), outcome, files, &error), qPrintable(error));
        }
        const QByteArray book = readAll(root.filePath(company));
        QHash<quint32, QByteArray> exth;
        for (const auto &[id, content] : MobiMetadata::exthRecords(book))
            exth.insert(id, content);
        const Book *black = m_data.book(2);
        QCOMPARE(exth.value(112), QStringLiteral("calibre:%1").arg(black->uuid).toUtf8());
        QCOMPARE(exth.value(101), QByteArray("Tor"));
        QCOMPARE(exth.value(105), QByteArray("Fantasy"));
        const QString thumbnail = MobiMetadata::thumbnailName(book);
        QVERIFY(!thumbnail.isEmpty());
        const QImage cover(root.filePath(QStringLiteral("system/thumbnails/") + thumbnail));
        QVERIFY(!cover.isNull());
        QCOMPARE(cover.height(), 500);
        QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("amazon-cover-bug/") + thumbnail)));
        QCOMPARE(readAll(root.filePath(QStringLiteral("amazon-cover-bug/") + QString::fromLatin1(index(company)))),
                 thumbnail.toUtf8());
        QVERIFY(QFileInfo::exists(root.filePath(
            QStringLiteral("documents/Black Company, The - Glen Cook.sdr/Black Company, The - Glen Cook.apnx"))));
        // The PDF as it is.
        const QString manual = QStringLiteral("documents/Manuel technique - Jean-Pierre Dupont.pdf");
        QCOMPARE(readAll(root.filePath(manual)), readAll(m_data.book(5)->formats.first().file));

        const QJsonArray entries = QJsonDocument::fromJson(readAll(root.filePath(QStringLiteral("metadata.calibre")))).array();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).toObject().value(QLatin1String("lpath")).toString(), company.toLower());
        QCOMPARE(entries.at(0).toObject().value(QLatin1String("uuid")).toString(), black->uuid);
        QCOMPARE(entries.at(0).toObject().value(QLatin1String("size")).toInteger(), book.size());
        QCOMPARE(entries.at(0).toObject().value(QLatin1String("thumbnail")).toArray().at(1).toInt(), 500);
        const QJsonObject info = QJsonDocument::fromJson(readAll(root.filePath(QStringLiteral("driveinfo.calibre")))).object();
        QCOMPARE(info.value(QLatin1String("device_name")).toString(), QStringLiteral("Kindle"));
        QCOMPARE(info.value(QLatin1String("last_library_uuid")).toString(), m_data.uuid);
        QVERIFY(info.value(QLatin1String("mtp_prefix")).toString().startsWith(QLatin1String("mtp:::")));
    }

    // A book with an EPUB alone, converted by Cousoir to AZW3 as the window
    // does before sending it, goes to the Kindle as a book Calibre made:
    // the library's metadata, its cover's thumbnail, its page numbers.
    void sendsBooksConvertedWithoutCalibre()
    {
        LibraryData data = m_data;
        const auto shadows = std::find_if(data.books.begin(), data.books.end(), [](const Book &book) { return book.id == 3; });
        QVERIFY(shadows != data.books.end());
        QCOMPARE(shadows->formats.size(), 1);
        QTemporaryDir job;
        const Conversion::Outcome converted = Conversion::convertNatively(*shadows, QStringLiteral("AZW3"), job.path(), QStringLiteral("fr"));
        QVERIFY2(converted.ok, qPrintable(converted.error));
        shadows->formats.append({ QStringLiteral("AZW3"), shadows->formats.first().name, QFileInfo(converted.file).size(), converted.file });

        QTemporaryDir root;
        QTemporaryDir config;
        QVERIFY(makeKindle(root.path()));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY2(files.scan(&error), qPrintable(error));
        QTemporaryDir send;
        QVERIFY2(KindleSend::prepareNatively(data, { 3 }, files, KindleSend::standIn(),
                                             Kindle::formatMap(config.path(), QStringLiteral("G000000000000000")), config.path(),
                                             QStringLiteral("fr"), send.path(), &error),
                 qPrintable(error));
        const KindleSend::Outcome outcome = KindleSend::outcome(send.path(), 0);
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.sent.size(), 1);
        QVERIFY2(outcome.sent.first().lpath.endsWith(QLatin1String(".azw3")), qPrintable(outcome.sent.first().lpath));
        QVERIFY2(KindleSend::apply(send.path(), outcome, files, &error), qPrintable(error));

        const QByteArray book = readAll(root.filePath(outcome.sent.first().lpath));
        QHash<quint32, QByteArray> exth;
        for (const auto &[id, content] : MobiMetadata::exthRecords(book))
            exth.insert(id, content);
        QCOMPARE(exth.value(503), shadows->title.toUtf8());
        QCOMPARE(exth.value(112), QStringLiteral("calibre:%1").arg(shadows->uuid).toUtf8());
        const QString thumbnail = MobiMetadata::thumbnailName(book);
        QVERIFY(!thumbnail.isEmpty());
        QVERIFY(!QImage(root.filePath(QStringLiteral("system/thumbnails/") + thumbnail)).isNull());
        const QString sdr = outcome.sent.first().lpath.chopped(5) + QStringLiteral(".sdr/");
        QVERIFY(QFileInfo::exists(root.filePath(sdr + QFileInfo(outcome.sent.first().lpath).completeBaseName() + QStringLiteral(".apnx"))));
    }

    // A metadata.calibre that cannot be read stops the send rather than be
    // written over without the books it lists.
    void keepsWhatItCannotRead()
    {
        QTemporaryDir root;
        QTemporaryDir config;
        QVERIFY(makeKindle(root.path()));
        const QString metadata = root.filePath(QStringLiteral("metadata.calibre"));
        QVERIFY(writeAll(metadata, "[]"));
        QVERIFY(QFile::setPermissions(metadata, QFileDevice::WriteOwner));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QTemporaryDir job;
        QVERIFY(!KindleSend::prepareNatively(m_data, { 2 }, files, KindleSend::standIn(),
                                             Kindle::formatMap(config.path(), QStringLiteral("G000000000000000")),
                                             config.path(), QStringLiteral("fr"), job.path(), &error));
        QVERIFY(!error.isEmpty());
        QFile::setPermissions(metadata, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(readAll(metadata), QByteArray("[]"));
    }

    // My Clippings.txt as Kindles write it, in their languages and their
    // older forms.
    void readsClippings()
    {
        const QList<KindleClippings::Clipping> clippings
            = KindleClippings::parse(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kindle/My Clippings.txt")));
        QCOMPARE(clippings.size(), 13);
        const KindleClippings::Clipping &first = clippings.first();
        QCOMPARE(first.title, QStringLiteral("L’Écume des jours"));
        QCOMPARE(first.authors, QStringLiteral("Vian, Boris"));
        QCOMPARE(first.kind, KindleClippings::Clipping::Kind::Highlight);
        QCOMPARE(first.page, 12);
        QCOMPARE(first.start, 170);
        QCOMPARE(first.end, 171);
        // The Kindle's local time, with this computer's offset.
        QCOMPARE(first.added.left(19), QStringLiteral("2024-03-03T22:15:32"));
        QCOMPARE(QDateTime::fromString(first.added, Qt::ISODate).toLocalTime().date(), QDate(2024, 3, 3));
        QCOMPARE(first.text, QStringLiteral("Chick, Alise, Chloé et Colin"));
        QCOMPARE(clippings.at(3).kind, KindleClippings::Clipping::Kind::Bookmark);
        QCOMPARE(clippings.at(4).added.left(19), QStringLiteral("2024-03-04T00:30:00"));
        QCOMPARE(QDateTime::fromString(clippings.at(4).added, Qt::ISODate).toLocalTime().date(), QDate(2024, 3, 4));
        // In French.
        QCOMPARE(clippings.at(6).kind, KindleClippings::Clipping::Kind::Highlight);
        QCOMPARE(clippings.at(6).start, 300);
        QCOMPARE(clippings.at(6).added.left(19), QStringLiteral("2024-03-02T21:05:09"));
        QCOMPARE(clippings.at(7).kind, KindleClippings::Clipping::Kind::Note);
        // "Loc. 1702-05" of the older Kindles.
        QCOMPARE(clippings.at(8).start, 1702);
        QCOMPARE(clippings.at(8).end, 1705);
        QCOMPARE(clippings.at(8).added.left(19), QStringLiteral("2011-01-03T20:12:00"));
        // In German.
        QCOMPARE(clippings.at(11).title, QStringLiteral("Der Prozess"));
        QCOMPARE(clippings.at(11).kind, KindleClippings::Clipping::Kind::Highlight);
        QCOMPARE(clippings.at(11).added.left(19), QStringLiteral("2024-03-03T10:15:32"));
        QCOMPARE(KindleClippings::readDate(QStringLiteral("Added on someday")), QString());
    }

    // By book: a highlight changed is its last version, a note goes on the
    // highlight it was made on, in the book's order.
    void groupsClippings()
    {
        const QList<KindleClippings::Book> books = KindleClippings::byBook(
            KindleClippings::parse(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kindle/My Clippings.txt"))));
        QCOMPARE(books.size(), 5);
        const KindleClippings::Book &ecume = books.first();
        QCOMPARE(ecume.authors, QStringList{ QStringLiteral("Boris Vian") });
        QCOMPARE(ecume.notes.size(), 4);
        QCOMPARE(ecume.notes.at(0).type, QStringLiteral("Annotation"));
        QCOMPARE(ecume.notes.at(0).text,
                 QStringLiteral("Chick, Alise, Chloé et Colin passent leur temps à dire des choses rigolotes"));
        QCOMPARE(ecume.notes.at(0).annotation, QStringLiteral("Le ton du livre"));
        QCOMPARE(ecume.notes.at(0).position, QStringLiteral("page 12, location 170-172"));
        QCOMPARE(ecume.notes.at(1).type, QStringLiteral("Highlight"));
        // A note on no highlight.
        QCOMPARE(ecume.notes.at(2).type, QStringLiteral("Annotation"));
        QVERIFY(ecume.notes.at(2).text.isEmpty());
        QCOMPARE(ecume.notes.at(2).annotation, QStringLiteral("Relire ce passage"));
        QCOMPARE(ecume.notes.at(3).type, QStringLiteral("Bookmark"));
        QCOMPARE(ecume.notes.at(3).position, QStringLiteral("page 40, location 610"));

        QCOMPARE(books.at(1).notes.size(), 1);
        QCOMPARE(books.at(1).notes.first().annotation, QStringLiteral("L’essentiel est invisible pour les yeux."));
        QCOMPARE(books.at(2).authors, (QStringList{ QStringLiteral("Terry Pratchett"), QStringLiteral("Neil Gaiman") }));
        // Pages only, a PDF's.
        QCOMPARE(books.at(3).notes.size(), 1);
        QCOMPARE(books.at(3).notes.first().annotation, QStringLiteral("À essayer"));
        QCOMPARE(books.at(3).notes.first().position, QStringLiteral("page 3"));
        // An emptied highlight left out.
        QCOMPARE(books.at(4).notes.size(), 1);

        const QString markdown = DeviceBooks::notesMarkdown(ecume.title, ecume.authors, ecume.notes);
        QVERIFY(markdown.contains(QStringLiteral("> Chick, Alise")));
        QVERIFY(markdown.contains(QStringLiteral("*Bookmark, page 40, location 610*")));
    }

    // The Kindle's books get their clippings, matched with the library's
    // books; those of books no longer on it stay apart.
    void attachesClippings()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(QFile::copy(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kindle/My Clippings.txt"),
                            root.filePath(QStringLiteral("documents/My Clippings.txt"))));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/L'Écume des jours - Boris Vian.azw3")), "book"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QList<DeviceBook> books = Kindle::books(files, &error);
        // The clippings file is a book too, as Calibre's driver lists it:
        // it leaves out text files at the root only.
        QCOMPARE(books.size(), 2);
        std::sort(books.begin(), books.end(), [](const DeviceBook &a, const DeviceBook &b) { return a.lpath < b.lpath; });
        QList<DeviceBook> clippings = Kindle::clippings(files, &error);
        QCOMPARE(error, QString());
        QCOMPARE(clippings.size(), 5);
        DeviceBooks::match(&books, m_data);
        DeviceBooks::match(&clippings, m_data);
        Kindle::attachClippings(&books, &clippings);
        QCOMPARE(books.first().bookId, 1);
        QCOMPARE(books.first().reading.notes.size(), 4);
        QCOMPARE(books.first().reading.status, 1);
        QStringList left;
        for (const DeviceBook &book : std::as_const(clippings))
            left.append(QStringLiteral("%1:%2").arg(book.title).arg(book.bookId));
        QCOMPARE(left, (QStringList{ QStringLiteral("Le Petit Prince:6"), QStringLiteral("Good Omens:4"),
                                     QStringLiteral("Manuel technique:5"), QStringLiteral("Der Prozess:0") }));
        // The same book under two headings: the notes of both, in order.
        QList<DeviceBook> twice(2);
        for (int i = 0; i < 2; ++i) {
            twice[i].bookId = 1;
            ReaderNote note;
            note.type = QStringLiteral("Highlight");
            note.text = QString::number(i);
            note.location = 20 - i * 10;
            twice[i].reading.notes = { note };
        }
        QList<DeviceBook> onKindle(1);
        onKindle.first().bookId = 1;
        Kindle::attachClippings(&onKindle, &twice);
        QVERIFY(twice.isEmpty());
        QCOMPARE(onKindle.first().reading.notes.size(), 2);
        QCOMPARE(onKindle.first().reading.notes.first().text, QStringLiteral("1"));

        // A Kindle without the file has none.
        QVERIFY(QFile::remove(root.filePath(QStringLiteral("documents/My Clippings.txt"))));
        FolderFiles bare(root.path(), true);
        QVERIFY(bare.scan(&error));
        QVERIFY(Kindle::clippings(bare, &error).isEmpty());
    }

    // is_folder_ignored() of Calibre's MTP driver, with its default rules.
    void ignoresFolders()
    {
        const auto ignored = [](const QString &path, bool kindle = true) {
            return DeviceFiles::ignored(path.split(u'/'), kindle);
        };
        QVERIFY(!ignored(QStringLiteral("documents")));
        QVERIFY(ignored(QStringLiteral("documents/Book.sdr")));
        QVERIFY(!ignored(QStringLiteral("documents/Book.sdr"), false));
        QVERIFY(!ignored(QStringLiteral("system")));
        QVERIFY(!ignored(QStringLiteral("system/thumbnails")));
        QVERIFY(ignored(QStringLiteral("system/fonts")));
        QVERIFY(ignored(QStringLiteral("system"), false));
        QVERIFY(ignored(QStringLiteral(".hidden")));
        QVERIFY(!ignored(QStringLiteral(".notebooks")));
        QVERIFY(ignored(QStringLiteral("DCIM")));
        QVERIFY(ignored(QStringLiteral("Android/obb")));
        QVERIFY(!ignored(QStringLiteral("Android/data/com.amazon.kindle/files")));
        QVERIFY(ignored(QStringLiteral("Android/data/com.other")));
    }

    // A folder standing for a device's storage: listed but in the folders
    // ignored, found whatever the case, changed through it.
    void handlesFiles()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Book.azw3")), "book"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Book.sdr/Book.apnx")), "pages"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("system/fonts/font.ttf")), "font"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY2(files.scan(&error), qPrintable(error));
        QCOMPARE(paths(files.entries()),
                 QStringList({ QStringLiteral("documents"), QStringLiteral("documents/Book.azw3"),
                               QStringLiteral("documents/Book.sdr"), QStringLiteral("system"),
                               QStringLiteral("system/fonts"), QStringLiteral("system/thumbnails") }));
        QVERIFY(files.find(QStringLiteral("DOCUMENTS/book.AZW3")));
        QCOMPARE(files.find(QStringLiteral("documents/book.azw3"))->size, 4);
        const quint32 documents = files.find(QStringLiteral("documents"))->id;
        QCOMPARE(files.find(QStringLiteral("documents/Book.azw3"))->parent, documents);
        QCOMPARE(files.find(QStringLiteral("documents"))->parent, 0u);
        QCOMPARE(paths(files.children(QStringLiteral("Documents"))),
                 QStringList({ QStringLiteral("documents/Book.azw3"), QStringLiteral("documents/Book.sdr") }));

        QTemporaryDir local;
        QVERIFY(writeAll(local.filePath(QStringLiteral("new")), "newer"));
        // Into the folder as it is named there.
        QVERIFY2(files.makeFolder(QStringLiteral("Documents/Other"), &error), qPrintable(error));
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("documents/Other"))).isDir());
        QVERIFY2(files.write(QStringLiteral("documents/book.azw3"), local.filePath(QStringLiteral("new")), &error),
                 qPrintable(error));
        QCOMPARE(readAll(root.filePath(QStringLiteral("documents/Book.azw3"))), QByteArray("newer"));
        QVERIFY(!files.write(QStringLiteral("nowhere/x"), local.filePath(QStringLiteral("new")), &error));
        QVERIFY2(files.read(QStringLiteral("documents/Book.azw3"), local.filePath(QStringLiteral("back")), &error),
                 qPrintable(error));
        QCOMPARE(readAll(local.filePath(QStringLiteral("back"))), QByteArray("newer"));
        QVERIFY2(files.remove(QStringLiteral("documents/Other"), &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("documents/Other"))));
        QVERIFY(!files.find(QStringLiteral("documents/Other")));
    }

    // books() of the MTP driver: metadata.calibre's for the books Calibre
    // sent, file names for the others, no text file at the root.
    void listsBooks()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(writeAll(root.filePath(company), "azw3"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Some Book - Ann Author & Bob Author.pdf")), "pdf"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Book.sdr/Book.apnx")), "pages"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("notes.txt")), "notes"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("system/thumbnails/thumbnail.jpg")), "jpg"));
        const Book &black = *std::find_if(m_data.books.cbegin(), m_data.books.cend(), [](const Book &book) { return book.id == 2; });
        const QJsonArray sent{ QJsonObject{
            { QStringLiteral("lpath"), company.toLower() },
            { QStringLiteral("title"), black.title },
            { QStringLiteral("authors"), QJsonArray::fromStringList(black.authors) },
            { QStringLiteral("uuid"), black.uuid },
            { QStringLiteral("application_id"), 2 },
        } };
        QVERIFY(writeAll(root.filePath(QStringLiteral("metadata.calibre")), QJsonDocument(sent).toJson()));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QList<DeviceBook> books = Kindle::books(files, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(books.size(), 2);
        QCOMPARE(books.at(0).lpath, company);
        QCOMPARE(books.at(0).title, black.title);
        QCOMPARE(books.at(0).format, QStringLiteral("AZW3"));
        QVERIFY(books.at(0).sent);
        QCOMPARE(books.at(1).title, QStringLiteral("Some Book"));
        QCOMPARE(books.at(1).authors, QStringList({ QStringLiteral("Ann Author"), QStringLiteral("Bob Author") }));
        QVERIFY(!books.at(1).sent);
        DeviceBooks::match(&books, m_data);
        QCOMPARE(books.at(0).bookId, 2);
        QCOMPARE(books.at(1).bookId, 0);

        QVERIFY(writeAll(root.filePath(QStringLiteral("driveinfo.calibre")), R"({"device_name": "Liseuse"})"));
        QVERIFY(files.scan(&error));
        QCOMPARE(Kindle::name(Kindle::readDriveInfo(files), QStringLiteral("Kindle"), QStringLiteral("Model")),
                 QStringLiteral("Liseuse"));
        QCOMPARE(Kindle::name({}, QStringLiteral("Kindle"), QStringLiteral("Model")), QStringLiteral("Kindle"));
        QCOMPARE(Kindle::name({}, {}, QStringLiteral("Model")), QStringLiteral("Model"));
    }

    void choosesFormats()
    {
        QTemporaryDir config;
        const QStringList defaults = Kindle::formatMap(config.path(), QStringLiteral("G000"));
        QCOMPARE(defaults.first(), QStringLiteral("azw3"));
        QVERIFY(!defaults.contains(QStringLiteral("epub")));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("AZW3"), QStringLiteral("EPUB") }, defaults), QStringLiteral("AZW3"));
        QCOMPARE(DeviceBooks::sendFormat({ QStringLiteral("EPUB") }, defaults), QString());
        QVERIFY(writeAll(QDir(config.path()).filePath(QStringLiteral("mtp_devices.json")),
                         R"({"device-G000": {"format_map": ["pdf", "azw3"]}, "history": {}})"));
        QCOMPARE(Kindle::formatMap(config.path(), QStringLiteral("G000")),
                 QStringList({ QStringLiteral("pdf"), QStringLiteral("azw3") }));
        QCOMPARE(Kindle::formatMap(config.path(), QStringLiteral("OTHER")), defaults);
    }

    // delete_books() of the MTP driver: the file, the folders it leaves
    // empty and its cover, then its entry in metadata.calibre; the others
    // stay as written.
    void removesBooks()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        const QString other = QStringLiteral("documents/Kept/Other - Someone.azw3");
        QVERIFY(writeAll(root.filePath(company), "azw3"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Kept/Lone - One.pdf")), "pdf"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/Gone/Only - One.azw3")), "azw3"));
        QVERIFY(writeAll(root.filePath(other), "azw3"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("system/thumbnails/thumbnail_x_EBOK_portrait.jpg")), "cover"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("amazon-cover-bug/thumbnail_x_EBOK_portrait.jpg")), "cover"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("amazon-cover-bug/") + QString::fromLatin1(index(company))),
                         "thumbnail_x_EBOK_portrait.jpg"));
        const QByteArray kept = "{\n    \"lpath\": \"documents/kept/other - someone.azw3\",\n    \"series_index\": 1.0\n  }";
        const QByteArray gone = "{\n    \"lpath\": \"documents/black company, the - glen cook.azw3\",\n    \"size\": 4\n  }";
        QVERIFY(writeAll(root.filePath(QStringLiteral("metadata.calibre")), "[\n  " + gone + ",\n  " + kept + "\n]"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QList<DeviceBook> books = Kindle::books(files, &error);
        books.removeIf([&](const DeviceBook &book) {
            return book.lpath != company && book.lpath != QLatin1String("documents/Gone/Only - One.azw3");
        });
        QCOMPARE(books.size(), 2);
        QVERIFY2(Kindle::removeBooks(files, books, &error), qPrintable(error));
        QCOMPARE(filesUnder(root.path()),
                 QStringList({ QStringLiteral("documents/Kept/Lone - One.pdf"), other, QStringLiteral("metadata.calibre") }));
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("documents"))).isDir());
        QCOMPARE(readAll(root.filePath(QStringLiteral("metadata.calibre"))), "[\n  " + kept + "\n]");
    }

    // _sync_kindle_thumbnails(): the covers Amazon blanked come back.
    void restoresThumbnails()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(writeAll(root.filePath(QStringLiteral("system/thumbnails/a.jpg")), "blank"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("system/thumbnails/b.jpg")), "cover b"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("amazon-cover-bug/a.jpg")), "cover a"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("amazon-cover-bug/b.jpg")), "cover b"));
        QVERIFY(writeAll(root.filePath(QStringLiteral("amazon-cover-bug/c.jpg")), "cover c"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QCOMPARE(Kindle::restoreThumbnails(files, &error), 1);
        QCOMPARE(readAll(root.filePath(QStringLiteral("system/thumbnails/a.jpg"))), QByteArray("cover a"));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system/thumbnails/c.jpg"))));
        QCOMPARE(Kindle::restoreThumbnails(files, &error), 0);
    }

    void writesSendJobs()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(writeAll(root.filePath(QStringLiteral("metadata.calibre")), "[]"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QTemporaryDir folder;
        const KindleSend::Device device{ QStringLiteral("Kindle"), QStringLiteral("Model"), QStringLiteral("G000"),
                                         Kindle::VendorId, 0x9981 };
        QVERIFY2(KindleSend::prepare(m_data, { 2 }, files, device, Kindle::formatMap(folder.path(), device.serial),
                                     folder.path(), &error),
                 qPrintable(error));
        const QJsonObject job = QJsonDocument::fromJson(readAll(folder.filePath(QStringLiteral("job.json")))).object();
        QCOMPARE(job.value(QLatin1String("books")).toArray(), QJsonArray{ 2 });
        QCOMPARE(job.value(QLatin1String("device")).toObject().value(QLatin1String("vendor")).toInt(), 0x1949);
        QCOMPARE(job.value(QLatin1String("storage")).toObject().value(QLatin1String("id")).toInteger(), 0x10001);
        QStringList names;
        for (const QJsonValue &object : job.value(QLatin1String("objects")).toArray())
            names.append(object.toObject().value(QLatin1String("name")).toString());
        QCOMPARE(names, QStringList({ QStringLiteral("documents"), QStringLiteral("metadata.calibre"), QStringLiteral("system"),
                                      QStringLiteral("thumbnails") }));
        QCOMPARE(job.value(QLatin1String("staged")).toObject().value(QLatin1String("metadata.calibre")).toString(),
                 QStringLiteral("device/metadata.calibre"));
        QCOMPARE(readAll(folder.filePath(QStringLiteral("device/metadata.calibre"))), QByteArray("[]"));
        QVERIFY(readAll(folder.filePath(QStringLiteral("send.py"))).contains("MTP_DEVICE"));
        QVERIFY(QFileInfo::exists(folder.filePath(QStringLiteral("library/metadata.db"))));

        // No format a Kindle reads.
        QTemporaryDir other;
        QVERIFY(!KindleSend::prepare(m_data, { 1 }, files, device, Kindle::formatMap(other.path(), device.serial),
                                     other.path(), &error));
        QVERIFY2(error.contains(QStringLiteral("none of the formats")), qPrintable(error));
    }

    // The job's operations done on the Kindle, a file put again only once.
    void appliesSends()
    {
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        QVERIFY(writeAll(root.filePath(QStringLiteral("documents/old.azw3")), "old"));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        QTemporaryDir folder;
        QVERIFY(writeAll(folder.filePath(QStringLiteral("device/put-0")), "book"));
        QVERIFY(writeAll(folder.filePath(QStringLiteral("device/put-1")), "first"));
        QVERIFY(writeAll(folder.filePath(QStringLiteral("device/put-3")), "last"));
        QVERIFY(writeAll(folder.filePath(QStringLiteral("result.json")), R"({"sent": [{"book": 2, "lpath": "documents/new/b.azw3"}],
            "unsendable": [], "operations": [
                {"op": "mkdir", "path": "documents/new"},
                {"op": "put", "path": "documents/new/b.azw3", "file": "device/put-0"},
                {"op": "put", "path": "metadata.calibre", "file": "device/put-1"},
                {"op": "delete", "path": "documents/old.azw3"},
                {"op": "put", "path": "metadata.calibre", "file": "device/put-3"}]})"));
        const KindleSend::Outcome outcome = KindleSend::outcome(folder.path(), 0);
        QVERIFY(outcome.ok);
        QCOMPARE(outcome.sent.first().lpath, QStringLiteral("documents/new/b.azw3"));
        QCOMPARE(outcome.operations.size(), 5);
        QCOMPARE(outcome.operations.at(3).kind, KindleSend::Operation::Delete);
        QList<double> progress;
        QVERIFY2(KindleSend::apply(folder.path(), outcome, files, &error, [&progress](double f) { progress.append(f); }),
                 qPrintable(error));
        QCOMPARE(filesUnder(root.path()), QStringList({ QStringLiteral("documents/new/b.azw3"), QStringLiteral("metadata.calibre") }));
        QCOMPARE(readAll(root.filePath(QStringLiteral("metadata.calibre"))), QByteArray("last"));
        QCOMPARE(progress.last(), 1.0);
    }

    // Calibre itself, when it is installed, with its default settings: the
    // AZW3, its cover kept against Amazon's bug, its page numbers, and the
    // book in metadata.calibre, which Cousoir then finds as the library's.
    void sendsWithCalibre()
    {
        if (SendJob::program().isEmpty())
            QSKIP("Calibre is not installed");
        QTemporaryDir config;
        qputenv("CALIBRE_CONFIG_DIRECTORY", QFile::encodeName(config.path()));
        QTemporaryDir root;
        QVERIFY(makeKindle(root.path()));
        FolderFiles files(root.path(), true);
        QString error;
        QVERIFY(files.scan(&error));
        const KindleSend::Device device{ QStringLiteral("Kindle"), QStringLiteral("Kindle"), QStringLiteral("G000"),
                                         Kindle::VendorId, 0x9981 };
        QTemporaryDir folder;
        QVERIFY2(KindleSend::prepare(m_data, { 2 }, files, device, Kindle::formatMap(config.path(), device.serial),
                                     folder.path(), &error),
                 qPrintable(error));
        const KindleSend::Outcome outcome = KindleSend::run(folder.path(), QStringLiteral("fr"));
        qunsetenv("CALIBRE_CONFIG_DIRECTORY");
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.sent.size(), 1);
        QCOMPARE(outcome.sent.first().lpath, company);
        QVERIFY2(KindleSend::apply(folder.path(), outcome, files, &error), qPrintable(error));
        const QStringList written = filesUnder(root.path());
        QVERIFY(written.contains(company));
        QVERIFY(written.contains(QStringLiteral("documents/Black Company, The - Glen Cook.sdr/Black Company, The - Glen Cook.apnx")));
        QVERIFY(written.contains(QStringLiteral("amazon-cover-bug/") + QString::fromLatin1(index(company))));
        QCOMPARE(written.filter(QStringLiteral("system/thumbnails/thumbnail_")).size(), 1);
        QVERIFY(readAll(root.filePath(company)).mid(60, 8) == "BOOKMOBI");
        const QJsonObject driveInfo = QJsonDocument::fromJson(readAll(root.filePath(QStringLiteral("driveinfo.calibre")))).object();
        QCOMPARE(driveInfo.value(QLatin1String("mtp_prefix")).toString(), QStringLiteral("mtp:::65537:::"));
        QCOMPARE(driveInfo.value(QLatin1String("device_name")).toString(), QStringLiteral("Kindle"));

        QVERIFY(files.scan(&error));
        QList<DeviceBook> books = Kindle::books(files, &error);
        DeviceBooks::match(&books, m_data);
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().bookId, 2);
        QVERIFY(books.first().sent);
    }
};

QTEST_GUILESS_MAIN(KindleTest)

#include "kindle_test.moc"
