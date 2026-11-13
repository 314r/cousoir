// cousoir-cli: the readers, a Kobo or a Kindle: what is on them, sending
// and removing books, a Kobo's highlights.

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

// A reader the command line reaches: a Kobo by the folder it is mounted on;
// a Kindle over MTP, "mtp" for the first one connected or "mtp:SERIAL"; or
// "mtpdir:FOLDER", a folder that stands for a Kindle over MTP.
struct Reader
{
    QString name;
    Kobo::Device kobo;
    std::unique_ptr<Mtp::Session> session;
    std::unique_ptr<DeviceFiles> files;
    KindleSend::Device kindle;

    bool isKindle() const { return files != nullptr; }
};

static bool openReader(const QString &spec, Reader *reader)
{
    QString error;
    if (spec.startsWith(QLatin1String("mtpdir:"))) {
        reader->files = std::make_unique<FolderFiles>(spec.mid(7), true);
        reader->kindle = KindleSend::standIn();
    } else if (spec == QLatin1String("mtp") || spec.startsWith(QLatin1String("mtp:"))) {
        const QString serial = spec.mid(4);
        for (const Mtp::RawDevice &raw : Mtp::detect(&error)) {
            if (raw.vendorId != Kindle::VendorId)
                continue;
            auto session = std::make_unique<Mtp::Session>();
            if (!session->open(raw.bus, raw.number, &error) || (!serial.isEmpty() && session->serialNumber() != serial))
                continue;
            reader->kindle = { session->friendlyName(), session->modelName(), session->serialNumber(), raw.vendorId,
                               raw.productId };
            reader->files = std::make_unique<MtpFiles>(session.get(), true);
            reader->session = std::move(session);
            break;
        }
        if (!reader->files) {
            err() << (error.isEmpty() ? QStringLiteral("No Kindle is connected over MTP.") : error) << Qt::endl;
            return false;
        }
    } else {
        if (!Kobo::isKobo(spec)) {
            err() << QStringLiteral("%1 is not a Kobo: it has no .kobo folder.").arg(spec) << Qt::endl;
            return false;
        }
        reader->kobo = Kobo::describe(QFileInfo(spec).absoluteFilePath(), QStorageInfo(spec).device());
        reader->name = reader->kobo.name;
        return true;
    }
    if (!reader->files->scan(&error)) {
        err() << error << Qt::endl;
        return false;
    }
    reader->name = Kindle::name(Kindle::readDriveInfo(*reader->files), reader->kindle.friendlyName,
                                reader->kindle.modelName);
    return true;
}

static QList<DeviceBook> booksOn(Reader &reader, QString *error)
{
    return reader.isKindle() ? Kindle::books(*reader.files, error) : Kobo::books(reader.kobo, error);
}

// Sends the books to the reader as Calibre's "Send to device" does. With
// COUSOIR_KEEP_JOBS set, the job's folder stays for a look at Calibre's log.
bool send(const QString &libraryPath, const CalibreConfig &config, const QString &spec, const QList<int> &bookIds)
{
    LibraryData data;
    Reader reader;
    if (!load(libraryPath, &data) || !openReader(spec, &reader))
        return false;
    QTemporaryDir job;
    job.setAutoRemove(qEnvironmentVariableIsEmpty("COUSOIR_KEEP_JOBS"));
    QString error;
    if (!job.isValid()) {
        err() << QStringLiteral("Cannot create a temporary folder.") << Qt::endl;
        return false;
    }
    QList<SendJob::Sent> sent;
    QStringList unsendable;
    // Cousoir alone without Calibre, or when asked to.
    const bool native = SendJob::program().isEmpty() || !qEnvironmentVariableIsEmpty("COUSOIR_NATIVE_SEND");
    if (reader.isKindle()) {
        // What Calibre does as it connects.
        Kindle::restoreThumbnails(*reader.files, &error);
        const QStringList formats = Kindle::formatMap(CalibreConfig::configDirectory(), reader.kindle.serial);
        const bool prepared = native ? KindleSend::prepareNatively(data, bookIds, *reader.files, reader.kindle, formats,
                                                                   CalibreConfig::configDirectory(),
                                                                   config.interfaceLanguage, job.path(), &error)
                                     : KindleSend::prepare(data, bookIds, *reader.files, reader.kindle, formats,
                                                           job.path(), &error);
        if (!prepared) {
            err() << error << Qt::endl;
            return false;
        }
        const KindleSend::Outcome outcome = native ? KindleSend::outcome(job.path(), 0)
                                                   : KindleSend::run(job.path(), config.interfaceLanguage);
        if (!outcome.ok || !KindleSend::apply(job.path(), outcome, *reader.files, &error)) {
            err() << (outcome.ok ? error : outcome.error) << Qt::endl;
            return false;
        }
        sent = outcome.sent;
        unsendable = outcome.unsendable;
    } else {
        const QStringList formats = Kobo::formatMap(CalibreConfig::configDirectory());
        const bool prepared = native ? KoboSend::prepareNatively(data, bookIds, reader.kobo, formats,
                                                                 CalibreConfig::configDirectory(),
                                                                 config.interfaceLanguage, job.path(), &error)
                                     : KoboSend::prepare(data, bookIds, reader.kobo, formats, job.path(), &error);
        if (!prepared) {
            err() << error << Qt::endl;
            return false;
        }
        const KoboSend::Outcome outcome = native ? KoboSend::outcome(job.path(), 0)
                                                 : KoboSend::run(job.path(), config.interfaceLanguage);
        if (!outcome.ok || !KoboSend::apply(job.path(), outcome, reader.kobo, &error)) {
            err() << (outcome.ok ? error : outcome.error) << Qt::endl;
            return false;
        }
        sent = outcome.sent;
        unsendable = outcome.unsendable;
    }
    for (const QString &title : std::as_const(unsendable))
        err() << QStringLiteral("Not sent, no format the device reads: %1").arg(title) << Qt::endl;
    out() << QStringLiteral("Sent %1 book(s) to %2").arg(sent.size()).arg(reader.name) << Qt::endl;
    return true;
}

bool removeFromDevice(const QString &spec, const QStringList &lpaths)
{
    Reader reader;
    if (!openReader(spec, &reader))
        return false;
    QString error;
    if (reader.isKindle())
        Kindle::restoreThumbnails(*reader.files, &error);
    QList<DeviceBook> books = booksOn(reader, &error);
    books.removeIf([&](const DeviceBook &book) { return !lpaths.contains(book.lpath, Qt::CaseInsensitive); });
    if (books.size() != lpaths.size()) {
        err() << QStringLiteral("Not all of these books are on the device.") << Qt::endl;
        return false;
    }
    if (reader.isKindle() ? !Kindle::removeBooks(*reader.files, books, &error)
                          : !Kobo::removeBooks(reader.kobo, books, &error)) {
        err() << error << Qt::endl;
        return false;
    }
    return true;
}

// The books on the reader, matched with the library's.
int device(const QString &libraryPath, const QString &spec)
{
    LibraryData data;
    Reader reader;
    if (!load(libraryPath, &data) || !openReader(spec, &reader))
        return 1;
    QString error;
    QList<DeviceBook> books = booksOn(reader, &error);
    if (!error.isEmpty())
        err() << error << Qt::endl;
    DeviceBooks::match(&books, data);
    QJsonArray list;
    for (const DeviceBook &book : std::as_const(books)) {
        // What the reader knows of the book's reading, as
        // tools/oracle/kobo_reading.py prints Calibre's.
        QJsonArray notes;
        for (const ReaderNote &note : book.reading.notes) {
            notes.append(QJsonObject{
                { QStringLiteral("location"), note.location },
                { QStringLiteral("type"), note.type },
                { QStringLiteral("text"), note.text },
                { QStringLiteral("annotation"), note.annotation },
                { QStringLiteral("chapter"), note.chapter },
                { QStringLiteral("chapter_title"), note.chapterTitle },
                { QStringLiteral("chapter_progress"), note.chapterProgress },
                { QStringLiteral("created"), note.created },
            });
        }
        const QJsonObject reading{
            { QStringLiteral("status"), book.reading.status },
            { QStringLiteral("percent_read"), book.reading.percent ? QJsonValue(*book.reading.percent) : QJsonValue() },
            { QStringLiteral("last_read"), book.reading.lastRead.isEmpty() ? QJsonValue() : QJsonValue(book.reading.lastRead) },
            { QStringLiteral("seconds_spent"), book.reading.secondsSpent },
            { QStringLiteral("notes"), notes },
        };
        list.append(QJsonObject{
            { QStringLiteral("lpath"), book.lpath },
            { QStringLiteral("content_id"), book.contentId },
            { QStringLiteral("reading"), reading },
            { QStringLiteral("title"), book.title },
            { QStringLiteral("authors"), QJsonArray::fromStringList(book.authors) },
            { QStringLiteral("format"), book.format },
            { QStringLiteral("size"), book.size },
            { QStringLiteral("sent"), book.sent },
            { QStringLiteral("imported"), book.imported },
            { QStringLiteral("purchased"), book.purchased },
            { QStringLiteral("book"), book.bookId },
        });
    }
    out() << QJsonDocument(QJsonObject{ { QStringLiteral("name"), reader.name },
                                         { QStringLiteral("books"), list } })
                 .toJson();
    return 0;
}

// A Kobo's highlights and notes, by the folder it is mounted on, all of
// them, or of a Kobo's database file, which may be old: whose they are, its
// serial number, else a name.
bool readKobo(const QString &spec, KoboHighlights::Snapshot *snapshot)
{
    const bool folder = QFileInfo(spec).isDir();
    const QString database = folder ? QDir(spec).filePath(QStringLiteral(".kobo/KoboReader.sqlite")) : spec;
    SqliteDb db;
    QTemporaryDir copy;
    QString error;
    if (!QFileInfo(database).isFile()) {
        err() << QStringLiteral("No Kobo database at %1.").arg(database) << Qt::endl;
        return false;
    }
    if (!Kobo::openDatabase(database, &db, &copy, &error)) {
        err() << error << Qt::endl;
        return false;
    }
    snapshot->books = KoboHighlights::read(db, &error);
    if (!error.isEmpty()) {
        err() << error << Qt::endl;
        return false;
    }
    const KoboHighlights::Identity identity = KoboHighlights::identify(database);
    snapshot->name = identity.name;
    snapshot->source = folder ? QStringLiteral("usb") : QStringLiteral("file");
    snapshot->seen = QDateTime::currentDateTimeUtc();
    if (folder) {
        const Kobo::Device kobo = Kobo::describe(spec, {});
        KoboHighlights::attach(&snapshot->books, Kobo::books(kobo, &error));
        snapshot->name = kobo.name;
    }
    snapshot->device = identity.serial.isEmpty() ? snapshot->name : identity.serial;
    return true;
}

// The highlights and notes of a Kobo, by the folder it is mounted on, or of
// a Kobo's database file: each book, the library's book it is, and each
// highlight as the Kobo keeps it and where it is in the library's copy.
// Neither is written.
int koboHighlights(const QString &libraryPath, const QString &spec)
{
    LibraryData data;
    KoboHighlights::Snapshot snapshot;
    if (!load(libraryPath, &data) || !readKobo(spec, &snapshot))
        return 1;
    const QList<KoboHighlights::Book> &books = snapshot.books;
    const QList<int> ids = KoboHighlights::match(books, data);
    QJsonArray list = KoboHighlights::toJson(books);
    QHash<QString, int> counts;
    for (qsizetype i = 0; i < list.size(); ++i) {
        QJsonObject book = list.at(i).toObject();
        book.insert(QStringLiteral("book"), ids.at(i));
        const Book *libraryBook = data.book(ids.at(i));
        const QString format = libraryBook ? KoboPlacement::targetFormat(*libraryBook) : QString();
        std::unique_ptr<KoboPlacement::PlacementBook> placing;
        QString reason = !libraryBook ? QStringLiteral("not in the library")
            : format.isEmpty()        ? QStringLiteral("no EPUB in the library")
                                      : QString();
        if (reason.isEmpty()) {
            for (const BookFormat &file : libraryBook->formats) {
                if (file.format == format)
                    placing = KoboPlacement::PlacementBook::open(file.file, format, &reason);
            }
        }
        book.insert(QStringLiteral("library_format"), format);
        QJsonArray highlights = book.value(QLatin1String("highlights")).toArray();
        for (qsizetype h = 0; h < highlights.size(); ++h) {
            const KoboHighlights::Highlight &highlight = books.at(i).highlights.at(h);
            QJsonObject entry = highlights.at(h).toObject();
            if (!libraryBook) {
                ++counts[QStringLiteral("not_in_library")];
            } else if (!placing) {
                entry.insert(QStringLiteral("placement"), QJsonObject{ { QStringLiteral("method"), QStringLiteral("none") },
                                                                       { QStringLiteral("reason"), reason } });
                ++counts[QStringLiteral("none")];
            } else {
                const KoboPlacement::Placement placement = placing->place(highlight);
                entry.insert(QStringLiteral("placement"),
                             QJsonObject{ { QStringLiteral("method"), KoboPlacement::methodName(placement.method) },
                                          { QStringLiteral("spine_index"), placement.spineIndex },
                                          { QStringLiteral("spine_name"), placement.spineName },
                                          { QStringLiteral("start_cfi"), placement.startCfi },
                                          { QStringLiteral("end_cfi"), placement.endCfi },
                                          { QStringLiteral("toc_family_titles"), QJsonArray::fromStringList(placement.tocFamily) },
                                          { QStringLiteral("reason"), placement.reason } });
                ++counts[KoboPlacement::methodName(placement.method)];
            }
            highlights[h] = entry;
        }
        book.insert(QStringLiteral("highlights"), highlights);
        list[i] = book;
    }
    QJsonObject summary;
    for (const QString &key : { QStringLiteral("spans"), QStringLiteral("text"), QStringLiteral("book"), QStringLiteral("none"),
                                QStringLiteral("not_in_library") })
        summary.insert(key, counts.value(key));
    out() << QJsonDocument(QJsonObject{ { QStringLiteral("device"), snapshot.device },
                                         { QStringLiteral("name"), snapshot.name },
                                         { QStringLiteral("summary"), summary },
                                         { QStringLiteral("books"), list } })
                 .toJson();
    return 0;
}

} // namespace Cli
