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
#include <QPointer>
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
#include "kindle.h"
#include "kindle_send.h"
#include "kobo.h"
#include "kobo_send.h"
#include "library_check.h"
#include "library_server.h"
#include "metadata_download.h"
#include "mtp.h"
#include "open_library.h"
#include "cousoir_paths.h"
#include "wireless.h"
#include "wireless_send.h"

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

// Reads the library from a copy of metadata.db in a temporary folder, so a
// running Cousoir's own copy is left alone.
bool load(const QString &libraryPath, LibraryData *data)
{
    QTemporaryDir folder;
    QString error;
    if (!folder.isValid()
        || !CalibreDb::loadLibrary(libraryPath, folder.filePath(QStringLiteral("snapshot.db")), CalibreConfig::load(),
                                   data, &error)) {
        err() << (error.isEmpty() ? QStringLiteral("Cannot create a temporary folder.") : error) << Qt::endl;
        return false;
    }
    return true;
}

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

// Serves the library on the network, as the application shares it, until
// stopped; the books are read again when Calibre changes them.
int serve(const QString &libraryPath, int port, const QString &password)
{
    struct Loaded
    {
        LibraryData data;
        std::unique_ptr<CalibreSearch::Index> index;
        std::unique_ptr<BookSort::Sorter> sorter;
    };
    const auto read = [&libraryPath]() -> std::unique_ptr<Loaded> {
        auto loaded = std::make_unique<Loaded>();
        if (!load(libraryPath, &loaded->data))
            return {};
        const CalibreConfig config = CalibreConfig::load();
        loaded->index = std::make_unique<CalibreSearch::Index>(
            loaded->data, CalibreSearch::Preferences::fromRows(loaded->data.preferences), config.tweaks,
            config.interfaceLanguage);
        loaded->sorter = std::make_unique<BookSort::Sorter>(loaded->data, config);
        return loaded;
    };
    std::unique_ptr<Loaded> library = read();
    if (!library)
        return 2;

    LibraryServer server;
    const auto publish = [&] {
        LibraryServer::Catalog catalog;
        catalog.data = &library->data;
        catalog.find = [&library](const QString &query, const QList<BookSort::Level> &sort, QString *error) {
            return LibraryServer::findBooks(library->data, *library->index, library->sorter.get(), query, sort, error);
        };
        catalog.name = QDir(libraryPath).dirName();
        catalog.interfaceLanguage = CalibreConfig::load().interfaceLanguage;
        server.setCatalog(catalog);
    };
    publish();
    server.setPassword(password);
    const QString error = server.listen(quint16(port));
    if (!error.isEmpty()) {
        err() << error << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("Serving %1 (%2 books), until stopped, at:")
                 .arg(QDir::toNativeSeparators(libraryPath))
                 .arg(library->data.books.size())
          << Qt::endl;
    QStringList addresses = LibraryServer::addresses(server.port());
    addresses.prepend(QStringLiteral("http://localhost:%1/").arg(server.port()));
    for (const QString &address : std::as_const(addresses))
        out() << QStringLiteral("  %1  (the catalog for readers: %1opds)").arg(address) << Qt::endl;
    QObject::connect(&server, &LibraryServer::served, [](const QString &request, int status) {
        err() << status << u' ' << request << Qt::endl;
    });

    // Calibre's changes, seen in metadata.db.
    QTimer watch;
    watch.setInterval(2000);
    QObject::connect(&watch, &QTimer::timeout, [&] {
        const QString database = QDir(libraryPath).filePath(QStringLiteral("metadata.db"));
        if (CalibreDb::identify(database) == library->data.source)
            return;
        if (std::unique_ptr<Loaded> again = read()) {
            library = std::move(again);
            publish();
            err() << QStringLiteral("Read the library again: %1 books.").arg(library->data.books.size())
                  << Qt::endl;
        }
    });
    watch.start();
    return QCoreApplication::exec();
}

// Calibre's wireless device server for the library: an app that connects is
// listed and given the metadata the library changed since, as Calibre does
// on connect; then the operations of `opsPath`, if any, are done in order,
// and the app is let go. Without operations, it serves until stopped.
//   [{ "send": [1, 2] }, { "remove": ["Title - Author (1).epub"] }]
int wireless(const QString &libraryPath, int port, const QString &password, const QString &opsPath, int waitSeconds)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return 2;
    QJsonArray ops;
    if (!opsPath.isEmpty()) {
        QFile file(opsPath);
        QJsonParseError parseError;
        const QJsonDocument document = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll(), &parseError)
                                                                       : QJsonDocument();
        if (!document.isArray()) {
            err() << QStringLiteral("Cannot read %1 as a JSON array.").arg(opsPath) << Qt::endl;
            return 2;
        }
        ops = document.array();
    }
    const CalibreConfig config = CalibreConfig::load();
    WirelessServer server;
    WirelessServer::Settings settings;
    settings.port = quint16(port);
    settings.contentServerPort = 8090;
    settings.connection.password = password;
    settings.connection.libraryName = QDir(libraryPath).dirName();
    settings.connection.libraryUuid = data.uuid;
    settings.connection.interfaceLanguage = config.interfaceLanguage;
    settings.connection.cacheFolder = QDir(CousoirPaths::cacheFolder(libraryPath)).filePath(QStringLiteral("wireless"));
    QString error;
    if (!server.start(settings, &error)) {
        err() << error << Qt::endl;
        return 1;
    }
    err() << QStringLiteral("Waiting for a reader app on port %1 (datagrams on %2).")
                 .arg(server.port())
                 .arg(server.broadcastPort())
          << Qt::endl;

    // With operations, one app and done; without, serving until stopped.
    const bool once = !opsPath.isEmpty();
    int status = 0;
    qsizetype step = 0;
    QPointer<WirelessConnection> connection;
    std::function<void()> nextStep;
    nextStep = [&] {
        if (!connection)
            return;
        if (step >= ops.size()) {
            if (once)
                connection->eject();
            return;
        }
        const QJsonObject op = ops.at(step++).toObject();
        if (op.contains(QLatin1String("send"))) {
            QList<Wireless::Outgoing> books;
            for (const QJsonValue &id : op.value(QLatin1String("send")).toArray()) {
                const Book *book = data.book(id.toInt());
                const QString format = book ? WirelessSend::format(*book, connection->app()) : QString();
                if (format.isEmpty()) {
                    err() << QStringLiteral("Book %1: nothing the reader takes.").arg(id.toInt()) << Qt::endl;
                    status = 1;
                    continue;
                }
                QString file;
                for (const BookFormat &bookFormat : book->formats) {
                    if (bookFormat.format.compare(format, Qt::CaseInsensitive) == 0)
                        file = bookFormat.file;
                }
                const QString extension = u'.' + format.toLower();
                QString lpath = connection->lpathOfUuid(book->uuid, extension);
                if (lpath.isEmpty())
                    lpath = WirelessSend::uploadPath(data, *book, extension, connection->app(), config.interfaceLanguage);
                books.append({ lpath,
                               WirelessSend::metadata(data, *book, lpath, QFileInfo(file).size(), connection->app(),
                                                      config.interfaceLanguage),
                               file });
            }
            connection->send(books);
        } else if (op.contains(QLatin1String("remove"))) {
            QStringList lpaths;
            for (const QJsonValue &lpath : op.value(QLatin1String("remove")).toArray())
                lpaths.append(lpath.toString());
            connection->remove(lpaths);
        } else {
            nextStep();
        }
    };
    QTimer giveUp;
    giveUp.setSingleShot(true);
    QObject::connect(&giveUp, &QTimer::timeout, [&] {
        err() << QStringLiteral("No reader app connected.") << Qt::endl;
        QCoreApplication::exit(1);
    });
    if (once)
        giveUp.start(waitSeconds * 1000);
    QObject::connect(&server, &WirelessServer::connected, [&](WirelessConnection *opened) {
        giveUp.stop();
        connection = opened;
        out() << QStringLiteral("%1 connected: %2 books.").arg(opened->name()).arg(opened->books().size()) << Qt::endl;
        for (const DeviceBook &book : WirelessSend::deviceBooks(opened->books(), data))
            out() << QStringLiteral("  %1  %2").arg(book.lpath, book.bookId ? QStringLiteral("(book %1)").arg(book.bookId)
                                                                            : QString())
                  << Qt::endl;
        QObject::connect(opened, &WirelessConnection::finished, [&](bool ok, const QString &message) {
            if (!ok) {
                err() << message << Qt::endl;
                status = 1;
            }
            nextStep();
        });
        // As Calibre on connect: the metadata the library changed since.
        opened->updateMetadata(
            WirelessSend::changedMetadata(opened->books(), data, opened->app(), config.interfaceLanguage));
    });
    QObject::connect(&server, &WirelessServer::disconnected, [&](const QString &reason) {
        out() << reason << Qt::endl;
        if (once)
            QCoreApplication::exit(status);
    });
    return QCoreApplication::exec();
}

QString ago(qint64 seconds)
{
    if (seconds < 120)
        return QStringLiteral("%1 s ago").arg(seconds);
    if (seconds < 7200)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 172800)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    return QStringLiteral("%1 days ago").arg(seconds / 86400);
}

void row(const QString &label, const QString &value)
{
    out() << QStringLiteral("  %1 %2").arg(label.leftJustified(14), value) << Qt::endl;
}

int doctor(const QString &libraryPath)
{
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

    out() << QStringLiteral("Library") << Qt::endl;
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

    out() << Qt::endl << QStringLiteral("Problems") << Qt::endl;
    if (problems.isEmpty())
        out() << QStringLiteral("  None") << Qt::endl;
    for (const QString &problem : std::as_const(problems))
        out() << QStringLiteral("  ") << problem << Qt::endl;

    const QString database = library.filePath(QStringLiteral("metadata.db"));
    const FileIdentity identity = CalibreDb::identify(database);
    const QDateTime changed = QDateTime::fromMSecsSinceEpoch(identity.modifiedMs);
    out() << Qt::endl << QStringLiteral("Writing") << Qt::endl;
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
    return problems.isEmpty() ? 0 : 1;
}

// The checks the application makes before writing, Dropbox's answer
// included; empty when the library could be written now.
QString readOnlyReason(const QString &libraryPath)
{
    LibraryData data;
    if (!load(libraryPath, &data))
        return QStringLiteral("the library cannot be read");
    ConcurrencyGuard guard;
    guard.setLibraryReadable(true);
    guard.setSchemaKnown(data.schemaKnown);
    guard.setLibraryUuid(data.uuid);
    guard.setLibraryPath(data.libraryPath);
    if (guard.state() == QLatin1String("dropbox")) {
        QEventLoop loop;
        QObject::connect(&guard, &ConcurrencyGuard::stateChanged, &loop, [&] {
            if (!guard.dropboxStatus().isEmpty())
                loop.quit();
        });
        QTimer::singleShot(3500, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return guard.writable() ? QString() : guard.readOnlyReason();
}

// Converts the books to the formats, as Calibre's convert action does, and
// adds what it adds to `request`. The jobs' folders stay until the process
// ends, with the files the request adds; with COUSOIR_KEEP_JOBS set, after,
// for a look at Calibre's log.
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
        QString error;
        if (!jobs.isValid() || !QDir().mkpath(folder)
            || !Conversion::prepare(data, *book, format, config.interfaceLanguage, folder, &error)) {
            err() << (error.isEmpty() ? QStringLiteral("Cannot create a temporary folder.") : error) << Qt::endl;
            return false;
        }
        const Conversion::Outcome outcome = Conversion::run(folder, config.interfaceLanguage);
        if (!outcome.ok) {
            err() << book->title << QStringLiteral(": ") << outcome.error << Qt::endl;
            return false;
        }
        Conversion::addTo(request, *book, format, outcome);
    }
    return true;
}

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

bool openReader(const QString &spec, Reader *reader)
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

QList<DeviceBook> booksOn(Reader &reader, QString *error)
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

QJsonArray pairs(const QList<QPair<QString, QString>> &list)
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

// A device of an apply file, its folders relative to the file.
QString deviceSpec(const QDir &base, const QString &device)
{
    if (device == QLatin1String("mtp") || device.startsWith(QLatin1String("mtp:")))
        return device;
    if (device.startsWith(QLatin1String("mtpdir:")))
        return QStringLiteral("mtpdir:") + base.absoluteFilePath(device.mid(7));
    return base.absoluteFilePath(device);
}

// Saves the library changes of `request`.
int commit(const QString &libraryPath, const CalibreConfig &config, CommitEngine::Request &request)
{
    request.libraryPath = libraryPath;
    request.cacheFolder = CousoirPaths::cacheFolder(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath()));
    request.backupFolder = CousoirPaths::backupFolder(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath()));
    request.config = config;
    request.producer = QStringLiteral("Cousoir (%1)").arg(QStringLiteral(COUSOIR_VERSION));
    request.producerId = QStringLiteral("cousoir");
    const CommitEngine::Result result = CommitEngine::commit(request);
    if (!result.ok) {
        err() << result.error << Qt::endl;
        return 1;
    }
    // The other computers sharing the library hold off, as after the
    // application's change.
    if (result.written) {
        ConcurrencyGuard guard;
        guard.setLibraryUuid(result.data.uuid);
        guard.setLibraryPath(libraryPath);
        guard.noteWrite();
    }
    QStringList added;
    for (const int id : std::as_const(result.added))
        added.append(QString::number(id));
    out() << QStringLiteral("Changed %1 book(s)%2; backup %3")
                 .arg(result.dirtied.size())
                 .arg(added.isEmpty() ? QString() : QStringLiteral(", added %1").arg(added.join(QStringLiteral(", "))))
                 .arg(result.backup)
          << Qt::endl;
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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("cousoir"));
    app.setApplicationName(QStringLiteral("cousoir"));
    app.setApplicationVersion(QStringLiteral(COUSOIR_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Reads a Calibre library without changing it, unless told to apply changes.\n\n"
        "Commands:\n"
        "  list    Print the books as `calibredb list --for-machine --fields all` does.\n"
        "  search  Print the ids of the books an expression of Calibre's search\n"
        "          language finds, as `calibredb search` does.\n"
        "  doctor  Report missing files, conflict copies and whether the library could be written now.\n"
        "  apply   Apply the changes of a JSON file (edits, added and removed books and\n"
        "          formats, restorations, conversions, books sent to or removed from a\n"
        "          Kobo), as the application makes them.\n"
        "  read    Print the metadata adding the given book files would read.\n"
        "  device  Print the books on a reader: a Kobo by the folder it is mounted on,\n"
        "          \"mtp\" or \"mtp:SERIAL\" for a Kindle connected over MTP.\n"
        "  template  Print what each template of a JSON array makes of each book.\n"
        "  download  Print what Calibre's metadata sources, on the web, find for the\n"
        "          book of the id given, and what it would change; --covers adds the\n"
        "          covers found. Without Calibre, Open Library is asked.\n"
        "  kepubify  Make a KEPUB of an EPUB, as Calibre does, without Calibre:\n"
        "          kepubify BOOK.epub BOOK.kepub\n"
        "  wireless  Let reader apps (KOReader, Calibre Companion) connect as to\n"
        "          Calibre's wireless device server; with a JSON file of operations,\n"
        "          send or remove books once one connects, then let it go.\n"
        "  serve   Share the library on the local network until stopped: an OPDS\n"
        "          catalog for e-readers and reading apps, a web page for browsers.\n\n"
        "Cousoir follows Calibre's rules, ported from the code of Calibre, by Kovid\n"
        "Goyal and the Calibre contributors, https://calibre-ebook.com. GPL v3."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption(QStringList{ QStringLiteral("library"), QStringLiteral("with-library") },
                                           QStringLiteral("The library folder; by default the one Calibre opens."),
                                           QStringLiteral("path"));
    parser.addOption(libraryOption);
    const QCommandLineOption forceOption(QStringLiteral("force"),
                                         QStringLiteral("apply: skip the sync, recent-change and conflict checks; "
                                                        "Calibre's lock still applies."));
    parser.addOption(forceOption);
    const QCommandLineOption limitOption(QStringList{ QStringLiteral("l"), QStringLiteral("limit") },
                                         QStringLiteral("search: the most ids to print."), QStringLiteral("n"),
                                         QStringLiteral("-1"));
    parser.addOption(limitOption);
    const QCommandLineOption coversOption(QStringLiteral("covers"), QStringLiteral("download: the covers too."));
    parser.addOption(coversOption);
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("serve: the port, 8090 by default."),
                                        QStringLiteral("port"), QStringLiteral("8090"));
    parser.addOption(portOption);
    const QCommandLineOption passwordOption(QStringLiteral("password"),
                                            QStringLiteral("serve: a password to ask for, any user name."),
                                            QStringLiteral("password"));
    parser.addOption(passwordOption);
    const QCommandLineOption waitOption(QStringLiteral("wait"),
                                        QStringLiteral("wireless: seconds to wait for an app, 60 by default."),
                                        QStringLiteral("seconds"), QStringLiteral("60"));
    parser.addOption(waitOption);
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("list, search, doctor, apply, read, device, template, download, kepubify, wireless or serve"));
    parser.addPositionalArgument(QStringLiteral("edits"), QStringLiteral("apply: a JSON file of edits"), QStringLiteral("[edits]"));
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("read: book files"), QStringLiteral("[files…]"));
    parser.process(app);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.value(0) == QLatin1String("read") && arguments.size() > 1)
        return read(arguments.mid(1));
    if (arguments.value(0) == QLatin1String("search")) {
        if (arguments.size() < 2) {
            err() << QStringLiteral("Error: You must specify the search expression") << Qt::endl;
            return 2;
        }
        const QString path = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                         : CalibreConfig::defaultLibraryPath();
        if (path.isEmpty()) {
            err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
            return 2;
        }
        return search(path, arguments.mid(1).join(u' '), parser.value(limitOption).toInt());
    }
    if (arguments.value(0) == QLatin1String("kepubify") && arguments.size() == 3) {
        QString error;
        Kepub::Options options;
        if (!Kepub::kepubify(arguments.at(1), arguments.at(2), options, &error)) {
            err() << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    if (arguments.value(0) == QLatin1String("wireless") && arguments.size() <= 2) {
        const QString path = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                         : CalibreConfig::defaultLibraryPath();
        if (path.isEmpty()) {
            err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
            return 2;
        }
        return wireless(path, parser.isSet(portOption) ? parser.value(portOption).toInt() : 0,
                        parser.value(passwordOption), arguments.value(1), parser.value(waitOption).toInt());
    }
    const bool takesOne = arguments.value(0) == QLatin1String("apply") || arguments.value(0) == QLatin1String("device")
        || arguments.value(0) == QLatin1String("template") || arguments.value(0) == QLatin1String("download");
    if (arguments.isEmpty() || arguments.size() > 2 || (arguments.size() == 2) != takesOne)
        parser.showHelp(2);
    const QString libraryPath = parser.isSet(libraryOption) ? parser.value(libraryOption)
                                                            : CalibreConfig::defaultLibraryPath();
    if (libraryPath.isEmpty()) {
        err() << QStringLiteral("Calibre has no library on this computer; pass --library.") << Qt::endl;
        return 2;
    }
    if (arguments.first() == QLatin1String("list"))
        return list(libraryPath);
    if (arguments.first() == QLatin1String("doctor"))
        return doctor(libraryPath);
    if (arguments.first() == QLatin1String("serve"))
        return serve(libraryPath, parser.value(portOption).toInt(), parser.value(passwordOption));
    if (arguments.first() == QLatin1String("apply"))
        return apply(libraryPath, arguments.at(1), parser.isSet(forceOption));
    if (arguments.first() == QLatin1String("device"))
        return device(libraryPath, arguments.at(1));
    if (arguments.first() == QLatin1String("template"))
        return templates(libraryPath, arguments.at(1));
    if (arguments.first() == QLatin1String("download"))
        return download(libraryPath, arguments.at(1), parser.isSet(coversOption));
    err() << QStringLiteral("Unknown command “%1”.").arg(arguments.first()) << Qt::endl;
    return 2;
}
