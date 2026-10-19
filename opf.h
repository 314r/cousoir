#pragma once

#include <QByteArray>
#include <QString>

#include "book.h"

// The metadata.opf Calibre keeps beside a book's files, from which its
// "Restore database" rebuilds a library. Ported from metadata_to_opf() in
// src/calibre/ebooks/metadata/opf2.py of Calibre 9.16, as Cache.dump_metadata()
// calls it.
namespace Opf {

struct Settings
{
    // Calibre's interface language: the language of a book without one and
    // the title of the cover reference.
    QString interfaceLanguage;
    // The software writing the file, as its dc:contributor: its name and
    // version, and the short name it files under.
    QString producer;
    QString producerId;
};

// `book` of `data` as an OPF. The calibre: metadata Reliure does not keep,
// custom columns, annotations and user categories, comes from `previous`,
// the file it replaces: it cannot have changed, as Reliure never writes it.
QByteArray metadataToOpf(const LibraryData &data, const Book &book, const Settings &settings,
                         const QByteArray &previous = {});

} // namespace Opf
