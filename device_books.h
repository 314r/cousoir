#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

struct LibraryData;

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

} // namespace DeviceBooks
