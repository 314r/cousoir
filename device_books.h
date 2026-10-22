#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

struct LibraryData;

// A highlight, note or bookmark made on a reader, as Calibre's Kobo
// Bookmark reads it.
struct ReaderNote
{
    // Calibre's kinds: "Bookmark", a corner turned down; "Highlight";
    // "Annotation", a highlight with a note; "Unknown annotation type".
    QString type;
    // The text highlighted; a bookmark's chapter title.
    QString text;
    QString annotation;
    // The chapter, by its index in the book, and its title.
    int chapter = 0;
    QString chapterTitle;
    // How far into the chapter, in percent, to two decimals.
    double chapterProgress = 0;
    // Calibre's key, the chapter times 1000 and the note's rank in it: the
    // notes come in its order, and a note of the same key replaces another.
    int location = 0;
    // When it was made, as the reader wrote it.
    QString created;
};

// What a reader knows of the reading of a book.
struct ReadingState
{
    // 0 not begun, 1 being read, 2 finished.
    int status = 0;
    // Percent read: 0 before it is begun, 100 once finished; none when the
    // reader does not say.
    std::optional<int> percent;
    // When it was last read, as the reader wrote it; empty when not begun.
    QString lastRead;
    // Time spent reading it, in seconds, 0 when unknown.
    qint64 secondsSpent = 0;
    QList<ReaderNote> notes;
};

// A book on a reader as Calibre's device drivers list it: its file on the
// device, the metadata Calibre keeps for the books it sent, and the library
// book it is.
struct DeviceBook
{
    // Calibre's lpath: the file's path from the root, / separated.
    QString lpath;
    QString title;
    QStringList authors;
    QString authorSort;
    // From metadata.calibre: the library book it was sent from.
    QString uuid;
    int applicationId = 0;
    qint64 size = 0;
    // As the library names formats: KEPUB, EPUB, PDF…
    QString format;
    // In metadata.calibre: Calibre or Cousoir sent it.
    bool sent = false;
    // In a Kobo's database: the Kobo has taken it in.
    bool imported = false;
    // Bought from Kobo and kept in .kobo/kepub; not a file one sends or
    // removes without editing the Kobo's database.
    bool purchased = false;
    // A Kobo's id for its cover images, when its database has one.
    QString imageId;
    // A Kobo's id for the book in its database.
    QString contentId;
    // The type the Kobo's database gives its file: "application/x-kobo-epub+zip".
    QString mime;
    // What the reader knows of its reading.
    ReadingState reading;
    // Set by match(): the library book it is, 0 when none.
    int bookId = 0;
};

namespace DeviceBooks {

// set_books_in_library() of Calibre's GUI: each book is the library's book
// with its uuid, else the one with its title and its authors, author sort or
// one of its authors, punctuation and case aside.
void match(QList<DeviceBook> *books, const LibraryData &data);

// The format Calibre sends of `formats`, the book's: the first of
// `formatMap`, the driver's ordered formats, the book has. Empty for none.
QString sendFormat(const QStringList &formats, const QStringList &formatMap);

// The text of each element of the JSON array `json`, as written there:
// metadata.calibre's entries, so that some go and the others stay as
// Calibre's JsonCodec wrote them.
QList<QByteArray> arrayElements(const QByteArray &json);
// An array of those elements, laid out as Python's json.dumps(indent=2).
QByteArray joinElements(const QList<QByteArray> &elements);

// A book's notes as Markdown: its title and authors, then each chapter's
// highlights as quotes, each followed by the note made on it.
QString notesMarkdown(const QString &title, const QStringList &authors, const QList<ReaderNote> &notes);

} // namespace DeviceBooks
