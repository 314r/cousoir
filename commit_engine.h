#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include <functional>

#include "book.h"
#include "book_edit.h"
#include "book_operations.h"
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
    // Cousoir's folder for the library in the user's cache: the copy being
    // edited, the journal, the snapshot the library is read from.
    QString cacheFolder;
    // Where to keep the database as it was before the change; empty for no
    // backup.
    QString backupFolder;
    CalibreConfig config;
    QList<BookEdit> edits;
    // Then bulk edits, each worked out on the books as the changes before it
    // left them.
    QList<BulkEdit> bulkEdits;
    // After the edits, in this order: formats copied from one book to
    // another, before anything leaves, then formats moved to the trash, books
    // moved to the trash, books and formats restored from it, books added and
    // formats added.
    QList<FormatCopy> formatCopies;
    QList<FormatRemoval> formatRemovals;
    QList<int> trash;
    QList<Restoration> restorations;
    QList<NewBook> newBooks;
    QList<NewFormat> newFormats;
    // Last, the settings of conversions, for the books' next ones: book and
    // conversion_options data.
    QList<QPair<int, QByteArray>> conversionOptions;
    // The software writing the metadata.opf files: name and version, and the
    // short name it files under.
    QString producer;
    QString producerId;
    // last_modified of the changed books; the current time when invalid.
    QDateTime now;
    // Called just before metadata.db is replaced, for tests that change it
    // there as Dropbox could.
    std::function<void()> beforePublish;

    // Whether it does more than edit books: any of the changes after the
    // edits.
    bool hasOperations() const
    {
        return !formatCopies.isEmpty() || !formatRemovals.isEmpty() || !trash.isEmpty() || !restorations.isEmpty()
            || !newBooks.isEmpty() || !newFormats.isEmpty() || !conversionOptions.isEmpty();
    }
};

struct Result
{
    bool ok = false;
    // Whether the library changed: false when there was nothing to write.
    bool written = false;
    QString error;
    // The library as it now is, read from the database written; without
    // books when nothing changed and nothing was written.
    LibraryData data;
    QSet<int> dirtied;
    // The ids of the books added, in order.
    QList<int> added;
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
