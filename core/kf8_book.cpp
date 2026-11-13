// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/mobi/writer8/mobi.py: MOBIHeader, KF8Book, FLIS,
//   fcis()
// - src/calibre/ebooks/mobi/writer8/exth.py: build_exth()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kf8_book.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

#include "calibre_db.h"
#include "calibre_text.h"
#include "mobi_format.h"

namespace Kf8Book {

using namespace MobiFormat;

namespace {

constexpr quint32 nullIndex = 0xFFFFFFFF;

const QByteArray flis("FLIS\0\0\0\x08\0\x41\0\0\0\0\0\0\xff\xff\xff\xff\0\x01\0\x03\0\0\0\x03\0\0\0\x01\xff\xff\xff\xff", 36);

QByteArray fcis(quint32 textLength)
{
    return QByteArray("FCIS\x00\x00\x00\x14\x00\x00\x00\x10\x00\x00\x00\x02\x00\x00\x00\x00", 20) + pack32(textLength)
        + QByteArray("\x00\x00\x00\x00\x00\x00\x00\x28\x00\x00\x00\x00\x00\x00\x00", 15)
        + QByteArray("\x28\x00\x00\x00\x08\x00\x01\x00\x01\x00\x00\x00\x00", 13);
}

// isoformat() of a date, as the conversion's metadata has it.
QString isoDate(const QDateTime &date)
{
    return date.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + QStringLiteral("+00:00");
}

// build_exth(), as a conversion to AZW3 calls it, kindlegen 2 on Linux.
QByteArray exth(const Parts &parts, quint32 resources)
{
    const MobiMetadata::Metadata &metadata = parts.metadata;
    QByteArray body;
    int count = 0;
    const auto add = [&](quint32 code, const QByteArray &data) {
        body += pack32(code) + pack32(quint32(data.size() + 8)) + data;
        ++count;
    };
    static const QRegularExpression collapse(QStringLiteral("[ \\t\\r\\n\\v]+"));
    const auto text = [&](const QString &value) { return utf8Text(QString(value).replace(collapse, QStringLiteral(" ")), parts.unknown); };

    // The metadata, in the order a conversion's metadata has it.
    for (const QString &author : metadata.authors) {
        add(100, text(parts.options.preferAuthorSort ? CalibreText::authorToAuthorSort(author, CalibreText::Tweaks::defaults())
                                                     : author));
    }
    if (!metadata.publisher.isEmpty())
        add(101, text(metadata.publisher));
    if (!metadata.comments.trimmed().isEmpty())
        add(103, utf8Text(metadata.comments, parts.unknown));
    if (!metadata.isbn.isEmpty())
        add(104, text(metadata.isbn));
    for (const QString &tag : metadata.tags)
        add(105, text(tag));
    add(503, text(metadata.title));
    for (const QString &language : metadata.languages) {
        if (language == QLatin1String("und"))
            continue;
        const QString code = CalibreText::languageAsIso6391(language);
        add(524, text(code.isEmpty() ? language : code));
    }

    // The uuid as the ASIN and the source.
    const QByteArray uuid = metadata.uuid.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8() : metadata.uuid.toUtf8();
    if (!parts.options.shareNotSync)
        add(113, uuid);
    add(112, QByteArray("calibre:") + uuid);
    if (!parts.options.shareNotSync)
        add(501, "EBOK");
    const QDateTime date = metadata.pubdate.isValid() && !CalibreDb::isUndefinedDate(metadata.pubdate) ? metadata.pubdate
                                                                                                         : QDateTime::currentDateTimeUtc();
    add(106, isoDate(date).toUtf8());
    // kindlegen 2.9 on Linux.
    add(204, pack32(201));
    add(205, pack32(2));
    add(206, pack32(9));
    add(207, pack32(0));
    add(535, "0730-890adc2");
    if (parts.cover) {
        add(201, pack32(*parts.cover));
        add(203, pack32(0));
    }
    if (parts.thumbnail) {
        add(202, pack32(*parts.thumbnail));
        add(129, (QStringLiteral("kindle:embed:") + toBase32(*parts.thumbnail, 4)).toUtf8());
    }
    if (parts.start)
        add(116, pack32(*parts.start));
    add(125, pack32(resources));
    if (resources > 0)
        add(131, pack32(0));
    if (!parts.primaryWritingMode.isEmpty())
        add(525, parts.primaryWritingMode.toUtf8());
    if (!parts.pageProgressionDirection.isEmpty())
        add(527, parts.pageProgressionDirection.toUtf8());
    add(528, "true");

    // At least a byte of padding.
    return QByteArray("EXTH") + pack32(quint32(body.size() + 12)) + pack32(quint32(count)) + body
        + QByteArray(4 - body.size() % 4, '\0');
}

} // namespace

QByteArray write(const Parts &parts)
{
    QList<QByteArray> records{ QByteArray() };
    records += parts.text;
    const int lastText = int(parts.text.size());
    int firstNonText = lastText + 1;
    qsizetype textSize = 0;
    for (const QByteArray &record : parts.text)
        textSize += record.size();
    // So that the next record starts at four bytes, Calibre says; its pad
    // is as long as what is over a multiple of four.
    if (textSize % 4 != 0) {
        records.append(QByteArray(textSize % 4, '\0'));
        ++firstNonText;
    }
    const quint32 chunkIndex = quint32(records.size());
    records += parts.chunkIndex;
    const quint32 skeletonIndex = quint32(records.size());
    records += parts.skeletonIndex;
    quint32 guideIndex = nullIndex;
    if (!parts.guideIndex.isEmpty()) {
        guideIndex = quint32(records.size());
        records += parts.guideIndex;
    }
    quint32 ncxIndex = nullIndex;
    if (!parts.ncxIndex.isEmpty()) {
        ncxIndex = quint32(records.size());
        records += parts.ncxIndex;
    }
    quint32 firstResource = nullIndex;
    if (!parts.resources.isEmpty()) {
        firstResource = quint32(records.size());
        records += parts.resources;
    }
    const quint32 fdstRecord = quint32(records.size());
    records.append(parts.fdst);
    const quint32 flisRecord = quint32(records.size());
    records.append(flis);
    const quint32 fcisRecord = quint32(records.size());
    records.append(fcis(quint32(parts.textLength)));
    records.append(QByteArray("\xe9\x8e\r\n", 4));

    // KF8Book.record0.
    const QByteArray title = utf8Text(parts.metadata.title, parts.unknown);
    const QByteArray exthBlock = exth(parts, quint32(parts.resources.size()));
    QByteArray header;
    header += pack16(2); // PalmDoc compression
    header += QByteArray(2, '\0');
    header += pack32(quint32(parts.textLength));
    header += pack16(quint16(lastText));
    header += pack16(4096);
    header += pack16(0); // no encryption
    header += pack16(0);
    header += "MOBI";
    header += pack32(264);
    header += pack32(2); // a book
    header += pack32(65001);
    header += pack32(QRandomGenerator::global()->generate());
    header += pack32(8);
    for (int i = 0; i < 10; ++i) // orth and infl indices, extra indices
        header += pack32(nullIndex);
    header += pack32(quint32(firstNonText));
    const qsizetype titleOffsetAt = header.size();
    header += pack32(0);
    header += pack32(quint32(title.size()));
    header += CalibreText::mobiLanguageCode(parts.metadata.languages.value(0));
    header += QByteArray(8, '\0'); // dictionary languages
    header += pack32(8);
    header += pack32(firstResource);
    header += QByteArray(16, '\0'); // Huffman
    header += pack32(0b1010000 | (parts.fonts ? 0b1000000000000 : 0));
    header += QByteArray(32, '\0');
    header += pack32(nullIndex);
    header += pack32(nullIndex); // DRM
    header += QByteArray(12, '\0');
    header += QByteArray(8, '\0');
    header += pack32(fdstRecord);
    header += pack32(quint32(parts.flows));
    header += pack32(fcisRecord);
    header += pack32(1);
    header += pack32(flisRecord);
    header += pack32(1);
    header += QByteArray(8, '\0');
    header += pack32(nullIndex); // SRCS
    header += pack32(0);
    header += QByteArray(8, '\xff');
    header += pack32(0b1 | (parts.trailingByteSequences ? 0b10 : 0));
    header += pack32(ncxIndex);
    header += pack32(chunkIndex);
    header += pack32(skeletonIndex);
    header += pack32(nullIndex); // DATP
    header += pack32(guideIndex);
    header += pack32(nullIndex);
    header += pack32(0);
    header += pack32(nullIndex);
    header += pack32(0);
    header += exthBlock;
    put32(&header, titleOffsetAt, quint32(header.size()));
    header += title;
    // Room for Amazon's publishing service to add data.
    header += QByteArray(8192, '\0');
    records[0] = header;

    // The Palm database.
    QByteArray name = CalibreText::asciiFilename(QString::fromUtf8(title)).replace(u' ', u'_').toLatin1().left(31);
    name += QByteArray(32 - name.size(), '\0');
    const quint32 now = quint32(QDateTime::currentSecsSinceEpoch());
    const quint16 count = quint16(records.size());
    QByteArray file = name;
    file += pack16(0) + pack16(0) + pack32(now) + pack32(now) + QByteArray(16, '\0');
    file += "BOOKMOBI";
    file += pack32(2 * quint32(count) - 1) + pack32(0) + pack16(count);
    quint32 offset = quint32(file.size()) + 8 * count + 2;
    for (int i = 0; i < count; ++i) {
        file += pack32(offset);
        // No flags, and the record's unique id on three bytes.
        file += pack32(2 * quint32(i));
        offset += quint32(records.at(i).size());
    }
    file += QByteArray(2, '\0');
    for (const QByteArray &record : std::as_const(records))
        file += record;
    return file;
}

} // namespace Kf8Book
