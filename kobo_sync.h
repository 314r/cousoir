#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "book.h"
#include "kobo_highlights.h"

// What putting a Kobo's highlights into a library takes: each highlight of
// a book of the library placed in the library's copy, as the Kobo's own
// user's annotation, and what is left out, kept to be shown and copied all
// the same: the books the library has not, the highlights not found, the
// notes made on no text. Where the Kobo stopped goes in too, when it was
// read since the library last heard of the book's reading; and what became
// on the Kobo of the library's own highlights put there.
namespace KoboSync {

// The annotations user a Kobo's highlights go by: type "kobo", its serial;
// the user its places go by in last_read_positions: "kobo", its serial as
// the device.
const QString userType = QStringLiteral("kobo");

struct Input
{
    // The library as read, its books and their files.
    LibraryData data;
    // The copy of metadata.db it was read from.
    QString snapshotDb;
    // Where placings are kept, to place a highlight once; none when empty.
    QString cacheFile;
    KoboHighlights::Snapshot snapshot;
    // The library's highlights put on the Kobo, KoboHighlights::readSent().
    QHash<QString, QDateTime> sent;
};

// A book's highlights left out of the library, and why.
struct Left
{
    KoboHighlights::Book book;
    // The library's book it is, 0 for none.
    int bookId = 0;
    // By highlight id.
    QHash<QString, QString> reasons;
};

struct Plan
{
    // Only the books with something to write.
    QList<DeviceAnnotations> changes;
    // Where the Kobo stopped, by book, newer than what the library has.
    QList<QPair<int, ReadPosition>> positions;
    QList<Left> left;
    // Highlights in the library as they are on the Kobo, placed now,
    // and left out.
    int kept = 0;
    int placed = 0;
    int leftOut = 0;
};

// A Kobo read gives all it has: what it no longer has goes from the library
// too. A database opened as a file only adds and updates: it may be old.
Plan plan(const Input &input);

} // namespace KoboSync
