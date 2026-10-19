#pragma once

#include <QByteArray>
#include <QString>

#include "book.h"
#include "calibre_date.h"

// The metadata.opf Calibre keeps beside a book's files, from which its
// "Restore database" rebuilds a library. Ported from metadata_to_opf() in
// src/calibre/ebooks/metadata/opf2.py of Calibre 9.16, as Cache.dump_metadata()
// calls it, and as create_opf_file() of gui2/convert/metadata.py calls it
// for a conversion.
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
    // The first identifier, Calibre's application_id: the book's id, or
    // another value, like the uuid a conversion's OPF has there.
    QString applicationId;
    // Whether the guide names the cover: a metadata.opf always does, the OPF
    // a conversion reads never.
    bool coverReference = true;
};

// The dates of a book as Calibre holds them in memory, to the microsecond,
// right after adding or restoring it; read back from the database, they stop
// at the second.
struct ExactDates
{
    CalibreDate pubdate;
    CalibreDate timestamp;
};

// `book` of `data` as an OPF. The calibre: metadata Reliure does not keep,
// custom columns, annotations and user categories, comes from `previous`,
// the file it replaces: it cannot have changed, as Reliure never writes it.
QByteArray metadataToOpf(const LibraryData &data, const Book &book, const Settings &settings,
                         const QByteArray &previous = {}, const ExactDates *exact = nullptr);

} // namespace Opf
