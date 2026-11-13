#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

#include "mobi_metadata.h"

// The records of a KF8 book put together, as Calibre's KF8Book writes them
// (src/calibre/ebooks/mobi/writer8/mobi.py and exth.py): the MOBI header and
// its EXTH, the text, the indices, the resources, FDST, FLIS, FCIS and the
// end, in a Palm database.
namespace Kf8Book {

struct Parts
{
    // The text records, with their trailing entries, and the text's length.
    QList<QByteArray> text;
    qsizetype textLength = 0;
    bool trailingByteSequences = false;
    QList<QByteArray> chunkIndex;
    QList<QByteArray> skeletonIndex;
    // Empty for none.
    QList<QByteArray> guideIndex;
    QList<QByteArray> ncxIndex;
    QList<QByteArray> resources;
    bool fonts = false;
    // Their numbers among the resources.
    std::optional<quint32> cover;
    std::optional<quint32> thumbnail;
    QByteArray fdst;
    int flows = 1;
    // Where the book opens, in the text.
    std::optional<quint32> start;
    // "rtl", "ltr", or empty.
    QString pageProgressionDirection;
    QString primaryWritingMode;
    // The title and the rest of what the EXTH tells.
    MobiMetadata::Metadata metadata;
    MobiMetadata::Options options;
    // Calibre's "Unknown" in its language.
    QString unknown = QStringLiteral("Unknown");
};

// The book's file.
QByteArray write(const Parts &parts);

} // namespace Kf8Book
