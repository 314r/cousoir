#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "book.h"
#include "device_books.h"

// What going back to a Kobo takes, when asked and only then: where reading
// stopped in the library's books, when newer than what the Kobo has, put in
// the Kobo's database as the Kobo keeps its own place; the library's
// highlights and notes put there as the Kobo keeps its own, and those
// changed or removed in the library since changed or removed there too.
// Only for the books put on the Kobo by hand as KEPUBs, whose places the
// Kobo keeps by Kobo's spans: each place is found in the KEPUB the Kobo
// reads by the library's text there.
namespace KoboWrite {

struct Input
{
    // The Kobo's folder, and its serial, the user its highlights go by in
    // the library.
    QString root;
    QString device;
    // Its books, matched with the library's.
    QList<DeviceBook> books;
    LibraryData data;
    // The copy of metadata.db the library was read from.
    QString snapshotDb;
    // Cousoir's reader's own places, newer than the library's while a book
    // is open in it.
    QString positionsFile;
    // The library's highlights put on the Kobo before,
    // KoboHighlights::readSent().
    QHash<QString, QDateTime> sent;
};

// Where reading stopped in a book, for its row of `content`.
struct Place
{
    QString contentId;
    QString title;
    // ChapterIDBookmarked, "OEBPS/c2.xhtml#kobo.12.1".
    QString bookmark;
    int percent = 0;
    double chapterProgress = 0;
    // DateLastRead.
    QString lastRead;
    // Not begun on the Kobo: begun now.
    bool begin = false;
};

// A highlight as the Kobo keeps one, a row of Bookmark.
struct Highlight
{
    QString id;
    QString volumeId;
    QString contentId;
    QString startPath;
    qint64 startOffset = 0;
    QString endPath;
    qint64 endOffset = 0;
    QString text;
    // Empty for none.
    QString note;
    QString created;
    QString modified;
    double chapterProgress = 0;
    int color = 0;
};

struct Changes
{
    QList<Place> places;
    QList<Highlight> added;
    // Highlights whose note or colour changed in the library: id, note,
    // colour and modified alone.
    QList<Highlight> changed;
    // BookmarkIDs of highlights removed in the library.
    QStringList removed;
    // What cannot go, a line for each: the book, and why.
    QStringList skipped;

    // The database as it was read, to write it only as it is still.
    QList<qint64> identity;
    // Its columns used if it has them, and the Kobo's user, for the rows
    // added.
    bool hasColor = false;
    bool hasChapterProgress = false;
    QString userId;

    bool isEmpty() const { return places.isEmpty() && added.isEmpty() && changed.isEmpty() && removed.isEmpty(); }
};

// What the Kobo at `input.root` lacks of the library; an error, and none,
// when its database cannot be read or is of a Kobo Cousoir cannot write to.
Changes plan(const Input &input, QString *error);

// Writes `changes` in the Kobo's database at `root`, in one transaction,
// after keeping a copy of it in `backupFolder`; refused when the database
// changed since it was read. False, and an error, when nothing was written.
bool apply(const QString &root, const Changes &changes, const QString &backupFolder, QString *error);

// Where the copies of a Kobo's database are kept, the last five.
QString backupFolder(const QString &device);

// What a sync did, for the one who asked.
struct Result
{
    bool ok = false;
    QString error;
    int places = 0;
    int highlights = 0;
    QStringList skipped;
};

// plan(), then apply(), then the record of what went kept: all a sync takes.
Result sync(const Input &input);

} // namespace KoboWrite
