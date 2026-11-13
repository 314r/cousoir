// cousoir-cli: apply, the changes of a JSON file, conversions and metadata
// downloads.

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

// Converts the books to the formats, as Calibre's convert action does, and
// adds what it adds to `request`; without Calibre, or with
// COUSOIR_NATIVE_CONVERT set, as Cousoir converts itself. The jobs' folders
// stay until the process ends, with the files the request adds; with
// COUSOIR_KEEP_JOBS set, after, for a look at Calibre's log.
bool convert(const QString &libraryPath, const CalibreConfig &config, const QList<QPair<int, QString>> &conversions,
             CommitEngine::Request *request)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return false;
    static QTemporaryDir jobs;
    jobs.setAutoRemove(qEnvironmentVariableIsEmpty("COUSOIR_KEEP_JOBS"));
    for (const auto &[bookId, format] : conversions) {
        const Book *book = data.book(bookId);
        if (!book) {
            err() << QStringLiteral("No book %1 in the library.").arg(bookId) << Qt::endl;
            return false;
        }
        const QString folder = jobs.filePath(QStringLiteral("%1-%2").arg(bookId).arg(format));
        const bool native = Conversion::program().isEmpty() || !qEnvironmentVariableIsEmpty("COUSOIR_NATIVE_CONVERT");
        QString error;
        if (!jobs.isValid() || !QDir().mkpath(folder)
            || (!native && !Conversion::prepare(data, *book, format, config.interfaceLanguage, folder, &error))) {
            err() << (error.isEmpty() ? QStringLiteral("Cannot create a temporary folder.") : error) << Qt::endl;
            return false;
        }
        const Conversion::Outcome outcome = native ? Conversion::convertNatively(*book, format, folder, config.interfaceLanguage)
                                                   : Conversion::run(folder, config.interfaceLanguage);
        if (!outcome.ok) {
            err() << book->title << QStringLiteral(": ") << outcome.error << Qt::endl;
            return false;
        }
        Conversion::addTo(request, *book, format, outcome);
    }
    return true;
}

static QJsonArray pairs(const QList<QPair<QString, QString>> &list)
{
    QJsonArray array;
    for (const auto &[first, second] : list)
        array.append(QJsonArray{ first, second });
    return array;
}

// What Calibre's sources find for a book, as the download of Calibre's Edit
// metadata dialog does, and what each book found would change, the fields
// Calibre is set to leave aside left aside; with `covers`, the covers found
// with the first. Calibre's sources are web sites.
int download(const QString &libraryPath, const QString &id, bool covers)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 1;
    const Book *book = data.book(id.toInt());
    if (!book) {
        err() << QStringLiteral("No book has the id %1.").arg(id) << Qt::endl;
        return 1;
    }
    const QString program = MetadataDownload::program();
    // Open Library without Calibre, as the application asks it.
    const bool native = program.isEmpty() || qEnvironmentVariableIsSet("COUSOIR_NATIVE_DOWNLOAD");
    const CalibreConfig config = CalibreConfig::load();
    QTemporaryDir folder;
    MetadataDownload::Query query{ book->title, book->authors, book->identifiers };
    QString error;
    const auto run = [&](bool (*prepare)(const QString &, const MetadataDownload::Query &, QString *)) {
        return prepare(folder.path(), query, &error)
            && CalibreJob::run(program, MetadataDownload::arguments(folder.path()),
                               CalibreJob::environment(folder.path(), config.interfaceLanguage), [](double) {}, &error);
    };
    OpenLibrary::Client client;
    MetadataDownload::Identified identified;
    if (native) {
        const OpenLibrary::Preferences preferences = OpenLibrary::Preferences::load(CalibreConfig::configDirectory());
        identified.ignoredFields = preferences.ignoredFields;
        identified.appendComments = preferences.appendComments;
        QEventLoop loop;
        QObject::connect(&client, &OpenLibrary::Client::identified, &loop,
                         [&](const QList<MetadataDownload::Found> &books, const QString &failure) {
                             identified.books = books;
                             error = failure;
                             loop.quit();
                         });
        client.identify(query, book->languages.value(0), config.interfaceLanguage, preferences);
        loop.exec();
        if (!error.isEmpty()) {
            err() << error << Qt::endl;
            return 1;
        }
    } else if (!run(MetadataDownload::prepareIdentify)
               || !MetadataDownload::readIdentified(folder.path(), &identified, &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    QStringList fields = MetadataDownload::fields();
    fields.removeIf([&identified](const QString &field) { return identified.ignoredFields.contains(field); });
    QJsonArray books;
    for (const MetadataDownload::Found &found : std::as_const(identified.books)) {
        books.append(QJsonObject{
            { QStringLiteral("title"), found.title },
            { QStringLiteral("authors"), QJsonArray::fromStringList(found.authors) },
            { QStringLiteral("author_sort"), found.authorSort },
            { QStringLiteral("publisher"), found.publisher },
            { QStringLiteral("pubdate"), found.pubdate ? QJsonValue(found.pubdate->toString(Qt::ISODate)) : QJsonValue() },
            { QStringLiteral("series"), found.series },
            { QStringLiteral("series_index"), found.seriesIndex ? QJsonValue(*found.seriesIndex) : QJsonValue() },
            { QStringLiteral("tags"), QJsonArray::fromStringList(found.tags) },
            { QStringLiteral("rating"), found.rating ? QJsonValue(*found.rating) : QJsonValue() },
            { QStringLiteral("comments"), found.comments },
            { QStringLiteral("languages"), QJsonArray::fromStringList(found.languages) },
            { QStringLiteral("identifiers"), pairs(found.identifiers) },
            { QStringLiteral("has_cover"), found.hasCover },
            { QStringLiteral("changes"),
              QJsonObject::fromVariantMap(MetadataDownload::changes(data, *book, config, found, fields,
                                                                    identified.appendComments)) },
        });
    }
    QJsonObject result{ { QStringLiteral("books"), books },
                        { QStringLiteral("ignore_fields"), QJsonArray::fromStringList(identified.ignoredFields) },
                        { QStringLiteral("append_comments"), identified.appendComments } };
    if (covers && !identified.books.isEmpty()) {
        // The dialog's: the book's title and authors, the identifiers found.
        query.identifiers = identified.books.first().identifiers;
        QList<MetadataDownload::Cover> found;
        if (native) {
            QEventLoop loop;
            QObject::connect(&client, &OpenLibrary::Client::coverFound, &loop,
                             [&found](const MetadataDownload::Cover &cover) { found.append(cover); });
            QObject::connect(&client, &OpenLibrary::Client::coversFinished, &loop, [&](const QString &failure) {
                error = failure;
                loop.quit();
            });
            client.covers(0, folder.path());
            loop.exec();
            std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
                return qint64(a.width) * a.height > qint64(b.width) * b.height;
            });
            if (!error.isEmpty())
                err() << error << Qt::endl;
        } else if (!run(MetadataDownload::prepareCovers) || !MetadataDownload::readCovers(folder.path(), &found, &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        QJsonArray list;
        for (const MetadataDownload::Cover &cover : std::as_const(found))
            list.append(QJsonObject{ { QStringLiteral("source"), cover.source },
                                     { QStringLiteral("width"), cover.width },
                                     { QStringLiteral("height"), cover.height } });
        result.insert(QStringLiteral("covers"), list);
    }
    out() << QJsonDocument(result).toJson();
    return 0;
}

// Applies the changes of a JSON file, an array of:
//   { "book": 1, "set": { "title": …, … } }   BookEdit's keys; a coverFile
//                                               is relative to the JSON file
//   { "copyFormats": { "from": 7, "to": 2 } }  the formats book 2 lacks
//   { "removeFormats": { "book": 2, "formats": ["AZW3"] } }
//   { "trash": [3, 4] }
//   { "restore": 3 } or { "restore": { "book": 2, "format": "AZW3" } }
//   { "add": "book.epub" } or { "add": ["book.epub", "book.pdf"] }
//   { "addFormat": { "book": 2, "file": "book.azw3" } }
//   { "convert": { "book": 1, "format": "KEPUB" } }
//   { "download": { "book": 2, "found": "folder", "result": 0,
//                   "fields": ["title", …], "append": false } }
//                                               what a book a download found
//                                               changes, from the result.json
//                                               of an identify job's folder,
//                                               for the book as it was before
//                                               the file's changes
//   { "note": { "field": "tags", "item": 3, "name": "Fantasy",
//               "html": "<p>…</p>", "text": "…" } }   an item's note, as
//                                               Calibre 7 keeps them; no html
//                                               removes it
//   { "send": { "books": [1, 2], "device": "/run/media/…/KOBOeReader" } }
//   { "removeFromDevice": { "device": …, "lpaths": ["Vian, Boris/…"] } }
//   { "syncHighlights": { "device": "/run/media/…/KOBOeReader" } }   a
//                                               Kobo's highlights merged
//                                               into the library's, as its
//                                               own user's; a database file
//                                               adds and updates only
// A device is a Kobo's folder, "mtp" or "mtp:SERIAL" for a Kindle, or
// "mtpdir:FOLDER" for a folder that stands for one.
// Files are relative to the JSON file. Edits apply first, then the other
// changes in the order above, as the application saves them. Conversions
// run first, on the books as the library has them, and add their files
// with the formats added. What goes to devices comes last, in its order, from
// the library as changed.
int apply(const QString &libraryPath, const QString &opsPath, bool force)
{
    QFile file(opsPath);
    if (!file.open(QIODevice::ReadOnly)) {
        err() << QStringLiteral("Cannot read %1: %2").arg(opsPath, file.errorString()) << Qt::endl;
        return 2;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isArray()) {
        err() << QStringLiteral("%1 is not a JSON array: %2").arg(opsPath, parseError.errorString()) << Qt::endl;
        return 2;
    }
    const QDir base = QFileInfo(opsPath).absoluteDir();
    const CalibreConfig config = CalibreConfig::load();
    CommitEngine::Request request;
    QList<QPair<int, QString>> conversions;
    QList<QJsonObject> deviceOps;
    bool libraryChanges = false;
    // The library as it is, read for the first download.
    std::optional<LibraryData> before;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject op = value.toObject();
        if (op.contains(QLatin1String("send")) || op.contains(QLatin1String("removeFromDevice"))) {
            deviceOps.append(op);
            continue;
        }
        libraryChanges = true;
        if (op.contains(QLatin1String("syncHighlights"))) {
            KoboSync::Input input;
            if (!readKobo(deviceSpec(base, op.value(QLatin1String("syncHighlights")).toObject().value(QLatin1String("device")).toString()),
                          &input.snapshot)
                || !load(libraryPath, &input.data))
                return 1;
            // The library's own database read as it is, nothing kept aside.
            input.snapshotDb = QDir(libraryPath).filePath(QStringLiteral("metadata.db"));
            request.deviceAnnotations += KoboSync::plan(input).changes;
            continue;
        }
        if (op.contains(QLatin1String("copyFormats"))) {
            const QJsonObject copy = op.value(QLatin1String("copyFormats")).toObject();
            request.formatCopies.append({ copy.value(QLatin1String("from")).toInt(), copy.value(QLatin1String("to")).toInt() });
            continue;
        }
        if (op.contains(QLatin1String("removeFormats"))) {
            const QJsonObject removal = op.value(QLatin1String("removeFormats")).toObject();
            FormatRemoval formats{ removal.value(QLatin1String("book")).toInt(), {} };
            for (const QJsonValue &format : removal.value(QLatin1String("formats")).toArray())
                formats.formats.append(format.toString());
            request.formatRemovals.append(formats);
            continue;
        }
        if (op.contains(QLatin1String("trash"))) {
            for (const QJsonValue &book : op.value(QLatin1String("trash")).toArray())
                request.trash.append(book.toInt());
            continue;
        }
        if (op.contains(QLatin1String("restore"))) {
            const QJsonValue restore = op.value(QLatin1String("restore"));
            request.restorations.append(restore.isObject()
                                            ? Restoration{ restore.toObject().value(QLatin1String("book")).toInt(),
                                                           restore.toObject().value(QLatin1String("format")).toString() }
                                            : Restoration{ restore.toInt(), {} });
            continue;
        }
        if (op.contains(QLatin1String("add"))) {
            const QJsonValue add = op.value(QLatin1String("add"));
            NewBook book;
            for (const QJsonValue &path : add.isArray() ? add.toArray() : QJsonArray{ add })
                book.files.append(base.absoluteFilePath(path.toString()));
            if (book.files.isEmpty()) {
                err() << QStringLiteral("An add names no file.") << Qt::endl;
                return 2;
            }
            QString warning;
            book.metadata = BookImport::metadataForAdding(book.files.first(), config, &warning);
            if (!warning.isEmpty())
                err() << book.files.first() << QStringLiteral(": ") << warning << Qt::endl;
            request.newBooks.append(book);
            continue;
        }
        if (op.contains(QLatin1String("note"))) {
            const QJsonObject note = op.value(QLatin1String("note")).toObject();
            request.notes.append({ note.value(QLatin1String("field")).toString(), note.value(QLatin1String("item")).toInt(),
                                   note.value(QLatin1String("name")).toString(), note.value(QLatin1String("html")).toString(),
                                   note.value(QLatin1String("text")).toString() });
            continue;
        }
        if (op.contains(QLatin1String("convert"))) {
            const QJsonObject convert = op.value(QLatin1String("convert")).toObject();
            conversions.append({ convert.value(QLatin1String("book")).toInt(),
                                 convert.value(QLatin1String("format")).toString().toUpper() });
            continue;
        }
        if (op.contains(QLatin1String("bulk"))) {
            BulkEdit bulk;
            QString error;
            if (!BulkEdit::fromVariantMap(op.value(QLatin1String("bulk")).toObject().toVariantMap(), &bulk, &error)) {
                err() << error << Qt::endl;
                return 2;
            }
            request.bulkEdits.append(bulk);
            continue;
        }
        if (op.contains(QLatin1String("addFormat"))) {
            const QJsonObject format = op.value(QLatin1String("addFormat")).toObject();
            request.newFormats.append({ format.value(QLatin1String("book")).toInt(),
                                        base.absoluteFilePath(format.value(QLatin1String("file")).toString()) });
            continue;
        }
        if (op.contains(QLatin1String("download"))) {
            const QJsonObject download = op.value(QLatin1String("download")).toObject();
            if (!before) {
                before.emplace();
                if (!load(libraryPath, &*before))
                    return 1;
            }
            const Book *book = before->book(download.value(QLatin1String("book")).toInt());
            MetadataDownload::Identified identified;
            QString error;
            if (!MetadataDownload::readIdentified(base.absoluteFilePath(download.value(QLatin1String("found")).toString()),
                                                  &identified, &error)) {
                err() << error << Qt::endl;
                return 2;
            }
            const qsizetype index = download.value(QLatin1String("result")).toInt();
            if (!book || index < 0 || index >= identified.books.size()) {
                err() << QStringLiteral("A download names no book or no result.") << Qt::endl;
                return 2;
            }
            QStringList fields;
            for (const QJsonValue &field : download.value(QLatin1String("fields")).toArray())
                fields.append(field.toString());
            BookEdit edit;
            if (!BookEdit::fromVariantMap(book->id,
                                          MetadataDownload::changes(*before, *book, config, identified.books.at(index), fields,
                                                                    download.value(QLatin1String("append")).toBool()),
                                          &edit, &error)) {
                err() << error << Qt::endl;
                return 2;
            }
            request.edits.append(edit);
            continue;
        }
        QVariantMap changes = op.value(QLatin1String("set")).toObject().toVariantMap();
        if (changes.contains(QStringLiteral("coverFile")))
            changes.insert(QStringLiteral("coverFile"), base.absoluteFilePath(changes.value(QStringLiteral("coverFile")).toString()));
        BookEdit edit;
        QString error;
        if (!BookEdit::fromVariantMap(op.value(QLatin1String("book")).toInt(), changes, &edit, &error)) {
            err() << error << Qt::endl;
            return 2;
        }
        request.edits.append(edit);
    }
    if (libraryChanges && !force) {
        const QString reason = readOnlyReason(libraryPath);
        if (!reason.isEmpty()) {
            err() << reason << Qt::endl;
            return 1;
        }
    }
    if (!conversions.isEmpty() && !convert(libraryPath, config, conversions, &request))
        return 1;
    if (libraryChanges && commit(libraryPath, config, request) != 0)
        return 1;
    for (const QJsonObject &op : std::as_const(deviceOps)) {
        if (op.contains(QLatin1String("send"))) {
            const QJsonObject sending = op.value(QLatin1String("send")).toObject();
            QList<int> books;
            for (const QJsonValue &book : sending.value(QLatin1String("books")).toArray())
                books.append(book.toInt());
            if (!send(libraryPath, config, deviceSpec(base, sending.value(QLatin1String("device")).toString()), books))
                return 1;
        } else {
            const QJsonObject removal = op.value(QLatin1String("removeFromDevice")).toObject();
            QStringList lpaths;
            for (const QJsonValue &lpath : removal.value(QLatin1String("lpaths")).toArray())
                lpaths.append(lpath.toString());
            if (!removeFromDevice(deviceSpec(base, removal.value(QLatin1String("device")).toString()), lpaths))
                return 1;
        }
    }
    return 0;
}

} // namespace Cli
