#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// The metadata of a MOBI or AZW3 book, written into it as Calibre writes it
// into a copy it sends to a Kindle (MetadataUpdater of
// src/calibre/ebooks/metadata/mobi.py): the EXTH records of the first
// record, the title after them, the cover and its thumbnail replaced where
// they fit. And what Calibre's Kindle driver reads of a book: the name of
// its cover thumbnail, its APNX page numbers.
struct Book;

namespace MobiMetadata {

struct Metadata
{
    QString title;
    QStringList authors;
    QString authorSort;
    QString publisher;
    // HTML.
    QString comments;
    QString isbn;
    QStringList tags;
    // Calibre's undefined date for none.
    QDateTime pubdate;
    QString uuid;
    // ISO 639-2 codes.
    QStringList languages;
    // The cover image's bytes; empty for none.
    QByteArray cover;
};

// Calibre's MOBI output settings that the writer follows, from
// conversion/mobi_output.py in Calibre's configuration folder.
struct Options
{
    bool preferAuthorSort = false;
    // A tag that makes a personal document, "*" for every book; empty for
    // none.
    QString personalDoc;
    bool shareNotSync = false;

    // Of `output`, "mobi_output" or "azw3_output", the conversion's settings
    // of that format.
    static Options load(const QString &calibreConfigDirectory, const QString &output = QStringLiteral("mobi_output"));
};

// The library's metadata of `book`, with `cover`, as Calibre writes it into
// a Kindle book.
Metadata metadataFor(const ::Book &book, const QByteArray &cover);

// rescale_image() of a cover for its thumbnail record: a JPEG of at most
// 16 KB, within 180 by 240.
QByteArray thumbnail(const QByteArray &cover);

// MetadataUpdater.update(): `book`, a MOBI file's bytes, with `metadata`.
// `uuid4` makes the ASIN Calibre adds to a book Amazon would not sync
// without, a random one unless given (for the tests).
bool update(QByteArray *book, const Metadata &metadata, const Options &options, QString *error,
            const QString &uuid4 = {});

// The EXTH records of the first record, in their order.
QList<QPair<quint32, QByteArray>> exthRecords(const QByteArray &book);

// thumbnail_filename() of Calibre's Kindle driver:
// "thumbnail_<ASIN>_<cdetype>_portrait.jpg", empty without both.
QString thumbnailName(const QByteArray &book);

// APNXBuilder.write_apnx() with the fast page generator: a page every 2300
// characters of the book's text. `guid` is the eight hexadecimal digits
// Calibre draws at random. Empty with `error` for no MOBI file.
QByteArray apnx(const QByteArray &book, const QString &guid, QString *error);

} // namespace MobiMetadata
