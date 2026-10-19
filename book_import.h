#pragma once

#include <QString>

#include "calibre_config.h"
#include "opf_reader.h"

// Reading an ebook file's metadata to add it to a library, as calibredb add
// reads it in Calibre 9.16: the metadata of the file's name, updated with the
// file's own. Reliure reads EPUB and KEPUB, MOBI, AZW, AZW3 and PRC itself,
// PDF through poppler's tools; any other format, and the cover of an EPUB
// that has no image for one, through Calibre's own reader when Calibre is
// installed.
namespace BookImport {

// The format Calibre files `path` under: its extension in upper case.
QString formatOf(const QString &path);

// Calibre's calibre-debug, empty when Calibre is not installed.
QString calibreDebug();

// The file's own metadata, from Calibre's metadata reader for its format,
// with `config`'s interface language and tweaks. False with `error` when the
// file is not what its extension says.
bool readFileMetadata(const QString &path, const CalibreConfig &config, ImportedMetadata *metadata, QString *error);

// metadata_from_filename(): the title and authors in a name like "Title -
// Author.epub", by Calibre's file name pattern.
ImportedMetadata metadataFromFilename(const QString &path, const CalibreConfig &config);

// What calibredb add makes of the file: the file name's metadata, updated
// with the file's by Calibre's smart_update(). `warning` says what could not
// be read, the file is still added.
ImportedMetadata metadataForAdding(const QString &path, const CalibreConfig &config, QString *warning);

} // namespace BookImport
