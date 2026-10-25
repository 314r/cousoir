// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/mobi/reader/headers.py: EXTHHeader and BookHeader,
//   the fields a reader needs
// - src/calibre/ebooks/mobi/reader/mobi6.py: MobiReader's sections, the KF8
//   of a joint file, check_for_drm(), sizeof_trailing_entries(),
//   extract_text()
// - src/calibre/ebooks/mobi/reader/mobi8.py: Mobi8Reader's read_indices(),
//   build_parts(), get_file_info(), get_id_tag_by_pos_fid(), get_id_tag(),
//   create_ncx(), extract_resources()
// - src/calibre/ebooks/mobi/reader/index.py: read_index() with the INDX,
//   TAGX, CNCX and IDXT records it reads
// - src/calibre/ebooks/mobi/reader/markup.py: expand_mobi8_markup()
// - src/calibre/ebooks/mobi/reader/ncx.py: read_ncx(), build_toc()
// - src/calibre/ebooks/mobi/reader/containers.py: Container
// - src/calibre/ebooks/mobi/huffcdic.py: HuffReader
// - src/calibre/ebooks/compression/palmdoc.c: cpalmdoc_decompress()
// - src/calibre/ebooks/mobi/utils.py: decint(), decode_string(),
//   read_font_record()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "mobi_book.h"

#include <QFile>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>

#include <zip.h>

#include <functional>
#include <optional>

#include "calibre_text.h"
#include "html_soup.h"
#include "xml_tree.h"

namespace MobiBook {
namespace {

constexpr quint32 nullIndex = 0xFFFFFFFF;

quint32 u32(const QByteArray &data, qsizetype at)
{
    return at >= 0 && at + 4 <= data.size() ? qFromBigEndian<quint32>(data.constData() + at) : 0;
}

quint16 u16(const QByteArray &data, qsizetype at)
{
    return at >= 0 && at + 2 <= data.size() ? qFromBigEndian<quint16>(data.constData() + at) : 0;
}

quint8 u8(const QByteArray &data, qsizetype at)
{
    return at >= 0 && at < data.size() ? quint8(data.at(at)) : 0;
}

// The markup is worked on as bytes, as Calibre works on it: a byte a
// character, its ASCII markup matched by the expressions, decoded once done.
QString bytes(const QByteArray &data)
{
    return QString::fromLatin1(data);
}

// A number in base 32, its digits 0-9 and A-V.
qint64 base32(const QString &digits)
{
    bool ok = false;
    const qint64 value = digits.toLongLong(&ok, 32);
    return ok ? value : -1;
}

// --- Decompression ----------------------------------------------------------

// cpalmdoc_decompress().
QByteArray palmdoc(const QByteArray &input)
{
    QByteArray out;
    out.reserve(input.size() * 2);
    qsizetype i = 0;
    while (i < input.size()) {
        quint32 c = quint8(input.at(i++));
        if (c >= 1 && c <= 8) {
            while (c-- && i < input.size())
                out.append(input.at(i++));
        } else if (c <= 0x7F) {
            out.append(char(c));
        } else if (c >= 0xC0) {
            out.append(' ');
            out.append(char(c ^ 0x80));
        } else if (i < input.size()) {
            c = (c << 8) + quint8(input.at(i++));
            const qsizetype distance = (c & 0x3FFF) >> 3;
            if (distance <= out.size() && distance > 0) {
                for (quint32 n = (c & 7) + 3; n--;)
                    out.append(out.at(out.size() - distance));
            }
        }
    }
    return out;
}

// HuffReader: the HUFF record's codes, the CDIC records' phrases.
class Huff
{
public:
    bool load(const QList<QByteArray> &records, QString *error)
    {
        if (records.isEmpty() || !records.first().startsWith(QByteArray("HUFF\0\0\0\x18", 8))) {
            *error = QObject::tr("The book's HUFF record is not valid.");
            return false;
        }
        const QByteArray &huff = records.first();
        const quint32 off1 = u32(huff, 8);
        const quint32 off2 = u32(huff, 12);
        for (int i = 0; i < 256; ++i) {
            const quint32 v = u32(huff, off1 + 4 * i);
            Code code;
            code.length = v & 0x1F;
            code.term = (v & 0x80) != 0;
            code.max = ((quint64(v >> 8) + 1) << (32 - code.length)) - 1;
            m_dict1[i] = code;
        }
        m_min[0] = 0;
        m_max[0] = (quint64(1) << 32) - 1;
        for (int length = 1; length <= 32; ++length) {
            m_min[length] = quint64(u32(huff, off2 + 8 * (length - 1))) << (32 - length);
            m_max[length] = ((quint64(u32(huff, off2 + 8 * (length - 1) + 4)) + 1) << (32 - length)) - 1;
        }
        for (qsizetype r = 1; r < records.size(); ++r) {
            const QByteArray &cdic = records.at(r);
            if (!cdic.startsWith(QByteArray("CDIC\0\0\0\x10", 8))) {
                *error = QObject::tr("The book's CDIC record is not valid.");
                return false;
            }
            const quint32 phrases = u32(cdic, 8);
            const quint32 bits = u32(cdic, 12);
            const qsizetype n = std::min<qsizetype>(qsizetype(1) << std::min<quint32>(bits, 30),
                                                    qsizetype(phrases) - m_dictionary.size());
            for (qsizetype k = 0; k < n; ++k) {
                const quint16 offset = u16(cdic, 16 + 2 * k);
                const quint16 length = u16(cdic, 16 + offset);
                m_dictionary.append({ cdic.mid(18 + offset, length & 0x7FFF), (length & 0x8000) != 0 });
            }
        }
        return true;
    }

    QByteArray unpack(QByteArray data, int depth = 0)
    {
        QByteArray out;
        qint64 bitsLeft = qint64(data.size()) * 8;
        data.append(QByteArray(8, '\0'));
        qsizetype pos = 0;
        const auto q = [&data](qsizetype at) { return qFromBigEndian<quint64>(data.constData() + at); };
        quint64 x = q(pos);
        int n = 32;
        while (depth < 64) {
            if (n <= 0) {
                pos += 4;
                if (pos + 8 > data.size())
                    break;
                x = q(pos);
                n += 32;
            }
            const quint64 code = (x >> n) & 0xFFFFFFFFULL;
            Code entry = m_dict1[code >> 24];
            int length = int(entry.length);
            quint64 max = entry.max;
            if (!entry.term) {
                while (length < 32 && code < m_min[length])
                    ++length;
                max = m_max[length];
            }
            n -= length;
            bitsLeft -= length;
            if (bitsLeft < 0 || length == 0)
                break;
            const quint64 r = (max - code) >> (32 - length);
            if (r >= quint64(m_dictionary.size()))
                break;
            Phrase &phrase = m_dictionary[qsizetype(r)];
            if (!phrase.done) {
                phrase.done = true;
                phrase.data = unpack(phrase.data, depth + 1);
            }
            out += phrase.data;
        }
        return out;
    }

private:
    struct Code
    {
        quint32 length = 0;
        bool term = false;
        quint64 max = 0;
    };
    struct Phrase
    {
        QByteArray data;
        bool done = false;
    };
    Code m_dict1[256];
    quint64 m_min[33] = {};
    quint64 m_max[33] = {};
    QList<Phrase> m_dictionary;
};

// --- Headers ----------------------------------------------------------------

// BookHeader, what the reader needs of it.
struct Header
{
    QByteArray compression;
    quint16 records = 0;
    quint16 encryption = 0;
    bool utf8 = false;
    quint16 extraFlags = 0;
    quint32 huffOffset = 0;
    quint32 huffNumber = 0;
    QByteArray title;
    quint32 mobiVersion = 0;
    quint32 firstImageIndex = nullIndex;
    quint32 ncxIndex = nullIndex;
    quint32 divIndex = nullIndex;
    quint32 skelIndex = nullIndex;
    quint32 othIndex = nullIndex;
    quint32 fdstIndex = nullIndex;
    // EXTH records, in their order.
    QList<QPair<quint32, QByteArray>> exth;

    QByteArray exthValue(quint32 id) const
    {
        for (const auto &[key, value] : exth) {
            if (key == id)
                return value;
        }
        return {};
    }
    std::optional<quint32> exthNumber(quint32 id) const
    {
        const QByteArray value = exthValue(id);
        if (value.size() != 4)
            return std::nullopt;
        return u32(value, 0);
    }
};

Header readHeader(const QByteArray &raw, bool textread)
{
    Header header;
    header.compression = raw.left(2);
    header.records = u16(raw, 8);
    header.encryption = u16(raw, 12);
    if (raw.size() <= 16)
        return header;
    const quint32 length = u32(raw, 20);
    header.utf8 = u32(raw, 28) == 65001;
    if (!textread && length >= 0xE4 && length <= 500)
        header.extraFlags = u16(raw, 0xF2);
    if (header.compression == "DH") {
        header.huffOffset = u32(raw, 0x70);
        header.huffNumber = u32(raw, 0x74);
    }
    const quint32 titleOffset = u32(raw, 0x54);
    const quint32 titleLength = u32(raw, 0x58);
    if (qsizetype(titleOffset) + titleLength < raw.size())
        header.title = raw.mid(titleOffset, titleLength);
    header.mobiVersion = u32(raw, 0x68);
    header.firstImageIndex = u32(raw, 0x6C);
    if (u32(raw, 0x80) & 0x40) {
        const QByteArray exth = raw.mid(16 + length);
        if (exth.startsWith("EXTH")) {
            const quint32 count = u32(exth, 8);
            qsizetype pos = 12;
            for (quint32 i = 0; i < count && pos + 8 <= exth.size(); ++i) {
                const quint32 id = u32(exth, pos);
                const quint32 size = u32(exth, pos + 4);
                if (size < 8)
                    break;
                header.exth.append({ id, exth.mid(pos + 8, size - 8) });
                pos += size;
            }
        }
    }
    if (raw.size() >= 0xF8)
        header.ncxIndex = u32(raw, 0xF4);
    if (header.mobiVersion == 8 && raw.size() >= 0xF8 + 16) {
        header.divIndex = u32(raw, 0xF8);
        header.skelIndex = u32(raw, 0xFC);
        header.othIndex = u32(raw, 0x104);
        header.fdstIndex = u32(raw, 0xC0);
        if (u32(raw, 0xC4) <= 1)
            header.fdstIndex = nullIndex;
    }
    return header;
}

// --- Indices ----------------------------------------------------------------

// decint(): a number of 7 bits a byte, the last byte's high bit set.
quint64 decint(const QByteArray &data, qsizetype at, qsizetype *consumed)
{
    quint64 value = 0;
    qsizetype i = at;
    while (i < data.size()) {
        const quint8 byte = quint8(data.at(i++));
        value = (value << 7) | (byte & 0x7F);
        if (byte & 0x80)
            break;
    }
    *consumed = i - at;
    return value;
}

struct Entry
{
    QString ident;
    QMap<int, QList<quint64>> tags;

    quint64 tag(int id, int index = 0) const
    {
        const QList<quint64> values = tags.value(id);
        return index < values.size() ? values.at(index) : 0;
    }
};

struct Index
{
    QList<Entry> entries;
    QHash<quint32, QString> cncx;
};

struct TagX
{
    quint8 tag = 0;
    quint8 values = 0;
    quint8 mask = 0;
    quint8 eof = 0;
};

// get_tag_map().
QMap<int, QList<quint64>> tagMap(const QList<TagX> &tagx, const QByteArray &record, quint32 controlBytes)
{
    struct PTag
    {
        quint8 tag;
        std::optional<quint64> count;
        std::optional<quint64> bytes;
        quint8 values;
    };
    QByteArray control = record.left(controlBytes);
    qsizetype pos = controlBytes;
    QList<PTag> ptags;
    for (const TagX &x : tagx) {
        if (x.eof == 1) {
            control.remove(0, 1);
            continue;
        }
        if (control.isEmpty())
            break;
        quint32 value = quint8(control.at(0)) & x.mask;
        if (!value)
            continue;
        PTag ptag{ x.tag, std::nullopt, std::nullopt, x.values };
        if (value == x.mask) {
            if (qPopulationCount(x.mask) > 1) {
                qsizetype consumed = 0;
                ptag.bytes = decint(record, pos, &consumed);
                pos += consumed;
            } else {
                ptag.count = 1;
            }
        } else {
            quint32 mask = x.mask;
            while (mask && !(mask & 1)) {
                mask >>= 1;
                value >>= 1;
            }
            ptag.count = value;
        }
        ptags.append(ptag);
    }
    QMap<int, QList<quint64>> map;
    for (const PTag &ptag : std::as_const(ptags)) {
        QList<quint64> values;
        if (ptag.count) {
            for (quint64 k = 0; k < *ptag.count * ptag.values && pos < record.size(); ++k) {
                qsizetype consumed = 0;
                values.append(decint(record, pos, &consumed));
                pos += consumed;
            }
        } else {
            quint64 total = 0;
            while (total < *ptag.bytes && pos < record.size()) {
                qsizetype consumed = 0;
                values.append(decint(record, pos, &consumed));
                pos += consumed;
                total += consumed;
            }
        }
        map.insert(ptag.tag, values);
    }
    return map;
}

// read_index(): the table of an index, and its CNCX strings.
Index readIndex(const QList<QByteArray> &sections, quint32 idx, const std::function<QString(const QByteArray &)> &decode)
{
    Index index;
    if (idx >= quint32(sections.size()))
        return index;
    const QByteArray &data = sections.at(idx);
    if (!data.startsWith("INDX"))
        return index;
    const auto field = [](const QByteArray &record, int i) { return u32(record, 4 + 4 * i); };
    const quint32 count = field(data, 5);
    const quint32 code = field(data, 6);
    const quint32 ncncx = field(data, 12);
    const quint32 oentries = field(data, 41);
    const quint32 ordt2 = field(data, 43);
    // The ORDT that maps an entry's bytes, of an EBCDIC-UTF table among them.
    QString ordtMap;
    if (ordt2 > 0 && data.mid(ordt2, 4) == "ORDT") {
        const QByteArray raw = data.mid(ordt2 + 4, 2 * oentries);
        for (quint32 i = 0; i < oentries; ++i) {
            const quint8 second = u8(raw, 2 * i + 1);
            ordtMap += code == 65002 && second > 0x20 && second < 0x7F ? QChar(second) : QChar(u'?');
        }
    }
    if (ncncx > 0) {
        quint32 recordOffset = 0;
        for (quint32 r = 0; r < ncncx; ++r) {
            const QByteArray raw = sections.value(qsizetype(idx) + count + 1 + r);
            qsizetype pos = 0;
            while (pos < raw.size()) {
                qsizetype consumed = 0;
                const quint64 length = decint(raw, pos, &consumed);
                if (consumed == 0)
                    break;
                if (length > 0)
                    index.cncx.insert(quint32(pos) + recordOffset, decode(raw.mid(pos + consumed, qsizetype(length))));
                pos += consumed + qsizetype(length);
            }
            recordOffset += 0x10000;
        }
    }
    qsizetype tagxStart = field(data, 44);
    if (data.mid(tagxStart, 4) != "TAGX")
        tagxStart = data.indexOf("TAGX", 184);
    if (tagxStart < 0)
        return index;
    const quint32 firstEntry = u32(data, tagxStart + 4);
    const quint32 controlBytes = u32(data, tagxStart + 8);
    QList<TagX> tagx;
    for (qsizetype i = 12; i + 4 <= qsizetype(firstEntry); i += 4)
        tagx.append({ u8(data, tagxStart + i), u8(data, tagxStart + i + 1), u8(data, tagxStart + i + 2),
                      u8(data, tagxStart + i + 3) });
    QHash<QString, qsizetype> positions;
    for (quint32 i = idx + 1; i <= idx + count && i < quint32(sections.size()); ++i) {
        const QByteArray &record = sections.at(i);
        const quint32 idxt = field(record, 4);
        const quint32 entries = field(record, 5);
        QList<quint32> starts;
        for (quint32 j = 0; j < entries; ++j)
            starts.append(u16(record, idxt + 4 + 2 * j));
        starts.append(idxt);
        for (quint32 j = 0; j < entries; ++j) {
            const QByteArray entry = record.mid(starts.at(j), qsizetype(starts.at(j + 1)) - starts.at(j));
            // decode_string(): a length, then the ident.
            const quint8 length = u8(entry, 0);
            const QByteArray raw = entry.mid(1, length);
            QString ident;
            if (!ordtMap.isEmpty()) {
                for (const char c : raw)
                    ident += quint8(c) < ordtMap.size() ? ordtMap.at(quint8(c)) : QChar(u'?');
            } else {
                ident = decode(raw);
            }
            const QMap<int, QList<quint64>> tags = tagMap(tagx, entry.mid(1 + length), controlBytes);
            // As an ordered dictionary: an ident again changes its entry.
            const auto found = positions.constFind(ident);
            if (found != positions.cend()) {
                index.entries[*found].tags = tags;
            } else {
                positions.insert(ident, index.entries.size());
                index.entries.append({ ident, tags });
            }
        }
    }
    return index;
}

// --- The book ---------------------------------------------------------------

struct Part
{
    QString filename;
    qint64 start = 0;
    qint64 end = 0;
};

struct Element
{
    qint64 insertPos = 0;
    QString tocText;
    quint64 fileNumber = 0;
    qint64 startPos = 0;
    qint64 length = 0;
};

struct FlowInfo
{
    // "svg" or "css"; "inline" or "file"; where the file goes.
    QString type;
    QString format;
    QString dir;
    QString name;
};

struct TocItem
{
    QString title;
    QString href;
    QList<TocItem> children;
};

// What find_imgtype() tells of a picture's bytes, empty for none.
QString imageType(const QByteArray &data)
{
    if (data.startsWith("\xFF\xD8\xFF"))
        return QStringLiteral("jpg");
    if (data.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("png");
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a"))
        return QStringLiteral("gif");
    if (data.startsWith("BM"))
        return QStringLiteral("bmp");
    if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return QStringLiteral("webp");
    if (data.startsWith("II*\0") || data.startsWith("MM\0*"))
        return QStringLiteral("tiff");
    return {};
}

QString mediaTypeOf(const QString &name)
{
    static const QHash<QString, QString> types{
        { QStringLiteral("jpg"), QStringLiteral("image/jpeg") },  { QStringLiteral("png"), QStringLiteral("image/png") },
        { QStringLiteral("gif"), QStringLiteral("image/gif") },   { QStringLiteral("bmp"), QStringLiteral("image/bmp") },
        { QStringLiteral("webp"), QStringLiteral("image/webp") }, { QStringLiteral("tiff"), QStringLiteral("image/tiff") },
        { QStringLiteral("svg"), QStringLiteral("image/svg+xml") }, { QStringLiteral("css"), QStringLiteral("text/css") },
        { QStringLiteral("ttf"), QStringLiteral("application/x-font-truetype") },
        { QStringLiteral("otf"), QStringLiteral("application/vnd.ms-opentype") },
        { QStringLiteral("dat"), QStringLiteral("application/octet-stream") },
    };
    return types.value(name.section(u'.', -1).toLower(), QStringLiteral("application/octet-stream"));
}

// read_font_record(): the font, its XOR obfuscation undone and inflated.
QPair<QByteArray, QString> readFont(const QByteArray &data)
{
    const quint32 size = u32(data, 4);
    const quint32 flags = u32(data, 8);
    const quint32 start = u32(data, 12);
    const quint32 keyLength = u32(data, 16);
    const quint32 keyStart = u32(data, 20);
    QByteArray font = data.mid(start);
    if ((flags & 0b10) && keyLength > 0) {
        const QByteArray key = data.mid(keyStart, keyLength);
        const qsizetype extent = std::min<qsizetype>(1040, font.size());
        for (qsizetype n = 0; n < extent && !key.isEmpty(); ++n)
            font[n] = char(font.at(n) ^ key.at(n % key.size()));
    }
    if (flags & 0b1) {
        QByteArray sized(4, '\0');
        qToBigEndian(size, sized.data());
        font = qUncompress(sized + font);
        if (font.size() != qsizetype(size))
            return {};
    }
    const QByteArray signature = font.left(4);
    const QString ext = signature == QByteArray("\0\1\0\0", 4) || signature == "true" || signature == "ttcf" ? QStringLiteral("ttf")
                        : signature == "OTTO" ? QStringLiteral("otf")
                                              : QStringLiteral("dat");
    return { font, ext };
}

// Named characters of HTML as numbers, XML's own five aside: an XHTML page
// read as XML knows no others.
QString xmlEntities(const QString &text)
{
    static const QRegularExpression entity(QStringLiteral("&([A-Za-z][A-Za-z0-9]*);"));
    static const QSet<QString> xml{ QStringLiteral("amp"), QStringLiteral("lt"), QStringLiteral("gt"), QStringLiteral("quot"),
                                    QStringLiteral("apos") };
    QString result;
    qsizetype last = 0;
    for (QRegularExpressionMatchIterator it = entity.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (xml.contains(match.captured(1)))
            continue;
        const QString replaced = CalibreText::replaceEntities(match.captured(0));
        if (replaced == match.captured(0))
            continue;
        result += QStringView(text).mid(last, match.capturedStart() - last);
        for (const char32_t code : replaced.toUcs4())
            result += QStringLiteral("&#%1;").arg(quint32(code));
        last = match.capturedEnd();
    }
    result += QStringView(text).mid(last);
    return result;
}

class Reader
{
public:
    bool read(const QByteArray &file, QString *error);
    bool writeEpub(const QString &path, QString *error);

private:
    QString decode(const QByteArray &data) const;
    QByteArray textSection(qsizetype index) const;
    bool extractText(qsizetype offset, QString *error);
    void readIndices();
    void buildParts();
    const Part *fileInfo(qint64 pos) const;
    int fileIndex(qint64 pos) const;
    QString idTag(qint64 pos);
    std::optional<QPair<QString, QString>> idTagByPosFid(quint64 posFid, quint64 offset);
    void extractResources();
    void expandMarkup();
    QList<TocItem> readNcx();
    // KF8's pages and styles, as the EPUB's.
    void finishKf8();
    // A MOBI of the old kind: its one page of HTML made pages of XHTML.
    void readMobi6();

    QList<QByteArray> m_sections;
    Header m_header;
    QList<QByteArray> m_kf8;
    QByteArray m_raw;
    QList<QPair<qint64, qint64>> m_resourceRanges;
    QList<QPair<qint64, qint64>> m_flowTable;
    struct File
    {
        QString name;
        quint64 divCount = 0;
        qint64 start = 0;
        qint64 length = 0;
    };
    QList<File> m_files;
    QList<Element> m_elements;
    QList<QString> m_parts;
    QList<Part> m_partInfo;
    QList<QString> m_flows;
    QList<FlowInfo> m_flowInfo;
    QSet<QString> m_linkedAids;
    // By resource, from the first: its file, empty for none.
    QStringList m_resourceMap;
    // The EPUB's files besides its package: its pages, in their order, the
    // others by name.
    QMap<QString, QByteArray> m_resources;
    QList<QPair<QString, QByteArray>> m_pages;
    QList<TocItem> m_toc;
};

QString Reader::decode(const QByteArray &data) const
{
    if (m_header.utf8)
        return QString::fromUtf8(data);
    QStringDecoder decoder("windows-1252");
    return decoder.isValid() ? QString(decoder(data)) : QString::fromLatin1(data);
}

bool Reader::read(const QByteArray &file, QString *error)
{
    if (file.startsWith("TPZ")) {
        *error = QObject::tr("This is an Amazon Topaz book, which Cousoir cannot read.");
        return false;
    }
    if (file.startsWith("\xea" "DRMION\xee")) {
        *error = QObject::tr("This is an Amazon KFX book, which Cousoir cannot read.");
        return false;
    }
    const QByteArray ident = file.mid(0x3C, 8).toUpper();
    if (ident != "BOOKMOBI" && ident != "TEXTREAD") {
        *error = QObject::tr("This is not a MOBI or AZW3 book.");
        return false;
    }
    const quint16 count = u16(file, 76);
    for (quint16 i = 0; i < count; ++i) {
        const quint32 start = u32(file, 78 + 8 * i);
        const quint32 end = i + 1 < count ? u32(file, 78 + 8 * (i + 1)) : quint32(file.size());
        m_sections.append(file.mid(start, qsizetype(end) - start));
    }
    if (m_sections.isEmpty()) {
        *error = QObject::tr("This book has no records.");
        return false;
    }
    m_header = readHeader(m_sections.first(), ident == "TEXTREAD");
    const Header mobi6 = m_header;
    // The KF8 part: the whole book, or after the boundary of a joint one.
    qsizetype offset = 1;
    if (!(m_header.mobiVersion == 8 && m_header.skelIndex != nullIndex)) {
        const std::optional<quint32> boundary = m_header.exthNumber(121);
        if (!boundary || *boundary == nullIndex || *boundary == 0 || *boundary >= quint32(m_sections.size())
            || m_sections.at(*boundary - 1) != "BOUNDARY") {
            // A MOBI of the old kind alone.
            if (m_header.encryption != 0) {
                *error = QObject::tr("This book has DRM, which Cousoir cannot read.");
                return false;
            }
            if (ident == "TEXTREAD" || m_sections.first().size() <= 16) {
                *error = QObject::tr("This is a PalmDoc book, which Cousoir cannot read.");
                return false;
            }
            if (!extractText(1, error))
                return false;
            readMobi6();
            return true;
        }
        const quint32 k8 = *boundary;
        m_header = readHeader(m_sections.at(k8), false);
        if (m_header.huffOffset)
            m_header.huffOffset += k8;
        offset = k8 + 1;
        const auto firstResource = [](quint32 firstImage, quint32 records, qsizetype first) {
            return firstImage == nullIndex || firstImage == quint32(-1) ? qint64(records) + first : qint64(firstImage);
        };
        m_resourceRanges = { { firstResource(mobi6.firstImageIndex, mobi6.records, 1), k8 - 1 },
                             { firstResource(m_header.firstImageIndex == nullIndex ? nullIndex : m_header.firstImageIndex + k8,
                                             m_header.records, offset),
                               m_sections.size() } };
        // The EXTH of the KF8 header is the book's.
        if (m_header.exth.isEmpty())
            m_header.exth = mobi6.exth;
    } else {
        const quint32 first = m_header.firstImageIndex;
        m_resourceRanges = { { first == nullIndex ? qint64(m_header.records) + 1 : qint64(first), m_sections.size() } };
    }
    if (m_header.encryption != 0) {
        *error = QObject::tr("This book has DRM, which Cousoir cannot read.");
        return false;
    }
    if (!extractText(offset, error))
        return false;
    m_kf8 = m_sections.mid(offset - 1);
    readIndices();
    buildParts();
    extractResources();
    expandMarkup();
    finishKf8();
    return true;
}

// text_section(): a text record without its trailing entries.
QByteArray Reader::textSection(qsizetype index) const
{
    const QByteArray data = m_sections.value(index);
    qsizetype size = data.size();
    qsizetype num = 0;
    const auto trailing = [&](qsizetype end) {
        quint32 result = 0;
        int bits = 0;
        while (end > 0) {
            const quint8 v = quint8(data.at(end - 1));
            result |= quint32(v & 0x7F) << bits;
            bits += 7;
            --end;
            if ((v & 0x80) || bits >= 28 || end == 0)
                break;
        }
        return qsizetype(result);
    };
    for (quint16 flags = m_header.extraFlags >> 1; flags; flags >>= 1) {
        if (flags & 1)
            num += trailing(size - num);
    }
    if ((m_header.extraFlags & 1) && size - num - 1 >= 0)
        num += (quint8(data.at(size - num - 1)) & 0x3) + 1;
    return data.left(std::max<qsizetype>(0, size - num));
}

bool Reader::extractText(qsizetype offset, QString *error)
{
    std::function<QByteArray(const QByteArray &)> unpack;
    Huff huff;
    if (m_header.compression == "DH") {
        QList<QByteArray> records;
        for (quint32 i = 0; i < m_header.huffNumber; ++i)
            records.append(m_sections.value(qsizetype(m_header.huffOffset) + i));
        if (!huff.load(records, error))
            return false;
        unpack = [&huff](const QByteArray &data) { return huff.unpack(data); };
    } else if (m_header.compression == QByteArray("\0\2", 2)) {
        unpack = palmdoc;
    } else if (m_header.compression == QByteArray("\0\1", 2)) {
        unpack = [](const QByteArray &data) { return data; };
    } else {
        *error = QObject::tr("This book is compressed in a way Cousoir does not know.");
        return false;
    }
    for (qsizetype i = offset; i < std::min<qsizetype>(offset + m_header.records, m_sections.size()); ++i)
        m_raw += unpack(textSection(i));
    if (m_raw.endsWith('#'))
        m_raw.chop(1);
    return true;
}

void Reader::readIndices()
{
    const auto decode = [this](const QByteArray &data) { return this->decode(data); };
    if (m_header.fdstIndex != nullIndex && m_header.fdstIndex < quint32(m_kf8.size())) {
        const QByteArray &fdst = m_kf8.at(m_header.fdstIndex);
        if (fdst.startsWith("FDST")) {
            const quint32 start = u32(fdst, 4);
            const quint32 count = u32(fdst, 8);
            for (quint32 i = 0; i < count; ++i)
                m_flowTable.append({ u32(fdst, start + 8 * i), u32(fdst, start + 8 * i + 4) });
        }
    }
    if (m_header.skelIndex != nullIndex) {
        const Index table = readIndex(m_kf8, m_header.skelIndex, decode);
        for (const Entry &entry : table.entries)
            m_files.append({ entry.ident, entry.tag(1), qint64(entry.tag(6)), qint64(entry.tag(6, 1)) });
    }
    if (m_header.divIndex != nullIndex) {
        const Index table = readIndex(m_kf8, m_header.divIndex, decode);
        for (const Entry &entry : table.entries)
            m_elements.append({ entry.ident.toLongLong(), table.cncx.value(quint32(entry.tag(2))), entry.tag(3),
                                qint64(entry.tag(6)), qint64(entry.tag(6, 1)) });
    }
}

void Reader::buildParts()
{
    const QList<QPair<qint64, qint64>> table = m_flowTable.isEmpty() ? QList<QPair<qint64, qint64>>{ { 0, m_raw.size() } }
                                                                     : m_flowTable;
    for (const auto &[start, end] : table)
        m_flows.append(bytes(m_raw.mid(start, end - start)));
    const QString text = m_flows.value(0);
    if (!m_flows.isEmpty())
        m_flows[0].clear();

    // The skeletons, each with its fragments put back in.
    qsizetype div = 0;
    for (const File &file : std::as_const(m_files)) {
        qint64 base = file.start + file.length;
        QString skeleton = text.mid(file.start, file.length);
        QString filename;
        QString aid;
        for (quint64 i = 0; i < file.divCount && div < m_elements.size(); ++i, ++div) {
            const Element &element = m_elements.at(div);
            if (i == 0) {
                aid = element.tocText.mid(12).chopped(std::min<qsizetype>(2, element.tocText.mid(12).size()));
                filename = QStringLiteral("part%1.html").arg(element.fileNumber, 4, 10, QChar(u'0'));
            }
            const QString part = text.mid(base, element.length);
            qint64 insert = element.insertPos - file.start;
            const QString head = skeleton.left(insert);
            const QString tail = skeleton.mid(insert);
            // An insert position inside a tag, of badly made books: after the
            // tag of the fragment's aid.
            if (tail.indexOf(u'>') < tail.indexOf(u'<') || head.lastIndexOf(u'>') < head.lastIndexOf(u'<')) {
                const QRegularExpression tag(QStringLiteral("<[^>]*\\said\\s*=\\s*['\"]%1['\"][^>]*>")
                                                 .arg(QRegularExpression::escape(aid)),
                                             QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch match = tag.match(skeleton);
                if (match.hasMatch())
                    insert = skeleton.indexOf(u'>', match.capturedStart() + 1) + 1 + element.startPos;
            }
            skeleton = skeleton.left(insert) + part + skeleton.mid(insert);
            base += element.length;
        }
        if (file.divCount < 1)
            filename = QStringLiteral("empty%1.html").arg(m_partInfo.size());
        m_parts.append(skeleton);
        m_partInfo.append({ filename, file.start, base });
    }

    // The other flows: styles, and pictures in SVG, inlined when they hold
    // a raster picture.
    m_flowInfo.append(FlowInfo());
    static const QRegularExpression svgTag(QStringLiteral("(<svg[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression imageTag(QStringLiteral("(<(?:svg:)?image[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    for (qsizetype j = 1; j < m_flows.size(); ++j) {
        QString flow = m_flows.at(j);
        const QString number = QStringLiteral("%1").arg(j, 4, 10, QChar(u'0'));
        FlowInfo info;
        const QRegularExpressionMatch svg = svgTag.match(flow);
        if (svg.hasMatch()) {
            info.type = QStringLiteral("svg");
            const QString fromSvg = flow.mid(svg.capturedStart());
            if (imageTag.match(fromSvg).hasMatch()) {
                info.format = QStringLiteral("inline");
                flow = fromSvg;
            } else {
                info.format = QStringLiteral("file");
                info.dir = QStringLiteral("images");
                info.name = QStringLiteral("svgimg") + number + QStringLiteral(".svg");
            }
        } else if (flow.contains(QLatin1String("[CDATA["))) {
            info.type = QStringLiteral("css");
            flow = QStringLiteral("<style type=\"text/css\">\n") + flow + QStringLiteral("\n</style>\n");
            info.format = QStringLiteral("inline");
        } else {
            info.type = QStringLiteral("css");
            info.format = QStringLiteral("file");
            info.dir = QStringLiteral("styles");
            info.name = number + QStringLiteral(".css");
        }
        m_flows[j] = flow;
        m_flowInfo.append(info);
    }
}

int Reader::fileIndex(qint64 pos) const
{
    for (int i = 0; i < m_partInfo.size(); ++i) {
        if (pos >= m_partInfo.at(i).start && pos < m_partInfo.at(i).end)
            return i;
    }
    return -1;
}

const Part *Reader::fileInfo(qint64 pos) const
{
    const int index = fileIndex(pos);
    return index < 0 ? nullptr : &m_partInfo.at(index);
}

// get_id_tag(): the id or name of the last tag before a place, or a tag's
// aid, made an id.
QString Reader::idTag(qint64 pos)
{
    const int index = fileIndex(pos);
    if (index < 0)
        return {};
    QString block = m_parts.at(index);
    qsizetype at = pos - m_partInfo.at(index).start;
    const qsizetype gt = block.indexOf(u'>', at);
    const qsizetype lt = block.indexOf(u'<', at);
    if (lt == at || gt < lt)
        at = gt + 1;
    block = block.left(at);
    static const QRegularExpression id(QStringLiteral("^<[^>]+\\s(?:id|ID)\\s*=\\s*['\"]([^'\"]+)['\"]"));
    static const QRegularExpression name(QStringLiteral("^<\\s*a\\s*\\s(?:name|NAME)\\s*=\\s*['\"]([^'\"]+)['\"]"));
    static const QRegularExpression aidTag(QStringLiteral("^<[^>]+\\s(?:aid|AID)\\s*=\\s*['\"]([^'\"]+)['\"]"));
    qsizetype end = block.size();
    while (true) {
        const qsizetype gtAt = block.lastIndexOf(u'>', end - 1);
        if (gtAt < 0)
            break;
        const qsizetype ltAt = block.lastIndexOf(u'<', gtAt);
        if (ltAt < 0)
            break;
        const QString tag = block.mid(ltAt, gtAt - ltAt + 1);
        QRegularExpressionMatch match = id.match(tag);
        if (!match.hasMatch())
            match = name.match(tag);
        if (match.hasMatch())
            return match.captured(1);
        match = aidTag.match(tag);
        if (match.hasMatch()) {
            m_linkedAids.insert(match.captured(1));
            return match.captured(1) + QStringLiteral("-aid");
        }
        end = ltAt;
        if (end <= 0)
            break;
    }
    return {};
}

std::optional<QPair<QString, QString>> Reader::idTagByPosFid(quint64 posFid, quint64 offset)
{
    if (posFid >= quint64(m_elements.size()))
        return std::nullopt;
    const qint64 pos = m_elements.at(qsizetype(posFid)).insertPos + qint64(offset);
    const Part *part = fileInfo(pos);
    if (!part)
        return std::nullopt;
    return QPair<QString, QString>{ part->filename, idTag(pos) };
}

void Reader::extractResources()
{
    bool imageContainer = false;
    bool inContainer = false;
    int containerIndex = 0;
    static const QSet<QByteArray> ignored{ "FLIS", "FCIS", "SRCS", "\xe9\x8e\r\n", "BOUN", "FDST", "DATP", "AUDI", "VIDE",
                                           "RESC", "CMET", "PAGE" };
    for (const auto &[start, end] : std::as_const(m_resourceRanges)) {
        for (qint64 i = start; i < end && i < m_sections.size(); ++i) {
            const int number = int(i - start) + 1;
            const QByteArray &data = m_sections.at(i);
            const QByteArray type = data.left(4);
            QString href;
            if (ignored.contains(type)) {
            } else if (type == "FONT") {
                const auto [font, ext] = readFont(data);
                if (!font.isEmpty()) {
                    href = QStringLiteral("fonts/%1.%2").arg(number, 5, 10, QChar(u'0')).arg(ext);
                    m_resources.insert(href, font);
                }
            } else if (type == "CONT") {
                if (data == "CONTBOUNDARY") {
                    inContainer = false;
                    continue;
                }
                // Container: whether its EXTH says it holds pictures.
                inContainer = true;
                containerIndex = 0;
                imageContainer = false;
                if (data.size() > 60 && data.mid(48, 4) == "EXTH") {
                    const quint32 length = u32(data, 52);
                    qsizetype pos = 60;
                    while (pos < 60 + qsizetype(length) - 8 && pos + 8 <= data.size()) {
                        const quint32 id = u32(data, pos);
                        const qint64 size = qint64(u32(data, pos + 4)) - 8;
                        pos += 8;
                        if (size < 0)
                            break;
                        if (id == 539) {
                            imageContainer = data.mid(pos, size) == "application/image";
                            break;
                        }
                        pos += size;
                    }
                }
            } else if (type == "CRES" && inContainer) {
                ++containerIndex;
                const QByteArray image = data.mid(12);
                const QString ext = imageType(image);
                if (imageContainer && !ext.isEmpty()) {
                    href = QStringLiteral("images/%1.%2").arg(containerIndex, 5, 10, QChar(u'0')).arg(ext);
                    m_resources.insert(href, image);
                }
            } else if (type == "\xa0\xa0\xa0\xa0" && data.size() == 4 && inContainer) {
                ++containerIndex;
            } else if (!inContainer) {
                const QString ext = imageType(data);
                if (!ext.isEmpty()) {
                    href = QStringLiteral("images/%1.%2").arg(number, 5, 10, QChar(u'0')).arg(ext);
                    m_resources.insert(href, data);
                }
            }
            m_resourceMap.append(href);
        }
    }
}

void Reader::expandMarkup()
{
    // update_internal_links(): kindle:pos:fid:XXXX:off:YYYY, a place in a
    // fragment, made a file and an id, before anything moves.
    static const QRegularExpression link(QStringLiteral("(<a.*?href=.*?>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression posFid(QStringLiteral("['\"]kindle:pos:fid:([0-9|A-V]+):off:([0-9|A-V]+).*?[\"']"));
    const auto eachTag = [](QString *text, const QRegularExpression &pattern, const std::function<QString(const QString &)> &change) {
        QString result;
        qsizetype last = 0;
        for (QRegularExpressionMatchIterator it = pattern.globalMatch(*text); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            result += QStringView(*text).mid(last, match.capturedStart() - last);
            result += change(match.captured(0));
            last = match.capturedEnd();
        }
        result += QStringView(*text).mid(last);
        *text = result;
    };
    const auto subEach = [](QString tag, const QRegularExpression &pattern, const std::function<QString(const QRegularExpressionMatch &)> &replacement) {
        QString result;
        qsizetype last = 0;
        for (QRegularExpressionMatchIterator it = pattern.globalMatch(tag); it.hasNext();) {
            const QRegularExpressionMatch match = it.next();
            result += QStringView(tag).mid(last, match.capturedStart() - last);
            result += replacement(match);
            last = match.capturedEnd();
        }
        result += QStringView(tag).mid(last);
        return result;
    };
    for (QString &part : m_parts) {
        eachTag(&part, link, [&](const QString &tag) {
            return subEach(tag, posFid, [&](const QRegularExpressionMatch &match) {
                const auto target = idTagByPosFid(quint64(base32(match.captured(1))), quint64(base32(match.captured(2))));
                if (!target)
                    return QStringLiteral("\"#\"");
                QString replacement = target->first + (target->second.isEmpty() ? QString() : u'#' + target->second);
                return u'"' + replacement.replace(u'"', QStringLiteral("&quot;")) + u'"';
            });
        });
    }

    // remove_kindlegen_markup(): kindlegen's aids gone, but those linked
    // to, made ids; its page breaks, styles.
    static const QRegularExpression aidTag(QStringLiteral("(<[^>]*\\s[ac]id\\s*=[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression aidAttribute(QStringLiteral("\\s[ac]id\\s*=['\"]([^'\"]*)['\"]"));
    static const QRegularExpression breakTag(QStringLiteral("(<[^>]*\\sdata-AmznPageBreak=[^>]*>)"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression breakAttribute(QStringLiteral("\\sdata-AmznPageBreak=['\"]([^'\"]*)['\"]"));
    for (QString &part : m_parts) {
        eachTag(&part, aidTag, [&](const QString &tag) {
            return subEach(tag, aidAttribute, [&](const QRegularExpressionMatch &match) {
                return m_linkedAids.contains(match.captured(1)) ? QStringLiteral(" id=\"%1-aid\"").arg(match.captured(1))
                                                                : QString();
            });
        });
        eachTag(&part, breakTag, [&](const QString &tag) {
            return subEach(tag, breakAttribute, [](const QRegularExpressionMatch &match) {
                return QStringLiteral(" style=\"page-break-after:%1\"").arg(match.captured(1));
            });
        });
    }

    const auto resource = [this](qint64 number) { return number >= 1 ? m_resourceMap.value(number - 1) : QString(); };
    static const QRegularExpression anyTag(QStringLiteral("<[^>]*>"));
    static const QRegularExpression embed(QStringLiteral("['\"]kindle:embed:([0-9|A-V]+)[^'\"]*['\"]"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression url(QStringLiteral("(url\\(.*?\\))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression urlImage(QStringLiteral("kindle:embed:([0-9|A-V]+)\\?mime=image/[^\\)]*"),
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression urlFont(QStringLiteral("kindle:embed:([0-9|A-V]+)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression urlCss(QStringLiteral("kindle:flow:([0-9|A-V]+)\\?mime=text/css[^\\)]*"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression flowRef(QStringLiteral("['\"]kindle:flow:([0-9|A-V]+)\\?mime=([^'\"]+)['\"]"),
                                            QRegularExpression::CaseInsensitiveOption);
    const auto flowPath = [this](qint64 number) {
        const FlowInfo info = m_flowInfo.value(number);
        return QStringLiteral("../") + info.dir + u'/' + info.name;
    };

    // update_flow_links(): in the styles and pictures, their pictures, fonts
    // and other styles.
    for (qsizetype f = 1; f < m_flows.size(); ++f) {
        QString &flow = m_flows[f];
        eachTag(&flow, anyTag, [&](const QString &tag) {
            if (!tag.startsWith(QLatin1String("<im")) && !tag.startsWith(QLatin1String("<svg:image")))
                return tag;
            return subEach(tag, embed, [&](const QRegularExpressionMatch &match) {
                const QString href = resource(base32(match.captured(1)));
                return href.isEmpty() ? match.captured(0) : QStringLiteral("\"../%1\"").arg(href);
            });
        });
        eachTag(&flow, url, [&](QString tag) {
            tag = subEach(tag, urlImage, [&](const QRegularExpressionMatch &match) {
                const QString href = resource(base32(match.captured(1)));
                return href.isEmpty() ? match.captured(0) : QStringLiteral("\"../%1\"").arg(href);
            });
            tag = subEach(tag, urlFont, [&](const QRegularExpressionMatch &match) {
                const QString href = resource(base32(match.captured(1)));
                return href.isEmpty() ? match.captured(0) : QStringLiteral("\"../%1\"").arg(href);
            });
            return subEach(tag, urlCss, [&](const QRegularExpressionMatch &match) {
                return QStringLiteral("\"%1\"").arg(flowPath(base32(match.captured(1))));
            });
        });
        eachTag(&flow, anyTag, [&](const QString &tag) {
            const QRegularExpressionMatch match = flowRef.match(tag);
            if (!match.hasMatch())
                return tag;
            const qint64 number = base32(match.captured(1));
            if (number < 0 || number >= m_flowInfo.size())
                return QString();
            if (m_flowInfo.at(number).format == QLatin1String("inline"))
                return m_flows.at(number);
            return subEach(tag, flowRef, [&](const QRegularExpressionMatch &) { return u'"' + flowPath(number) + u'"'; });
        });
    }

    // insert_flows_into_markup(), insert_images_into_markup(),
    // upshift_markup().
    static const QRegularExpression embedded(QStringLiteral("([('\"])kindle:embed:([0-9|A-V]+)[^')\"]*([)'\"])"));
    static const QRegularExpression styled(QStringLiteral("<[a-zA-Z0-9]+\\s[^>]*style\\s*=\\s*[^>]*>"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression svgStart(QStringLiteral("<svg[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    for (QString &part : m_parts) {
        eachTag(&part, anyTag, [&](const QString &tag) {
            const QRegularExpressionMatch match = flowRef.match(tag);
            if (!match.hasMatch())
                return tag;
            const qint64 number = base32(match.captured(1));
            if (number < 0 || number >= m_flowInfo.size())
                return QString();
            if (m_flowInfo.at(number).format == QLatin1String("inline"))
                return m_flows.at(number);
            return subEach(tag, flowRef, [&](const QRegularExpressionMatch &) { return u'"' + flowPath(number) + u'"'; });
        });
        eachTag(&part, anyTag, [&](const QString &tag) {
            if (!tag.startsWith(QLatin1String("<im")))
                return tag;
            return subEach(tag, embedded, [&](const QRegularExpressionMatch &match) {
                const QString href = resource(base32(match.captured(2)));
                return href.isEmpty() ? match.captured(0) : QStringLiteral("\"../%1\"").arg(href);
            });
        });
        eachTag(&part, styled, [&](const QString &tag) {
            if (!tag.contains(QLatin1String("kindle:embed")))
                return tag;
            return subEach(tag, embedded, [&](const QRegularExpressionMatch &match) {
                const QString href = resource(base32(match.captured(2)));
                return href.isEmpty() ? match.captured(0)
                                      : match.captured(1) + QStringLiteral("../") + href + match.captured(3);
            });
        });
        eachTag(&part, svgStart, [](QString tag) {
            return tag.replace(QLatin1String("preserveaspectratio"), QLatin1String("preserveAspectRatio"))
                .replace(QLatin1String("viewbox"), QLatin1String("viewBox"));
        });
    }
}

// read_ncx() and build_toc(): the entries by level, each under its parent.
QList<TocItem> Reader::readNcx()
{
    QList<TocItem> toc;
    if (m_header.ncxIndex == nullIndex)
        return toc;
    const Index table = readIndex(m_kf8, m_header.ncxIndex, [this](const QByteArray &data) { return decode(data); });
    struct Item
    {
        int num = 0;
        qint64 level = -1;
        qint64 parent = -1;
        QString title;
        QString href;
    };
    QList<Item> items;
    for (int num = 0; num < table.entries.size(); ++num) {
        const Entry &entry = table.entries.at(num);
        Item item;
        item.num = num;
        item.level = entry.tags.contains(4) ? qint64(entry.tag(4)) : -1;
        item.parent = entry.tags.contains(21) ? qint64(entry.tag(21)) : -1;
        item.title = CalibreText::replaceEntities(table.cncx.value(quint32(entry.tag(3)), QStringLiteral("Unknown Text")));
        if (entry.tags.contains(6)) {
            const auto target = idTagByPosFid(entry.tag(6), entry.tag(6, 1));
            if (!target)
                continue;
            item.href = QStringLiteral("text/") + target->first + (target->second.isEmpty() ? QString() : u'#' + target->second);
        } else {
            const qint64 pos = qint64(entry.tag(1));
            const Part *part = fileInfo(pos);
            if (!part)
                continue;
            const QString id = idTag(pos);
            item.href = QStringLiteral("text/") + part->filename + (id.isEmpty() ? QString() : u'#' + id);
        }
        // The ids are the markup's bytes still.
        item.href = decode(item.href.toLatin1());
        items.append(item);
    }
    // Each level after the one above it, so that a parent is there first.
    QList<qint64> levels;
    for (const Item &item : std::as_const(items)) {
        if (!levels.contains(item.level))
            levels.append(item.level);
    }
    std::sort(levels.begin(), levels.end());
    // Where each entry is: its path of indices in the tree.
    QHash<int, QList<int>> paths;
    const auto childrenAt = [&toc](const QList<int> &path) {
        QList<TocItem> *children = &toc;
        for (const int i : path)
            children = &(*children)[i].children;
        return children;
    };
    for (const qint64 level : std::as_const(levels)) {
        for (const Item &item : std::as_const(items)) {
            if (item.level != level)
                continue;
            QList<int> path = item.parent >= 0 && paths.contains(int(item.parent)) ? paths.value(int(item.parent)) : QList<int>();
            QList<TocItem> *children = childrenAt(path);
            children->append({ item.title, item.href, {} });
            path.append(int(children->size()) - 1);
            paths.insert(item.num, path);
        }
    }
    return toc;
}

// MobiReader.extract_content() for the reader: an anchor at each file
// position a link goes to (add_anchors()), the markup mended as Calibre
// mends it (cleanup_html()), the pictures of the records after the text
// (extract_images()), the page breaks made pages, the file positions and
// record numbers made addresses (upshift_markup()), the contents read from
// the table the guide names (create_opf()).
void Reader::readMobi6()
{
    const QString raw = bytes(m_raw);
    static const QRegularExpression linkPos(QStringLiteral("<[^<>]+filepos=['\"]{0,1}(\\d+)[^<>]*>"),
                                            QRegularExpression::CaseInsensitiveOption);
    QSet<qint64> found;
    for (QRegularExpressionMatchIterator it = linkPos.globalMatch(raw); it.hasNext();)
        found.insert(it.next().captured(1).toLongLong());
    QList<qint64> positions(found.cbegin(), found.cend());
    std::sort(positions.begin(), positions.end());
    static const QRegularExpression endTag(QStringLiteral("^<\\s*/"));
    QString marked;
    qsizetype pos = 0;
    for (qint64 end : std::as_const(positions)) {
        if (end <= 0 || end > raw.size())
            continue;
        const qint64 position = end;
        const qsizetype lt = raw.indexOf(u'<', end);
        const qsizetype gt = raw.indexOf(u'>', end);
        QString anchor = QStringLiteral("<a id=\"filepos%1\"></a>").arg(position);
        // Inside a tag: on the tag, as its id, unless it closes.
        if (gt > -1 && (gt < lt || lt == end || lt == -1)) {
            const qsizetype open = raw.lastIndexOf(u'<', end);
            if (pos < end && open > -1 && !endTag.match(raw.mid(open, gt - open)).hasMatch()
                && !raw.mid(open, gt + 1 - open).endsWith(QLatin1String("/>"))) {
                anchor = QStringLiteral(" filepos-id=\"filepos%1\"").arg(position);
                end = gt;
            } else {
                end = gt + 1;
            }
        }
        end = std::max<qint64>(end, pos);
        marked += raw.mid(pos, end - pos) + anchor;
        pos = end;
    }
    marked += raw.mid(pos);
    static const QRegularExpression inEntity(QStringLiteral("&([^;]*?)(<a id=\"filepos\\d+\"></a>)([^;]*);"));
    marked.replace(inEntity, QStringLiteral("&\\1\\3;\\2"));

    QString html = decode(marked.toLatin1());
    html.replace(QLatin1String("</</"), QLatin1String("</"));
    static const QRegularExpression brokenEnd(QStringLiteral("</([a-zA-Z]+)<"));
    html.replace(brokenEnd, QStringLiteral("</\\1><"));
    html.remove(QChar(0xFEFF));
    static const QRegularExpression spacedNamespace(QStringLiteral("</{0,1}[a-zA-Z]+:\\s+[^>]*>"));
    html.remove(spacedNamespace);
    static const QRegularExpression pageBreak(
        QStringLiteral("<\\s*/{0,1}\\s*mbp:pagebreak((?:\\s+[^/>]*){0,1})/{0,1}\\s*>\\s*(?:<\\s*/{0,1}\\s*mbp:pagebreak\\s*/{0,1}\\s*>)*"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(pageBreak, QStringLiteral("<div \\1 class=\"mbp_pagebreak\" />"));
    static const QRegularExpression emptyDiv(QStringLiteral("<div height=\"0(pt|px|ex|em|%){0,1}\"></div>"));
    html.remove(emptyDiv);
    html.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    html.replace(QLatin1String("> <"), QLatin1String(">\n<"));
    static const QRegularExpression xmlDeclaration(QStringLiteral("<\\?xml[^>]*>"));
    html.remove(xmlDeclaration);
    static const QRegularExpression officeParagraph(QStringLiteral("<\\s*(/?)\\s*o:p[^>]*>"));
    html.remove(officeParagraph);
    // Styles and blocks swapped so that blocks hold styles.
    static const QRegularExpression stylesBeforeParagraph(
        QStringLiteral("(?i)(?P<styletags>(<(h\\d+|i|b|u|em|small|big|strong|tt)>\\s*){1,})(?P<para><p[^>]*>)"));
    html.replace(stylesBeforeParagraph, QStringLiteral("\\4\\1"));
    static const QRegularExpression stylesAfterParagraph(
        QStringLiteral("(?i)(?P<para></p[^>]*>)\\s*(?P<styletags>(</(h\\d+|i|b|u|em|small|big|strong|tt)>\\s*){1,})"));
    html.replace(stylesAfterParagraph, QStringLiteral("\\2\\1"));
    static const QRegularExpression blocksBeforeEnd(
        QStringLiteral("(?i)(?P<blockquote>(</(blockquote|div)[^>]*>\\s*){1,})(?P<para></p[^>]*>)"));
    html.replace(blocksBeforeEnd, QStringLiteral("\\4\\1"));
    static const QRegularExpression blocksAfterStart(
        QStringLiteral("(?i)(?P<para><p[^>]*>)\\s*(?P<blockquote>(<(blockquote|div)[^>]*>\\s*){1,})"));
    html.replace(blocksAfterStart, QStringLiteral("\\2\\1"));

    // extract_images(): the records after the text, numbered from the
    // first picture's, those that are no picture counted too.
    QSet<qsizetype> processed;
    for (qsizetype i = 0; i <= m_header.records; ++i)
        processed.insert(i);
    for (quint32 i = 0; i < m_header.huffNumber; ++i)
        processed.insert(qsizetype(m_header.huffOffset) + i);
    qsizetype start = m_header.firstImageIndex;
    if (m_header.firstImageIndex == nullIndex || start > m_sections.size())
        start = 0;
    QHash<qint64, QString> images;
    int imageIndex = 0;
    static const QSet<QByteArray> ignored{ "FLIS", "FCIS", "SRCS", "\xe9\x8e\r\n", "RESC", "BOUN", "FDST", "DATP", "AUDI",
                                           "VIDE" };
    for (qsizetype i = start; i < m_sections.size(); ++i) {
        if (processed.contains(i))
            continue;
        ++imageIndex;
        const QByteArray &data = m_sections.at(i);
        const QString ext = ignored.contains(data.left(4)) ? QString() : imageType(data);
        if (ext.isEmpty())
            continue;
        const QString name = QStringLiteral("%1.%2").arg(imageIndex, 5, 10, QChar(u'0')).arg(ext);
        images.insert(imageIndex, name);
        m_resources.insert(QStringLiteral("images/") + name, data);
    }

    std::unique_ptr<XmlTree::Node> document = HtmlSoup::parse(html);
    XmlTree::Node *root = XmlTree::documentElement(document.get());
    XmlTree::Node *head = XmlTree::childElement(root, "head");
    XmlTree::Node *body = XmlTree::childElement(root, "body");

    // upshift_markup(): file positions made links and ids, pictures by
    // their records' numbers made addresses; the guide's table of
    // contents noted, the guide and the metadata out.
    QString tocId;
    const std::function<void(XmlTree::Node &)> upshift = [&](XmlTree::Node &node) {
        if (node.kind != XmlTree::Node::Kind::Element)
            return;
        if (node.isElement("img")) {
            QString record;
            for (const char *attribute : { "lowrecindex", "recindex", "hirecindex" }) {
                const QString value = node.attribute(QLatin1String(attribute));
                if (!value.isEmpty())
                    record = value;
                node.removeAttribute(QLatin1String(attribute));
            }
            if (!record.isEmpty()) {
                const qint64 number = record.toLongLong();
                node.setAttribute(QStringLiteral("src"),
                                  QStringLiteral("../images/") + images.value(number, QStringLiteral("%1.jpg").arg(number, 5, 10, QChar(u'0'))));
            }
        }
        const QString fileposId = node.attribute(QStringLiteral("filepos-id"));
        if (!fileposId.isEmpty()) {
            node.removeAttribute(QStringLiteral("filepos-id"));
            node.setAttribute(QStringLiteral("id"), fileposId);
        }
        const QString filepos = node.attribute(QStringLiteral("filepos"));
        if (!filepos.isEmpty()) {
            node.removeAttribute(QStringLiteral("filepos"));
            node.setAttribute(QStringLiteral("href"), QStringLiteral("#filepos%1").arg(filepos.toLongLong()));
        }
        if (node.isElement("reference") && node.attribute(QStringLiteral("type")).compare(QLatin1String("toc"), Qt::CaseInsensitive) == 0)
            tocId = node.attribute(QStringLiteral("href")).section(u'#', 1);
        for (auto &child : node.children)
            upshift(*child);
    };
    upshift(*root);
    for (XmlTree::Node *parent : { head, body }) {
        for (size_t i = parent->children.size(); i-- > 0;) {
            if (parent->children[i]->isElement("guide") || parent->children[i]->isElement("metadata"))
                parent->take(i);
        }
    }

    // The pages, from one page break to the next; a break with an id kept
    // at the start of the page after it, for the links to it.
    std::vector<std::vector<std::unique_ptr<XmlTree::Node>>> pageBodies(1);
    const auto hasContent = [](const std::vector<std::unique_ptr<XmlTree::Node>> &nodes) {
        return std::any_of(nodes.cbegin(), nodes.cend(), [](const auto &node) {
            return node->kind == XmlTree::Node::Kind::Element || !node->text.trimmed().isEmpty();
        });
    };
    while (!body->children.empty()) {
        std::unique_ptr<XmlTree::Node> child = body->take(0);
        if (child->isElement("div") && child->attribute(QStringLiteral("class")).contains(QLatin1String("mbp_pagebreak"))) {
            if (hasContent(pageBodies.back()))
                pageBodies.emplace_back();
            if (child->attribute(QStringLiteral("id")).isEmpty())
                continue;
        }
        pageBodies.back().push_back(std::move(child));
    }
    if (!hasContent(pageBodies.back()) && pageBodies.size() > 1)
        pageBodies.pop_back();

    std::vector<std::unique_ptr<XmlTree::Node>> pages;
    QHash<QString, int> pageOfId;
    for (size_t k = 0; k < pageBodies.size(); ++k) {
        std::unique_ptr<XmlTree::Node> page = XmlTree::clone(*document, false);
        XmlTree::Node *pageRoot = XmlTree::documentElement(page.get());
        XmlTree::Node *pageHead = XmlTree::childElement(pageRoot, "head");
        XmlTree::Node *pageBody = XmlTree::childElement(pageRoot, "body");
        auto link = XmlTree::element(*pageHead, QStringLiteral("link"));
        link->setAttribute(QStringLiteral("rel"), QStringLiteral("stylesheet"));
        link->setAttribute(QStringLiteral("type"), QStringLiteral("text/css"));
        link->setAttribute(QStringLiteral("href"), QStringLiteral("../styles.css"));
        pageHead->insert(0, std::move(link));
        if (!XmlTree::childElement(pageHead, "title")) {
            auto titleElement = XmlTree::element(*pageHead, QStringLiteral("title"));
            titleElement->append(XmlTree::textNode(decode(m_header.title)));
            pageHead->insert(0, std::move(titleElement));
        }
        for (auto &child : pageBodies[k])
            pageBody->append(std::move(child));
        for (XmlTree::Node *node : XmlTree::findAll(pageBody, [](const XmlTree::Node &n) {
                 return n.kind == XmlTree::Node::Kind::Element && !n.attribute(QStringLiteral("id")).isEmpty();
             }))
            pageOfId.insert(node->attribute(QStringLiteral("id")), int(k));
        pages.push_back(std::move(page));
    }
    const auto pageName = [](size_t k) { return QStringLiteral("part%1.html").arg(k, 4, 10, QChar(u'0')); };
    // Links to another page, to that page.
    for (size_t k = 0; k < pages.size(); ++k) {
        for (XmlTree::Node *anchor : XmlTree::findAll(pages[k].get(), [](const XmlTree::Node &n) { return n.isElement("a"); })) {
            const QString href = anchor->attribute(QStringLiteral("href"));
            if (!href.startsWith(u'#'))
                continue;
            const int target = pageOfId.value(href.mid(1), -1);
            if (target >= 0 && size_t(target) != k)
                anchor->setAttribute(QStringLiteral("href"), pageName(target) + href);
        }
    }

    // The contents: the links after the table's place, to the end of its
    // page.
    if (pageOfId.contains(tocId)) {
        const int k = pageOfId.value(tocId);
        bool reached = false;
        const std::function<void(const XmlTree::Node &)> walk = [&](const XmlTree::Node &node) {
            for (const auto &child : node.children) {
                if (child->kind != XmlTree::Node::Kind::Element)
                    continue;
                if (child->attribute(QStringLiteral("id")) == tocId) {
                    reached = true;
                    walk(*child);
                    continue;
                }
                const QString href = child->attribute(QStringLiteral("href"));
                static const QRegularExpression external(QStringLiteral("^\\w+://"));
                if (reached && child->isElement("a") && !href.isEmpty() && !external.match(href).hasMatch()) {
                    QString text;
                    for (XmlTree::Node *piece : XmlTree::findAll(const_cast<XmlTree::Node *>(child.get()), [](const XmlTree::Node &n) {
                             return n.kind == XmlTree::Node::Kind::Text;
                         }))
                        text += piece->text + u' ';
                    const QString target = href.startsWith(u'#') ? pageName(k) + href : href;
                    m_toc.append({ text.simplified(), QStringLiteral("text/") + target, {} });
                    continue;
                }
                walk(*child);
            }
        };
        walk(*pages[k]);
    }

    for (size_t k = 0; k < pages.size(); ++k)
        m_pages.append({ QStringLiteral("text/") + pageName(k), XmlTree::serialize(*pages[k]) });
    // base_css_rules of Calibre's MobiReader.
    m_resources.insert(QStringLiteral("styles.css"),
                       "body { text-align: justify }\n\nblockquote { margin: 0em 0em 0em 2em; }\n\n"
                       "p { margin: 0em; text-indent: 1.5em }\n\n.bold { font-weight: bold }\n\n"
                       ".italic { font-style: italic }\n\n.underline { text-decoration: underline }\n\n"
                       ".mbp_pagebreak { page-break-after: always; margin: 0; display: block }\n");
}

void Reader::finishKf8()
{
    const auto text = [this](const QString &markup) { return decode(markup.toLatin1()); };
    static const QRegularExpression xmlDeclaration(QStringLiteral("^\\s*<\\?xml[^>]*\\?>"));
    static const QRegularExpression charset(QStringLiteral("<meta[^>]+charset[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    for (qsizetype i = 0; i < m_parts.size(); ++i) {
        QString page = text(m_parts.at(i));
        // strip_encoding_declarations(): the page is UTF-8 now.
        page.remove(xmlDeclaration).remove(charset);
        page = xmlEntities(page);
        page = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n") + page.trimmed();
        m_pages.append({ QStringLiteral("text/") + m_partInfo.at(i).filename, page.toUtf8() });
    }
    for (qsizetype f = 1; f < m_flows.size(); ++f) {
        const FlowInfo &info = m_flowInfo.at(f);
        if (info.format == QLatin1String("file"))
            m_resources.insert(info.dir + u'/' + info.name, text(m_flows.at(f)).toUtf8());
    }
    m_toc = readNcx();
}

bool Reader::writeEpub(const QString &path, QString *error)
{
    QMap<QString, QByteArray> files = m_resources;
    QStringList spine;
    QHash<QString, QString> types;
    for (const auto &[name, data] : std::as_const(m_pages)) {
        files.insert(name, data);
        spine.append(name);
        // A page that is no XML, as some are, read as HTML.
        types.insert(name, XmlTree::parse(data) ? QStringLiteral("application/xhtml+xml") : QStringLiteral("text/html"));
    }

    // The metadata: the EXTH's title, authors, language and identifier.
    const auto exthText = [this](quint32 id) { return decode(m_header.exthValue(id)).trimmed(); };
    QString title = exthText(503);
    if (title.isEmpty())
        title = decode(m_header.title);
    QStringList authors;
    for (const auto &[id, value] : m_header.exth) {
        if (id == 100)
            authors.append(decode(value).trimmed());
    }
    QString identifier = exthText(112);
    if (identifier.startsWith(QLatin1String("calibre:")))
        identifier = QStringLiteral("urn:uuid:") + identifier.mid(8);
    if (identifier.isEmpty())
        identifier = exthText(113);
    if (identifier.isEmpty())
        identifier = QStringLiteral("cousoir:") + title;
    const QString language = exthText(524).isEmpty() ? QStringLiteral("und") : exthText(524);
    const QString direction = exthText(527);

    QString manifest;
    QString itemrefs;
    int id = 0;
    QHash<QString, QString> ids;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QString itemId = QStringLiteral("item%1").arg(++id);
        ids.insert(it.key(), itemId);
        const QString type = types.value(it.key(), mediaTypeOf(it.key()));
        manifest += QStringLiteral("<item id=\"%1\" href=\"%2\" media-type=\"%3\"/>")
                        .arg(itemId, it.key().toHtmlEscaped(), type);
    }
    for (const QString &name : std::as_const(spine))
        itemrefs += QStringLiteral("<itemref idref=\"%1\"/>").arg(ids.value(name));
    QString creators;
    for (const QString &author : std::as_const(authors))
        creators += QStringLiteral("<dc:creator>%1</dc:creator>").arg(author.toHtmlEscaped());
    const QString opf = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>%1</dc:title>%2"
        "<dc:language>%3</dc:language><dc:identifier id=\"id\">%4</dc:identifier></metadata>"
        "<manifest>%5<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/></manifest>"
        "<spine toc=\"ncx\"%6>%7</spine></package>\n")
                            .arg(title.toHtmlEscaped(), creators, language.toHtmlEscaped(), identifier.toHtmlEscaped(),
                                 manifest,
                                 direction == QLatin1String("rtl") ? QStringLiteral(" page-progression-direction=\"rtl\"")
                                                                   : QString(),
                                 itemrefs);

    int playOrder = 0;
    const std::function<QString(const QList<TocItem> &)> points = [&](const QList<TocItem> &items) {
        QString xml;
        for (const TocItem &item : items) {
            ++playOrder;
            xml += QStringLiteral("<navPoint id=\"np%1\" playOrder=\"%1\"><navLabel><text>%2</text></navLabel>"
                                  "<content src=\"%3\"/>")
                       .arg(playOrder)
                       .arg(item.title.toHtmlEscaped(), item.href.toHtmlEscaped());
            xml += points(item.children) + QStringLiteral("</navPoint>");
        }
        return xml;
    };
    const QString ncx = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head>"
        "<meta name=\"dtb:uid\" content=\"%1\"/></head><docTitle><text>%2</text></docTitle><navMap>%3</navMap></ncx>\n")
                            .arg(identifier.toHtmlEscaped(), title.toHtmlEscaped(), points(m_toc));

    files.insert(QStringLiteral("content.opf"), opf.toUtf8());
    files.insert(QStringLiteral("toc.ncx"), ncx.toUtf8());
    files.insert(QStringLiteral("META-INF/container.xml"),
                 "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                 "<rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/>"
                 "</rootfiles></container>");

    QFile::remove(path);
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!archive) {
        *error = QObject::tr("Cannot write %1.").arg(path);
        return false;
    }
    QList<QPair<QString, QByteArray>> entries{ { QStringLiteral("mimetype"), "application/epub+zip" } };
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        entries.append({ it.key(), it.value() });
    for (const auto &[name, data] : std::as_const(entries)) {
        zip_source_t *source = zip_source_buffer(archive, data.constData(), zip_uint64_t(data.size()), 0);
        const zip_int64_t index = source ? zip_file_add(archive, name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            if (source)
                zip_source_free(source);
            zip_discard(archive);
            *error = QObject::tr("Cannot write %1.").arg(path);
            return false;
        }
        zip_set_file_compression(archive, zip_uint64_t(index), name == QLatin1String("mimetype") ? ZIP_CM_STORE : ZIP_CM_DEFLATE,
                                 0);
    }
    if (zip_close(archive) != 0) {
        *error = QObject::tr("Cannot write %1: %2").arg(path, QString::fromUtf8(zip_strerror(archive)));
        zip_discard(archive);
        return false;
    }
    return true;
}

} // namespace

bool toEpub(const QString &mobi, const QString &epub, QString *error)
{
    QFile file(mobi);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cannot read %1.").arg(mobi);
        return false;
    }
    Reader reader;
    return reader.read(file.readAll(), error) && reader.writeEpub(epub, error);
}

} // namespace MobiBook
