#include "cousoir_paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>

namespace CousoirPaths {

QString libraryKey(const QString &libraryPath)
{
    return QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(libraryPath).toUtf8(), QCryptographicHash::Sha1)
                                   .toHex().left(16));
}

QString cacheFolder(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/cousoir/")
        + libraryKey(libraryPath);
}

QString backupFolder(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/cousoir/backups/")
        + libraryKey(libraryPath);
}

QString readingFile(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/cousoir/reading/")
        + libraryKey(libraryPath) + QStringLiteral(".json");
}

QString positionsFile(const QString &libraryPath)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/cousoir/reading/")
        + libraryKey(libraryPath) + QStringLiteral("-positions.json");
}

QString koboFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/cousoir/kobo");
}

} // namespace CousoirPaths
