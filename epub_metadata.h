#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

#include "python_json.h"

struct Book;
struct LibraryData;

// The library's metadata written into an EPUB, as Calibre's metadata writer
// writes it into the copy of a book it sends to a reader
// (ebooks/metadata/epub.py, set_metadata()): the package's metadata
// replaced field by field as opf3.py's apply_metadata() replaces it, the
// package laid out again as its pretty printer lays it out, and the
// library's cover put in place of the book's, or added when it has none.
// What the library does not know of the book is left as the book has it.
namespace EpubMetadata {

struct Metadata
{
    QString title;
    QString titleSort;
    QStringList authors;
    QString authorSort;
    // ISO 639-2, as the library has them.
    QStringList languages;
    QDateTime pubdate;
    QString comments;
    QString publisher;
    QStringList tags;
    // 0 to 10.
    int rating = 0;
    QString series;
    double seriesIndex = 1;
    QList<QPair<QString, QString>> identifiers;
    int applicationId = 0;
    QString uuid;
    // The custom columns, by key, each its field metadata with the book's
    // value as metadata.opf keeps it, and the keys of all of them.
    QList<QPair<QString, PythonJson::Value>> userMetadata;
    QStringList customKeys;
    // The library's cover, as its file holds it.
    QByteArray cover;
    // Calibre's interface language: its "Unknown" is no title.
    QString interfaceLanguage;
};

// What db.get_metadata(get_cover=True, cover_as_data=True) gives of a book.
Metadata fromBook(const LibraryData &data, const Book &book, const QString &interfaceLanguage);

struct Package
{
    // The package as lxml writes it, without a declaration.
    QByteArray opf;
    // The cover image's address from the package, its own or the one
    // added; empty for none.
    QString rasterCover;
};

// set_metadata() of ebooks/metadata/opf.py on a package: apply_metadata()
// for OPF 3, `coverPrefix` the package's folder in the book. Nothing when
// the package cannot be read, or is OPF 2, which Cousoir leaves as it is.
std::optional<Package> apply(const QByteArray &opf, const Metadata &metadata, const QString &coverPrefix);

// set_metadata() of ebooks/metadata/epub.py: the book at `source` written
// to `target` with the metadata. When its package cannot be changed, as
// when Calibre's writer fails, the book is copied as it is; false, with
// `error`, only when it cannot be read or written.
bool write(const QString &source, const QString &target, const Metadata &metadata, QString *error);

} // namespace EpubMetadata
