#include "kobo_send.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>

#include "book.h"
#include "book_import.h"
#include "calibre_db.h"

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
import json, os, sys, traceback, types

folder = sys.argv[-1]
sys.argv = sys.argv[:1]
progress_out = os.fdopen(os.dup(1), 'w')


def path(*names):
    return os.path.join(folder, *names)


class Refused(Exception):
    def __init__(self, kind, detail=''):
        Exception.__init__(self, detail)
        self.kind, self.detail = kind, detail


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
    import calibre.gui2.device as gdevice
    from calibre.devices.errors import FreeSpaceError
    from calibre.devices.kobo.driver import KOBOTOUCH
    from calibre.devices.usbms.driver import USBMS
    from calibre.gui2.device import DeviceManager, DeviceMixin
    from calibre.gui2.library.models import BooksModel
    from calibre.library import db as open_db

    root = job['device']
    staging = path('device')
    db = open_db(path('library'))

    class NoPageCounts:
        # Calibre counts pages in the background, which sending has nothing
        # to do with, and the library's copy has none of the files.
        def queue_scan(self, book_id=0):
            pass

        def shutdown(self):
            pass

        def wait_for_worker_shutdown(self, timeout=None):
            pass

    db.new_api.maintain_page_counts.shutdown()
    db.new_api.maintain_page_counts = NoPageCounts()

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


    state = {'phase': 0, 'last': 0.0}

    def report(fraction, message=''):
        # Making KEPUBs, then copying: each from 0 to 1.
        if fraction < state['last']:
            state['phase'] = 1
        state['last'] = fraction
        progress_out.write(json.dumps({'progress': min(1.0, (state['phase'] + fraction) / 2)}) + '\n')
        progress_out.flush()

    dev = Device(None)
    dev.startup()
    dev.reset(detected_device=(0x2237, job['product'], 0x0110, 'Kobo', 'eReader', ''), report_progress=report)
    dev._main_prefix = staging + os.sep
    dev.current_library_uuid = db.library_id

    class Job:
        def __init__(self, result):
            self.result, self.exception, self.failed = result, None, False

    class Manager:
        # DeviceManager's jobs, run there and then.
        def __init__(self, device):
            self.device = self.connected_device = device
            self.is_device_connected = True

        def upload_books(self, done, files, names, on_card=None, titles=None, metadata=None, plugboards=None,
                         add_as_step_to_job=None):
            return Job(DeviceManager._upload_books(self, files, names, on_card=on_card, metadata=metadata,
                                                   plugboards=plugboards))

        def add_books_to_metadata(self, locations, metadata, booklists):
            DeviceManager.add_books_to_metadata(self, locations, metadata, booklists)

        def sync_booklists(self, done, booklists, plugboards, add_as_step_to_job=None):
            if hasattr(self.connected_device, 'set_plugboards'):
                self.connected_device.set_plugboards(plugboards, gdevice.find_plugboard)
            DeviceManager._sync_booklists(self, booklists)

    class Model:
        def __init__(self, db):
            self.db = db

        get_preferred_formats_from_ids = BooksModel.get_preferred_formats_from_ids
        metadata_for = BooksModel.metadata_for

        def refresh_ondevice(self):
            pass

        def resort(self, reset=False):
            pass

        def research(self):
            pass

    class Gui(DeviceMixin):
        def __init__(self):
            self.current_db = db
            self.device_manager = Manager(dev)
            self.library_view = types.SimpleNamespace(
                _model=Model(db), preserve_state=lambda: __import__('contextlib').nullcontext(),
                currentIndex=lambda: types.SimpleNamespace(row=lambda: -1))
            self.memory_view = self.card_a_view = self.card_b_view = types.SimpleNamespace(_model=Model(db))
            self.status_bar = types.SimpleNamespace(show_message=lambda *a, **k: None)
            self.upload_memory = {}
            self.lists = None
            self.unsendable = []
            self.uploads = []

        def booklists(self):
            return self.lists

        # Converting first is Reliure's to offer, not the job's.
        def auto_convert_question(self, msg, autos):
            self.unsendable.extend(autos)
            return False

        def upload_books(self, files, names, metadata, on_card=None, memory=None):
            DeviceMixin.upload_books(self, files, names, metadata, on_card=on_card, memory=memory)
            self.uploads.append((list(self.upload_memory)[-1], metadata))

    gui = Gui()

    def warning(parent, title, msg, det_msg='', **kw):
        gui.unsendable.extend(t for t in det_msg.split('\n') if t)
        return types.SimpleNamespace(exec=lambda: None)

    gdevice.warning_dialog = warning
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

    try:
        gui.sync_to_device(None, False, send_ids=job['books'])
    except FreeSpaceError as e:
        raise Refused('space', str(e))
    sent = []
    for upload, metadata in gui.uploads:
        for (staged, _), mi in zip(upload.result, metadata):
            sent.append({'book': mi.id, 'lpath': os.path.relpath(staged, staging).replace(os.sep, '/')})
        gui.books_uploaded(upload)

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


with open(path('job.json'), encoding='utf-8') as f:
    job = json.load(f)
with open(path('log.txt'), 'w', encoding='utf-8') as log_file:
    sys.stdout = sys.stderr = log_file
    try:
        from calibre.gui2 import Application
        app = Application([], headless=True)
        result = send(job)
    except Refused as e:
        result = {'error': e.kind, 'detail': e.detail}
    except Exception as e:
        log_file.write(traceback.format_exc())
        result = {'error': 'failed', 'detail': str(e) or type(e).__name__}
    log_file.flush()
with open(path('result.json'), 'w', encoding='utf-8') as f:
    json.dump(result, f)
progress_out.flush()
# PyQt's exit handler can crash over the Qt objects left; the temporary
# files it would remove are in the job's folder.
os._exit(1 if 'error' in result else 0)
)py";

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool copyInto(const QString &source, const QString &target, QString *error)
{
    QDir().mkpath(QFileInfo(target).path());
    // QFile::copy() clones the file where the filesystem can.
    if (!QFile::copy(source, target)) {
        if (error)
            *error = QObject::tr("Cannot copy %1.").arg(QDir::toNativeSeparators(source));
        return false;
    }
    return true;
}

// What tells that a file of the Kobo changed: its size and time.
QJsonArray identity(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    return { info.size(), info.lastModified().toMSecsSinceEpoch() };
}

QString logTail(const QString &folder)
{
    QFile file(QDir(folder).filePath(QStringLiteral("log.txt")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QStringList lines = QString::fromUtf8(file.readAll()).trimmed().split(u'\n');
    return lines.mid(qMax(0, int(lines.size()) - 3)).join(u'\n').trimmed();
}

} // namespace

namespace KoboSend {

QString program()
{
    return BookImport::calibreDebug();
}

bool prepare(const LibraryData &data, const QList<int> &bookIds, const Kobo::Device &device,
             const QStringList &formatMap, const QString &folder, QString *error)
{
    const QDir dir(folder);
    const QDir library(dir.filePath(QStringLiteral("library")));
    QDir().mkpath(library.path());
    FileIdentity source;
    QByteArray sha256;
    if (!CalibreDb::takeSnapshot(data.libraryPath, library.filePath(QStringLiteral("metadata.db")), &source, &sha256,
                                 error))
        return false;
    // Of each book, what Calibre reads: the file it sends and the cover.
    QJsonArray ids;
    for (const int bookId : bookIds) {
        const auto found = std::find_if(data.books.cbegin(), data.books.cend(),
                                        [bookId](const Book &book) { return book.id == bookId; });
        if (found == data.books.cend()) {
            if (error)
                *error = QObject::tr("This book is no longer in the library.");
            return false;
        }
        const Book &book = *found;
        QStringList formats;
        for (const BookFormat &format : book.formats)
            formats.append(format.format);
        const QString format = Kobo::sendFormat(formats, formatMap);
        if (format.isEmpty()) {
            if (error)
                *error = QObject::tr("“%1” has none of the formats the Kobo reads.").arg(book.title);
            return false;
        }
        const QDir folderOfBook(library.filePath(book.path));
        for (const BookFormat &bookFormat : book.formats) {
            if (bookFormat.format != format)
                continue;
            if (bookFormat.file.isEmpty()) {
                if (error)
                    *error = QObject::tr("The %1 file of “%2” is missing.").arg(format, book.title);
                return false;
            }
            if (!copyInto(bookFormat.file, folderOfBook.filePath(bookFormat.name + u'.' + format.toLower()), error))
                return false;
        }
        if (!book.coverFile.isEmpty() && !copyInto(book.coverFile, folderOfBook.filePath(QStringLiteral("cover.jpg")), error))
            return false;
        ids.append(bookId);
    }

    const QDir root(device.root);
    const QDir staging(dir.filePath(QStringLiteral("device")));
    QJsonObject identities;
    for (const char *name : deviceFiles) {
        const QString file = root.filePath(QLatin1String(name));
        if (QFileInfo(file).isFile() && !copyInto(file, staging.filePath(QLatin1String(name)), error))
            return false;
        identities.insert(QLatin1String(name), identity(file));
    }
    QDir().mkpath(staging.filePath(QStringLiteral(".kobo")));
    const QJsonObject job{
        { QStringLiteral("device"), device.root },
        { QStringLiteral("product"), device.productId },
        { QStringLiteral("books"), ids },
        { QStringLiteral("identities"), identities },
    };
    return writeFile(dir.filePath(QStringLiteral("send.py")), sendScript, error)
        && writeFile(dir.filePath(QStringLiteral("job.json")), QJsonDocument(job).toJson(), error);
}

QStringList arguments(const QString &folder)
{
    return { QStringLiteral("-e"), QDir(folder).filePath(QStringLiteral("send.py")), QStringLiteral("--"), folder };
}

QProcessEnvironment environment(const QString &folder, const QString &interfaceLanguage)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString temporary = QDir(folder).filePath(QStringLiteral("tmp"));
    QDir().mkpath(temporary);
    environment.insert(QStringLiteral("TMPDIR"), temporary);
    if (!interfaceLanguage.isEmpty())
        environment.insert(QStringLiteral("CALIBRE_OVERRIDE_LANG"), interfaceLanguage);
    return environment;
}

double progress(const QByteArray &line)
{
    const QJsonObject object = QJsonDocument::fromJson(line.trimmed()).object();
    const QJsonValue value = object.value(QLatin1String("progress"));
    return value.isDouble() ? qBound(0.0, value.toDouble(), 1.0) : -1;
}

Outcome outcome(const QString &folder, int exitCode)
{
    Outcome result;
    QFile file(QDir(folder).filePath(QStringLiteral("result.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        const QString tail = logTail(folder);
        result.error = tail.isEmpty() ? QObject::tr("Calibre stopped before the books were ready.")
                                      : QObject::tr("Calibre stopped before the books were ready: %1").arg(tail);
        return result;
    }
    const QJsonObject read = QJsonDocument::fromJson(file.readAll()).object();
    const QString error = read.value(QLatin1String("error")).toString();
    const QString detail = read.value(QLatin1String("detail")).toString();
    if (!error.isEmpty() || exitCode != 0) {
        if (error == QLatin1String("space"))
            result.error = QObject::tr("There is not enough room on the device.");
        else
            result.error = QObject::tr("Calibre could not get the books ready: %1")
                               .arg(detail.isEmpty() ? logTail(folder) : detail);
        return result;
    }
    result.ok = true;
    for (const QJsonValue &value : read.value(QLatin1String("sent")).toArray()) {
        const QJsonObject sent = value.toObject();
        result.sent.append({ sent.value(QLatin1String("book")).toInt(), sent.value(QLatin1String("lpath")).toString() });
    }
    for (const QJsonValue &value : read.value(QLatin1String("unsendable")).toArray())
        result.unsendable.append(value.toString());
    for (const QJsonValue &value : read.value(QLatin1String("files")).toArray())
        result.files.append(value.toString());
    result.clearCovers = read.value(QLatin1String("clear_covers")).toBool(true);
    return result;
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
        for (const Sent &sent : outcome.sent)
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
    Outcome result;
    const QString calibre = program();
    if (calibre.isEmpty()) {
        result.error = QObject::tr("Sending books needs Calibre, which is not installed.");
        return result;
    }
    QProcess process;
    process.setProcessEnvironment(environment(folder, interfaceLanguage));
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(calibre, arguments(folder));
    if (!process.waitForStarted()) {
        result.error = QObject::tr("Cannot run %1.").arg(calibre);
        return result;
    }
    const auto readLines = [&] {
        while (process.canReadLine()) {
            const double fraction = KoboSend::progress(process.readLine());
            if (fraction >= 0 && progress)
                progress(fraction);
        }
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        readLines();
    }
    readLines();
    return outcome(folder, process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1);
}

} // namespace KoboSend
