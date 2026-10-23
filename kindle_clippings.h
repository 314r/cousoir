#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "device_books.h"

// A Kindle's "documents/My Clippings.txt": every highlight, note and
// bookmark made on it, by book, in the Kindle's language. A note comes as
// an entry of its own, after the highlight it was made on; a highlight
// changed leaves its old version behind; one deleted stays. Read as a
// reader's notes, as the Kobo's are.
namespace KindleClippings {

struct Clipping
{
    enum class Kind { Highlight, Note, Bookmark, Unknown };

    // The book as the Kindle names it: its title, and its authors as the
    // Kindle wrote them, "Vian, Boris" or "Boris Vian".
    QString title;
    QString authors;
    Kind kind = Kind::Unknown;
    // 0 when not said.
    int page = 0;
    int start = 0;
    int end = 0;
    // When it was made: ISO 8601 when the date could be read, else as
    // written.
    QString added;
    QString text;
};

QList<Clipping> parse(const QByteArray &file);

// A book's clippings as a reader's notes: highlights with the notes made on
// them, the versions a later highlight replaced left out, in the book's
// order.
struct Book
{
    QString title;
    QStringList authors;
    QList<ReaderNote> notes;
};

QList<Book> byBook(const QList<Clipping> &clippings);

// "Added on Sunday, March 3, 2024 10:15:32 PM", or in another of the
// Kindle's languages, as ISO 8601; empty when it cannot be read.
QString readDate(const QString &text);

} // namespace KindleClippings
