#pragma once

#include <QString>

// Where Cousoir keeps what it makes for a library, outside of it.
namespace CousoirPaths {

// A folder per library under the user's cache: the copy of metadata.db the
// library is read from, cover thumbnails, the journal of a change.
QString cacheFolder(const QString &libraryPath);
// The copies of metadata.db taken before changing it.
QString backupFolder(const QString &libraryPath);
// What the readers last told of the reading of the library's books.
QString readingFile(const QString &libraryPath);
// Where Cousoir's reader stopped in the library's books, beside it.
QString positionsFile(const QString &libraryPath);
// The highlights each Kobo met last had, one file each, whatever the
// library.
QString koboFolder();
// The key the files of a library go by.
QString libraryKey(const QString &libraryPath);

} // namespace CousoirPaths
