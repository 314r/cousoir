#include "kobo_send.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QRegularExpression>
#include <QUuid>

#include <cmath>

#include "book.h"
#include "calibre_db.h"
#include "calibre_job.h"
#include "calibre_template.h"
#include "calibre_text.h"
#include "device_books.h"
#include "kepub.h"
#include "native_send.h"
#include "opf.h"
#include "python_json.h"
#include "send_job.h"

namespace {

// The files of the Kobo Calibre reads, beside the books: the job's device
// folder starts with their copies.
const char *const deviceFiles[] = { ".kobo/version",    ".kobo/KoboReader.sqlite", ".kobo/KoboReader.sqlite-wal",
                                    "metadata.calibre", "driveinfo.calibre",       "kobo_extra.css" };

// Calibre 9.16's "Send to device" for a Kobo, as its GUI runs it once the
// Kobo is connected: get_device_information(), KoboTouch's books() and
// set_books_in_library() of the connection, then sync_to_device(), the
// upload job of DeviceManager with KoboTouch's upload_books(), and
// books_uploaded(), which adds the books to metadata.calibre. The GUI's
// parts are its own methods, called on a stand-in for the window; the job's
// device folder stands for the Kobo, books() aside, which reads the Kobo's
// books there and its database from the copy in that folder.
const char sendScript[] = R"py(

def files_in(top):
    # Each file under `top`, with what tells a changed one.
    found = {}
    for dirpath, dirnames, filenames in os.walk(top):
        for name in filenames:
            full = os.path.join(dirpath, name)
            st = os.stat(full)
            found[os.path.relpath(full, top)] = (st.st_size, st.st_mtime_ns)
    return found


def send(job):
    from calibre.devices.kobo.driver import KOBOTOUCH
    from calibre.devices.usbms.driver import USBMS
    from calibre.gui2.device import DeviceManager

    root = job['device']
    staging = path('device')
    db = open_library()

    def on_device(rel):
        # `rel` as it is on the Kobo: in the case of the folders and file
        # already there, which put_file() opens on its FAT filesystem.
        actual = root
        for part in rel.split(os.sep):
            name = part
            try:
                for entry in os.listdir(actual):
                    if entry.lower() == part.lower():
                        name = entry
                        break
            except OSError:
                pass
            actual = os.path.join(actual, name)
        return os.path.relpath(actual, root)

    class Device(KOBOTOUCH):
        # KoboTouch on the job's device folder, but for what depends on the
        # Kobo itself; what it changes in the Kobo's database stays in the
        # copy.

        def create_upload_path(self, path, mdata, fname, create_dirs=True):
            # Long names are shortened to fit the Kobo's path.
            where = os.path.join(root, os.path.relpath(path, staging))
            real = KOBOTOUCH.create_upload_path(self, where, mdata, fname, create_dirs=False)
            staged = os.path.join(staging, on_device(os.path.relpath(real, root)))
            if create_dirs:
                os.makedirs(os.path.dirname(staged), exist_ok=True)
            return staged

        def free_space(self, end_session=True):
            stats = os.statvfs(root)
            return stats.f_frsize * stats.f_bavail, 0, 0

        def _update_driveinfo_record(self, dinfo, prefix, location_code, name=None):
            return KOBOTOUCH._update_driveinfo_record(self, dinfo, root + os.sep, location_code, name)

    dev = Device(None)
    dev.startup()
    # Making KEPUBs, then copying.
    dev.reset(detected_device=(0x2237, job['product'], 0x0110, 'Kobo', 'eReader', ''), report_progress=reporter(2))
    dev._main_prefix = staging + os.sep
    dev.current_library_uuid = db.library_id
    gui = stand_in(db, dev)
    before = files_in(staging)

    # The connection: the device's information, then the books on the Kobo
    # as books() reads them, from the Kobo and its database, but without
    # writing metadata.calibre there, matched with the library.
    DeviceManager._get_device_information(gui.device_manager)
    gui.set_default_thumbnail(dev.THUMBNAIL_HEIGHT)
    copy = os.path.join(staging, '.kobo', 'KoboReader.sqlite')
    if os.path.exists(copy):
        from calibre.devices.kobo.db import Database
        dev.db_manager = Database(copy)
        dev.dbversion = dev.db_manager.dbversion or dev.dbversion
    dev._main_prefix = root + os.sep
    write_metadata = USBMS.sync_booklists
    USBMS.sync_booklists = lambda self, booklists, end_session=True: None
    try:
        gui.lists = list(DeviceManager._books(gui.device_manager)) if os.path.exists(copy) else [
            dev.booklist_class(None, dev._main_prefix, dev.settings),
            dev.booklist_class('carda', None, dev.settings), dev.booklist_class('cardb', None, dev.settings)]
    finally:
        USBMS.sync_booklists = write_metadata
        dev._main_prefix = staging + os.sep
    gui.set_books_in_library(gui.lists, reset=True)

    sent = [{'book': book_id, 'lpath': os.path.relpath(staged, staging).replace(os.sep, '/')}
            for book_id, staged in send_books(gui, job['books'])]

    after = files_in(staging)
    written = sorted(rel for rel, stat in after.items()
                     if before.get(rel) != stat and not rel.startswith('.kobo' + os.sep))
    last = ['driveinfo.calibre', 'metadata.calibre']
    files = [rel for rel in written if rel not in last] + [rel for rel in last if rel in written]
    return {
        'sent': sent,
        'unsendable': gui.unsendable,
        'files': [rel.replace(os.sep, '/') for rel in files],
        'clear_covers': not dev.upload_covers,
    }


main(send)
)py";

// What tells that a file of the Kobo changed: its size and time.
QJsonArray identity(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    return { info.size(), info.lastModified().toMSecsSinceEpoch() };
}

} // namespace

namespace KoboSend {

DriverSettings driverSettings(const QString &configDirectory)
{
    DriverSettings settings;
    QFile file(QDir(configDirectory).filePath(QStringLiteral("device_drivers_KOBOTOUCH.py.json")));
    if (!file.open(QIODevice::ReadOnly))
        return settings;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    if (json.value(QLatin1String("save_template")).isString())
        settings.saveTemplate = json.value(QLatin1String("save_template")).toString();
    if (json.value(QLatin1String("kepubify")).isBool())
        settings.kepubify = json.value(QLatin1String("kepubify")).toBool();
    if (json.value(QLatin1String("use_subdirs")).isBool())
        settings.useSubdirs = json.value(QLatin1String("use_subdirs")).toBool();
    return settings;
}

QString uploadPath(const LibraryData &data, const Book &book, const QString &root, const QString &extension, bool kepub,
                   const DriverSettings &settings, const QString &interfaceLanguage)
{
    NativeSend::UploadOptions options;
    options.saveTemplate = settings.saveTemplate;
    // USBMS puts the folder the Kobo is mounted on before the path.
    options.prefix = QDir::cleanPath(root);
    // MAX_PATH_LEN of KoboTouch.
    options.maxLength = 185;
    options.useSubdirs = settings.useSubdirs;
    // filename_callback(): a KEPUB's ".kepub.epub".
    options.nameSuffix = kepub ? QStringLiteral(".kepub") : QString();
    // KoboTouch's sanitize_path_components().
    options.sanitizeComponents = [](QStringList *components) {
        static const QRegularExpression invalid(QStringLiteral("[\\/\\\\\\?%\\*:;\\|\"'><\\$!]"));
        for (QString &component : *components)
            component.replace(invalid, QStringLiteral("_"));
    };
    return NativeSend::uploadPath(data, book, extension, options, interfaceLanguage);
}

bool prepareNatively(const LibraryData &data, const QList<int> &bookIds, const Kobo::Device &device,
                     const QStringList &formatMap, const QString &configDirectory, const QString &interfaceLanguage,
                     const QString &folder, QString *error)
{
    const DriverSettings settings = driverSettings(configDirectory);
    const QDir root(device.root);
    const QDir staging(QDir(folder).filePath(QStringLiteral("device")));
    QDir().mkpath(staging.path());
    // What tells, at apply(), that the Kobo changed meanwhile.
    QJsonObject identities;
    for (const char *name : deviceFiles)
        identities.insert(QLatin1String(name), identity(root.filePath(QLatin1String(name))));
    QFile metadataFile(root.filePath(QStringLiteral("metadata.calibre")));
    QList<QByteArray> entries = metadataFile.open(QIODevice::ReadOnly) ? DeviceBooks::arrayElements(metadataFile.readAll())
                                                                       : QList<QByteArray>();
    // KoboTouch's books(), which a send reads first, gives the books the Kobo
    // took in their type from its database, and no thumbnail.
    QHash<QString, QString> mimes;
    QString listError;
    for (const DeviceBook &onDevice : Kobo::books(device, &listError)) {
        if (!onDevice.mime.isEmpty())
            mimes.insert(onDevice.lpath, onDevice.mime);
    }
    for (QByteArray &element : entries) {
        PythonJson::Value entry = PythonJson::parse(element);
        const auto mime = mimes.constFind(entry.text(QStringLiteral("lpath")));
        if (entry.kind != PythonJson::Value::Kind::Dict || mime == mimes.cend())
            continue;
        entry.insert(QStringLiteral("mime"), PythonJson::Value::from(*mime));
        entry.insert(QStringLiteral("thumbnail"), PythonJson::Value::null());
        PythonJson::Format indented;
        indented.indent = 2;
        indented.ensureAscii = true;
        element = PythonJson::dumps(entry, indented).toUtf8().replace("\n", "\n  ");
    }
    QJsonArray sent;
    QJsonArray unsendable;
    QStringList files;
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
        const bool epub = format.compare(QLatin1String("EPUB"), Qt::CaseInsensitive) == 0;
        const bool kepub = format.compare(QLatin1String("KEPUB"), Qt::CaseInsensitive) == 0 || (epub && settings.kepubify);
        const QString extension = epub || kepub ? QStringLiteral(".epub") : u'.' + format.toLower();
        const QString lpath = uploadPath(data, *book, device.root, extension, kepub, settings, interfaceLanguage);
        const QString target = staging.filePath(lpath);
        QDir().mkpath(QFileInfo(target).path());
        if (epub && settings.kepubify) {
            // The library's metadata and cover in the KEPUB, as Calibre's
            // send puts them in what it sends.
            Kepub::Options options;
            options.language = book->languages.value(0, QStringLiteral("en"));
            options.title = book->title;
            options.authors = book->authors.join(QStringLiteral(" & "));
            QFile cover(book->coverFile);
            if (!book->coverFile.isEmpty() && cover.open(QIODevice::ReadOnly))
                options.cover = cover.readAll();
            Kepub::Metadata metadata;
            metadata.title = book->title;
            metadata.authors = book->authors;
            metadata.authorSort = book->authorSort;
            metadata.publisher = book->publisher;
            metadata.languages = book->languages;
            metadata.tags = book->tags;
            metadata.series = book->series;
            metadata.seriesIndex = book->seriesIndex;
            metadata.comments = book->comments;
            metadata.uuid = book->uuid;
            metadata.identifiers = book->identifiers;
            options.metadata = metadata;
            if (!Kepub::kepubify(source, target, options, error))
                return false;
        } else if (!SendJob::copyInto(source, target, error)) {
            return false;
        }
        // The book's entry, in place of one at the same place.
        const PythonJson::Value entry = NativeSend::bookEntry(data, *book, lpath, QFileInfo(target).size(), NativeSend::thumbnail(*book, 0, 68));
        PythonJson::Format format2;
        format2.indent = 2;
        format2.ensureAscii = true;
        QByteArray text = PythonJson::dumps(entry, format2).toUtf8();
        text.replace("\n", "\n  ");
        entries.removeIf([&lpath](const QByteArray &element) {
            return PythonJson::parse(element).text(QStringLiteral("lpath")) == lpath;
        });
        entries.append(text);
        sent.append(QJsonObject{ { QStringLiteral("book"), bookId }, { QStringLiteral("lpath"), lpath } });
        files.append(lpath);
    }
    if (!sent.isEmpty()) {
        // driveinfo.calibre, as Calibre leaves it after a send.
        QFile infoFile(root.filePath(QStringLiteral("driveinfo.calibre")));
        PythonJson::Value info = infoFile.open(QIODevice::ReadOnly) ? PythonJson::parse(infoFile.readAll()) : PythonJson::Value::dict();
        if (info.kind != PythonJson::Value::Kind::Dict)
            info = PythonJson::Value::dict();
        if (info.text(QStringLiteral("device_store_uuid")).isEmpty())
            info.insert(QStringLiteral("device_store_uuid"),
                        PythonJson::Value::from(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (info.text(QStringLiteral("device_name")).isEmpty())
            info.insert(QStringLiteral("device_name"), PythonJson::Value::from(QStringLiteral("Kobo eReader")));
        info.insert(QStringLiteral("location_code"), PythonJson::Value::from(QStringLiteral("main")));
        info.insert(QStringLiteral("last_library_uuid"), PythonJson::Value::from(data.uuid));
        info.insert(QStringLiteral("date_last_connected"),
                    PythonJson::Value::from(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
        info.insert(QStringLiteral("prefix"), PythonJson::Value::from(QDir::cleanPath(device.root) + u'/'));
        if (!CalibreJob::writeFile(staging.filePath(QStringLiteral("driveinfo.calibre")),
                                PythonJson::dumps(info).toUtf8(), error)
            || !CalibreJob::writeFile(staging.filePath(QStringLiteral("metadata.calibre")),
                                   DeviceBooks::joinElements(entries), error))
            return false;
        files << QStringLiteral("driveinfo.calibre") << QStringLiteral("metadata.calibre");
    }
    const QJsonObject job{
        { QStringLiteral("device"), device.root },
        { QStringLiteral("identities"), identities },
    };
    const QJsonObject result{
        { QStringLiteral("sent"), sent },
        { QStringLiteral("unsendable"), unsendable },
        { QStringLiteral("files"), QJsonArray::fromStringList(files) },
        { QStringLiteral("clear_covers"), true },
    };
    return CalibreJob::writeFile(QDir(folder).filePath(QStringLiteral("job.json")), QJsonDocument(job).toJson(), error)
        && CalibreJob::writeFile(QDir(folder).filePath(QStringLiteral("result.json")), QJsonDocument(result).toJson(), error);
}

bool prepare(const LibraryData &data, const QList<int> &bookIds, const Kobo::Device &device,
             const QStringList &formatMap, const QString &folder, QString *error)
{
    if (!SendJob::prepareLibrary(data, bookIds, formatMap, folder, error))
        return false;
    const QDir root(device.root);
    const QDir staging(QDir(folder).filePath(QStringLiteral("device")));
    QJsonObject identities;
    for (const char *name : deviceFiles) {
        const QString file = root.filePath(QLatin1String(name));
        if (QFileInfo(file).isFile() && !SendJob::copyInto(file, staging.filePath(QLatin1String(name)), error))
            return false;
        identities.insert(QLatin1String(name), identity(file));
    }
    QDir().mkpath(staging.filePath(QStringLiteral(".kobo")));
    const QJsonObject job{
        { QStringLiteral("device"), device.root },
        { QStringLiteral("product"), device.productId },
        { QStringLiteral("identities"), identities },
    };
    return SendJob::writeJob(folder, job, bookIds, sendScript, error);
}

Outcome outcome(const SendJob::Result &result)
{
    Outcome outcome;
    outcome.ok = result.ok;
    outcome.error = result.error;
    outcome.sent = result.sent;
    outcome.unsendable = result.unsendable;
    for (const QJsonValue &value : result.json.value(QLatin1String("files")).toArray())
        outcome.files.append(value.toString());
    outcome.clearCovers = result.json.value(QLatin1String("clear_covers")).toBool(true);
    return outcome;
}

Outcome outcome(const QString &folder, int exitCode)
{
    return outcome(SendJob::result(folder, exitCode));
}

bool apply(const QString &folder, const Outcome &outcome, const Kobo::Device &device, QString *error,
           const std::function<void(double)> &progress)
{
    if (!Kobo::isKobo(device.root)) {
        if (error)
            *error = QObject::tr("The device is no longer connected.");
        return false;
    }
    QFile jobFile(QDir(folder).filePath(QStringLiteral("job.json")));
    const QJsonObject identities = jobFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(jobFile.readAll()).object().value(QLatin1String("identities")).toObject()
        : QJsonObject();
    const QDir root(device.root);
    for (auto it = identities.begin(); it != identities.end(); ++it) {
        if (identity(root.filePath(it.key())) != it.value().toArray()) {
            if (error)
                *error = QObject::tr("The device's books changed while these were being made ready; send them again.");
            return false;
        }
    }
    const QDir staging(QDir(folder).filePath(QStringLiteral("device")));
    qint64 total = 0;
    for (const QString &file : outcome.files)
        total += QFileInfo(staging.filePath(file)).size();
    qint64 done = 0;
    const auto copy = [&](const QString &file) {
        if (!Kobo::copyFile(staging.filePath(file), root.filePath(file), error))
            return false;
        done += QFileInfo(staging.filePath(file)).size();
        if (progress && total > 0)
            progress(double(done) / double(total));
        return true;
    };
    // The books, then what tells of them.
    const QStringList last{ QStringLiteral("driveinfo.calibre"), QStringLiteral("metadata.calibre") };
    for (const QString &file : outcome.files) {
        if (!last.contains(file) && !copy(file))
            return false;
    }
    if (outcome.clearCovers) {
        for (const SendJob::Sent &sent : outcome.sent)
            Kobo::clearCoverCache(device, sent.lpath);
    }
    for (const QString &file : last) {
        if (outcome.files.contains(file) && !copy(file))
            return false;
    }
    return true;
}

Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress)
{
    return outcome(SendJob::run(folder, interfaceLanguage, progress));
}

} // namespace KoboSend
