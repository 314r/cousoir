#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

// The indices of a KF8 book, as Calibre's KF8 writer makes them
// (src/calibre/ebooks/mobi/writer8/index.py and tbs.py): each a header
// record, its index records and the records of its strings.
namespace Kf8Index {

// A skeleton: the markup of a file of the book without its chunks.
struct Skeleton
{
    int fileNumber = 0;
    int chunkCount = 0;
    quint32 start = 0;
    quint32 length = 0;
};

// A chunk of markup, put into its skeleton at `insertPosition`.
struct Chunk
{
    quint32 insertPosition = 0;
    // "P-//*[@aid='0']": inside the tag with that aid, or "S-" after it.
    QString selector;
    int fileNumber = 0;
    int sequenceNumber = 0;
    quint32 start = 0;
    quint32 length = 0;
};

struct GuideReference
{
    QString title;
    QString type;
    int position = 0;
    int offset = 0;
};

// An entry of the table of contents, in the order of the index.
struct NcxEntry
{
    int index = 0;
    QString label;
    // In the text, and from its chunk.
    quint32 offset = 0;
    quint32 length = 0;
    int depth = 0;
    int position = 0;
    int fidOffset = 0;
    std::optional<int> parent;
    std::optional<int> firstChild;
    std::optional<int> lastChild;
};

QList<QByteArray> skeletonIndex(const QList<Skeleton> &skeletons);
QList<QByteArray> chunkIndex(const QList<Chunk> &chunks);
QList<QByteArray> guideIndex(const QList<GuideReference> &references);
QList<QByteArray> ncxIndex(const QList<NcxEntry> &entries);

// apply_trailing_byte_sequences(): for each text record, of the lengths
// given, the trailing entry that tells the entries it touches.
QList<QByteArray> trailingByteSequences(const QList<NcxEntry> &entries, const QList<qsizetype> &recordLengths);

} // namespace Kf8Index
