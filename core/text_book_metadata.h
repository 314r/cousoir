#pragma once

#include <QString>

#include "opf_reader.h"

// The metadata Calibre's readers find in an FB2 book and in a plain text
// file, ported from Calibre 9.17 (src/calibre/ebooks/metadata/fb2.py and
// txt.py), for adding them without Calibre. The file's name is read apart,
// as for every format (BookImport::metadataForAdding()).
namespace TextBookMetadata {

// An FB2 book, or one in a ZIP (FBZ): title, authors, genres as tags,
// annotation as plain comments, series, ISBN, publisher, year, language
// and cover. False with `error` for a file that is no FB2.
bool readFb2(const QString &path, ImportedMetadata *metadata, QString *error);
// The FB2 book itself, or the first .fb2 of an FBZ; empty with `error`.
QByteArray fb2Data(const QString &path, QString *error);

// A text file: a title, then three line breaks and the authors, in its
// first four lines; else its name without the extension as its title.
bool readTxt(const QString &path, ImportedMetadata *metadata, QString *error);

} // namespace TextBookMetadata
