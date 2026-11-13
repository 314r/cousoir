#pragma once

#include <QString>

#include "mobi_metadata.h"

struct Book;

// Making an AZW3 of an EPUB without Calibre, as Calibre's AZW3 output does
// (src/calibre/ebooks/conversion/plugins/mobi_output.py and
// src/calibre/ebooks/mobi/writer8/): a KF8 book alone, its pages, styles,
// pictures and fonts those of the EPUB, its table of contents and guide, its
// metadata the library's. The EPUB's styles go as they are: Calibre's
// conversion rewrites them first, which a Kindle does not need.
namespace Kf8 {

struct Options
{
    // The title, authors, cover... of the book's EXTH; the EPUB's cover
    // stays when it has none.
    MobiMetadata::Metadata metadata;
    MobiMetadata::Options mobi;
    // Calibre's "Unknown", and the title of the table of contents it adds,
    // in its language.
    QString unknown = QStringLiteral("Unknown");
    QString tocTitle = QStringLiteral("Table of Contents");
};

// The library's metadata and cover of `book`.
Options optionsFor(const Book &book);

// Writes the AZW3 of the EPUB at `epub` to `azw3`; false with `error`.
bool write(const QString &epub, const QString &azw3, const Options &options, QString *error);

} // namespace Kf8
