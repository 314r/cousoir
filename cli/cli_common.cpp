// cousoir-cli: what its commands share: printing, reading the library,
// saving changes.

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

} // namespace Cli
