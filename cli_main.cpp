#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

#include "book_edit.h"
#include "calibre_db.h"
#include "calibre_list.h"
#include "commit_engine.h"
#include "concurrency_guard.h"
#include "reliure_paths.h"

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
// running Reliure's own copy is left alone.
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

    qint64 files = 0;
    qint64 bytes = 0;
    int covers = 0;
    QStringList problems;
    const auto relative = [&library](const QString &path) { return library.relativeFilePath(path); };
    for (const Book &book : std::as_const(data.books)) {
        const QString name = QStringLiteral("Book %1 “%2”").arg(book.id).arg(book.title);
        const QString folder = library.filePath(book.path);
        if (book.path.isEmpty() || !QFileInfo(folder).isDir()) {
            problems.append(QStringLiteral("%1: its folder is missing\n      %2").arg(name, book.path));
            continue;
        }
        for (const BookFormat &format : book.formats) {
            const QString expected = QDir(folder).filePath(format.name + u'.' + format.format.toLower());
            if (format.file.isEmpty()) {
                problems.append(QStringLiteral("%1: the %2 file is missing\n      %3")
                                    .arg(name, format.format, relative(expected)));
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
                                       "Reliure stays read-only").arg(data.userVersion).arg(data.schemaFingerprint));
    }

    // The guard answers about Dropbox asynchronously; give it a moment.
    ConcurrencyGuard guard;
    guard.setLibraryReadable(true);
    guard.setSchemaKnown(data.schemaKnown);
    guard.setLibraryPath(data.libraryPath);
    QEventLoop loop;
    QObject::connect(&guard, &ConcurrencyGuard::stateChanged, &loop, [&] {
        if (!guard.dropboxStatus().isEmpty())
            loop.quit();
    });
    QTimer::singleShot(3500, &loop, &QEventLoop::quit);
    if (!ConcurrencyGuard::dropboxRoots(ConcurrencyGuard::Environment::system().dropboxInfo).isEmpty())
        loop.exec();
    for (const QVariant &value : guard.conflicts()) {
        const QVariantMap conflict = value.toMap();
        const bool seen = conflict.value(QStringLiteral("acknowledged")).toBool();
        const QDateTime modified = conflict.value(QStringLiteral("modified")).toDateTime();
        problems.append(QStringLiteral("Dropbox conflict copy%1: %2, %3, %4")
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
    row(QStringLiteral("Dropbox"), guard.dropboxStatus().isEmpty() ? QStringLiteral("not in Dropbox")
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

// Applies the edits of a JSON file: [{ "book": 1, "set": { "title": …, … } }].
// The keys are BookEdit's; a coverFile is relative to the JSON file.
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
    CommitEngine::Request request;
    for (const QJsonValue &value : document.array()) {
        const QJsonObject op = value.toObject();
        QVariantMap changes = op.value(QLatin1String("set")).toObject().toVariantMap();
        if (changes.contains(QStringLiteral("coverFile"))) {
            changes.insert(QStringLiteral("coverFile"), QFileInfo(opsPath).absoluteDir().absoluteFilePath(
                                                            changes.value(QStringLiteral("coverFile")).toString()));
        }
        BookEdit edit;
        QString error;
        if (!BookEdit::fromVariantMap(op.value(QLatin1String("book")).toInt(), changes, &edit, &error)) {
            err() << error << Qt::endl;
            return 2;
        }
        request.edits.append(edit);
    }
    if (!force) {
        const QString reason = readOnlyReason(libraryPath);
        if (!reason.isEmpty()) {
            err() << reason << Qt::endl;
            return 1;
        }
    }
    request.libraryPath = libraryPath;
    request.cacheFolder = ReliurePaths::cacheFolder(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath()));
    request.backupFolder = ReliurePaths::backupFolder(QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath()));
    request.config = CalibreConfig::load();
    request.producer = QStringLiteral("Reliure (%1)").arg(QStringLiteral(RELIURE_VERSION));
    request.producerId = QStringLiteral("reliure");
    const CommitEngine::Result result = CommitEngine::commit(request);
    if (!result.ok) {
        err() << result.error << Qt::endl;
        return 1;
    }
    out() << QStringLiteral("Changed %1 book(s); backup %2").arg(result.dirtied.size()).arg(result.backup) << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("reliure"));
    app.setApplicationName(QStringLiteral("reliure"));
    app.setApplicationVersion(QStringLiteral(RELIURE_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Reads a Calibre library without changing it.\n\n"
        "Commands:\n"
        "  list    Print the books as `calibredb list --for-machine --fields all` does.\n"
        "  doctor  Report missing files, conflict copies and whether the library could be written now.\n"
        "  apply   Apply the edits of a JSON file, as the application saves them."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption libraryOption(QStringList{ QStringLiteral("library"), QStringLiteral("with-library") },
                                           QStringLiteral("The library folder; by default the one Calibre opens."),
                                           QStringLiteral("path"));
    parser.addOption(libraryOption);
    const QCommandLineOption forceOption(QStringLiteral("force"),
                                         QStringLiteral("apply: skip the Dropbox and recent-change checks."));
    parser.addOption(forceOption);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("list, doctor or apply"));
    parser.addPositionalArgument(QStringLiteral("edits"), QStringLiteral("apply: a JSON file of edits"), QStringLiteral("[edits]"));
    parser.process(app);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty() || arguments.size() > 2 || (arguments.size() == 2) != (arguments.first() == QLatin1String("apply")))
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
    if (arguments.first() == QLatin1String("apply"))
        return apply(libraryPath, arguments.at(1), parser.isSet(forceOption));
    err() << QStringLiteral("Unknown command “%1”.").arg(arguments.first()) << Qt::endl;
    return 2;
}
