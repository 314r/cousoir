#pragma once

#include <QString>

// FB2 books and text files made EPUBs for Cousoir's reader, without
// Calibre: an FB2's sections as pages, its notes, pictures, cover and
// contents; a text file's paragraphs. For reading, not for keeping: the
// library keeps the book's own file.
namespace TextBookEpub {

// Changes with what these make, for the EPUBs kept to be made again.
constexpr int version = 2;

// Writes the EPUB of the FB2 (or FBZ) at `fb2` to `epub`; false with
// `error`.
bool fb2ToEpub(const QString &fb2, const QString &epub, QString *error);

// Writes the EPUB of the text file at `txt` to `epub`: its paragraphs, by
// the blank lines between them, else its lines.
bool txtToEpub(const QString &txt, const QString &epub, QString *error);

} // namespace TextBookEpub
