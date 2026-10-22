// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/gui2/convert/single.py: setup_pipeline(), in the Python the
//   job runs
// - src/calibre/gui2/tools.py: convert_single_ebook()
// - src/calibre/ebooks/conversion/config.py: load_defaults()
// - src/calibre/db/legacy.py: original_fmt()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "conversion.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include "book_import.h"
#include "calibre_job.h"
#include "commit_engine.h"
#include "opf.h"

namespace {

// Calibre's name for itself and its version in the OPF a conversion reads,
// which only Calibre knows: the job script puts them in place of this.
const char producerMark[] = "@CALIBRE_PRODUCER@";

// The conversion as Calibre 9.16's convert action runs it for one book:
// convert_single_ebook() of gui2/tools.py with auto_conversion, the dialog
// accepted as it opens, then the job, gui_convert_override(), and the checks
// of book_converted() in gui2/actions/convert.py. The dialog's pages are
// built as Config.setup_pipeline() builds them, without a window, so the
// settings are the ones the dialog would save; the library they ask about
// is the book's formats and saved settings, from job.json.
const char convertScript[] = R"(
import base64, gc, json, os, shutil, subprocess, sys, traceback

folder = sys.argv[-1]
# Calibre's worker runs a job without arguments; the PDF reflow would take
# one for the book's title.
sys.argv = sys.argv[:1]


def path(name):
    return os.path.join(folder, name)


# gui_convert_override() with the settings, its log after the job's. The
# worker runs it with exec() in a function: names go in a function of its
# own, where the inner one finds them.
WORKER = '''
def run(folder):
    import json, sys, traceback
    from calibre.gui2.convert.gui_conversion import gui_convert_override
    from calibre.utils.logging import FileStream, Log

    # The job manager's worker has no arguments, as the PDF reflow notices.
    sys.argv = sys.argv[:1]

    with open(folder + '/recommendations.json', encoding='utf-8') as f:
        in_file, out_file, recommendations = json.load(f)
    with open(folder + '/log.txt', 'a', encoding='utf-8') as log_file:
        log = Log()
        log.outputs = [FileStream(log_file)]

        def notification(fraction, message=''):
            print(json.dumps({'progress': fraction}), flush=True)

        try:
            gui_convert_override(in_file, out_file, [tuple(r) for r in recommendations],
                                 notification=notification, log=log)
        except Exception as e:
            from calibre.ebooks import DRMError
            log_file.write(traceback.format_exc())
            kind = 'drm' if isinstance(e, DRMError) else 'failed'
            print(json.dumps({'error': kind, 'detail': str(e) or type(e).__name__}), flush=True)
            sys.exit(1)


run(%(folder)r)
'''


class Refused(Exception):
    def __init__(self, kind, detail=''):
        Exception.__init__(self, detail)
        self.kind, self.detail = kind, detail


class Library:
    # The book as the dialog asks the library about it.
    def __init__(self, files, options):
        self.files, self.options = files, options
        self.new_api = self

    def formats(self, book_id, verify_formats=True):
        return tuple(self.files)

    def has_format(self, book_id, fmt, index_is_id=False):
        return fmt.upper() in self.files

    def conversion_options(self, book_id, fmt='PIPE'):
        return self.options if fmt.upper() == 'PIPE' else None

    def original_fmt(self, book_id, fmt):
        nfmt = f'ORIGINAL_{fmt}'.upper()
        return nfmt if nfmt in self.files else fmt


def settings(library, book_id, input_format, output_format):
    # The settings of the dialog accepted as it opens: its pages made as
    # setup_pipeline() makes them, then read as accept() reads them.
    import calibre.gui2.convert as pages
    import calibre.gui2.convert.debug
    import calibre.gui2.convert.search_and_replace
    import calibre.gui2.convert.toc
    from calibre.ebooks.conversion.config import OPTIONS, GuiRecommendations, name_to_path
    from calibre.ebooks.conversion.plumber import create_dummy_plumber
    from calibre.gui2.convert.debug import DebugWidget
    from calibre.gui2.convert.heuristics import HeuristicsWidget
    from calibre.gui2.convert.look_and_feel import LookAndFeelWidget
    from calibre.gui2.convert.metadata import MetadataWidget
    from calibre.gui2.convert.page_setup import PageSetupWidget
    from calibre.gui2.convert.search_and_replace import SearchAndReplaceWidget
    from calibre.gui2.convert.structure_detection import StructureDetectionWidget
    from calibre.gui2.convert.toc import TOCWidget

    # A page that would ask something refuses instead: nobody is there to
    # answer.
    def refuse(parent, title, msg='', *args, **kwargs):
        raise Refused('settings', f'{title}: {msg}' if msg else title)

    for module in (calibre.gui2.convert.debug, calibre.gui2.convert.search_and_replace, calibre.gui2.convert.toc):
        for name in ('error_dialog', 'question_dialog', 'warning_dialog'):
            if hasattr(module, name):
                setattr(module, name, refuse)

    # Calibre's defaults, read without creating the file of a page that has
    # none, which Calibre's own reading does.
    def load_defaults(name):
        recs = GuiRecommendations()
        try:
            with open(name_to_path(name), 'rb') as f:
                raw = f.read()
        except FileNotFoundError:
            raw = b''
        if raw:
            recs.deserialize(raw)
        return recs

    pages.load_defaults = load_defaults

    plumber = create_dummy_plumber(input_format, output_format)
    get_option, get_help = plumber.get_option_by_name, plumber.get_option_help

    def page(cls):
        return cls(None, get_option, get_help, library, book_id)

    # The metadata page shows the library's metadata, which the OPF of the
    # job has, and writes back what was changed in it, which is nothing:
    # only its form and its one setting are made here.
    metadata = MetadataWidget.__new__(MetadataWidget)
    pages.Widget.__init__(metadata, None, OPTIONS['pipe']['metadata'])
    metadata.initialize_options(get_option, get_help, library, book_id)
    widgets = [page(LookAndFeelWidget), page(HeuristicsWidget), page(PageSetupWidget),
               page(StructureDetectionWidget), page(TOCWidget), page(SearchAndReplaceWidget)]
    for widget in (plumber.input_plugin.gui_configuration_widget(None, get_option, get_help, library, book_id),
                   plumber.output_plugin.gui_configuration_widget(None, get_option, get_help, library, book_id)):
        if widget is not None:
            widgets.append(widget)
    widgets.append(page(DebugWidget))
    if plumber.archive_input_tdir:
        shutil.rmtree(plumber.archive_input_tdir, ignore_errors=True)

    try:
        recs = metadata.commit_options(False)
        for widget in widgets:
            if not widget.pre_commit_check():
                raise Refused('settings', widget.config_title())
            recs.update(widget.commit(save_defaults=False))
        recs['gui_preferred_input_format'] = input_format
        return recs
    finally:
        for widget in [metadata] + widgets:
            widget.break_cycles()


def convert(job, log_file):
    from calibre.constants import __appname__, __version__
    from calibre.customize.conversion import OptionRecommendation
    from calibre.ebooks.conversion.config import NoSupportedInputFormats, get_input_format_for_book
    from calibre.utils import pickle_binary_string, unpickle_binary_string
    from calibre.utils.config import tweaks
    from calibre.utils.ipc.simple_worker import start_pipe_worker

    options = None
    if job.get('options'):
        try:
            options = unpickle_binary_string(base64.b64decode(job['options']))
        except Exception:
            pass
    library = Library(job['formats'], options)
    book_id = job['book']
    output_format = job['format'].lower()
    try:
        input_format = get_input_format_for_book(library, book_id, None)[0]
    except NoSupportedInputFormats:
        raise Refused('input')
    recs = settings(library, book_id, input_format, output_format)
    gc.collect()
    # What save_specifics() stores.
    saved = pickle_binary_string(recs.serialize())
    recommendations = [(key, value, OptionRecommendation.HIGH) for key, value in recs.items()]

    source_format = library.original_fmt(book_id, input_format)
    source = job['formats'][source_format.upper()]
    if not os.path.exists(source):
        raise Refused('missing', source_format.upper())
    in_file = path('input.' + input_format)
    shutil.copyfile(source, in_file)
    opf = path('book.opf')
    with open(opf, 'rb') as f:
        raw = f.read()
    producer = __appname__ + f' ({__version__}) ' + '[https://calibre-ebook.com]'
    with open(opf, 'wb') as f:
        f.write(raw.replace(b'@CALIBRE_PRODUCER@', producer.encode('utf-8')))
    recommendations.append(('read_metadata_from_opf', opf, OptionRecommendation.HIGH))
    if job.get('cover'):
        recommendations.append(('cover', path(job['cover']), OptionRecommendation.HIGH))
    out_file = path('output.' + output_format)
    with open(path('recommendations.json'), 'w', encoding='utf-8') as f:
        json.dump([in_file, out_file, recommendations], f)

    # The job, in a worker of its own as Calibre's job manager runs it: a
    # process without the dialog's QApplication, which encodes images its own
    # way. Its progress comes through.
    log_file.flush()
    worker = start_pipe_worker(WORKER % {'folder': folder}, stdin=subprocess.DEVNULL)
    failure = None
    for line in worker.stdout:
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if not isinstance(message, dict):
            continue
        if 'progress' in message:
            sys.stdout.write(json.dumps(message) + '\n')
            sys.stdout.flush()
        elif 'error' in message:
            failure = message
    if worker.wait() != 0 or failure is not None:
        if failure is None:
            raise Refused('failed', f'the worker ended with {worker.returncode}')
        raise Refused(failure['error'], failure['detail'])
    if not os.path.exists(out_file) or os.stat(out_file).st_size < 1:
        raise Refused('empty')
    return {
        'input': input_format,
        'file': os.path.basename(out_file),
        'save_original': source_format.lower() == output_format and bool(tweaks['save_original_format']),
        'options': base64.b64encode(saved).decode('ascii'),
    }


with open(path('job.json'), encoding='utf-8') as f:
    job = json.load(f)
with open(path('log.txt'), 'w', encoding='utf-8') as log_file:
    try:
        from calibre.gui2 import Application
        app = Application([], headless=True)
        result = convert(job, log_file)
    except Refused as e:
        result = {'error': e.kind, 'detail': e.detail}
    except Exception as e:
        log_file.write(traceback.format_exc())
        result = {'error': 'failed', 'detail': str(e) or type(e).__name__}
with open(path('result.json'), 'w', encoding='utf-8') as f:
    json.dump(result, f)
sys.stdout.flush()
# Python's end, PyQt's exit handler, can crash over the Qt objects the
# dialog's pages leave: the process ends without it. The temporary files it
# would remove are in the job's folder.
os._exit(1 if 'error' in result else 0)
)";

} // namespace

namespace Conversion {

QStringList outputFormats()
{
    return { QStringLiteral("KEPUB"), QStringLiteral("AZW3") };
}

QStringList inputFormats(const Book &book)
{
    // supported_input_formats() of ebooks/conversion/plumber.py in Calibre
    // 9.16: its input plugins' file types and the archives it opens.
    static const QStringList supported{
        QStringLiteral("AZW"),   QStringLiteral("AZW3"),     QStringLiteral("AZW4"),   QStringLiteral("CB7"),
        QStringLiteral("CBC"),   QStringLiteral("CBR"),      QStringLiteral("CBZ"),    QStringLiteral("CHM"),
        QStringLiteral("DJV"),   QStringLiteral("DJVU"),     QStringLiteral("DOCM"),   QStringLiteral("DOCX"),
        QStringLiteral("DOWNLOADED_RECIPE"),                 QStringLiteral("EPUB"),   QStringLiteral("FB2"),
        QStringLiteral("FBZ"),   QStringLiteral("HTM"),      QStringLiteral("HTML"),   QStringLiteral("HTMLZ"),
        QStringLiteral("KEPUB"), QStringLiteral("LIT"),      QStringLiteral("LRF"),    QStringLiteral("MARKDOWN"),
        QStringLiteral("MD"),    QStringLiteral("MOBI"),     QStringLiteral("ODT"),    QStringLiteral("OEBZIP"),
        QStringLiteral("OPF"),   QStringLiteral("PDB"),      QStringLiteral("PDF"),    QStringLiteral("PML"),
        QStringLiteral("PMLZ"),  QStringLiteral("POBI"),     QStringLiteral("PRC"),    QStringLiteral("RAR"),
        QStringLiteral("RB"),    QStringLiteral("RECIPE"),   QStringLiteral("RTF"),    QStringLiteral("SHTM"),
        QStringLiteral("SHTML"), QStringLiteral("SNB"),      QStringLiteral("TCR"),    QStringLiteral("TEXT"),
        QStringLiteral("TEXTILE"), QStringLiteral("TXT"),    QStringLiteral("TXTZ"),   QStringLiteral("UPDB"),
        QStringLiteral("XHTM"),  QStringLiteral("XHTML"),    QStringLiteral("ZIP"),
    };
    QStringList formats;
    for (const BookFormat &format : book.formats) {
        if (supported.contains(format.format))
            formats.append(format.format);
    }
    return formats;
}

QString program()
{
    return BookImport::calibreDebug();
}

bool prepare(const LibraryData &data, const Book &book, const QString &format, const QString &interfaceLanguage,
             const QString &folder, QString *error)
{
    const QDir dir(folder);
    QJsonObject files;
    for (const BookFormat &bookFormat : book.formats) {
        // A missing file is still one of the book's formats for Calibre,
        // which finds it missing when it converts from it.
        const QString file = bookFormat.file.isEmpty()
            ? QDir(QDir(data.libraryPath).filePath(book.path))
                  .filePath(bookFormat.name + u'.' + bookFormat.format.toLower())
            : bookFormat.file;
        files.insert(bookFormat.format, file);
    }
    QJsonObject job{
        { QStringLiteral("book"), book.id },
        { QStringLiteral("format"), format.toLower() },
        { QStringLiteral("formats"), files },
    };
    if (!book.conversionOptions.isEmpty())
        job.insert(QStringLiteral("options"), QString::fromLatin1(book.conversionOptions.toBase64()));

    // create_opf_file(): the book's metadata.opf, but for its uuid in the
    // first identifier and no cover.
    Opf::Settings settings;
    settings.interfaceLanguage = interfaceLanguage;
    settings.producer = QLatin1String(producerMark);
    settings.producerId = QStringLiteral("calibre");
    settings.applicationId = book.uuid;
    settings.coverReference = false;
    QFile previous(QDir(QDir(data.libraryPath).filePath(book.path)).filePath(QStringLiteral("metadata.opf")));
    const QByteArray kept = previous.open(QIODevice::ReadOnly) ? previous.readAll() : QByteArray();
    if (!CalibreJob::writeFile(dir.filePath(QStringLiteral("book.opf")), Opf::metadataToOpf(data, book, settings, kept), error))
        return false;
    if (!book.coverFile.isEmpty()) {
        QFile cover(book.coverFile);
        if (!cover.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QObject::tr("Cannot read the cover of “%1”.").arg(book.title);
            return false;
        }
        if (!CalibreJob::writeFile(dir.filePath(QStringLiteral("cover.jpeg")), cover.readAll(), error))
            return false;
        job.insert(QStringLiteral("cover"), QStringLiteral("cover.jpeg"));
    }
    return CalibreJob::writeFile(dir.filePath(QStringLiteral("convert.py")), convertScript, error)
        && CalibreJob::writeFile(dir.filePath(QStringLiteral("job.json")), QJsonDocument(job).toJson(), error);
}

QStringList arguments(const QString &folder)
{
    return { QStringLiteral("-e"), QDir(folder).filePath(QStringLiteral("convert.py")), QStringLiteral("--"), folder };
}

Outcome outcome(const QString &folder, int exitCode)
{
    Outcome result;
    QFile file(QDir(folder).filePath(QStringLiteral("result.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        const QString tail = CalibreJob::logTail(folder);
        result.error = tail.isEmpty() ? QObject::tr("Calibre stopped before the end of the conversion.")
                                      : QObject::tr("Calibre stopped before the end of the conversion: %1").arg(tail);
        return result;
    }
    const QJsonObject read = QJsonDocument::fromJson(file.readAll()).object();
    const QString error = read.value(QLatin1String("error")).toString();
    const QString detail = read.value(QLatin1String("detail")).toString();
    if (!error.isEmpty() || exitCode != 0) {
        if (error == QLatin1String("input"))
            result.error = QObject::tr("Calibre converts none of the book's formats.");
        else if (error == QLatin1String("missing"))
            result.error = QObject::tr("The book's %1 file is missing.").arg(detail);
        else if (error == QLatin1String("settings"))
            result.error = QObject::tr("Calibre's conversion settings are not right: %1").arg(detail);
        else if (error == QLatin1String("empty"))
            result.error = QObject::tr("Calibre's conversion gave an empty file.");
        else if (error == QLatin1String("drm"))
            result.error = QObject::tr("The book's file is locked by DRM, which Calibre does not convert.");
        else
            result.error = QObject::tr("Calibre could not convert the book: %1").arg(detail.isEmpty() ? CalibreJob::logTail(folder) : detail);
        return result;
    }
    result.ok = true;
    result.inputFormat = read.value(QLatin1String("input")).toString();
    result.file = QDir(folder).filePath(read.value(QLatin1String("file")).toString());
    result.saveOriginal = read.value(QLatin1String("save_original")).toBool();
    result.options = QByteArray::fromBase64(read.value(QLatin1String("options")).toString().toLatin1());
    return result;
}

Outcome run(const QString &folder, const QString &interfaceLanguage, const std::function<void(double)> &progress)
{
    Outcome result;
    const QString calibre = program();
    if (calibre.isEmpty()) {
        result.error = QObject::tr("Converting needs Calibre, which is not installed.");
        return result;
    }
    const std::optional<int> exitCode = CalibreJob::run(
        calibre, arguments(folder), CalibreJob::environment(folder, interfaceLanguage), progress, &result.error);
    return exitCode ? outcome(folder, *exitCode) : result;
}

void addTo(CommitEngine::Request *request, const Book &book, const QString &format, const Outcome &outcome)
{
    const QString upper = format.toUpper();
    if (outcome.saveOriginal) {
        // save_original_format(): nothing when the book's file is missing.
        for (const BookFormat &bookFormat : book.formats) {
            if (bookFormat.format == upper && !bookFormat.file.isEmpty())
                request->newFormats.append({ book.id, bookFormat.file, QStringLiteral("ORIGINAL_") + upper });
        }
    }
    request->newFormats.append({ book.id, outcome.file, upper });
    // Cousoir's own conversion has no settings to keep.
    if (!outcome.options.isEmpty())
        request->conversionOptions.append({ book.id, outcome.options });
}

} // namespace Conversion
