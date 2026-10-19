#include "reliure_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>

namespace {

QString libraryKey(const QString &libraryPath)
{
    return QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(libraryPath).toUtf8(), QCryptographicHash::Sha1)
                                   .toHex().left(16));
}

} // namespace

namespace ReliurePaths {

QString cacheFolder(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/reliure/")
        + libraryKey(libraryPath);
}

QString backupFolder(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/reliure/backups/")
        + libraryKey(libraryPath);
}

} // namespace ReliurePaths
