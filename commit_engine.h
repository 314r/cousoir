#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>

#include "book.h"
#include "book_edit.h"
#include "calibre_config.h"

// Writes edits to a library as Calibre would, without the library ever
// being half written. While Calibre's own lock keeps Calibre from starting:
// the database is read and kept as a backup, the edits go to a copy of it in
// the cache, book folders move and covers and metadata.opf files are written
// through a journal, and the copy finally replaces metadata.db in one rename.
// A change to metadata.db meanwhile, Dropbox bringing another computer's,
// undoes the files and starts again from the new database: the edits set
// values, so they apply to it as they are.
namespace CommitEngine {

struct Request
{
    QString libraryPath;
    // Reliure's folder for the library in the user's cache: the copy being
    // edited, the journal, the snapshot the library is read from.
    QString cacheFolder;
    // Where to keep the database as it was before the change; empty for no
    // backup.
    QString backupFolder;
    CalibreConfig config;
    QList<BookEdit> edits;
    // The software writing the metadata.opf files: name and version, and the
    // short name it files under.
    QString producer;
    QString producerId;
    // last_modified of the changed books; the current time when invalid.
    QDateTime now;
    // Called just before metadata.db is replaced, for tests that change it
    // there as Dropbox could.
    std::function<void()> beforePublish;
};

struct Result
{
    bool ok = false;
    QString error;
    // The library as it now is, read from the database written; without
    // books when nothing changed and nothing was written.
    LibraryData data;
    QSet<int> dirtied;
    // The backup made, if any.
    QString backup;
};

Result commit(const Request &request);

// Undoes or finishes a change a crash interrupted, from the journal in the
// cache folder. `done` says what was done; empty when there was nothing.
bool recover(const QString &libraryPath, const QString &cacheFolder, QString *done, QString *error);

// save_cover_data_to(): `data` as Calibre stores a cover. A JPEG that fits in
// 1650 by 2200 pixels stays as it is; anything else is scaled down to fit,
// its transparency laid on white, and saved as a JPEG of quality 90. Empty,
// with `error`, when it is no image.
QByteArray coverData(const QByteArray &data, QString *error);

} // namespace CommitEngine
