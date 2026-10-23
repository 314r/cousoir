// What happens to the golden tests' fake readers between their steps, the
// lists the tests take from them, and the fake sending job the unit tests
// run in place of Calibre's.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>

#include <algorithm>

#include "common.h"
#include "sqlite_db.h"

namespace Tool {

namespace {

// qhash() and imageid_from_contentid() of Calibre's Kobo driver,
// src/calibre/devices/kobo/driver.py, Copyright (C) Timothy Legge, Kovid
// Goyal and David Forrester, GPL v3.
quint64 qhash(const QString &text)
{
    quint64 h = 0;
    for (const char x : text.toUtf8()) {
        h = (h << 4) + quint8(x);
        h ^= (h & 0xF0000000) >> 23;
        h &= 0x0FFFFFFF;
    }
    return h;
}

QString imageId(QString contentId)
{
    for (const QChar c : { u'/', u' ', u':', u'.' })
        contentId.replace(c, u'_');
    return contentId;
}

QStringList sortedEntries(const QDir &dir, QDir::Filters filters)
{
    QStringList names = dir.entryList(filters | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::NoSort);
    std::sort(names.begin(), names.end());
    return names;
}

struct KoboImport
{
    QString root;
    QMap<QString, QJsonObject> known;
    SqliteDb &db;
    QSet<QString> present;
    QString error;

    // The book files of `folder` and of the folders in it, but hidden ones.
    bool walk(const QString &folder)
    {
        static const std::pair<const char *, const char *> types[] = {
            { ".kepub.epub", "application/x-kobo-epub+zip" },
            { ".epub", "application/epub+zip" },
            { ".pdf", "application/pdf" },
        };
        const QDir dir(folder);
        for (const QString &name : sortedEntries(dir, QDir::Files)) {
            const auto type = std::find_if(std::begin(types), std::end(types), [&name](const auto &type) {
                return name.toLower().endsWith(QLatin1String(type.first));
            });
            if (type == std::end(types))
                continue;
            const QString lpath = QDir(root).relativeFilePath(dir.filePath(name));
            const QString contentId = QStringLiteral("file:///mnt/onboard/") + lpath;
            present.insert(contentId);
            SqliteStatement exists(db, QStringLiteral("SELECT 1 FROM content WHERE ContentID = ?"));
            exists.bind(1, contentId);
            if (exists.step())
                continue;
            const QJsonObject book = known.value(lpath);
            const QString image = imageId(contentId);
            QStringList authors;
            for (const QJsonValue &author : book.value(QLatin1String("authors")).toArray())
                authors.append(author.toString());
            SqliteStatement insert(
                db, QStringLiteral("INSERT INTO content (ContentID, ContentType, MimeType, BookID, ImageId, Title, "
                                   "Attribution, ___UserID, ___FileSize, Accessibility, IsDownloaded, ReadStatus) "
                                   "VALUES (?, 6, ?, NULL, ?, ?, ?, ?, ?, -1, ?, 0)"));
            insert.bind(1, contentId);
            insert.bind(2, QString::fromLatin1(type->second));
            insert.bind(3, image);
            insert.bind(4, book.contains(QLatin1String("title")) ? book.value(QLatin1String("title")).toString()
                                                                  : name.left(name.size() - extension(name).size()));
            if (authors.isEmpty())
                insert.bindNull(5);
            else
                insert.bind(5, authors.join(QStringLiteral(" & ")));
            insert.bind(6, QString());
            insert.bind(7, QFileInfo(dir.filePath(name)).size());
            insert.bind(8, QStringLiteral("true"));
            if (insert.step() || !insert.error().isEmpty()) {
                error = insert.error();
                return false;
            }
            const quint64 h = qhash(image);
            const QString covers = QDir(root).filePath(
                QStringLiteral(".kobo-images/%1/%2").arg(h & 0xFF).arg((h & 0xFF00) >> 8));
            QDir().mkpath(covers);
            for (const char *ending : { " - N3_LIBRARY_FULL.parsed", " - N3_LIBRARY_GRID.parsed", " - N3_FULL.parsed" }) {
                if (!writeFile(QDir(covers).filePath(image + QLatin1String(ending)), "cover"))
                    return false;
            }
        }
        for (const QString &name : sortedEntries(dir, QDir::Dirs)) {
            if (!name.startsWith(u'.') && !walk(dir.filePath(name)))
                return false;
        }
        return true;
    }
};

} // namespace

// Does to a fake Kobo what a Kobo does once unplugged: takes in the book
// files sent to it, as rows of .kobo/KoboReader.sqlite with the title and
// authors of metadata.calibre, and makes their covers in its cache; forgets
// the books whose files are gone.
int koboImport(const QStringList &arguments)
{
    const QString root = QDir(arguments.at(0)).absolutePath();
    QMap<QString, QJsonObject> known;
    if (QFile::exists(QDir(root).filePath(QStringLiteral("metadata.calibre")))) {
        const std::optional<QJsonValue> books = readJson(QDir(root).filePath(QStringLiteral("metadata.calibre")));
        if (!books)
            return 1;
        for (const QJsonValue &book : books->toArray())
            known.insert(book.toObject().value(QLatin1String("lpath")).toString(), book.toObject());
    }
    SqliteDb db;
    QString error;
    if (!db.open(QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite")), SqliteDb::Mode::ReadWrite, &error)
        || !db.exec(QStringLiteral("BEGIN"), &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    KoboImport import{ root, known, db, {}, {} };
    if (!import.walk(root)) {
        err() << import.error << Qt::endl;
        return 1;
    }
    QStringList gone;
    SqliteStatement onboard(db, QStringLiteral("SELECT ContentID FROM content WHERE ContentID LIKE 'file:///mnt/onboard/%'"));
    while (onboard.step()) {
        if (!import.present.contains(onboard.text(0)))
            gone.append(onboard.text(0));
    }
    for (const QString &contentId : std::as_const(gone)) {
        SqliteStatement remove(db, QStringLiteral("DELETE FROM content WHERE ContentID = ?"));
        remove.bind(1, contentId);
        remove.step();
        if (!remove.error().isEmpty()) {
            err() << remove.error() << Qt::endl;
            return 1;
        }
    }
    if (!db.exec(QStringLiteral("COMMIT"), &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    return 0;
}

// Does to a fake Kindle what Amazon does to a Kindle once it syncs: replaces
// the covers of the books sent, in system/thumbnails, with blank ones.
int kindleSync(const QStringList &arguments)
{
    const QDir thumbnails(QDir(arguments.at(0)).filePath(QStringLiteral("system/thumbnails")));
    if (!thumbnails.exists()) {
        err() << "No folder " << thumbnails.path() << Qt::endl;
        return 1;
    }
    for (const QString &name : sortedEntries(thumbnails, QDir::Files)) {
        if (!writeFile(thumbnails.filePath(name), "blank"))
            return 1;
    }
    return 0;
}

// Runs the SQL of a file on a database, made if missing: a fake Kobo's.
int runSql(const QStringList &arguments)
{
    const std::optional<QByteArray> sql = readFile(arguments.at(1));
    if (!sql) {
        err() << "Cannot read " << arguments.at(1) << Qt::endl;
        return 1;
    }
    if (!QFile::exists(arguments.at(0)) && !writeFile(arguments.at(0), {}))
        return 1;
    SqliteDb db;
    QString error;
    if (!db.open(arguments.at(0), SqliteDb::Mode::ReadWrite, &error) || !db.exec(QString::fromUtf8(*sql), &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    return 0;
}

// The path on a reader of the book of id BOOK in its metadata.calibre, as a
// JSON array, empty when the reader does not have it.
int deviceLpath(const QStringList &arguments)
{
    const std::optional<QJsonValue> books = readJson(arguments.at(0));
    if (!books)
        return 1;
    QJsonArray found;
    for (const QJsonValue &book : books->toArray()) {
        if (book.toObject().value(QLatin1String("application_id")) == QJsonValue(arguments.at(1).toLongLong())) {
            found.append(book.toObject().value(QLatin1String("lpath")));
            break;
        }
    }
    out() << QJsonDocument(found).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}

// The books `cousoir-cli device` lists, from its standard input, without
// their size and sorted by path, to compare two readers' lists.
int deviceBooks(const QStringList &)
{
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) {
        err() << "Cannot read the standard input" << Qt::endl;
        return 1;
    }
    QJsonParseError error;
    const QJsonDocument listing = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        err() << error.errorString() << Qt::endl;
        return 1;
    }
    QList<QJsonObject> books;
    for (const QJsonValue &value : listing.object().value(QLatin1String("books")).toArray()) {
        QJsonObject book = value.toObject();
        book.remove(QStringLiteral("size"));
        books.append(book);
    }
    std::stable_sort(books.begin(), books.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QLatin1String("lpath")).toString().toLower() < b.value(QLatin1String("lpath")).toString().toLower();
    });
    QJsonArray sorted;
    for (const QJsonObject &book : std::as_const(books))
        sorted.append(book);
    out() << QJsonDocument(sorted).toJson(QJsonDocument::Indented);
    return 0;
}

// Whether Calibre took every book on a reader for one of the library's by
// its uuid, in each of the files send_books.py wrote its matches to.
int checkMatches(const QStringList &arguments)
{
    for (const QString &path : arguments) {
        const std::optional<QJsonValue> matches = readJson(path);
        if (!matches)
            return 1;
        const QJsonArray array = matches->toArray();
        if (array.isEmpty() || !std::all_of(array.cbegin(), array.cend(), [](const QJsonValue &match) {
                return match.toArray().at(1) == QJsonValue(QStringLiteral("UUID"));
            })) {
            err() << path << ": " << display(*matches) << Qt::endl;
            return 1;
        }
    }
    return 0;
}

// Stands in for calibre-debug in the tests of sending to a reader: run as
// Cousoir runs Calibre's job, "-e send.py -- <job folder>", it reports
// progress and writes what the job writes, after COUSOIR_FAKE_DELAY seconds
// if set. For a Kobo, each book as "Author/Title - Author.kepub.epub" in the
// job's device folder, its entry in metadata.calibre and driveinfo.calibre;
// for a Kindle, the same as operations to do, the book as
// "documents/Title - Author.azw3".
int fakeSend(const QString &folder)
{
    const QDir job(folder);
    const std::optional<QJsonValue> read = readJson(job.filePath(QStringLiteral("job.json")));
    if (!read)
        return 1;
    const QJsonObject description = read->toObject();
    const QDir device(job.filePath(QStringLiteral("device")));
    const bool kindle = description.contains(QLatin1String("objects"));
    QDir().mkpath(device.path());
    out() << "{\"progress\": 0.5}" << Qt::endl;
    if (const QByteArray delay = qgetenv("COUSOIR_FAKE_DELAY"); !delay.isEmpty())
        QThread::msleep(qulonglong(delay.toDouble() * 1000));

    SqliteDb db;
    QString error;
    if (!db.open(job.filePath(QStringLiteral("library/metadata.db")), SqliteDb::Mode::Immutable, &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    QJsonArray books;
    if (QFile::exists(device.filePath(QStringLiteral("metadata.calibre")))) {
        const std::optional<QJsonValue> known = readJson(device.filePath(QStringLiteral("metadata.calibre")));
        if (!known)
            return 1;
        books = known->toArray();
    }
    QJsonArray sent;
    QList<std::pair<QString, QString>> files;
    for (const QJsonValue &value : description.value(QLatin1String("books")).toArray()) {
        const qint64 bookId = value.toInteger();
        SqliteStatement book(db, QStringLiteral("SELECT title, uuid, author_sort FROM books WHERE id = ?"));
        book.bind(1, bookId);
        if (!book.step()) {
            err() << "No book " << bookId << Qt::endl;
            return 1;
        }
        const QString title = book.text(0);
        const QString uuid = book.text(1);
        const QString authorSort = book.text(2);
        QStringList authors;
        SqliteStatement names(db, QStringLiteral("SELECT name FROM authors JOIN books_authors_link ON author = authors.id "
                                                 "WHERE book = ? ORDER BY books_authors_link.id"));
        names.bind(1, bookId);
        while (names.step())
            authors.append(names.text(0).replace(u'|', u','));
        const QString byline = title + QStringLiteral(" - ") + authors.join(QStringLiteral(" & "));
        const QString lpath = kindle ? QStringLiteral("documents/%1.azw3").arg(byline)
                                     : QStringLiteral("%1/%2.kepub.epub").arg(authorSort, byline);
        const QString path = kindle ? device.filePath(QStringLiteral("put-%1").arg(files.size())) : device.filePath(lpath);
        QDir().mkpath(QFileInfo(path).path());
        if (!writeFile(path, QStringLiteral("sent %1").arg(title).toUtf8()))
            return 1;
        const QString entryLpath = kindle ? lpath.toLower() : lpath;
        QJsonArray kept;
        for (const QJsonValue &known : std::as_const(books)) {
            if (known.toObject().value(QLatin1String("lpath")).toString() != entryLpath)
                kept.append(known);
        }
        kept.append(QJsonObject{
            { QStringLiteral("lpath"), entryLpath },
            { QStringLiteral("title"), title },
            { QStringLiteral("authors"), QJsonArray::fromStringList(authors) },
            { QStringLiteral("author_sort"), authorSort },
            { QStringLiteral("uuid"), uuid },
            { QStringLiteral("application_id"), bookId },
            { QStringLiteral("size"), QFileInfo(path).size() },
            { QStringLiteral("mime"), QJsonValue::Null },
            { QStringLiteral("thumbnail"), QJsonValue::Null },
        });
        books = kept;
        sent.append(QJsonObject{ { QStringLiteral("book"), bookId }, { QStringLiteral("lpath"), lpath } });
        files.append({ lpath, job.relativeFilePath(path) });
    }
    const QJsonObject driveinfo{
        { QStringLiteral("device_name"), kindle ? QStringLiteral("Kindle") : QStringLiteral("Kobo eReader") },
        { QStringLiteral("location_code"), QStringLiteral("main") },
    };
    if (!writeFile(device.filePath(QStringLiteral("metadata.calibre")), QJsonDocument(books).toJson())
        || !writeFile(device.filePath(QStringLiteral("driveinfo.calibre")),
                      QJsonDocument(driveinfo).toJson(QJsonDocument::Compact)))
        return 1;
    QJsonObject result{ { QStringLiteral("sent"), sent }, { QStringLiteral("unsendable"), QJsonArray() } };
    if (kindle) {
        QJsonArray operations;
        for (const auto &[lpath, local] : std::as_const(files))
            operations.append(QJsonObject{
                { QStringLiteral("op"), QStringLiteral("put") },
                { QStringLiteral("path"), lpath },
                { QStringLiteral("file"), local },
            });
        for (const QString &name : { QStringLiteral("driveinfo.calibre"), QStringLiteral("metadata.calibre") })
            operations.append(QJsonObject{
                { QStringLiteral("op"), QStringLiteral("put") },
                { QStringLiteral("path"), name },
                { QStringLiteral("file"), QStringLiteral("device/") + name },
            });
        result.insert(QStringLiteral("operations"), operations);
    } else {
        QJsonArray written;
        for (const auto &[lpath, local] : std::as_const(files))
            written.append(lpath);
        written.append(QStringLiteral("driveinfo.calibre"));
        written.append(QStringLiteral("metadata.calibre"));
        result.insert(QStringLiteral("files"), written);
        result.insert(QStringLiteral("clear_covers"), true);
    }
    if (!writeFile(job.filePath(QStringLiteral("result.json")), QJsonDocument(result).toJson(QJsonDocument::Compact)))
        return 1;
    out() << "{\"progress\": 1.0}" << Qt::endl;
    return 0;
}

} // namespace Tool
