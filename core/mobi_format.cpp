// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/mobi/utils.py: encint(), decint(), to_base(),
//   align_block(), encode_trailing_data(), utf8_text(), create_text_record(),
//   write_font_record(), read_font_record(), CNCX
// - src/calibre/ebooks/compression/palmdoc.c: cpalmdoc_compress(),
//   cpalmdoc_decompress()
// - src/calibre/ebooks/mobi/writer8/main.py: KF8Writer.create_text_records()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "mobi_format.h"

#include <QRandomGenerator>
#include <QtEndian>

#include <array>
#include <vector>

namespace MobiFormat {

namespace {

constexpr qsizetype recordSize = 4096;

// The bytes PalmDoc writes as they are: NUL and 9 to 0x7F.
bool literal(quint8 c)
{
    return c == 0 || (c > 8 && c < 0x80);
}

} // namespace

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

QByteArray pack32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToBigEndian(value, bytes.data());
    return bytes;
}

QByteArray pack16(quint16 value)
{
    QByteArray bytes(2, '\0');
    qToBigEndian(value, bytes.data());
    return bytes;
}

void put32(QByteArray *data, qsizetype at, quint32 value)
{
    qToBigEndian(value, data->data() + at);
}

QString toBase32(quint64 value, int digits)
{
    QString text = QString::number(value, 32).toUpper();
    if (text.size() < digits)
        text.prepend(QString(digits - text.size(), u'0'));
    return text;
}

qint64 base32(const QString &digits)
{
    bool ok = false;
    const qint64 value = digits.toLongLong(&ok, 32);
    return ok ? value : -1;
}

QByteArray encint(quint64 value, bool forward)
{
    QByteArray bytes;
    do {
        bytes.prepend(char(value & 0x7F));
        value >>= 7;
    } while (value != 0);
    if (forward)
        bytes[bytes.size() - 1] = char(bytes.back() | 0x80);
    else
        bytes[0] = char(bytes.front() | 0x80);
    return bytes;
}

quint64 decint(const QByteArray &data, qsizetype at, qsizetype *consumed)
{
    quint64 value = 0;
    qsizetype i = std::max<qsizetype>(at, 0);
    *consumed = 0;
    if (at < 0 || at >= data.size())
        return 0;
    while (i < data.size()) {
        const quint8 byte = quint8(data.at(i++));
        value = (value << 7) | (byte & 0x7F);
        if (byte & 0x80)
            break;
    }
    *consumed = i - at;
    return value;
}

QByteArray palmdocCompress(const QByteArray &input)
{
    const auto *data = reinterpret_cast<const quint8 *>(input.constData());
    const qsizetype length = input.size();
    QByteArray out;
    out.reserve(length);
    // The earlier places of each three bytes, the nearest first: for each
    // place, the one before it with the same hash.
    constexpr int hashSize = 1 << 16;
    const auto hash = [data](qsizetype at) { return ((data[at] << 8) ^ (data[at + 1] << 4) ^ data[at + 2]) & (hashSize - 1); };
    std::vector<qsizetype> head(hashSize, -1);
    std::vector<qsizetype> previous(size_t(std::max<qsizetype>(length, 0)), -1);
    qsizetype chained = 0;
    qsizetype i = 0;
    while (i < length) {
        const quint8 c = data[i];
        // Repeats: the nearest earlier copy of the next 10 to 3 bytes, within
        // 2047 of them, as Calibre's search back from each place finds it.
        if (i > 10 && length - i > 10) {
            for (; chained + 3 <= i; ++chained) {
                const int h = hash(chained);
                previous[size_t(chained)] = head[size_t(h)];
                head[size_t(h)] = chained;
            }
            // By length, the nearest copy that ends before this place.
            std::array<qsizetype, 11> nearest;
            nearest.fill(-1);
            for (qsizetype j = head[size_t(hash(i))]; j >= 0 && i - j <= 2047 && nearest[10] < 0; j = previous[size_t(j)]) {
                int same = 0;
                while (same < 10 && data[j + same] == data[i + same])
                    ++same;
                const qsizetype longest = std::min<qsizetype>(same, i - j);
                for (qsizetype size = 3; size <= longest; ++size) {
                    if (nearest[size_t(size)] < 0)
                        nearest[size_t(size)] = j;
                }
            }
            int chunk = 10;
            while (chunk > 2 && nearest[size_t(chunk)] < 0)
                --chunk;
            if (chunk > 2) {
                const quint32 compound = quint32(((i - nearest[size_t(chunk)]) << 3) + chunk - 3);
                out.append(char(0x80 + (compound >> 8)));
                out.append(char(compound & 0xFF));
                i += chunk;
                continue;
            }
        }
        ++i;
        // A space and the character after it as one byte.
        if (c == 32 && i < length) {
            const quint8 next = data[i];
            if (next >= 0x40 && next <= 0x7F) {
                out.append(char(next ^ 0x80));
                ++i;
                continue;
            }
        }
        if (literal(c)) {
            out.append(char(c));
        } else {
            // Up to 8 bytes as they are, after their count.
            QByteArray run(1, char(c));
            for (qsizetype j = i; j < length && run.size() < 8 && !literal(data[j]); ++j)
                run.append(char(data[j]));
            i += run.size() - 1;
            out.append(char(run.size()));
            out.append(run);
        }
    }
    return out;
}

QByteArray palmdocDecompress(const QByteArray &input)
{
    QByteArray out;
    out.reserve(input.size() * 2);
    qsizetype i = 0;
    while (i < input.size() && out.size() < (1 << 20)) {
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

QByteArray alignBlock(const QByteArray &data, int multiple)
{
    const qsizetype extra = data.size() % multiple;
    return extra == 0 ? data : data + QByteArray(multiple - extra, '\0');
}

QByteArray trailingEntry(const QByteArray &data)
{
    // The size counts its own bytes.
    for (qsizetype size = 1;; ++size) {
        const QByteArray encoded = encint(quint64(data.size() + size), false);
        if (encoded.size() == size)
            return data + encoded;
    }
}

QByteArray utf8Text(const QString &text, const QString &unknown)
{
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? unknown.toUtf8() : trimmed.normalized(QString::NormalizationForm_C).toUtf8();
}

QList<QByteArray> textRecords(const QByteArray &text, QList<qsizetype> *lengths)
{
    QList<QByteArray> records;
    lengths->clear();
    for (qsizetype start = 0; start < text.size(); start += recordSize) {
        const qsizetype end = std::min(start + recordSize, text.size());
        // The bytes after the record of the character it cuts: from the
        // start of the last character that begins in it.
        qsizetype lead = end - 1;
        while (lead > start && lead > end - 4 && (quint8(text.at(lead)) & 0xC0) == 0x80)
            --lead;
        const quint8 first = quint8(text.at(lead));
        const qsizetype width = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
        const qsizetype overlap = std::clamp<qsizetype>(lead + width - end, 0, text.size() - end);
        const QByteArray data = text.mid(start, end - start);
        lengths->append(data.size());
        records.append(palmdocCompress(data) + text.mid(end, overlap) + char(overlap));
    }
    return records;
}

QByteArray fontRecord(const QByteArray &font, const QByteArray &givenKey)
{
    // zlib's stream, which qCompress() gives after the size it puts first.
    QByteArray data = qCompress(font, 9).mid(4);
    quint32 flags = 0b1;
    QByteArray key;
    if (data.size() >= 1040) {
        flags |= 0b10;
        key = givenKey;
        if (key.isEmpty()) {
            key.resize(20);
            for (char &byte : key)
                byte = char(QRandomGenerator::global()->bounded(256));
        }
        for (qsizetype i = 0; i < 1040; ++i)
            data[i] = char(data.at(i) ^ key.at(i % key.size()));
    }
    const quint32 keyStart = 24;
    const quint32 dataStart = keyStart + quint32(key.size());
    return QByteArray("FONT") + pack32(quint32(font.size())) + pack32(flags) + pack32(dataStart) + pack32(quint32(key.size()))
        + pack32(keyStart) + key + data;
}

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
        // A font is some megabytes at most.
        if (size > (32u << 20))
            return {};
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

Cncx::Cncx(const QStringList &strings)
{
    // kindlegen's margin; a record is 64 KB at most.
    constexpr qsizetype limit = 0x10000 - 1024;
    QByteArray buffer;
    quint32 offset = 0;
    for (const QString &string : strings) {
        if (m_offsets.contains(string))
            continue;
        const QByteArray utf8 = utf8Text(string.left(500));
        const QByteArray raw = encint(quint64(utf8.size())) + utf8;
        if (buffer.size() + raw.size() > limit) {
            m_records.append(alignBlock(buffer));
            buffer.clear();
            offset = quint32(m_records.size()) * 0x10000;
        }
        buffer += raw;
        m_offsets.insert(string, offset);
        offset += quint32(raw.size());
    }
    if (!buffer.isEmpty())
        m_records.append(alignBlock(buffer));
}

quint32 Cncx::offset(const QString &string) const
{
    return m_offsets.value(string);
}

} // namespace MobiFormat
