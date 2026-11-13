#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// The pieces of the MOBI and KF8 formats that Cousoir both reads, in
// MobiBook, and writes, in Kf8: Calibre's src/calibre/ebooks/mobi/utils.py and
// src/calibre/ebooks/compression/palmdoc.c.
namespace MobiFormat {

// Big-endian numbers in a record, 0 past its end.
quint32 u32(const QByteArray &data, qsizetype at);
quint16 u16(const QByteArray &data, qsizetype at);
quint8 u8(const QByteArray &data, qsizetype at);
QByteArray pack32(quint32 value);
QByteArray pack16(quint16 value);
void put32(QByteArray *data, qsizetype at, quint32 value);

// to_base(): a number in base 32, its digits 0-9 and A-V, at least
// `digits` of them.
QString toBase32(quint64 value, int digits = 0);
// The number of base 32 digits; -1 when they are none.
qint64 base32(const QString &digits);

// encint(): a number in 7 bits a byte, the first byte's high bit set when
// `forward`, the last one's otherwise.
QByteArray encint(quint64 value, bool forward = true);
// decint(), forward: the number at `at`, and how many bytes it took.
quint64 decint(const QByteArray &data, qsizetype at, qsizetype *consumed);

// cpalmdoc_compress() and cpalmdoc_decompress().
QByteArray palmdocCompress(const QByteArray &data);
QByteArray palmdocDecompress(const QByteArray &data);

// align_block(): padded with zeros to a multiple of `multiple`.
QByteArray alignBlock(const QByteArray &data, int multiple = 4);
// encode_trailing_data(): `data` followed by its whole length, backward.
QByteArray trailingEntry(const QByteArray &data);
// utf8_text(): the text stripped and in NFC, `unknown` for none.
QByteArray utf8Text(const QString &text, const QString &unknown = QStringLiteral("Unknown"));

// The text records of a book's text, as create_text_record() cuts them and
// KF8Writer.create_text_records() writes them: 4096 bytes each, PalmDoc
// compressed, followed by the bytes of the character the record cuts and
// their count. Their lengths uncompressed in `lengths`.
QList<QByteArray> textRecords(const QByteArray &text, QList<qsizetype> *lengths);

// write_font_record(): the font compressed and its first bytes obfuscated
// with `key`, 20 random bytes when empty.
QByteArray fontRecord(const QByteArray &font, const QByteArray &key = {});
// read_font_record(): the font of a FONT record, and its extension.
QPair<QByteArray, QString> readFont(const QByteArray &record);

// CNCX: the strings of an index, each once, in records of at most 64 KB.
class Cncx
{
public:
    Cncx() = default;
    explicit Cncx(const QStringList &strings);
    // Where `string` is: its record times 0x10000, plus its offset in it.
    quint32 offset(const QString &string) const;
    const QList<QByteArray> &records() const { return m_records; }

private:
    QHash<QString, quint32> m_offsets;
    QList<QByteArray> m_records;
};

} // namespace MobiFormat
