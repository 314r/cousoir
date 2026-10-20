#include "send_job.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QObject>
#include <QProcess>

#include "book.h"
#include "book_import.h"
#include "calibre_db.h"
#include "device_books.h"

namespace SendJob {

const char script[] = R"py(
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


def reporter(steps):
    # Calibre's reports of each of the device's `steps`, from 0 to 1 each.
    state = {'step': 0, 'last': 0.0}

    def report(fraction, message=''):
        if fraction < state['last']:
            state['step'] = min(state['step'] + 1, steps - 1)
        state['last'] = fraction
        progress_out.write(json.dumps({'progress': min(1.0, (state['step'] + fraction) / steps)}) + '\n')
        progress_out.flush()

    return report


def open_library():
    from calibre.library import db as open_db

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
    return db


def stand_in(db, dev):
    # Calibre's window as far as sending goes, its device manager's jobs run
    # there and then.
    import calibre.gui2.device as gdevice
    from calibre.gui2.device import DeviceManager, DeviceMixin
    from calibre.gui2.library.models import BooksModel

    class Job:
        def __init__(self, result):
            self.result, self.exception, self.failed = result, None, False

    class Manager:
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

        # Converting first is Cousoir's to offer, not the job's.
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
    return gui


def send_books(gui, book_ids):
    # sync_to_device(), then books_uploaded(): each book sent and where it
    # went.
    from calibre.devices.errors import FreeSpaceError

    try:
        gui.sync_to_device(None, False, send_ids=book_ids)
    except FreeSpaceError as e:
        raise Refused('space', str(e))
    sent = []
    for upload, metadata in gui.uploads:
        for (location, _), mi in zip(upload.result, metadata):
            sent.append((mi.id, location))
        gui.books_uploaded(upload)
    return sent


def main(send):
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

namespace {

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

QString logTail(const QString &folder)
{
    QFile file(QDir(folder).filePath(QStringLiteral("log.txt")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QStringList lines = QString::fromUtf8(file.readAll()).trimmed().split(u'\n');
    return lines.mid(qMax(0, int(lines.size()) - 3)).join(u'\n').trimmed();
}

} // namespace

QString program()
{
    return BookImport::calibreDebug();
}

bool prepareLibrary(const LibraryData &data, const QList<int> &bookIds, const QStringList &formatMap,
                    const QString &folder, QString *error)
{
    const QDir library(QDir(folder).filePath(QStringLiteral("library")));
    QDir().mkpath(library.path());
    FileIdentity source;
    QByteArray sha256;
    if (!CalibreDb::takeSnapshot(data.libraryPath, library.filePath(QStringLiteral("metadata.db")), &source, &sha256,
                                 error))
        return false;
    // Of each book, what Calibre reads: the file it sends and the cover.
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
        const QString format = DeviceBooks::sendFormat(formats, formatMap);
        if (format.isEmpty()) {
            if (error)
                *error = QObject::tr("“%1” has none of the formats the device reads.").arg(book.title);
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
    }
    return true;
}

bool writeJob(const QString &folder, QJsonObject job, const QList<int> &bookIds, const QByteArray &deviceScript,
              QString *error)
{
    QJsonArray ids;
    for (const int bookId : bookIds)
        ids.append(bookId);
    job.insert(QStringLiteral("books"), ids);
    const QDir dir(folder);
    return writeFile(dir.filePath(QStringLiteral("send.py")), QByteArray(script) + deviceScript, error)
        && writeFile(dir.filePath(QStringLiteral("job.json")), QJsonDocument(job).toJson(), error);
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

Result result(const QString &folder, int exitCode)
{
    Result outcome;
    QFile file(QDir(folder).filePath(QStringLiteral("result.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        const QString tail = logTail(folder);
        outcome.error = tail.isEmpty() ? QObject::tr("Calibre stopped before the books were ready.")
                                       : QObject::tr("Calibre stopped before the books were ready: %1").arg(tail);
        return outcome;
    }
    const QJsonObject read = QJsonDocument::fromJson(file.readAll()).object();
    const QString error = read.value(QLatin1String("error")).toString();
    const QString detail = read.value(QLatin1String("detail")).toString();
    if (!error.isEmpty() || exitCode != 0) {
        if (error == QLatin1String("space"))
            outcome.error = QObject::tr("There is not enough room on the device.");
        else
            outcome.error = QObject::tr("Calibre could not get the books ready: %1")
                                .arg(detail.isEmpty() ? logTail(folder) : detail);
        return outcome;
    }
    outcome.ok = true;
    for (const QJsonValue &value : read.value(QLatin1String("sent")).toArray()) {
        const QJsonObject sent = value.toObject();
        outcome.sent.append({ sent.value(QLatin1String("book")).toInt(), sent.value(QLatin1String("lpath")).toString() });
    }
    for (const QJsonValue &value : read.value(QLatin1String("unsendable")).toArray())
        outcome.unsendable.append(value.toString());
    outcome.json = read;
    return outcome;
}

Result run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress)
{
    Result outcome;
    const QString calibre = program();
    if (calibre.isEmpty()) {
        outcome.error = QObject::tr("Sending books needs Calibre, which is not installed.");
        return outcome;
    }
    QProcess process;
    process.setProcessEnvironment(environment(folder, interfaceLanguage));
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(calibre, arguments(folder));
    if (!process.waitForStarted()) {
        outcome.error = QObject::tr("Cannot run %1.").arg(calibre);
        return outcome;
    }
    const auto readLines = [&] {
        while (process.canReadLine()) {
            const double fraction = SendJob::progress(process.readLine());
            if (fraction >= 0 && progress)
                progress(fraction);
        }
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        readLines();
    }
    readLines();
    return result(folder, process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1);
}

} // namespace SendJob
