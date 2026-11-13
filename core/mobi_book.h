#pragma once

#include <QString>

// A Kindle book, AZW3 or MOBI, made into an EPUB the reader opens, as
// Calibre's MOBI reader extracts its files (src/calibre/ebooks/mobi/reader):
// KF8's pages put together again from their skeletons and fragments, their
// styles, pictures and fonts, its links and its table of contents; of a
// MOBI that holds both, its KF8 part. A MOBI of the old kind alone, a book
// with DRM, and Amazon's Topaz and KFX are refused, saying why.
namespace MobiBook {

// An old MOBI's one page of HTML made pages at its page breaks, as an EPUB
// has them, or kept whole, as Calibre's MOBI reader leaves it.
enum class OldPages { Split, Whole };

// Writes the EPUB of the book at `mobi` to `epub`; false with `error`.
bool toEpub(const QString &mobi, const QString &epub, QString *error, OldPages oldPages = OldPages::Split);

// Changes with what toEpub() makes, for the EPUBs kept to be made again.
constexpr int version = 2;

} // namespace MobiBook
