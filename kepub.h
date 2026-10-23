#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

struct Book;

// Making a KEPUB of an EPUB without Calibre, as Calibre's kepubify does
// (ebooks/oeb/polish/kepubify.py): the text of each page in Kobo's spans,
// a sentence each, numbered by paragraph; the body wrapped in Kobo's
// book-columns and book-inner divisions; Kobo's styles and script; a cover
// image and a title page when the book has none. Kobo's reader then shows
// the book with its own reading features: page counts, highlights,
// statistics.
namespace Kepub {

// The library's metadata, put into the book's package as Calibre puts it
// into a book it converts or sends.
struct Metadata
{
    QString title;
    QStringList authors;
    QString authorSort;
    QString publisher;
    QStringList languages;
    QStringList tags;
    QString series;
    double seriesIndex = 1;
    // HTML.
    QString comments;
    QList<QPair<QString, QString>> identifiers;
};

struct Options
{
    // The book's title and authors, for a title page without a cover.
    QString title;
    QString authors;
    // The library's cover, in place of the book's, as an image file's bytes.
    QByteArray cover;
    // The library's metadata in place of the book's.
    std::optional<Metadata> metadata;
};

// The options that put the library's metadata and cover of `book` into its
// KEPUB, as Calibre's conversion and send put them into what they make.
Options optionsFor(const Book &book);

// kepubify_html_data(): a page of XHTML with Kobo's markup, `scriptHref`
// the address of kobo.js from it; nothing for a page that is no XML.
std::optional<QByteArray> kepubifyPage(const QByteArray &xhtml, const QString &scriptHref);

// kepubify_path(): writes the KEPUB of the EPUB at `epub` to `kepub`.
bool kepubify(const QString &epub, const QString &kepub, const Options &options, QString *error);

} // namespace Kepub
