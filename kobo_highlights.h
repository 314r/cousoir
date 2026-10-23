#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "highlight_formats.h"

class QJsonArray;
class SqliteDb;
struct DeviceBook;
struct LibraryData;

// All the highlights and notes of a Kobo's database, where they are and the
// chapter they are in, read as kbextract2 reads them, hidden ones too: what
// goes into the library as the Kobo's own highlights, and is kept, a file
// per Kobo, once it is unplugged.
namespace KoboHighlights {

struct Highlight
{
    // The Kobo's BookmarkID, a UUID.
    QString id;
    // The book, and the file of it the highlight is in.
    QString volumeId;
    QString contentId;
    // Where it starts and ends in that file: "span#kobo\.3\.1" in a KEPUB.
    QString startPath;
    qint64 startOffset = 0;
    QString endPath;
    qint64 endOffset = 0;
    // The text highlighted and the note on it, line ends as LF, trimmed.
    QString text;
    QString note;
    // As the Kobo wrote them.
    QString created;
    QString modified;
    // The Kobo's colour, -1 when it keeps none.
    int color = -1;
    // Removed on the Kobo, which keeps it hidden.
    bool hidden = false;
    // How far into its file, from 0 to 1.
    double chapterProgress = 0;
    // The chapter of the book's table of contents.
    QString chapterKey;
    QString chapterTitle;
    std::optional<qint64> chapterOrder;
};

struct Book
{
    QString volumeId;
    QString title;
    QStringList authors;
    // Its file on the Kobo, from the Kobo's root, and what metadata.calibre
    // says of it, when the Kobo still has it.
    QString lpath;
    QString uuid;
    int applicationId = 0;
    // KEPUB, EPUB, PDF…
    QString format;
    bool purchased = false;
    bool onDevice = false;
    QList<Highlight> highlights;
};

// What Cousoir last read of a Kobo or a Kobo's database.
struct Snapshot
{
    // The Kobo's serial number, else a name for it: whose highlights they
    // are in the library.
    QString device;
    QString name;
    // "usb" for a Kobo read, "file" for a database opened as a file.
    QString source;
    QDateTime seen;
    QList<Book> books;
    // By library key, the digest of the books last put into that library.
    QHash<QString, QString> synced;

    QString digest() const;
};

// The books of the Kobo's database `db` with highlights or notes, by title;
// an error and none when it is no Kobo database.
QList<Book> read(SqliteDb &db, QString *error);
// The Kobo's own list of its books: which are still there, their files and
// what metadata.calibre says of them.
void attach(QList<Book> *books, const QList<DeviceBook> &deviceBooks);

// The library's book each book is, 0 for none, as Calibre matches a
// reader's books with the library's.
QList<int> match(const QList<Book> &books, const LibraryData &data);

// A highlight as an entry of HighlightFormats.
HighlightFormats::Entry entry(const Highlight &highlight);

// The builtin colour of Calibre's highlights nearest the Kobo's `color`.
QString calibreColor(int color);

// Whose a database file is: its Kobo's serial number, from .kobo/version or
// Kobo eReader.conf when it sits in a Kobo's .kobo folder, and a name, the
// folder's.
struct Identity
{
    QString serial;
    QString name;
};
Identity identify(const QString &databasePath);
// The first SerialNumber of a Kobo eReader.conf, whatever its section.
QString readSerialNumber(const QString &confPath);

// kepubBookmarkChapterID(): a KEPUB chapter's id without the "-N" Kobo
// adds after the file.
QString chapterIdOf(const QString &contentId);

QJsonArray toJson(const QList<Book> &books);
QByteArray toJson(const Snapshot &snapshot);
std::optional<Snapshot> fromJson(const QByteArray &json);
// The snapshot kept for `device`, and keeping one.
std::optional<Snapshot> readSnapshot(const QString &device);
bool writeSnapshot(const Snapshot &snapshot, QString *error);
// Every snapshot kept.
QList<Snapshot> snapshots();

} // namespace KoboHighlights
