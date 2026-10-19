#include "kobo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>

#include <unistd.h>

#include "book.h"
#include "sqlite_db.h"

namespace {

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

// clean_string() of set_books_in_library(): lower case, letters and digits
// only, as Python's \W and _ leave them.
QString clean(const QString &text)
{
    QString cleaned;
    const QString lower = text.toLower();
    cleaned.reserve(lower.size());
    for (const QChar c : lower) {
        if (c.isLetterOrNumber())
            cleaned.append(c);
    }
    return cleaned;
}

// authors_to_string().
QString authorsToString(const QStringList &authors)
{
    QStringList parts;
    for (const QString &author : authors) {
        if (!author.isEmpty())
            parts.append(QString(author).replace(u'&', QLatin1String("&&")));
    }
    return parts.join(QLatin1String(" & "));
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

// The text of each element of the JSON array `json`, as written there.
QList<QByteArray> arrayElements(const QByteArray &json)
{
    QList<QByteArray> elements;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    qsizetype start = -1;
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json.at(i);
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (depth == 1 && start < 0 && !QChar::isSpace(uchar(c)) && c != ',' && c != ']')
            start = i;
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            if (--depth == 0 && start >= 0)
                elements.append(json.mid(start, i - start).trimmed());
        } else if (c == ',' && depth == 1 && start >= 0) {
            elements.append(json.mid(start, i - start).trimmed());
            start = -1;
        }
    }
    return elements;
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
    return info.dir().filePath(QStringLiteral(".~reliure-") + info.fileName() + QStringLiteral(".tmp"));
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
                                     + column(QStringLiteral("ImageId")) + QStringLiteral(" FROM content") + where);
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
            book.format = formatOf(book.lpath, book.purchased);
            book.size = QFileInfo(root.filePath(book.lpath)).size();
            if (sent.contains(book.lpath))
                fromMetadata(&book, sent.value(book.lpath));
            listed.insert(book.lpath);
            found.append(book);
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

void match(QList<DeviceBook> *books, const LibraryData &data)
{
    struct Titled
    {
        QHash<QString, int> authors;
        QHash<QString, int> authorSorts;
        QSet<int> ids;
    };
    QHash<QString, Titled> byTitle;
    QHash<QString, int> byUuid;
    // When two books share a title and authors, the last one wins: they are
    // the same as far as anyone can tell.
    for (const Book &book : data.books) {
        Titled &titled = byTitle[clean(book.title)];
        const QString authors = clean(authorsToString(book.authors));
        if (!authors.isEmpty())
            titled.authors.insert(authors, book.id);
        if (!book.authorSort.isEmpty())
            titled.authorSorts.insert(clean(book.authorSort), book.id);
        titled.ids.insert(book.id);
        byUuid.insert(book.uuid, book.id);
    }
    for (DeviceBook &book : *books) {
        book.bookId = 0;
        if (!book.uuid.isEmpty() && byUuid.contains(book.uuid)) {
            book.bookId = byUuid.value(book.uuid);
            continue;
        }
        const auto titled = byTitle.constFind(clean(book.title));
        if (titled == byTitle.cend())
            continue;
        if (book.applicationId && titled->ids.contains(book.applicationId)) {
            book.bookId = book.applicationId;
            continue;
        }
        const auto byAuthor = [&](const QString &key) {
            const int id = titled->authors.value(key, titled->authorSorts.value(key));
            if (id)
                book.bookId = id;
            return id != 0;
        };
        if (!book.authors.isEmpty() && !byAuthor(clean(authorsToString(book.authors)))) {
            for (const QString &author : std::as_const(book.authors)) {
                if (byAuthor(clean(author)))
                    break;
            }
        }
    }
}

QString sendFormat(const QStringList &formats, const QStringList &formatMap)
{
    for (const QString &format : formatMap) {
        if (formats.contains(format.toUpper()))
            return format.toUpper();
    }
    return {};
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
    const QList<QByteArray> before = arrayElements(file.readAll());
    file.close();
    QList<QByteArray> after;
    for (const QByteArray &entry : before) {
        if (!removed.contains(QJsonDocument::fromJson(entry).object().value(QLatin1String("lpath")).toString()))
            after.append(entry);
    }
    if (after.size() == before.size())
        return true;
    return writeFile(file.fileName(), after.isEmpty() ? QByteArray("[]") : "[\n  " + after.join(",\n  ") + "\n]", error);
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
