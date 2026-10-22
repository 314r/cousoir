// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Timothy Legge,
// Kovid Goyal and David Forrester, GPL v3:
// - src/calibre/devices/kobo/driver.py: the books KOBOTOUCH's books() lists,
//   imageid_from_contentid(), images_path(), cover_file_endings(), qhash(),
//   path_from_contentid() and the device models
// - src/calibre/devices/kobo/bookmark.py: Bookmark.get_bookmark_data()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kobo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <unistd.h>

#include "sqlite_db.h"

namespace {

// The Bookmark columns Calibre's Bookmark reads.
const char *const noteColumns[] = { "bookmarkid", "volumeid", "contentid", "text", "annotation", "chapterprogress",
                                    "startcontainerchildindex", "startoffset", "hidden" };

const QString onboard = QStringLiteral("file:///mnt/onboard/");

// The model ids of .kobo/version that tell apart the models sharing the Libra
// Colour's USB product id, as detected_product_id() reads them.
int productFor(int productId, const QString &modelId)
{
    if (productId != 0x4237)
        return productId;
    const QString model = modelId.right(3);
    if (model == QLatin1String("391"))
        return 0x4239;
    if (model == QLatin1String("393"))
        return 0x4238;
    if (model == QLatin1String("691"))
        return 0x5239;
    if (model == QLatin1String("693"))
        return 0x5238;
    return productId;
}

// os.path.splitext()'s extension: from the last dot of the last name, unless
// the name is all dots before it.
QString extensionOf(const QString &path)
{
    const qsizetype separator = path.lastIndexOf(u'/');
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot <= separator)
        return {};
    for (qsizetype index = separator + 1; index < dot; ++index) {
        if (path.at(index) != u'.')
            return path.mid(dot);
    }
    return {};
}

// Python's round(x, 2): the nearest number of two decimals, a tie to the
// even one, as x stands exactly.
double roundTwo(double value)
{
    // x * 100 is exact in a long double: 53 bits of x and 7 of 100.
    const long double scaled = static_cast<long double>(value) * 100;
    const long double below = std::floor(scaled);
    if (scaled - below == 0.5L) {
        const long double even = std::fmod(below, 2.0L) == 0 ? below : below + 1;
        return QString::number(double(even / 100), 'f', 2).toDouble();
    }
    return QString::number(value, 'f', 2).toDouble();
}

// text[:index] in Python, where find()'s -1 drops the last character.
QString before(const QString &text, qsizetype index)
{
    return index < 0 ? text.left(text.size() - 1) : text.left(index);
}

QSet<QString> columnsOf(SqliteDb &db, const QString &table)
{
    QSet<QString> columns;
    SqliteStatement info(db, QStringLiteral("PRAGMA table_info(%1)").arg(table));
    while (info.step())
        columns.insert(info.text(1).toLower());
    return columns;
}

// Bookmark.get_bookmark_data(): the highlights, notes and bookmarks of the
// book `contentId` the Kobo shows, by Calibre's key. A KEPUB's chapters come
// from its rows of type 899, whose ids lose their last "-" part.
QList<ReaderNote> readNotes(SqliteDb &db, const QString &contentId, bool withDates)
{
    const bool kepub = contentId.endsWith(QLatin1String(".kepub.epub")) || extensionOf(contentId).isEmpty();
    const auto text = [](const SqliteStatement &row, int column) {
        return row.isNull(column) ? QString() : row.text(column);
    };
    QHash<QString, std::pair<QString, int>> chapters;
    if (kepub) {
        SqliteStatement rows(db, QStringLiteral("SELECT c.ContentID, c.BookTitle, c.Title, c.VolumeIndex, c.___NumPages, "
                                                "c.MimeType FROM content c WHERE ContentType = 899 AND c.BookID = ? "
                                                "ORDER BY c.VolumeIndex"));
        rows.bind(1, contentId);
        while (rows.step()) {
            const QString id = rows.text(0);
            chapters.insert(before(id, id.lastIndexOf(u'-')), { text(rows, 2), int(rows.integer(3)) });
        }
    }
    SqliteStatement rows(db, QStringLiteral("SELECT bm.BookmarkID, bm.ContentID, bm.Text, bm.Annotation, "
                                            "bm.ChapterProgress, bm.StartContainerChildIndex, bm.StartOffset, "
                                            "c.BookTitle, c.Title, c.volumeIndex, c.MimeType, %1 "
                                            "FROM Bookmark bm LEFT OUTER JOIN Content c ON c.ContentID = bm.ContentID "
                                            "WHERE bm.Hidden = \"false\" AND bm.volumeid = ? "
                                            "ORDER BY bm.ContentID, bm.chapterprogress")
                                 .arg(withDates ? QStringLiteral("bm.DateCreated") : QStringLiteral("NULL")));
    rows.bind(1, contentId);
    QMap<int, ReaderNote> notes;
    int previousChapter = 0;
    int count = 0;
    while (rows.step()) {
        ReaderNote note;
        note.chapter = rows.isNull(9) ? 0 : int(rows.integer(9));
        note.chapterTitle = text(rows, 8);
        if (kepub) {
            // The bookmark's id as book!opf!file, the way the chapters are
            // keyed.
            const QString id = rows.text(1);
            qsizetype index = id.indexOf(u'!');
            const QString book = before(id, index);
            QString file = id.mid(index + 1);
            index = file.indexOf(u'!');
            const QString opf = before(file, index);
            file = file.mid(index + 1);
            const auto chapter = chapters.constFind(book + u'!' + opf + u'!' + file);
            note.chapterTitle = chapter != chapters.cend() ? chapter->first : QString();
            note.chapter = chapter != chapters.cend() ? chapter->second : 0;
        }
        count = previousChapter == note.chapter ? count + 1 : 0;
        note.text = text(rows, 2);
        note.annotation = text(rows, 3);
        const QVariant childIndex = rows.value(5);
        const QVariant offset = rows.value(6);
        const auto zero = [](const QVariant &value) {
            return value.typeId() == QMetaType::LongLong && value.toLongLong() == 0;
        };
        if (zero(childIndex) && zero(offset)) {
            // A corner turned down.
            note.type = QStringLiteral("Bookmark");
            note.text = text(rows, 8);
        } else if (!rows.isNull(2) && note.annotation.isEmpty()) {
            note.type = QStringLiteral("Highlight");
        } else if (!note.text.isEmpty() && !note.annotation.isEmpty()) {
            note.type = QStringLiteral("Annotation");
        } else {
            note.type = QStringLiteral("Unknown annotation type");
        }
        note.location = note.chapter * 1000 + count;
        // round(100 * progress, 2), at most 100; none counts as 0.
        note.chapterProgress = std::min(roundTwo(100 * rows.real(4)), 100.0);
        note.created = text(rows, 11);
        notes.insert(note.location, note);
        previousChapter = note.chapter;
    }
    return notes.values();
}

bool isTolino(const Kobo::Device &device)
{
    const int product = productFor(device.productId, device.modelId);
    return product == 0x5237 || product == 0x5238 || product == 0x5239;
}

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

QString formatOf(const QString &lpath, bool purchased)
{
    const QString lower = lpath.toLower();
    if (purchased || lower.endsWith(QLatin1String(".kepub.epub")) || lower.endsWith(QLatin1String(".kepub")))
        return QStringLiteral("KEPUB");
    return QFileInfo(lpath).suffix().toUpper();
}

// os.removedirs(): removes `folder`, then each parent left empty, but never
// the device's root.
void removeEmptyFolders(const QString &root, QString folder)
{
    const QString top = QDir::cleanPath(root);
    folder = QDir::cleanPath(folder);
    while (folder.startsWith(top + u'/') && QDir().rmdir(folder))
        folder = QFileInfo(folder).path();
}

// imageid_from_contentid().
QString imageIdFor(const QString &contentId, bool tolino)
{
    QString id = contentId;
    id.replace(u'/', u'_').replace(u' ', u'_').replace(u':', u'_');
    if (tolino) {
        const qsizetype dot = id.lastIndexOf(u'.');
        if (dot >= 0)
            return id.left(dot).replace(u'.', u'_') + id.mid(dot);
        return id;
    }
    return id.replace(u'.', u'_');
}

// qhash() of the Kobo driver: Qt 4's qHash() of the UTF-8 bytes.
quint32 qtFourHash(const QString &text)
{
    quint32 h = 0;
    for (const char c : text.toUtf8()) {
        h = (h << 4) + quint8(c);
        h ^= (h & 0xF0000000) >> 23;
        h &= 0x0FFFFFFF;
    }
    return h;
}

bool syncAndRename(QFile &file, const QString &path, QString *error)
{
    ::fsync(file.handle());
    const QString temporary = file.fileName();
    file.close();
    // std::rename replaces `path`; QFile::rename would refuse it.
    if (std::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) != 0) {
        QFile::remove(temporary);
        if (error)
            *error = QObject::tr("Cannot write %1 on the device.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

// A hidden name beside `path` the Kobo does not take for a book.
QString temporaryFor(const QString &path)
{
    const QFileInfo info(path);
    return info.dir().filePath(QStringLiteral(".~cousoir-") + info.fileName() + QStringLiteral(".tmp"));
}

} // namespace

namespace Kobo {

bool isKobo(const QString &root)
{
    return QFileInfo(QDir(root).filePath(QStringLiteral(".kobo"))).isDir();
}

Device describe(const QString &root, const QString &node, const QString &sysRoot)
{
    Device device;
    device.root = QDir::cleanPath(root);
    device.node = node;
    const QStringList version = readText(QDir(root).filePath(QStringLiteral(".kobo/version"))).trimmed().split(u',');
    device.serial = version.value(0);
    device.firmware = version.value(2);
    device.modelId = version.size() > 1 ? version.last() : QString();
    device.productId = usbProductId(node, sysRoot);
    const QString named = readDriveInfo(root).value(QLatin1String("device_name")).toString();
    device.name = named.isEmpty() ? modelName(device.productId, device.modelId) : named;
    const QStorageInfo storage(root);
    device.freeBytes = storage.bytesAvailable();
    device.totalBytes = storage.bytesTotal();
    return device;
}

int usbProductId(const QString &node, const QString &sysRoot)
{
    const QString name = QFileInfo(node).fileName();
    if (name.isEmpty())
        return 0;
    // The block device's folder in sysfs sits under its USB device's, which
    // has the ids.
    QString folder = QFileInfo(QDir(sysRoot).filePath(QStringLiteral("class/block/") + name)).canonicalFilePath();
    const QString top = QDir::cleanPath(sysRoot);
    while (folder.startsWith(top + u'/')) {
        const QString product = readText(QDir(folder).filePath(QStringLiteral("idProduct"))).trimmed();
        if (!product.isEmpty()) {
            bool ok = false;
            const int id = product.toInt(&ok, 16);
            return ok ? id : 0;
        }
        folder = QFileInfo(folder).path();
    }
    return 0;
}

QString modelName(int productId, const QString &modelId)
{
    static const QHash<int, QString> names{
        { 0x4203, QStringLiteral("Kobo Aura") },
        { 0x4226, QStringLiteral("Kobo Aura Edition 2") },
        { 0x4193, QStringLiteral("Kobo Aura HD") },
        { 0x4213, QStringLiteral("Kobo Aura H2O") },
        { 0x4227, QStringLiteral("Kobo Aura H2O Edition 2") },
        { 0x4225, QStringLiteral("Kobo Aura ONE") },
        { 0x4228, QStringLiteral("Kobo Clara HD") },
        { 0x4235, QStringLiteral("Kobo Clara 2E") },
        { 0x4239, QStringLiteral("Kobo Clara BW") },
        { 0x4238, QStringLiteral("Kobo Clara Colour") },
        { 0x4233, QStringLiteral("Kobo Elipsa") },
        { 0x4236, QStringLiteral("Kobo Elipsa 2E") },
        { 0x4229, QStringLiteral("Kobo Forma") },
        { 0x4173, QStringLiteral("Kobo Glo") },
        { 0x4223, QStringLiteral("Kobo Glo HD") },
        { 0x4232, QStringLiteral("Kobo Libra H2O") },
        { 0x4234, QStringLiteral("Kobo Libra 2") },
        { 0x4237, QStringLiteral("Kobo Libra Colour") },
        { 0x4183, QStringLiteral("Kobo Mini") },
        { 0x4230, QStringLiteral("Kobo Nia") },
        { 0x4231, QStringLiteral("Kobo Sage") },
        { 0x5239, QStringLiteral("Tolino Shine 5") },
        { 0x5238, QStringLiteral("Tolino Shine Color") },
        { 0x4163, QStringLiteral("Kobo Touch") },
        { 0x4224, QStringLiteral("Kobo Touch 2") },
        { 0x5237, QStringLiteral("Tolino Vision Color") },
    };
    return names.value(productFor(productId, modelId), QStringLiteral("Kobo eReader"));
}

QList<int> firmwareVersion(const Device &device)
{
    QList<int> version;
    for (const QString &part : device.firmware.split(u'.')) {
        bool ok = false;
        const int number = part.toInt(&ok);
        if (!ok)
            return { 0, 0, 0 };
        version.append(number);
    }
    return version.size() >= 3 ? version : QList<int>{ 0, 0, 0 };
}

QList<DeviceBook> books(const Device &device, QString *error)
{
    const QDir root(device.root);
    QHash<QString, QJsonObject> sent;
    QStringList sentOrder;
    for (const QJsonValue &value : readMetadata(device.root, error)) {
        const QJsonObject entry = value.toObject();
        const QString lpath = entry.value(QLatin1String("lpath")).toString();
        if (!lpath.isEmpty() && !sent.contains(lpath)) {
            sent.insert(lpath, entry);
            sentOrder.append(lpath);
        }
    }
    const auto fromMetadata = [&](DeviceBook *book, const QJsonObject &entry) {
        book->sent = true;
        book->title = entry.value(QLatin1String("title")).toString();
        book->authors.clear();
        for (const QJsonValue &author : entry.value(QLatin1String("authors")).toArray())
            book->authors.append(author.toString());
        book->authorSort = entry.value(QLatin1String("author_sort")).toString();
        book->uuid = entry.value(QLatin1String("uuid")).toString();
        book->applicationId = entry.value(QLatin1String("application_id")).toInt();
    };

    QList<DeviceBook> found;
    QSet<QString> listed;
    // The Kobo's database is only read: as it is unless the Kobo left a
    // write-ahead log beside it, which only a copy can take in.
    const QString database = root.filePath(QStringLiteral(".kobo/KoboReader.sqlite"));
    QTemporaryDir copy;
    SqliteDb db;
    QString dbError;
    bool opened = false;
    if (QFileInfo(database).isFile()) {
        if (QFileInfo(database + QStringLiteral("-wal")).size() > 0) {
            const QString copied = copy.filePath(QStringLiteral("KoboReader.sqlite"));
            opened = copy.isValid() && QFile::copy(database, copied)
                && QFile::copy(database + QStringLiteral("-wal"), copied + QStringLiteral("-wal"))
                && db.open(copied, SqliteDb::Mode::ReadWrite, &dbError);
        } else {
            opened = db.open(database, SqliteDb::Mode::Immutable, &dbError);
        }
    }
    if (opened) {
        const int dbVersion = db.value(QStringLiteral("SELECT version FROM dbversion")).toInt();
        QSet<QString> columns;
        {
            SqliteStatement info(db, QStringLiteral("PRAGMA table_info(content)"));
            while (info.step())
                columns.insert(info.text(1).toLower());
        }
        const auto column = [&](const QString &name, const QString &fallback = QStringLiteral("NULL")) {
            return columns.contains(name.toLower()) ? name : fallback + QStringLiteral(" AS ") + name;
        };
        // KoboTouch's query for the main memory: sideloaded books the Kobo
        // took in and books bought and downloaded, unless archived.
        QString where = QStringLiteral(" WHERE BookID IS NULL");
        if (columns.contains(QStringLiteral("accessibility")) && columns.contains(QStringLiteral("isdownloaded")))
            where += QStringLiteral(" AND ((Accessibility = -1 AND IsDownloaded IN ('true', 1))"
                                    " OR (Accessibility IN (1, 2, 8, 9) AND IsDownloaded IN ('true', 1)))");
        if (columns.contains(QStringLiteral("externalid")) && dbVersion >= 65)
            where += QStringLiteral(" AND (ExternalId IS NULL OR ExternalId = '')");
        where += QStringLiteral(" AND ContentID NOT LIKE 'file:///mnt/sd/%'");
        SqliteStatement rows(db, QStringLiteral("SELECT ContentID, ContentType, MimeType, Title, Attribution, ")
                                     + column(QStringLiteral("ImageId")) + QStringLiteral(", ")
                                     + column(QStringLiteral("ReadStatus")) + QStringLiteral(", ")
                                     + column(QStringLiteral("___PercentRead")) + QStringLiteral(", ")
                                     + column(QStringLiteral("DateLastRead")) + QStringLiteral(", ")
                                     + column(QStringLiteral("TimeSpentReading")) + QStringLiteral(" FROM content")
                                     + where);
        while (rows.step()) {
            const QString contentId = rows.text(0);
            const QString contentType = rows.text(1);
            const QString mimeType = rows.text(2);
            if (contentId.isEmpty() || contentId.startsWith(QLatin1String("file:///usr/local/kobo/help/"), Qt::CaseInsensitive)
                || contentId.startsWith(QLatin1String("/usr/local/kobo/help/"), Qt::CaseInsensitive))
                continue;
            // Audiobooks and the store's stand-ins have no file to show.
            if (mimeType == QLatin1String("Shortcover") || mimeType == QLatin1String("application/octet-stream")
                || mimeType == QLatin1String("application/x-kobo-mp3z"))
                continue;
            // path_from_contentid().
            DeviceBook book;
            if ((contentType == QLatin1String("6") || contentType == QLatin1String("10"))
                && mimeType == QLatin1String("application/x-kobo-epub+zip")
                && !contentId.startsWith(onboard)) {
                book.lpath = QStringLiteral(".kobo/kepub/") + contentId;
                book.purchased = true;
            } else {
                book.lpath = contentId;
                if (book.lpath.startsWith(onboard))
                    book.lpath = book.lpath.mid(onboard.size());
                else if (book.lpath.startsWith(QLatin1String("/mnt/onboard/")))
                    book.lpath = book.lpath.mid(13);
                else
                    continue;
            }
            if (listed.contains(book.lpath))
                continue;
            book.imported = true;
            book.title = rows.text(3);
            const QString attribution = rows.text(4);
            if (rows.isNull(4)) {
                book.authors = { QObject::tr("Unknown") };
            } else {
                for (const QString &author : attribution.split(u'&'))
                    book.authors.append(author.trimmed());
            }
            book.imageId = rows.text(5);
            book.contentId = contentId;
            // Bookmark.get_bookmark_data(): when and how far a book begun was
            // read.
            const qint64 status = rows.isNull(6) ? 0 : rows.integer(6);
            book.reading.percent = 0;
            if (status > 0) {
                book.reading.status = int(status);
                book.reading.lastRead = rows.text(8);
                if (status == 2)
                    book.reading.percent = 100;
                else
                    book.reading.percent = rows.isNull(7) ? std::nullopt : std::optional<int>(int(rows.integer(7)));
            }
            book.reading.secondsSpent = rows.isNull(9) ? 0 : rows.integer(9);
            book.format = formatOf(book.lpath, book.purchased);
            book.size = QFileInfo(root.filePath(book.lpath)).size();
            if (sent.contains(book.lpath))
                fromMetadata(&book, sent.value(book.lpath));
            listed.insert(book.lpath);
            found.append(book);
        }
        // The notes, where the Kobo's database has what Calibre reads of them.
        const QSet<QString> bookmark = columnsOf(db, QStringLiteral("Bookmark"));
        const QSet<QString> content = columnsOf(db, QStringLiteral("content"));
        const bool notes = std::all_of(std::begin(noteColumns), std::end(noteColumns),
                                       [&bookmark](const char *name) { return bookmark.contains(QLatin1String(name)); })
            && content.contains(QStringLiteral("volumeindex")) && content.contains(QStringLiteral("___numpages"));
        if (notes) {
            const bool dates = bookmark.contains(QStringLiteral("datecreated"));
            for (DeviceBook &book : found)
                book.reading.notes = readNotes(db, book.contentId, dates);
        }
    } else if (error && QFileInfo(database).isFile()) {
        *error = QObject::tr("Cannot read the Kobo's list of books: %1").arg(dbError);
    }

    // Sent since the Kobo last looked: there, but not yet in its database.
    for (const QString &lpath : std::as_const(sentOrder)) {
        if (listed.contains(lpath) || !QFileInfo(root.filePath(lpath)).isFile())
            continue;
        DeviceBook book;
        book.lpath = lpath;
        fromMetadata(&book, sent.value(lpath));
        book.format = formatOf(lpath, false);
        book.size = QFileInfo(root.filePath(lpath)).size();
        listed.insert(lpath);
        found.append(book);
    }
    return found;
}

QStringList formatMap(const QString &configDirectory)
{
    QFile file(QDir(configDirectory).filePath(QStringLiteral("device_drivers_KOBOTOUCH.py.json")));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonArray map = QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("format_map")).toArray();
        if (!map.isEmpty()) {
            QStringList formats;
            for (const QJsonValue &format : map)
                formats.append(format.toString().toLower());
            return formats;
        }
    }
    return { QStringLiteral("kepub"), QStringLiteral("epub"), QStringLiteral("pdf"),
             QStringLiteral("txt"),   QStringLiteral("cbz"),  QStringLiteral("cbr") };
}

QStringList coverCache(const Device &device, const QString &lpath, const QString &imageId)
{
    const bool tolino = isTolino(device);
    const QString id = imageId.isEmpty() ? imageIdFor(onboard + lpath, tolino) : imageId;
    // images_path(): a tree under .kobo-images from firmware 2.9.0.
    const QList<int> firmware = firmwareVersion(device);
    const bool tree = !tolino && firmware >= QList<int>{ 2, 9, 0 };
    QString path = QDir(device.root).filePath(tree || tolino ? QStringLiteral(".kobo-images/") : QStringLiteral(".kobo/images/"));
    if (tree) {
        const quint32 hash = qtFourHash(id);
        path += QString::number(hash & 0xFF) + u'/' + QString::number((hash & 0xFF00) >> 8) + u'/';
    }
    path += id;
    // cover_file_endings(): the ones of every Kobo, and the full screen one.
    if (tolino)
        return { path };
    QStringList files;
    for (const char *ending : { " - N3_LIBRARY_FULL.parsed", " - N3_LIBRARY_GRID.parsed", " - N3_LIBRARY_LIST.parsed",
                                " - AndroidBookLoadTablet_Aspect.parsed", " - N3_FULL.parsed" })
        files.append(path + QLatin1String(ending));
    return files;
}

void clearCoverCache(const Device &device, const QString &lpath, const QString &imageId)
{
    const QStringList cache = coverCache(device, lpath, imageId);
    for (const QString &file : cache)
        QFile::remove(file);
    if (!cache.isEmpty())
        removeEmptyFolders(device.root, QFileInfo(cache.first()).path());
}

bool removeBooks(const Device &device, const QList<DeviceBook> &books, QString *error)
{
    const QDir root(device.root);
    QSet<QString> removed;
    for (const DeviceBook &book : books) {
        if (book.purchased)
            continue;
        clearCoverCache(device, book.lpath, book.imageId);
        const QString path = root.filePath(book.lpath);
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (error)
                *error = QObject::tr("Cannot remove “%1” from the device.").arg(book.title);
            return false;
        }
        removeEmptyFolders(device.root, QFileInfo(path).path());
        removed.insert(book.lpath);
    }
    if (removed.isEmpty())
        return true;
    // The other entries stay as they are written, as JsonCodec wrote them
    // with Python's json.dumps(indent=2).
    QFile file(root.filePath(QStringLiteral("metadata.calibre")));
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read the device's metadata.calibre: %1").arg(file.errorString());
        return false;
    }
    const QList<QByteArray> before = DeviceBooks::arrayElements(file.readAll());
    file.close();
    QList<QByteArray> after;
    for (const QByteArray &entry : before) {
        if (!removed.contains(QJsonDocument::fromJson(entry).object().value(QLatin1String("lpath")).toString()))
            after.append(entry);
    }
    if (after.size() == before.size())
        return true;
    return writeFile(file.fileName(), DeviceBooks::joinElements(after), error);
}

QJsonArray readMetadata(const QString &root, QString *error)
{
    QFile file(QDir(root).filePath(QStringLiteral("metadata.calibre")));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read the device's metadata.calibre: %1").arg(file.errorString());
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isArray()) {
        if (error)
            *error = QObject::tr("The device's metadata.calibre is damaged: %1").arg(parseError.errorString());
        return {};
    }
    return document.array();
}

QJsonObject readDriveInfo(const QString &root)
{
    QFile file(QDir(root).filePath(QStringLiteral("driveinfo.calibre")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(temporaryFor(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(data) != data.size() || !file.flush()) {
        if (error)
            *error = QObject::tr("Cannot write %1 on the device: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        file.remove();
        return false;
    }
    return syncAndRename(file, path, error);
}

bool copyFile(const QString &source, const QString &path, QString *error)
{
    QFile in(source);
    QFile out(temporaryFor(path));
    if (!in.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(source), in.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).path()) || !out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QObject::tr("Cannot write %1 on the device: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        return false;
    }
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 20);
        if (chunk.isEmpty() || out.write(chunk) != chunk.size()) {
            if (error)
                *error = QObject::tr("Cannot write %1 on the device: %2").arg(QDir::toNativeSeparators(path), out.errorString());
            out.remove();
            return false;
        }
    }
    if (!out.flush()) {
        out.remove();
        if (error)
            *error = QObject::tr("Cannot write %1 on the device.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return syncAndRename(out, path, error);
}

} // namespace Kobo
