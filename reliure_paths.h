#pragma once

#include <QString>

// Where Reliure keeps what it makes for a library, outside of it.
namespace ReliurePaths {

// A folder per library under the user's cache: the copy of metadata.db the
// library is read from, cover thumbnails, the journal of a change.
QString cacheFolder(const QString &libraryPath);
// The copies of metadata.db taken before changing it.
QString backupFolder(const QString &libraryPath);

} // namespace ReliurePaths
