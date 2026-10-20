#include <QtTest>

#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "calibre_db.h"
#include "device_files.h"
#include "kindle.h"
#include "kindle_send.h"

namespace {

QString fixtureLibrary()
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/library");
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QStringList filesUnder(const QString &root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(QDir(root).relativeFilePath(it.next()));
    files.sort();
    return files;
}

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
