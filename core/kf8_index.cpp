// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/mobi/writer8/index.py: IndexHeader, Index, SkelIndex,
//   ChunkIndex, GuideIndex, NCXIndex
// - src/calibre/ebooks/mobi/writer8/tbs.py: the trailing byte sequences
// - src/calibre/ebooks/mobi/utils.py: encode_fvwi(), encode_tbs()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kf8_index.h"

#include <QHash>

#include <algorithm>

#include "mobi_format.h"

namespace Kf8Index {

using namespace MobiFormat;

namespace {

constexpr qsizetype headerLength = 192;

struct TagMeta
{
    const char *name;
    quint8 number;
    quint8 valuesPerEntry;
    quint8 mask;
    quint8 endFlag;
};

const TagMeta endTag{ "eof", 0, 0, 0, 1 };

// mask_to_bit_shifts.
int shiftsOf(quint8 mask)
{
    static const QHash<int, int> shifts{ { 1, 0 },  { 2, 1 },  { 3, 0 },  { 4, 2 },  { 8, 3 },   { 12, 2 },
                                         { 16, 4 }, { 32, 5 }, { 48, 4 }, { 64, 6 }, { 128, 7 }, { 192, 6 } };
    return shifts.value(mask);
}

struct Entry
{
    QByteArray key;
    QHash<QString, QList<quint64>> tags;
};

// Index.__call__(): the header record, the index records, then the strings.
QList<QByteArray> index(const QList<TagMeta> &tagTypes, const QList<Entry> &entries, const Cncx &cncx)
{
    // kindlegen's margin, for the alignment of the blocks.
    constexpr qsizetype recordLimit = 0x10000 - headerLength - 1048;
    int controlByteCount = 0;
    for (const TagMeta &tag : tagTypes)
        controlByteCount += tag.endFlag;

    QList<QByteArray> indexBlocks{ QByteArray() };
    QList<QByteArray> idxtBlocks{ QByteArray() };
    QList<int> counts{ 0 };
    QList<QByteArray> lastKeys{ QByteArray() };
    for (const Entry &entry : entries) {
        QByteArray raw;
        raw.append(char(entry.key.size()));
        raw += entry.key;
        quint32 control = 0;
        for (const TagMeta &tag : tagTypes) {
            if (tag.endFlag) {
                raw.append(char(control));
                control = 0;
                continue;
            }
            const quint32 count = quint32(entry.tags.value(QLatin1String(tag.name)).size()) / tag.valuesPerEntry;
            control |= tag.mask & (count << shiftsOf(tag.mask));
        }
        for (const TagMeta &tag : tagTypes) {
            for (const quint64 value : entry.tags.value(QLatin1String(tag.name)))
                raw += encint(value);
        }
        qsizetype offset = indexBlocks.back().size();
        const qsizetype idxtPosition = idxtBlocks.back().size();
        if (offset + idxtPosition + raw.size() + 2 > recordLimit) {
            indexBlocks.append(QByteArray());
            idxtBlocks.append(QByteArray());
            counts.append(0);
            lastKeys.append(QByteArray());
            offset = 0;
        }
        ++counts.back();
        idxtBlocks.back() += pack16(quint16(headerLength + offset));
        indexBlocks.back() += raw;
        lastKeys.back() = entry.key;
    }

    QList<QByteArray> records;
    for (qsizetype i = 0; i < indexBlocks.size(); ++i) {
        const QByteArray block = alignBlock(indexBlocks.at(i));
        const QByteArray idxt = alignBlock(QByteArray("IDXT") + idxtBlocks.at(i));
        QByteArray record("INDX");
        record += pack32(quint32(headerLength));
        record += QByteArray(4, '\0');
        // 1 for an index record, 0 for the header.
        record += pack32(1);
        record += QByteArray(4, '\0');
        record += pack32(quint32(headerLength + block.size()));
        record += pack32(quint32(counts.at(i)));
        record += QByteArray(8, '\xff');
        record += QByteArray(156, '\0');
        records.append(record + block + idxt);
    }

    QByteArray tagBytes;
    for (const TagMeta &tag : tagTypes) {
        tagBytes.append(char(tag.number));
        tagBytes.append(char(tag.valuesPerEntry));
        tagBytes.append(char(tag.mask));
        tagBytes.append(char(tag.endFlag));
    }
    const QByteArray tagx = QByteArray("TAGX") + pack32(quint32(12 + tagBytes.size())) + pack32(quint32(controlByteCount)) + tagBytes;
    // The geometry of the index records, as entries the IDXT points to.
    QByteArray geometry;
    QByteArray idxt("IDXT");
    qsizetype position = headerLength + tagx.size();
    for (qsizetype i = 0; i < lastKeys.size(); ++i) {
        const qsizetype start = geometry.size();
        idxt += pack16(quint16(position));
        geometry.append(char(lastKeys.at(i).size()));
        geometry += lastKeys.at(i);
        geometry += pack16(quint16(counts.at(i)));
        position += geometry.size() - start;
    }
    int entryCount = 0;
    for (const int count : std::as_const(counts))
        entryCount += count;
    const QByteArray alignedTagx = alignBlock(tagx);
    const QByteArray alignedGeometry = alignBlock(geometry);
    QByteArray header("INDX");
    header += pack32(quint32(headerLength));
    header += QByteArray(8, '\0');
    // 2: an inflection index, as kindlegen says.
    header += pack32(2);
    header += pack32(quint32(headerLength + alignedTagx.size() + alignedGeometry.size()));
    header += pack32(quint32(records.size()));
    header += pack32(65001);
    header += pack32(0xFFFFFFFF);
    header += pack32(quint32(entryCount));
    // ORDT, LIGT and their count.
    header += QByteArray(12, '\0');
    header += pack32(quint32(cncx.records().size()));
    header += QByteArray(124, '\0');
    header += pack32(quint32(headerLength));
    header += QByteArray(8, '\0');
    header += alignedTagx + alignedGeometry + alignBlock(idxt);
    records.prepend(alignBlock(header));
    records += cncx.records();
    return records;
}

QList<quint64> values(std::initializer_list<qint64> list)
{
    QList<quint64> result;
    for (const qint64 value : list)
        result.append(quint64(std::max<qint64>(value, 0)));
    return result;
}

// --- Trailing byte sequences ------------------------------------------------

struct Located
{
    int index = 0;
    qint64 start = 0;
    qint64 length = 0;
    int depth = 0;
    std::optional<int> parent;
    // starts, ends, spans or completes the record.
    enum class Action { Spans, Ends, Starts, Completes } action = Action::Completes;
};

// A strand: its entries by depth, the depths in the order first met.
using Strand = QList<QPair<int, QList<Located>>>;

void removeIndex(QList<Located> *entries, int index)
{
    for (qsizetype i = 0; i < entries->size(); ++i) {
        if (entries->at(i).index == index) {
            entries->removeAt(i);
            return;
        }
    }
}

bool hasChildren(const QList<Located> &entries, int index)
{
    return std::any_of(entries.cbegin(), entries.cend(), [index](const Located &entry) { return entry.parent == index; });
}

// populate_strand().
QList<Located> populateStrand(const Located &parent, QList<Located> *entries)
{
    QList<Located> strand{ parent };
    for (const Located &child : std::as_const(*entries)) {
        if (child.parent == parent.index) {
            // The first child, and down from it.
            const Located first = child;
            removeIndex(entries, first.index);
            strand += populateStrand(first, entries);
            return strand;
        }
    }
    // The siblings that follow at the same depth, as one sequence.
    int current = parent.index;
    const QList<Located> candidates = *entries;
    for (const Located &entry : candidates) {
        if (entry.depth == parent.depth && entry.parent == parent.parent && entry.index == current + 1) {
            ++current;
            removeIndex(entries, entry.index);
            if (hasChildren(*entries, entry.index)) {
                strand += populateStrand(entry, entries);
                break;
            }
            strand.append(entry);
        }
    }
    return strand;
}

QList<Strand> separateStrands(QList<Located> entries)
{
    QList<Strand> strands;
    while (!entries.isEmpty()) {
        const Located top = entries.takeFirst();
        Strand layers;
        for (const Located &entry : populateStrand(top, &entries)) {
            auto layer = std::find_if(layers.begin(), layers.end(), [&entry](const auto &l) { return l.first == entry.depth; });
            if (layer == layers.end())
                layers.append({ entry.depth, { entry } });
            else
                layer->second.append(entry);
        }
        strands.append(layers);
    }
    return strands;
}

struct Sequence
{
    qint64 value = 0;
    std::optional<quint64> spans;   // 0b1
    std::optional<quint64> type;    // 0b10
    std::optional<quint64> count;   // 0b100
    bool fromPrevious = false;      // 0b1000
};

// encode_strands_as_sequences(); `negative` set, for type 8, when a strand
// starts before the last entry of the strand before it.
QList<Sequence> sequencesOf(const QList<Strand> &strands, int tbsType, bool *negative)
{
    QList<Sequence> sequences;
    int lastIndex = 0;
    for (qsizetype s = 0; s < strands.size(); ++s) {
        QList<Sequence> strandSequences;
        for (qsizetype l = 0; l < strands.at(s).size(); ++l) {
            const QList<Located> &entries = strands.at(s).at(l).second;
            Sequence sequence;
            if (entries.back().action == Located::Action::Spans)
                sequence.spans = 0;
            if (s == 0 && l == 0)
                sequence.type = quint64(tbsType);
            if (entries.size() > 1)
                sequence.count = quint64(entries.size());
            sequence.value = entries.front().index - entries.front().parent.value_or(0);
            if (!sequences.isEmpty() && strandSequences.isEmpty()) {
                // A later strand: from the last entry of the one before.
                sequence.value = lastIndex - entries.front().index;
                if (sequence.value < 0) {
                    if (tbsType != 5) {
                        *negative = true;
                        return {};
                    }
                    sequence.value = -sequence.value;
                } else {
                    sequence.fromPrevious = true;
                }
            }
            lastIndex = entries.back().index;
            strandSequences.append(sequence);
        }
        // Of consecutive spans, only the last one says so.
        for (qsizetype i = 0; i + 1 < strandSequences.size(); ++i) {
            if (strandSequences.at(i).spans && strandSequences.at(i + 1).spans)
                strandSequences[i].spans.reset();
        }
        sequences += strandSequences;
    }
    return sequences;
}

// encode_tbs() of each sequence, the first with 3 bits of flags.
QByteArray bytesOf(const QList<Sequence> &sequences)
{
    QByteArray bytes;
    int flagSize = 3;
    for (const Sequence &sequence : sequences) {
        const quint64 flags = (sequence.spans ? 0b1 : 0) | (sequence.type ? 0b10 : 0) | (sequence.count ? 0b100 : 0)
                            | (sequence.fromPrevious ? 0b1000 : 0);
        bytes += encint((quint64(std::max<qint64>(sequence.value, 0)) << flagSize) | (flags & ((1u << flagSize) - 1)));
        if (sequence.type)
            bytes += encint(*sequence.type);
        if (sequence.count)
            bytes.append(char(*sequence.count));
        if (sequence.spans)
            bytes += encint(*sequence.spans);
        flagSize = 4;
    }
    return bytes;
}

} // namespace

QList<QByteArray> skeletonIndex(const QList<Skeleton> &skeletons)
{
    static const QList<TagMeta> tags{ { "chunk_count", 1, 1, 3, 0 }, { "geometry", 6, 2, 12, 0 }, endTag };
    QList<Entry> entries;
    for (const Skeleton &skeleton : skeletons) {
        Entry entry;
        entry.key = QStringLiteral("SKEL%1").arg(skeleton.fileNumber, 10, 10, QLatin1Char('0')).toUtf8();
        // Each twice, as kindlegen writes them.
        entry.tags.insert(QStringLiteral("chunk_count"), values({ skeleton.chunkCount, skeleton.chunkCount }));
        entry.tags.insert(QStringLiteral("geometry"), values({ skeleton.start, skeleton.length, skeleton.start, skeleton.length }));
        entries.append(entry);
    }
    return index(tags, entries, Cncx());
}

QList<QByteArray> chunkIndex(const QList<Chunk> &chunks)
{
    static const QList<TagMeta> tags{ { "cncx_offset", 2, 1, 1, 0 },
                                      { "file_number", 3, 1, 2, 0 },
                                      { "sequence_number", 4, 1, 4, 0 },
                                      { "geometry", 6, 2, 8, 0 },
                                      endTag };
    QStringList selectors;
    for (const Chunk &chunk : chunks)
        selectors.append(chunk.selector);
    const Cncx cncx(selectors);
    QList<Entry> entries;
    for (const Chunk &chunk : chunks) {
        Entry entry;
        entry.key = QStringLiteral("%1").arg(chunk.insertPosition, 10, 10, QLatin1Char('0')).toUtf8();
        entry.tags.insert(QStringLiteral("cncx_offset"), values({ cncx.offset(chunk.selector) }));
        entry.tags.insert(QStringLiteral("file_number"), values({ chunk.fileNumber }));
        entry.tags.insert(QStringLiteral("sequence_number"), values({ chunk.sequenceNumber }));
        entry.tags.insert(QStringLiteral("geometry"), values({ chunk.start, chunk.length }));
        entries.append(entry);
    }
    return index(tags, entries, cncx);
}

QList<QByteArray> guideIndex(const QList<GuideReference> &references)
{
    static const QList<TagMeta> tags{ { "title", 1, 1, 1, 0 }, { "pos_fid", 6, 2, 2, 0 }, endTag };
    QStringList titles;
    for (const GuideReference &reference : references)
        titles.append(reference.title);
    const Cncx cncx(titles);
    QList<Entry> entries;
    for (const GuideReference &reference : references) {
        Entry entry;
        entry.key = reference.type.toUtf8();
        entry.tags.insert(QStringLiteral("title"), values({ cncx.offset(reference.title) }));
        entry.tags.insert(QStringLiteral("pos_fid"), values({ reference.position, reference.offset }));
        entries.append(entry);
    }
    return index(tags, entries, cncx);
}

QList<QByteArray> ncxIndex(const QList<NcxEntry> &ncx)
{
    static const QList<TagMeta> tags{ { "offset", 1, 1, 1, 0 },         { "length", 2, 1, 2, 0 },
                                      { "label", 3, 1, 4, 0 },          { "depth", 4, 1, 8, 0 },
                                      { "parent", 21, 1, 16, 0 },       { "first_child", 22, 1, 32, 0 },
                                      { "last_child", 23, 1, 64, 0 },   { "pos_fid", 6, 2, 128, 0 },
                                      endTag };
    QStringList labels;
    int largest = 0;
    for (const NcxEntry &entry : ncx) {
        labels.append(entry.label);
        largest = std::max(largest, entry.index);
    }
    const Cncx cncx(labels);
    const int width = std::max<int>(2, int(QString::number(largest, 16).size()));
    QList<Entry> entries;
    for (const NcxEntry &item : ncx) {
        Entry entry;
        entry.key = QStringLiteral("%1").arg(item.index, width, 16, QLatin1Char('0')).toUpper().toUtf8();
        entry.tags.insert(QStringLiteral("offset"), values({ item.offset }));
        entry.tags.insert(QStringLiteral("length"), values({ item.length }));
        entry.tags.insert(QStringLiteral("label"), values({ cncx.offset(item.label) }));
        entry.tags.insert(QStringLiteral("depth"), values({ item.depth }));
        entry.tags.insert(QStringLiteral("pos_fid"), values({ item.position, item.fidOffset }));
        if (item.parent)
            entry.tags.insert(QStringLiteral("parent"), values({ *item.parent }));
        if (item.firstChild)
            entry.tags.insert(QStringLiteral("first_child"), values({ *item.firstChild }));
        if (item.lastChild)
            entry.tags.insert(QStringLiteral("last_child"), values({ *item.lastChild }));
        entries.append(entry);
    }
    return index(tags, entries, cncx);
}

QList<QByteArray> trailingByteSequences(const QList<NcxEntry> &ncx, const QList<qsizetype> &recordLengths)
{
    // collect_indexing_data(): the entries each record touches, by strands.
    QList<NcxEntry> sorted = ncx;
    std::stable_sort(sorted.begin(), sorted.end(), [](const NcxEntry &a, const NcxEntry &b) { return a.offset < b.offset; });
    QList<QList<Strand>> data;
    qint64 recordStart = 0;
    for (const qsizetype length : recordLengths) {
        const qint64 nextStart = recordStart + length;
        QList<Located> local;
        for (const NcxEntry &entry : std::as_const(sorted)) {
            if (qint64(entry.offset) >= nextStart)
                break;
            if (qint64(entry.offset) + qint64(entry.length) <= recordStart)
                continue;
            Located located;
            located.index = entry.index;
            located.start = entry.offset;
            located.length = entry.length;
            located.depth = entry.depth;
            located.parent = entry.parent;
            const qint64 startOffset = qint64(entry.offset) - recordStart;
            const qint64 lengthOffset = startOffset + qint64(entry.length);
            if (startOffset < 0)
                located.action = lengthOffset > length ? Located::Action::Spans : Located::Action::Ends;
            else
                located.action = lengthOffset > length ? Located::Action::Starts : Located::Action::Completes;
            local.append(located);
        }
        data.append(separateStrands(local));
        recordStart = nextStart;
    }
    const auto all = [&data](int tbsType, bool *negative) {
        QList<QByteArray> entries;
        for (const QList<Strand> &strands : data)
            entries.append(trailingEntry(bytesOf(sequencesOf(strands, tbsType, negative))));
        return entries;
    };
    bool negative = false;
    const QList<QByteArray> entries = all(8, &negative);
    return negative ? all(5, &negative) : entries;
}

} // namespace Kf8Index
