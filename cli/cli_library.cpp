// cousoir-cli: the commands that read the library: list, search, doctor,
// templates, read, and the books' text, export.

#include <QBuffer>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <optional>

#include "book_edit.h"
#include "book_import.h"
#include "calibre_db.h"
#include "calibre_job.h"
#include "calibre_list.h"
#include "calibre_search.h"
#include "calibre_template.h"
#include "commit_engine.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "device_files.h"
#include "kepub.h"
#include "kf8_writer.h"
#include "mobi_book.h"
#include "kindle.h"
#include "kindle_send.h"
#include "kobo.h"
#include "kobo_highlights.h"
#include "kobo_placement.h"
#include "kobo_sync.h"
#include "kobo_send.h"
#include "sqlite_db.h"
#include "book_text_index.h"
#include "new_library.h"
#include "save_to_disk.h"
#include "book_text_search.h"
#include "library_check.h"
#include "metadata_download.h"
#include "mtp.h"
#include "open_library.h"
#include "cousoir_paths.h"

#include "cli_commands.h"

namespace Cli {

int list(const QString &libraryPath)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    const QByteArray json = CalibreList::machineList(data);
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    return 0;
}

// calibredb search: the ids of the books found, by Calibre's search
// language, comma-separated and in order; at most `limit` of them unless
// it is negative.
int search(const QString &libraryPath, const QString &query, int limit)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    const CalibreConfig config = CalibreConfig::load();
    const CalibreSearch::Index index(data, CalibreSearch::Preferences::fromRows(data.preferences), config.tweaks,
                                     config.interfaceLanguage);
    const CalibreSearch::Result result = CalibreSearch::search(index, query);
    if (!result.ok()) {
        err() << QStringLiteral("Failed to parse search query: (%1) with error: %2").arg(query, result.error)
              << Qt::endl;
        return 1;
    }
    QStringList ids;
    for (qsizetype row = 0; row < result.rows.size() && (limit < 0 || ids.size() < limit); ++row) {
        if (result.rows.testBit(row))
            ids.append(QString::number(data.books.at(row).id));
    }
    if (ids.isEmpty()) {
        err() << QStringLiteral("No books matching the search expression: %1").arg(query) << Qt::endl;
        return 1;
    }
    out() << ids.join(u',');
    out().flush();
    return 0;
}

static QString ago(qint64 seconds)
{
    if (seconds < 120)
        return QStringLiteral("%1 s ago").arg(seconds);
    if (seconds < 7200)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 172800)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    return QStringLiteral("%1 days ago").arg(seconds / 86400);
}

// A part of doctor's report: its title and its lines, a label and a value.
struct ReportSection
{
    QString title;
    QList<std::pair<QString, QString>> rows;
};

// "Last change" as a JSON key: "lastChange".
static QString jsonKey(const QString &label)
{
    QString key;
    bool upper = false;
    for (const QChar c : label) {
        if (c.isSpace()) {
            upper = true;
            continue;
        }
        key += key.isEmpty() ? c.toLower() : upper ? c.toUpper() : c;
        upper = false;
    }
    return key;
}

int doctor(const QString &libraryPath, bool json)
{
    QList<ReportSection> sections;
    const auto section = [&sections](const QString &title) { sections.append({ title, {} }); };
    const auto row = [&sections](const QString &label, const QString &value) {
        sections.last().rows.append({ label, value });
    };
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    const QLocale c = QLocale::c();
    const QDir library(data.libraryPath);

    // What the application's Check finds, but what was ignored there.
    const CalibreConfig config = CalibreConfig::load();
    LibraryCheck::Options options;
    options.tweaks = config.tweaks;
    options.interfaceLanguage = config.interfaceLanguage;
    const LibraryCheck::Findings findings = LibraryCheck::check(data, options);
    const QStringList ignored = LibraryCheck::ignoredKeys(data.uuid);
    QHash<int, LibraryCheck::MissingFiles> missing;
    for (const LibraryCheck::MissingFiles &entry : findings.missing) {
        if (!ignored.contains(LibraryCheck::key(entry)))
            missing.insert(entry.book, entry);
    }

    qint64 files = 0;
    qint64 bytes = 0;
    int covers = 0;
    QStringList problems;
    const auto relative = [&library](const QString &path) { return library.relativeFilePath(path); };
    for (const Book &book : std::as_const(data.books)) {
        const QString name = QStringLiteral("Book %1 “%2”").arg(book.id).arg(book.title);
        const QString folder = library.filePath(book.path);
        if (book.path.isEmpty() || !QFileInfo(folder).isDir()) {
            if (missing.contains(book.id))
                problems.append(QStringLiteral("%1: its folder is missing\n      %2").arg(name, book.path));
            continue;
        }
        if (missing.value(book.id).noFormats)
            problems.append(QStringLiteral("%1: it has no file").arg(name));
        for (const BookFormat &format : book.formats) {
            const QString expected = QDir(folder).filePath(format.name + u'.' + format.format.toLower());
            if (format.file.isEmpty()) {
                if (missing.value(book.id).formats.contains(format.format)) {
                    problems.append(QStringLiteral("%1: the %2 file is missing\n      %3")
                                        .arg(name, format.format, relative(expected)));
                }
                continue;
            }
            ++files;
            bytes += QFileInfo(format.file).size();
            if (format.file != expected) {
                problems.append(QStringLiteral("%1: the %2 file has another name than Calibre expects\n      %3")
                                    .arg(name, format.format, relative(format.file)));
            }
        }
        if (!book.coverFile.isEmpty())
            ++covers;
        if (book.hasCover && book.coverFile.isEmpty())
            problems.append(QStringLiteral("%1: Calibre lists a cover but cover.jpg is missing").arg(name));
        else if (!book.hasCover && !book.coverFile.isEmpty())
            problems.append(QStringLiteral("%1: cover.jpg is there but Calibre does not list it").arg(name));
    }
    if (!data.schemaKnown) {
        problems.append(QStringLiteral("The database structure is unknown (schema %1, fingerprint %2): "
                                       "Cousoir stays read-only").arg(data.userVersion).arg(data.schemaFingerprint));
    }
    for (const LibraryCheck::DuplicateGroup &group : findings.duplicates) {
        if (ignored.contains(LibraryCheck::key(group)))
            continue;
        QStringList ids;
        for (const int id : group.books)
            ids.append(QString::number(id));
        QStringList reasons;
        if (group.sameTitle)
            reasons.append(QStringLiteral("same title and authors"));
        for (const QString &isbn : group.isbns)
            reasons.append(QStringLiteral("same ISBN %1").arg(isbn));
        const Book *first = data.book(group.books.first());
        problems.append(QStringLiteral("Duplicate books %1 (%2): “%3” by %4")
                            .arg(ids.join(QStringLiteral(", ")), reasons.join(QStringLiteral(", ")), first->title,
                                 first->authors.join(QStringLiteral(" & "))));
    }
    for (const LibraryCheck::AuthorGroup &group : findings.authorVariants) {
        if (ignored.contains(LibraryCheck::key(group)))
            continue;
        QStringList names;
        for (const int id : group.authors) {
            for (const FacetItem &author : data.authors) {
                if (author.id == id)
                    names.append(QStringLiteral("“%1” (%2 book%3)").arg(author.name).arg(author.count)
                                     .arg(author.count == 1 ? QString() : QStringLiteral("s")));
            }
        }
        problems.append(QStringLiteral("Author name variants (%1): %2")
                            .arg(group.initials ? QStringLiteral("possibly the same person")
                                                : QStringLiteral("same name written differently"),
                                 names.join(QStringLiteral(", "))));
    }
    if (!findings.caseDuplicates.isEmpty()) {
        problems.append(QStringLiteral("Author names differ only by case, %1: no author can be changed until "
                                       "Calibre merges them, the next time it saves an author")
                            .arg(QStringLiteral("“") + findings.caseDuplicates.join(QStringLiteral("”, “"))
                                 + QStringLiteral("”")));
    }
    for (const LibraryCheck::OutsideFile &file : findings.outside) {
        if (!ignored.contains(LibraryCheck::key(file)))
            problems.append(QStringLiteral("Outside the database: %1").arg(file.folder ? file.path + QStringLiteral("/ (folder)")
                                                                                        : file.path));
    }
    for (const QString &folder : findings.unreadable)
        problems.append(QStringLiteral("Cannot read the folder %1").arg(folder));

    // The guard answers about Dropbox asynchronously; give it a moment.
    ConcurrencyGuard guard;
    guard.setLibraryReadable(true);
    guard.setSchemaKnown(data.schemaKnown);
    guard.setLibraryUuid(data.uuid);
    guard.setLibraryPath(data.libraryPath);
    QEventLoop loop;
    QObject::connect(&guard, &ConcurrencyGuard::stateChanged, &loop, [&] {
        if (!guard.dropboxStatus().isEmpty())
            loop.quit();
    });
    QTimer::singleShot(3500, &loop, &QEventLoop::quit);
    if (guard.syncService() == QLatin1String("dropbox"))
        loop.exec();
    for (const QVariant &value : guard.conflicts()) {
        const QVariantMap conflict = value.toMap();
        const bool seen = conflict.value(QStringLiteral("acknowledged")).toBool();
        const QDateTime modified = conflict.value(QStringLiteral("modified")).toDateTime();
        problems.append(QStringLiteral("Conflict copy%1: %2, %3, %4")
                            .arg(seen ? QStringLiteral(" (ignored)") : QString(),
                                 conflict.value(QStringLiteral("file")).toString(),
                                 modified.toString(QStringLiteral("yyyy-MM-dd")),
                                 c.formattedDataSize(conflict.value(QStringLiteral("size")).toLongLong(), 1,
                                                     QLocale::DataSizeTraditionalFormat)));
    }

    section(QStringLiteral("Library"));
    row(QStringLiteral("Folder"), QDir::toNativeSeparators(data.libraryPath));
    row(QStringLiteral("Library id"), data.uuid);
    row(QStringLiteral("Database"), QStringLiteral("schema %1, %2 structure (%3)")
                                       .arg(data.userVersion)
                                       .arg(data.schemaKnown ? QStringLiteral("known") : QStringLiteral("unknown"),
                                            data.schemaFingerprint));
    row(QStringLiteral("Books"), QStringLiteral("%1, %2 files, %3, %4 covers")
                                    .arg(data.books.size()).arg(files)
                                    .arg(c.formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat))
                                    .arg(covers));

    const QString database = library.filePath(QStringLiteral("metadata.db"));
    const FileIdentity identity = CalibreDb::identify(database);
    const QDateTime changed = QDateTime::fromMSecsSinceEpoch(identity.modifiedMs);
    section(QStringLiteral("Writing"));
    row(QStringLiteral("Calibre"), ConcurrencyGuard::calibreRunningHere()
                                       ? QStringLiteral("open on this computer") : QStringLiteral("not running here"));
    row(QStringLiteral("Journal"), QFileInfo::exists(database + QStringLiteral("-journal")) ? QStringLiteral("present")
                                                                                            : QStringLiteral("none"));
    const QString service = guard.syncService();
    const QString serviceName = service == QLatin1String("dropbox") ? QStringLiteral("Dropbox")
        : service == QLatin1String("syncthing")                     ? QStringLiteral("Syncthing")
        : service == QLatin1String("nextcloud")                     ? QStringLiteral("Nextcloud or ownCloud")
                                                                    : QString();
    const QString shared = !guard.shared() ? QStringLiteral("only this computer")
        : !serviceName.isEmpty()
            ? QStringLiteral("through %1 (%2)").arg(serviceName, QDir::toNativeSeparators(guard.syncRoot()))
        : service == QLatin1String("network")
            ? QStringLiteral("on a network folder (%1)")
                  .arg(QString::fromLatin1(QStorageInfo(data.libraryPath).fileSystemType()))
            : QStringLiteral("with other computers");
    row(QStringLiteral("Shared"), shared + (guard.sharing().isEmpty() ? QString() : QStringLiteral(", as set in Cousoir")));
    if (service == QLatin1String("dropbox"))
        row(QStringLiteral("Dropbox"), guard.dropboxStatus().isEmpty() ? QStringLiteral("no answer")
                                                                       : guard.dropboxStatus());
    row(QStringLiteral("Last change"), QStringLiteral("%1 (%2)").arg(changed.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                                                     ago(changed.secsTo(QDateTime::currentDateTime()))));
    row(QStringLiteral("State"), guard.writable() ? QStringLiteral("Could be written") : guard.readOnlyReason());

    if (json) {
        // The same report for a program: each part by its title, the
        // problems as a list, and whether the library could be written now.
        QJsonObject report;
        for (const ReportSection &part : std::as_const(sections)) {
            QJsonObject rows;
            for (const auto &[label, value] : part.rows)
                rows.insert(jsonKey(label), value);
            report.insert(jsonKey(part.title), rows);
        }
        report.insert(QStringLiteral("problems"), QJsonArray::fromStringList(problems));
        report.insert(QStringLiteral("writable"), guard.writable());
        const QByteArray text = QJsonDocument(report).toJson(QJsonDocument::Indented);
        std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
        return problems.isEmpty() ? 0 : 1;
    }
    const auto print = [](const ReportSection &part) {
        out() << part.title << Qt::endl;
        for (const auto &[label, value] : part.rows)
            out() << QStringLiteral("  %1 %2").arg(label.leftJustified(14), value) << Qt::endl;
    };
    print(sections.at(0));
    out() << Qt::endl << QStringLiteral("Problems") << Qt::endl;
    if (problems.isEmpty())
        out() << QStringLiteral("  None") << Qt::endl;
    for (const QString &problem : std::as_const(problems))
        out() << QStringLiteral("  ") << problem << Qt::endl;
    out() << Qt::endl;
    print(sections.at(1));
    return problems.isEmpty() ? 0 : 1;
}

// Indexes the text of the library's books into Calibre's full text index,
// as Calibre's workers do, after making the index and turning Calibre's
// full text search on when the library has none.
int indexText(const QString &libraryPath, bool force, bool json)
{
    if (!force) {
        const QString reason = readOnlyReason(libraryPath);
        if (!reason.isEmpty()) {
            err() << QStringLiteral("The library cannot be written: %1").arg(reason) << Qt::endl;
            return 1;
        }
    }
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    const CalibreConfig config = CalibreConfig::load();
    const QString calibreOpen = QStringLiteral("Calibre is open on this computer: close it to change the library.");
    if (!BookTextSearch::status(libraryPath).exists) {
        QList<BookTextIndex::Job> formats;
        for (const Book &book : std::as_const(data.books)) {
            for (const BookFormat &format : book.formats)
                formats.append({ book.id, format.format });
        }
        bool made = false;
        QString error;
        if (!CommitEngine::withLibraryLock(
                [&] { made = BookTextIndex::create(libraryPath, formats, config.interfaceLanguage, &error); })) {
            err() << calibreOpen << Qt::endl;
            return 1;
        }
        if (!made) {
            err() << error << Qt::endl;
            return 1;
        }
    }
    if (data.preferences.value(QStringLiteral("fts_enabled")) != QLatin1String("true")) {
        CommitEngine::Request request;
        request.preferences = { { QStringLiteral("fts_enabled"), QStringLiteral("true") } };
        request.libraryPath = libraryPath;
        request.cacheFolder = CousoirPaths::cacheFolder(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath()));
        request.config = config;
        request.producer = QStringLiteral("Cousoir (%1)").arg(QStringLiteral(COUSOIR_VERSION));
        request.producerId = QStringLiteral("cousoir");
        const CommitEngine::Result result = CommitEngine::commit(request);
        if (!result.ok) {
            err() << result.error << Qt::endl;
            return 1;
        }
    }
    QList<BookTextIndex::Task> tasks;
    for (const BookTextIndex::Job &job : BookTextIndex::waiting(libraryPath, nullptr)) {
        BookTextIndex::Task task{ job.book, job.format, {} };
        if (const Book *book = data.book(job.book)) {
            for (const BookFormat &format : book->formats) {
                if (format.format.compare(job.format, Qt::CaseInsensitive) == 0 && QFileInfo::exists(format.file))
                    task.path = format.file;
            }
        }
        tasks.append(task);
    }
    const BookTextIndex::Run run = BookTextIndex::index(libraryPath, config.interfaceLanguage, tasks);
    const BookTextSearch::Status status = BookTextSearch::status(libraryPath);
    if (json) {
        out() << QJsonDocument(QJsonObject{ { QStringLiteral("stored"), run.stored },
                                            { QStringLiteral("indexed"), status.indexed },
                                            { QStringLiteral("waiting"), status.pending },
                                            { QStringLiteral("calibreRunning"), run.calibreRunning } })
                     .toJson(QJsonDocument::Compact)
              << Qt::endl;
    } else {
        out() << QStringLiteral("%1 format(s) indexed now, %2 in all; %3 left to Calibre")
                     .arg(run.stored)
                     .arg(status.indexed)
                     .arg(status.pending)
              << Qt::endl;
    }
    if (run.calibreRunning) {
        err() << calibreOpen << Qt::endl;
        return 1;
    }
    return 0;
}

// Saves books out of the library as Calibre's Save to disk does, with
// Calibre's options for it: `ids` or "all".
int exportBooks(const QString &libraryPath, const QString &folder, const QStringList &ids)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    QList<int> books;
    if (ids == QStringList{ QStringLiteral("all") }) {
        for (const Book &book : std::as_const(data.books))
            books.append(book.id);
    } else {
        for (const QString &id : ids) {
            bool ok = false;
            const int value = id.toInt(&ok);
            if (!ok || !data.book(value)) {
                err() << QStringLiteral("No book %1 in the library.").arg(id) << Qt::endl;
                return 2;
            }
            books.append(value);
        }
    }
    const CalibreConfig config = CalibreConfig::load();
    Opf::Settings opf;
    opf.interfaceLanguage = config.interfaceLanguage;
    opf.producer = QStringLiteral("Cousoir (%1)").arg(QStringLiteral(COUSOIR_VERSION));
    opf.producerId = QStringLiteral("cousoir");
    const SaveToDisk::Outcome outcome = SaveToDisk::save(
        data, books, folder, SaveToDisk::Options::load(CalibreConfig::configDirectory()), opf);
    for (const QString &failure : outcome.failures)
        err() << failure << Qt::endl;
    out() << QStringLiteral("Saved %1 book(s) in %2").arg(outcome.saved).arg(QFileInfo(folder).absoluteFilePath())
          << Qt::endl;
    return outcome.failures.isEmpty() ? 0 : 1;
}

// The books whose text has what `query` asks, in Calibre's full text index,
// best first: the words found between ** in each passage.
int searchText(const QString &libraryPath, const QString &query, int limit, bool json)
{
    const CalibreConfig config = CalibreConfig::load();
    QString error;
    const QList<BookTextSearch::Hit> hits
        = BookTextSearch::find(libraryPath, query, true, config.interfaceLanguage, limit < 0 ? 1000 : limit, &error);
    if (!error.isEmpty()) {
        err() << error << Qt::endl;
        return 1;
    }
    QJsonArray rows;
    for (const BookTextSearch::Hit &hit : hits) {
        const QString snippet = QString(hit.snippet).replace(u'\x1d', QStringLiteral("**"));
        if (json) {
            rows.append(QJsonObject{ { QStringLiteral("book"), hit.bookId },
                                     { QStringLiteral("format"), hit.format },
                                     { QStringLiteral("snippet"), snippet } });
        } else {
            out() << hit.bookId << u'\t' << hit.format << u'\t' << QString(snippet).replace(u'\n', u' ') << Qt::endl;
        }
    }
    if (json)
        out() << QJsonDocument(rows).toJson(QJsonDocument::Compact) << Qt::endl;
    if (hits.isEmpty()) {
        err() << QStringLiteral("No book's text matches: %1").arg(query) << Qt::endl;
        return 1;
    }
    return 0;
}

// What each template of a JSON array makes of each book, as
// tools/oracle/template_oracle.py prints Calibre's: by book id, the text or
// {"error": message}.
int templates(const QString &libraryPath, const QString &file)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 1;
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        err() << QStringLiteral("Cannot read %1.").arg(file) << Qt::endl;
        return 1;
    }
    const QJsonArray list = QJsonDocument::fromJson(in.readAll()).array();
    const CalibreTemplate::Library library(data, CalibreConfig::load().interfaceLanguage);
    QList<const Book *> books;
    for (const Book &book : data.books)
        books.append(&book);
    std::sort(books.begin(), books.end(), [](const Book *a, const Book *b) { return a->id < b->id; });
    QJsonArray results;
    for (const QJsonValue &value : list) {
        const QString text = value.toString();
        QJsonObject row;
        for (const Book *book : std::as_const(books)) {
            QString error;
            const QString result = CalibreTemplate::evaluate(text, library, *book, &error);
            row.insert(QString::number(book->id), error.isNull() ? QJsonValue(result)
                                                                 : QJsonValue(QJsonObject{ { QStringLiteral("error"), error } }));
        }
        results.append(QJsonObject{ { QStringLiteral("template"), text }, { QStringLiteral("books"), row } });
    }
    out() << QJsonDocument(results).toJson();
    return 0;
}

// What adding each file would read, as tools/oracle/read_oracle.py prints
// Calibre's: [{ "file": …, "title": …, … }].
int read(const QStringList &paths)
{
    const CalibreConfig config = CalibreConfig::load();
    const auto date = [](const std::optional<CalibreDate> &value) {
        if (!value || CalibreDb::isUndefinedDate(value->dateTime))
            return QJsonValue();
        return QJsonValue(value->isoFormat().replace(QStringLiteral("+00:00"), QStringLiteral("Z")));
    };
    QJsonArray books;
    int status = 0;
    for (const QString &path : paths) {
        if (!QFileInfo(path).isFile()) {
            err() << QStringLiteral("%1 is not a file.").arg(path) << Qt::endl;
            status = 2;
            continue;
        }
        QString warning;
        const ImportedMetadata mi = BookImport::metadataForAdding(path, config, &warning);
        if (!warning.isEmpty())
            err() << path << QStringLiteral(": ") << warning << Qt::endl;
        QJsonObject identifiers;
        for (const auto &identifier : mi.identifiers)
            identifiers.insert(identifier.first, identifier.second);
        QJsonValue cover;
        if (!mi.cover.isEmpty()) {
            QBuffer buffer;
            buffer.setData(mi.cover);
            buffer.open(QIODevice::ReadOnly);
            const QSize size = QImageReader(&buffer).size();
            cover = QJsonObject{ { QStringLiteral("size"), mi.cover.size() },
                                 { QStringLiteral("sha1"), QString::fromLatin1(QCryptographicHash::hash(mi.cover, QCryptographicHash::Sha1).toHex()) },
                                 { QStringLiteral("width"), size.width() },
                                 { QStringLiteral("height"), size.height() } };
        }
        // The custom columns' fields as adding the book reads them.
        QJsonObject userMetadata;
        for (const auto &[key, field] : mi.userMetadata) {
            QJsonObject read;
            for (const QString &name : { QStringLiteral("datatype"), QStringLiteral("is_multiple"),
                                         QStringLiteral("#value#"), QStringLiteral("#extra#") }) {
                const PythonJson::Value *value = field.find(name);
                const QByteArray json = '[' + (value ? PythonJson::dumps(*value) : QStringLiteral("null")).toUtf8() + ']';
                read.insert(name, QJsonDocument::fromJson(json).array().at(0));
            }
            userMetadata.insert(key, read);
        }
        books.append(QJsonObject{
            { QStringLiteral("file"), QFileInfo(path).fileName() },
            { QStringLiteral("title"), mi.title },
            { QStringLiteral("title_sort"), mi.titleSort },
            { QStringLiteral("authors"), QJsonArray::fromStringList(mi.authors) },
            { QStringLiteral("author_sort"), mi.authorSort },
            { QStringLiteral("series"), mi.series },
            { QStringLiteral("series_index"), mi.seriesIndex ? QJsonValue(*mi.seriesIndex) : QJsonValue() },
            { QStringLiteral("tags"), QJsonArray::fromStringList(mi.tags) },
            { QStringLiteral("publisher"), mi.publisher },
            { QStringLiteral("pubdate"), date(mi.pubdate) },
            { QStringLiteral("timestamp"), date(mi.timestamp) },
            { QStringLiteral("uuid"), mi.uuid },
            { QStringLiteral("comments"), mi.comments },
            { QStringLiteral("languages"), QJsonArray::fromStringList(mi.languages) },
            { QStringLiteral("identifiers"), identifiers },
            { QStringLiteral("rating"), mi.rating ? QJsonValue(*mi.rating) : QJsonValue() },
            { QStringLiteral("book_producer"), mi.bookProducer },
            { QStringLiteral("cover"), cover },
            { QStringLiteral("user_metadata"), userMetadata },
        });
    }
    const QByteArray json = QJsonDocument(books).toJson(QJsonDocument::Indented);
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    return status;
}

} // namespace Cli
