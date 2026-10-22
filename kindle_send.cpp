// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/devices/mtp/unix/driver.py: put_file(), get_mtp_file() and
//   delete_file_or_folder(), in the Python the job runs
// - src/calibre/devices/mtp/driver.py: open() and books(); without Calibre,
//   prefix_for_location(), create_upload_path(), ensure_parent(),
//   upload_books(), upload_kindle_thumbnail(), upload_apnx(),
//   _update_drive_info() and get_pref()
// - src/calibre/devices/mtp/defaults.py: Amazon's settings
// - src/calibre/devices/kindle/driver.py: get_apnx_opts()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kindle_send.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include "book.h"
#include "calibre_job.h"
#include "device_books.h"
#include "device_files.h"
#include "kindle.h"
#include "mobi_metadata.h"
#include "native_send.h"
#include "python_json.h"
#include "send_job.h"

namespace {

// The files of the Kindle Calibre's driver reads when it sends: the job's
// device folder has their copies.
const char *const deviceFiles[] = { "metadata.calibre", "driveinfo.calibre" };

// Calibre 9.16's "Send to device" for a Kindle, as its GUI runs it once the
// Kindle is connected over MTP: MTP_DEVICE's open(), its books() and
// set_books_in_library() of the connection, then sync_to_device(), the upload
// job of DeviceManager with MTP_DEVICE's upload_books(), and
// books_uploaded(), which adds the books to metadata.calibre. The driver runs
// on a stand-in for the Kindle; what it would do there is noted.
const char sendScript[] = R"py(

def send(job):
    import io, itertools, shutil, time
    from calibre.devices.errors import DeviceError
    from calibre.devices.mtp.driver import DEFAULT_THUMBNAIL_HEIGHT, MTP_DEVICE
    from calibre.devices.mtp.filesystem_cache import FilesystemCache
    from calibre.gui2.device import DeviceManager

    db = open_library()
    storage = job['storage']
    operations = []
    staged = {name.lower(): path(local) for name, local in job['staged'].items()}
    new_ids = itertools.count(max([o['id'] for o in job['objects']] + [storage['id']]) + 1)

    def relpath(obj):
        return '/'.join(obj.full_path[1:])

    def entry(parent, name, is_folder, size=0):
        return {'id': next(new_ids), 'parent_id': 0 if parent.is_storage else parent.object_id,
                'storage_id': parent.storage_id, 'name': name, 'is_folder': is_folder, 'size': size,
                'modified': time.time()}

    class Device(MTP_DEVICE):
        # Calibre's MTP driver on a stand-in for the Kindle: its files and
        # folders as Cousoir listed them, the few the driver reads copied in
        # the job's folder, and what it does to them noted for Cousoir.

        @property
        def filesystem_cache(self):
            if self._filesystem_cache is None:
                self._filesystem_cache = FilesystemCache([{
                    'id': storage['id'], 'size': storage['capacity'], 'is_folder': True, 'name': storage['name'],
                    'can_delete': False, 'is_system': True}], job['objects'])
            return self._filesystem_cache

        @property
        def is_mtp_device_connected(self):
            return True

        def get_basic_device_information(self):
            return (self.current_friendly_name, '', '', '')

        def total_space(self, end_session=True):
            return (storage['capacity'], 0, 0)

        def free_space(self, end_session=True):
            return (storage['free'], 0, 0)

        def create_folder(self, parent, name):
            found = parent.folder_named(name)
            if found is not None:
                return found
            child = parent.add_child(entry(parent, name, True))
            operations.append({'op': 'mkdir', 'path': relpath(child)})
            return child

        def put_file(self, parent, name, stream, size, callback=None, replace=True):
            if parent.folder_named(name) is not None:
                raise ValueError(f'Cannot upload file, {parent.full_path} already has a folder named: {name}')
            existing = parent.file_named(name)
            if existing is not None:
                if not replace:
                    raise ValueError(f'Cannot upload file {existing.full_path}, it already exists')
                # The put replaces it.
                parent.remove_child(existing)
            local = path('device', f'put-{len(operations)}')
            with open(local, 'wb') as f:
                shutil.copyfileobj(stream, f)
            child = parent.add_child(entry(parent, name, False, os.path.getsize(local)))
            staged[relpath(child).lower()] = local
            operations.append({'op': 'put', 'path': relpath(child), 'file': os.path.relpath(local, folder)})
            return child

        def get_mtp_file(self, f, stream=None, callback=None):
            local = staged.get(relpath(f).lower())
            if local is None:
                raise DeviceError(f'Failed to get file: {f.full_path}: not copied for the job')
            set_name = stream is None
            if stream is None:
                stream = io.BytesIO()
            with open(local, 'rb') as src:
                stream.write(src.read())
            stream.seek(0)
            if set_name:
                stream.name = f.name
            return stream

        def delete_file_or_folder(self, obj):
            if obj.deleted:
                return
            if obj.files or obj.folders:
                raise ValueError(f'Cannot delete {obj.full_path} as it is not empty')
            parent = obj.parent
            parent.remove_child(obj)
            staged.pop(relpath(obj).lower(), None)
            operations.append({'op': 'delete', 'path': relpath(obj)})
            return parent

        def books(self, oncard=None, end_session=True):
            # MTP_DEVICE.books(), but for the e-books Calibre knows nothing of
            # yet, whose files it reads: those wait for Calibre.
            from calibre.devices.mtp.books import Book, BookList, JSONCodec

            self.get_driveinfo()
            sid = {'carda': self._carda_id, 'cardb': self._cardb_id}.get(oncard, self._main_id)
            if sid is None:
                return BookList(None)
            bl = BookList(sid)
            need_sync = False
            disk = self.filesystem_cache.storage(sid)
            cache = disk.find_path(self.find_calibre_file_path(disk, 'metadata'))
            if cache is not None:
                try:
                    JSONCodec().decode_from_file(self.get_mtp_file(cache), bl, Book, sid)
                except Exception:
                    need_sync = True
            relpath_cache = {b.mtp_relpath: i for i, b in enumerate(bl)}
            for mtp_file in self.filesystem_cache.iterebooks(sid):
                idx = relpath_cache.pop(mtp_file.mtp_relpath, None)
                if idx is None:
                    continue
                book = bl[idx]
                if book.size != mtp_file.size:
                    need_sync = True
                    book.size = mtp_file.size
                book.datetime = mtp_file.last_modified.timetuple()
                book.path = mtp_file.mtp_id_path
            for idx in sorted(relpath_cache.values(), reverse=True):
                del bl[idx]
                need_sync = True
            if need_sync:
                self.write_metadata_cache(disk, bl)
            return bl

    dev = Device(None)
    dev.startup()
    # Sending, then adding to metadata.calibre.
    dev.set_progress_reporter(reporter(2))
    # open(), but for libmtp's part, the history of devices in Calibre's
    # settings and the covers put back, which Cousoir does itself.
    device = job['device']
    dev.current_library_uuid = db.library_id
    dev.location_paths = None
    dev.driveinfo = {}
    dev._main_id = storage['id']
    dev._carda_id = dev._cardb_id = None
    dev.current_friendly_name = device['friendly_name'] or device['model_name'] or 'Unknown MTP device'
    dev.current_serial_num = device['serial']
    detected = types.SimpleNamespace(vendor_id=device['vendor'], product_id=device['product'])
    dev.currently_connected_dev = detected
    dev.current_device_defaults, dev.current_vid, dev.current_pid = dev.device_defaults(detected, dev)
    dev.calibre_file_paths = dev.current_device_defaults.get(
        'calibre_file_paths', {'metadata': dev.METADATA_CACHE, 'driveinfo': dev.DRIVEINFO})
    dev.THUMBNAIL_HEIGHT = 500 if dev.is_kindle else DEFAULT_THUMBNAIL_HEIGHT
    gui = stand_in(db, dev)

    DeviceManager._get_device_information(gui.device_manager)
    gui.set_default_thumbnail(dev.THUMBNAIL_HEIGHT)
    gui.lists = list(DeviceManager._books(gui.device_manager))
    gui.set_books_in_library(gui.lists, reset=True)

    sent = [{'book': book_id, 'lpath': relpath(mtp_file)} for book_id, mtp_file in send_books(gui, job['books'])]
    return {'sent': sent, 'unsendable': gui.unsendable, 'operations': operations}


main(send)
)py";

// The MTP driver's settings a send follows, as get_pref() finds them in
// mtp_devices.json: the Kindle's own, then Amazon's defaults, then the
// global ones.
struct MtpSettings
{
    QString sendTemplate = QStringLiteral("{title} - {authors}");
    QStringList sendTo{ QStringLiteral("documents"), QStringLiteral("kindle"), QStringLiteral("books") };
    // By extension, the folder a book goes to instead.
    QHash<QString, QString> rules;
};

MtpSettings mtpSettings(const QString &configDirectory, const QString &serial)
{
    MtpSettings settings;
    QFile file(QDir(configDirectory).filePath(QStringLiteral("mtp_devices.json")));
    if (!file.open(QIODevice::ReadOnly))
        return settings;
    const QJsonObject prefs = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject own = prefs.value(QStringLiteral("device-") + serial).toObject();
    const auto pref = [&](const QString &key, bool amazonHasOne) {
        if (own.contains(key))
            return own.value(key);
        return amazonHasOne ? QJsonValue() : prefs.value(key);
    };
    if (const QJsonValue value = pref(QStringLiteral("send_template"), false); value.isString())
        settings.sendTemplate = value.toString();
    if (const QJsonValue value = pref(QStringLiteral("send_to"), true); value.isArray()) {
        settings.sendTo.clear();
        for (const QJsonValue &path : value.toArray())
            settings.sendTo.append(path.toString());
    }
    for (const QJsonValue &rule : pref(QStringLiteral("rules"), false).toArray()) {
        const QJsonArray pair = rule.toArray();
        if (pair.size() == 2)
            settings.rules.insert(pair.at(0).toString().toLower(), pair.at(1).toString());
    }
    return settings;
}

// The Kindle driver's APNX settings, KINDLE2's extra_customization in
// device_drivers_KINDLE2.py.json: whether to send page numbers.
bool sendsApnx(const QString &configDirectory)
{
    QFile file(QDir(configDirectory).filePath(QStringLiteral("device_drivers_KINDLE2.py.json")));
    if (!file.open(QIODevice::ReadOnly))
        return true;
    const QJsonArray extra = QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("extra_customization")).toArray();
    return extra.isEmpty() || extra.at(0).toBool(true);
}

// The formats MetadataUpdater writes into.
bool isMobi(const QString &format)
{
    static const QStringList formats{ QStringLiteral("MOBI"), QStringLiteral("PRC"),  QStringLiteral("AZW"),
                                      QStringLiteral("AZW3"), QStringLiteral("AZW4"), QStringLiteral("POBI") };
    return formats.contains(format.toUpper());
}

// A file of the Kindle: empty when it has none; nothing, with `error`, when
// it has one that cannot be read, which would otherwise be written over
// without what it held.
std::optional<QByteArray> readFrom(DeviceFiles &files, const QString &path, QString *error)
{
    if (!files.find(path))
        return QByteArray();
    QTemporaryDir folder;
    const QString local = folder.filePath(QStringLiteral("file"));
    QFile file(local);
    if (!folder.isValid() || !files.read(path, local, error) || !file.open(QIODevice::ReadOnly)) {
        if (error->isEmpty())
            *error = QObject::tr("Cannot read %1 on the Kindle.").arg(path);
        return std::nullopt;
    }
    return file.readAll();
}

} // namespace

namespace KindleSend {

Device standIn()
{
    return { QStringLiteral("Kindle"), QStringLiteral("Kindle"), QStringLiteral("G000000000000000"), 0x1949, 0x9981 };
}

bool prepare(const LibraryData &data, const QList<int> &bookIds, DeviceFiles &files, const Device &device,
             const QStringList &formatMap, const QString &folder, QString *error)
{
    if (!SendJob::prepareLibrary(data, bookIds, formatMap, folder, error))
        return false;
    const QDir staging(QDir(folder).filePath(QStringLiteral("device")));
    QDir().mkpath(staging.path());
    QJsonObject staged;
    for (const char *name : deviceFiles) {
        const QString file = QLatin1String(name);
        if (!files.find(file))
            continue;
        if (!files.read(file, staging.filePath(file), error))
            return false;
        staged.insert(file, QStringLiteral("device/") + file);
    }
    QJsonArray objects;
    for (const DeviceFiles::Entry &entry : files.entries()) {
        objects.append(QJsonObject{
            { QStringLiteral("id"), qint64(entry.id) },
            { QStringLiteral("parent_id"), qint64(entry.parent) },
            { QStringLiteral("storage_id"), qint64(files.storageId()) },
            { QStringLiteral("name"), entry.path.mid(entry.path.lastIndexOf(u'/') + 1) },
            { QStringLiteral("is_folder"), entry.folder },
            { QStringLiteral("size"), entry.size },
            { QStringLiteral("modified"), entry.modified },
        });
    }
    const QJsonObject job{
        { QStringLiteral("device"), QJsonObject{
              { QStringLiteral("friendly_name"), device.friendlyName },
              { QStringLiteral("model_name"), device.modelName },
              { QStringLiteral("serial"), device.serial },
              { QStringLiteral("vendor"), device.vendorId },
              { QStringLiteral("product"), device.productId },
          } },
        { QStringLiteral("storage"), QJsonObject{
              { QStringLiteral("id"), qint64(files.storageId()) },
              { QStringLiteral("name"), files.storageName() },
              { QStringLiteral("capacity"), files.totalBytes() },
              { QStringLiteral("free"), files.freeBytes() },
          } },
        { QStringLiteral("objects"), objects },
        { QStringLiteral("staged"), staged },
    };
    return SendJob::writeJob(folder, job, bookIds, sendScript, error);
}

bool prepareNatively(const LibraryData &data, const QList<int> &bookIds, DeviceFiles &files, const Device &device,
                     const QStringList &formatMap, const QString &configDirectory, const QString &interfaceLanguage,
                     const QString &folder, QString *error)
{
    // MAX_PATH_LEN of the MTP driver; THUMBNAIL_HEIGHT for a Kindle.
    constexpr qsizetype maxPathLength = 230;
    constexpr int thumbnailSize = 500;
    const MtpSettings settings = mtpSettings(configDirectory, device.serial);
    const MobiMetadata::Options mobiOptions = MobiMetadata::Options::load(configDirectory);
    const bool apnxWanted = sendsApnx(configDirectory);
    const QDir staging(QDir(folder).filePath(QStringLiteral("device")));
    QDir().mkpath(staging.path());

    // The Kindle's folders by lower case path, as the driver finds them
    // whatever their case, and its books.
    QHash<QString, QString> folders;
    for (const DeviceFiles::Entry &entry : files.entries()) {
        if (entry.folder)
            folders.insert(entry.path.toLower(), entry.path);
    }
    QString listError;
    QSet<QString> onDevice;
    for (const DeviceBook &book : Kindle::books(files, &listError))
        onDevice.insert(book.lpath.toLower());
    // prefix_for_location(): the first folder of send_to the Kindle has.
    QString prefix = QStringLiteral("Books");
    for (const QString &candidate : settings.sendTo) {
        if (folders.contains(QString(candidate).replace(u'\\', u'/').toLower())) {
            prefix = folders.value(QString(candidate).replace(u'\\', u'/').toLower());
            break;
        }
    }

    QJsonArray operations;
    int puts = 0;
    // ensure_parent(): the folders of `path` the Kindle lacks, made; the
    // path with the folders' own case.
    const auto ensureParent = [&](const QString &path) {
        QStringList parts = path.split(u'/');
        const QString name = parts.takeLast();
        QString current;
        for (const QString &part : std::as_const(parts)) {
            const QString wanted = current.isEmpty() ? part : current + u'/' + part;
            const auto existing = folders.constFind(wanted.toLower());
            if (existing != folders.cend()) {
                current = *existing;
                continue;
            }
            operations.append(QJsonObject{ { QStringLiteral("op"), QStringLiteral("mkdir") }, { QStringLiteral("path"), wanted } });
            folders.insert(wanted.toLower(), wanted);
            current = wanted;
        }
        return current.isEmpty() ? name : current + u'/' + name;
    };
    // `bytes`, or the file at `source` copied as it is.
    const auto put = [&](const QString &path, const QByteArray &bytes, const QString &source = {}) -> std::optional<QString> {
        const QString placed = ensureParent(path);
        const QString local = QStringLiteral("device/put-%1").arg(puts++);
        const bool written = source.isEmpty() ? CalibreJob::writeFile(QDir(folder).filePath(local), bytes, error)
                                              : SendJob::copyInto(source, QDir(folder).filePath(local), error);
        if (!written)
            return std::nullopt;
        operations.append(QJsonObject{ { QStringLiteral("op"), QStringLiteral("put") },
                                       { QStringLiteral("path"), placed },
                                       { QStringLiteral("file"), local } });
        return placed;
    };

    // driveinfo.calibre, as the driver writes it when it lists the books.
    const std::optional<QByteArray> driveInfo = readFrom(files, QStringLiteral("driveinfo.calibre"), error);
    if (!driveInfo)
        return false;
    PythonJson::Value info = PythonJson::parse(*driveInfo);
    if (info.kind != PythonJson::Value::Kind::Dict)
        info = PythonJson::Value::dict();
    if (info.text(QStringLiteral("device_store_uuid")).isEmpty())
        info.insert(QStringLiteral("device_store_uuid"),
                    PythonJson::Value::from(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (info.text(QStringLiteral("device_name")).isEmpty()) {
        const QString name = !device.friendlyName.isEmpty() ? device.friendlyName
            : !device.modelName.isEmpty()                   ? device.modelName
                                                            : QStringLiteral("Unknown MTP device");
        info.insert(QStringLiteral("device_name"), PythonJson::Value::from(name));
    }
    info.insert(QStringLiteral("location_code"), PythonJson::Value::from(QStringLiteral("main")));
    info.insert(QStringLiteral("last_library_uuid"), PythonJson::Value::from(data.uuid));
    info.insert(QStringLiteral("date_last_connected"),
                PythonJson::Value::from(QDateTime::currentDateTime().toOffsetFromUtc(
                                            QDateTime::currentDateTime().offsetFromUtc()).toString(Qt::ISODateWithMs)));
    info.insert(QStringLiteral("mtp_prefix"), PythonJson::Value::from(QStringLiteral("mtp:::%1:::").arg(files.storageId())));
    if (!put(QStringLiteral("driveinfo.calibre"), PythonJson::dumps(info).toUtf8()))
        return false;

    // metadata.calibre's entries of the books still there, as books() keeps
    // them.
    const std::optional<QByteArray> metadataFile = readFrom(files, QStringLiteral("metadata.calibre"), error);
    if (!metadataFile)
        return false;
    QList<QByteArray> entries;
    for (const QByteArray &element : DeviceBooks::arrayElements(*metadataFile)) {
        if (onDevice.contains(PythonJson::parse(element).text(QStringLiteral("lpath")).toLower()))
            entries.append(element);
    }

    QJsonArray sent;
    QJsonArray unsendable;
    for (const int bookId : bookIds) {
        const Book *book = data.book(bookId);
        if (!book)
            continue;
        QStringList formats;
        for (const BookFormat &format : book->formats) {
            if (!format.file.isEmpty())
                formats.append(format.format);
        }
        const QString format = DeviceBooks::sendFormat(formats, formatMap);
        if (format.isEmpty()) {
            unsendable.append(book->title);
            continue;
        }
        QString source;
        for (const BookFormat &bookFormat : book->formats) {
            if (bookFormat.format.compare(format, Qt::CaseInsensitive) == 0)
                source = bookFormat.file;
        }
        // Only a MOBI is read whole, to write the metadata into it; other
        // files are copied as they are.
        QByteArray bytes;
        const bool mobi = isMobi(format);
        if (mobi) {
            QFile sourceFile(source);
            if (!sourceFile.open(QIODevice::ReadOnly)) {
                *error = QObject::tr("Cannot read %1.").arg(source);
                return false;
            }
            bytes = sourceFile.readAll();
        }
        QByteArray cover;
        if (!book->coverFile.isEmpty()) {
            QFile coverFile(book->coverFile);
            if (coverFile.open(QIODevice::ReadOnly))
                cover = coverFile.readAll();
        }
        if (mobi) {
            // The library's metadata in the copy, as Calibre's send writes it.
            MobiMetadata::Metadata metadata;
            metadata.title = book->title;
            metadata.authors = book->authors;
            metadata.authorSort = book->authorSort;
            metadata.publisher = book->publisher;
            metadata.comments = book->comments;
            for (const auto &[type, value] : book->identifiers) {
                if (type == QLatin1String("isbn"))
                    metadata.isbn = value;
            }
            metadata.tags = book->tags;
            metadata.pubdate = book->pubdate;
            metadata.uuid = book->uuid;
            metadata.languages = book->languages;
            metadata.cover = cover;
            // A book the writer refuses goes as it is, as Calibre's send
            // goes on when set_metadata() fails.
            QByteArray updated = bytes;
            QString mobiError;
            if (MobiMetadata::update(&updated, metadata, mobiOptions, &mobiError))
                bytes = std::move(updated);
        }

        // create_upload_path(), in the folder of a rule for the format.
        const QString extension = u'.' + format.toLower();
        NativeSend::UploadOptions options;
        options.saveTemplate = settings.sendTemplate;
        options.prefix = settings.rules.value(format.toLower(), prefix);
        options.maxLength = maxPathLength;
        options.useSubdirs = settings.sendTemplate.contains(u'/');
        const QString relative = NativeSend::uploadPath(data, *book, extension, options, interfaceLanguage);
        const std::optional<QString> placed = put(options.prefix + u'/' + relative, bytes, mobi ? QString() : source);
        if (!placed)
            return false;
        const QString lpath = *placed;

        // upload_kindle_thumbnail(): the cover where the Kindle looks, a copy
        // against Amazon's bug, and which file it is for.
        const QString thumbnailName = mobi ? MobiMetadata::thumbnailName(bytes) : QString();
        const QImage coverImage = QImage::fromData(cover);
        if (!thumbnailName.isEmpty() && !coverImage.isNull()) {
            const QByteArray jpeg = NativeSend::thumbnailJpeg(coverImage, thumbnailSize, thumbnailSize, 75);
            const QByteArray index
                = QCryptographicHash::hash(lpath.toLower().toUtf8(), QCryptographicHash::Sha1).toHex();
            if (!put(QStringLiteral("system/thumbnails/") + thumbnailName, jpeg)
                || !put(QStringLiteral("amazon-cover-bug/") + thumbnailName, jpeg)
                || !put(QStringLiteral("amazon-cover-bug/") + QString::fromLatin1(index), thumbnailName.toUtf8()))
                return false;
        }
        // upload_apnx(): the page numbers, in the book's .sdr folder.
        if (apnxWanted && mobi) {
            QString apnxError;
            const QString guid = QUuid::createUuid().toString(QUuid::Id128).left(8);
            const QByteArray apnx = MobiMetadata::apnx(bytes, guid, &apnxError);
            if (!apnx.isEmpty()) {
                const QString name = QFileInfo(lpath).completeBaseName();
                const QString parent = lpath.contains(u'/') ? lpath.left(lpath.lastIndexOf(u'/') + 1) : QString();
                if (!put(parent + name + QStringLiteral(".sdr/") + name + QStringLiteral(".apnx"), apnx))
                    return false;
            }
        }

        // The book's entry, by its path in lower case, in place of one there.
        const QString entryPath = lpath.toLower();
        const QByteArray text = NativeSend::entryText(NativeSend::bookEntry(
            data, *book, entryPath, mobi ? qint64(bytes.size()) : QFileInfo(source).size(),
            coverImage.isNull() ? PythonJson::Value::null() : NativeSend::thumbnail(*book, thumbnailSize, thumbnailSize)));
        bool replaced = false;
        for (QByteArray &element : entries) {
            if (PythonJson::parse(element).text(QStringLiteral("lpath")).toLower() == entryPath) {
                element = text;
                replaced = true;
            }
        }
        if (!replaced)
            entries.append(text);
        sent.append(QJsonObject{ { QStringLiteral("book"), bookId }, { QStringLiteral("lpath"), lpath } });
    }
    if (!sent.isEmpty() && !put(QStringLiteral("metadata.calibre"), DeviceBooks::joinElements(entries)))
        return false;
    const QJsonObject result{
        { QStringLiteral("sent"), sent },
        { QStringLiteral("unsendable"), unsendable },
        { QStringLiteral("operations"), operations },
    };
    return CalibreJob::writeFile(QDir(folder).filePath(QStringLiteral("result.json")), QJsonDocument(result).toJson(), error);
}

Outcome outcome(const SendJob::Result &result)
{
    Outcome outcome;
    outcome.ok = result.ok;
    outcome.error = result.error;
    outcome.sent = result.sent;
    outcome.unsendable = result.unsendable;
    for (const QJsonValue &value : result.json.value(QLatin1String("operations")).toArray()) {
        const QJsonObject operation = value.toObject();
        const QString kind = operation.value(QLatin1String("op")).toString();
        outcome.operations.append({ kind == QLatin1String("mkdir")    ? Operation::MakeFolder
                                    : kind == QLatin1String("delete") ? Operation::Delete
                                                                      : Operation::Put,
                                    operation.value(QLatin1String("path")).toString(),
                                    operation.value(QLatin1String("file")).toString() });
    }
    return outcome;
}

Outcome outcome(const QString &folder, int exitCode)
{
    return outcome(SendJob::result(folder, exitCode));
}

bool apply(const QString &folder, const Outcome &outcome, DeviceFiles &files, QString *error,
           const std::function<void(double)> &progress)
{
    // A file put again later, as metadata.calibre is, goes once.
    QList<bool> superseded(outcome.operations.size(), false);
    QSet<QString> later;
    qint64 total = 0;
    for (qsizetype i = outcome.operations.size() - 1; i >= 0; --i) {
        const Operation &operation = outcome.operations.at(i);
        if (operation.kind != Operation::Put)
            continue;
        superseded[i] = later.contains(operation.path.toLower());
        later.insert(operation.path.toLower());
        if (!superseded[i])
            total += QFileInfo(QDir(folder).filePath(operation.file)).size();
    }
    qint64 done = 0;
    for (qsizetype i = 0; i < outcome.operations.size(); ++i) {
        const Operation &operation = outcome.operations.at(i);
        switch (operation.kind) {
        case Operation::MakeFolder:
            if (!files.makeFolder(operation.path, error))
                return false;
            break;
        case Operation::Delete:
            if (!files.remove(operation.path, error))
                return false;
            break;
        case Operation::Put: {
            if (superseded.at(i))
                break;
            const QString local = QDir(folder).filePath(operation.file);
            const qint64 size = QFileInfo(local).size();
            const bool written = files.write(operation.path, local, error, [&](qint64 sent, qint64) {
                if (progress && total > 0)
                    progress(double(done + sent) / double(total));
            });
            if (!written)
                return false;
            done += size;
            if (progress && total > 0)
                progress(double(done) / double(total));
            break;
        }
        }
    }
    return true;
}

Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress)
{
    return outcome(SendJob::run(folder, interfaceLanguage, progress));
}

} // namespace KindleSend
