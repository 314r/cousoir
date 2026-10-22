#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

#include "opf_reader.h"

// Reading a PDF's metadata as Calibre 9.16 does in ebooks/metadata/pdf.py:
// poppler's pdfinfo for the document's info dictionary and XMP packet, merged
// by consolidate_metadata() of ebooks/metadata/xmp.py, and pdftoppm for the
// first page as cover. The tools are Calibre's own when it is installed,
// else the system's.
namespace PdfReader {

// False with `error` when the tools are missing or cannot read the file.
bool read(const QString &path, const QString &interfaceLanguage, ImportedMetadata *metadata, QString *error);

// metadata_from_xmp_packet(): the metadata of an XMP packet, empty when it
// cannot be parsed. `metadataDate` is the later of its metadata and
// modification dates.
struct Xmp
{
    ImportedMetadata metadata;
    std::optional<CalibreDate> metadataDate;
};
std::optional<Xmp> readXmp(const QByteArray &packet, const QString &interfaceLanguage);

} // namespace PdfReader
