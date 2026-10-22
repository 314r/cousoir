#include "mobi_metadata.h"

// Ported from Calibre 9.16:
// - src/calibre/ebooks/metadata/mobi.py: MetadataUpdater (fetchEXTHFields(),
//   create_exth(), patch(), update());
// - src/calibre/ebooks/mobi/langcodes.py: iana2mobi() (by
//   CalibreText::mobiLanguageCode());
// - src/calibre/ebooks/mobi/utils.py: rescale_image(), approximately;
// - src/calibre/ebooks/mobi/reader/headers.py: the ASIN and cdetype of
//   EXTHHeader, MetadataHeader's MOBI version;
// - src/calibre/devices/kindle/driver.py: thumbnail_filename();
// - src/calibre/devices/kindle/apnx.py and apnx_page_generator: APNXBuilder
//   with FastPageGenerator.
// Copyright (C) Kovid Goyal, John Schember and Vaso Peras-Likodric, GPL v3.

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <map>

#include "calibre_db.h"
#include "calibre_text.h"

namespace MobiMetadata {

namespace {

// The most a thumbnail record's image measures, MAX_THUMB_DIMEN.
constexpr int thumbWidth = 180;
constexpr int thumbHeight = 240;

quint32 u32(const QByteArray &data, qsizetype at)
{
    return at >= 0 && at + 4 <= data.size() ? qFromBigEndian<quint32>(data.constData() + at) : 0;
}

quint16 u16(const QByteArray &data, qsizetype at)
{
    return at >= 0 && at + 2 <= data.size() ? qFromBigEndian<quint16>(data.constData() + at) : 0;
}

QByteArray pack32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToBigEndian(value, bytes.data());
    return bytes;
}

void put32(QByteArray *data, qsizetype at, quint32 value)
{
    qToBigEndian(value, data->data() + at);
}

// The text as Calibre encodes it into the book: UTF-8, or cp1252 with "?"
// for what it lacks.
QByteArray encode(const QString &text, bool utf8)
{
    if (utf8)
        return text.toUtf8();
    static const QHash<char32_t, char> cp1252{
        { 0x20ac, '\x80' }, { 0x201a, '\x82' }, { 0x0192, '\x83' }, { 0x201e, '\x84' }, { 0x2026, '\x85' },
        { 0x2020, '\x86' }, { 0x2021, '\x87' }, { 0x02c6, '\x88' }, { 0x2030, '\x89' }, { 0x0160, '\x8a' },
        { 0x2039, '\x8b' }, { 0x0152, '\x8c' }, { 0x017d, '\x8e' }, { 0x2018, '\x91' }, { 0x2019, '\x92' },
        { 0x201c, '\x93' }, { 0x201d, '\x94' }, { 0x2022, '\x95' }, { 0x2013, '\x96' }, { 0x2014, '\x97' },
        { 0x02dc, '\x98' }, { 0x2122, '\x99' }, { 0x0161, '\x9a' }, { 0x203a, '\x9b' }, { 0x0153, '\x9c' },
        { 0x017e, '\x9e' }, { 0x0178, '\x9f' },
    };
    QByteArray bytes;
    for (const char32_t code : text.toUcs4()) {
        if (code < 0x80 || (code >= 0xa0 && code <= 0xff))
            bytes.append(char(code));
        else
            bytes.append(cp1252.value(code, '?'));
    }
    return bytes;
}

// calibre.ebooks.normalize(): NFC.
QString normalize(const QString &text)
{
    return text.normalized(QString::NormalizationForm_C);
}

// str() of the datetime Calibre's database gives, in UTC:
// "2015-07-07 00:00:00+00:00".
QString pythonDate(const QDateTime &date)
{
    const QDateTime utc = date.toUTC();
    return QStringLiteral("%1-%2+00:00")
        .arg(utc.date().year(), 4, 10, QLatin1Char('0'))
        .arg(utc.toString(QStringLiteral("MM-dd HH:mm:ss")));
}

// What imghdr's what() recognises at the start of a record.
bool isImage(const QByteArray &data)
{
    return data.startsWith("\xff\xd8") || data.startsWith("\x89PNG") || data.startsWith("GIF8") || data.startsWith("BM")
        || (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP");
}

// rescale_image(): the cover as a JPEG of at most `size` bytes, scaled to
// fit `width` by `height` first when given; lower qualities, then smaller
// sizes, until it fits.
QByteArray rescale(const QByteArray &data, qsizetype size, int width = 0, int height = 0)
{
    QImage image = QImage::fromData(data);
    if (image.isNull())
        return {};
    if (image.hasAlphaChannel()) {
        // Transparent pixels made white.
        QImage white(image.size(), QImage::Format_RGB32);
        white.fill(Qt::white);
        QPainter painter(&white);
        painter.drawImage(0, 0, image);
        painter.end();
        image = white;
    }
    if (width > 0 && height > 0 && (image.width() > width || image.height() > height))
        image = image.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const auto jpeg = [](const QImage &picture, int quality) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        picture.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", quality);
        return bytes;
    };
    int quality = 90;
    QByteArray bytes = jpeg(image, quality);
    while (bytes.size() > size && quality >= 5) {
        bytes = jpeg(image, quality);
        quality -= 5;
    }
    for (double scale = 0.9; bytes.size() > size && scale >= 0.05; scale -= 0.05)
        bytes = jpeg(image.scaled(int(scale * image.width()), int(scale * image.height()), Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation),
                     std::max(quality, 5));
    return bytes;
}

// A book's PDB records and its first one's EXTH, as MetadataUpdater reads
// them.
struct Book
{
    QByteArray *data = nullptr;
    int count = 0;

    qsizetype offset(int n) const { return u32(*data, 78 + 8 * n); }
    qsizetype end(int n) const { return n + 1 < count ? offset(n + 1) : data->size(); }
    QByteArray record(int n) const { return data->mid(offset(n), end(n) - offset(n)); }
};

struct Exth
{
    // By id, the last of the same id: original_exth_records.
    std::map<quint32, QByteArray> records;
    QByteArray timestamp;
    std::optional<quint32> coverIndex;
    std::optional<quint32> thumbnailIndex;
    bool found = false;
};

Exth readExth(const Book &book)
{
    Exth exth;
    const QByteArray record0 = book.record(0);
    const qsizetype start = qsizetype(u32(record0, 20)) + 16;
    if (start + 12 > record0.size() || record0.mid(start, 4) != "EXTH")
        return exth;
    exth.found = true;
    const quint32 items = u32(record0, start + 8);
    qsizetype pos = start + 12;
    for (quint32 i = 0; i < items && pos + 8 <= record0.size(); ++i) {
        const quint32 id = u32(record0, pos);
        const quint32 size = u32(record0, pos + 4);
        if (size < 8)
            break;
        const QByteArray content = record0.mid(pos + 8, size - 8);
        pos += size;
        exth.records[id] = content;
        if (id == 106)
            exth.timestamp = content;
        else if (id == 201 && content.size() >= 4 && u32(content, 0) != 0xffffffff)
            exth.coverIndex = u32(content, 0);
        else if (id == 202 && content.size() >= 4 && u32(content, 0) > 0 && u32(content, 0) != 0xffffffff)
            exth.thumbnailIndex = u32(content, 0);
    }
    return exth;
}

// patch(): the file with a new first record, the others moved after it.
void replaceRecord0(Book *book, const QByteArray &record0)
{
    QByteArray &data = *book->data;
    const qsizetype first = book->offset(0);
    const qsizetype restStart = book->count > 1 ? book->offset(1) : data.size();
    // The book once more at most, the rest taken by views of it.
    QByteArray rebuilt;
    rebuilt.reserve(first + record0.size() + data.size() - restStart);
    rebuilt.append(QByteArrayView(data).first(first));
    rebuilt.append(record0);
    for (int i = 1; i < book->count; ++i) {
        put32(&rebuilt, 78 + 8 * i, quint32(rebuilt.size()));
        // A damaged record list gives an empty record, not a read past the end.
        const qsizetype start = std::min(book->offset(i), data.size());
        const qsizetype end = std::clamp(book->end(i), start, data.size());
        rebuilt.append(QByteArrayView(data).sliced(start, end - start));
    }
    data = std::move(rebuilt);
}

// create_exth(): the first record with `exth` and the title after it.
bool createExth(Book *book, const QByteArray &exth, const QByteArray &newTitle, quint32 flags, QString *error)
{
    QByteArray record0 = book->record(0);
    const quint32 titleOffset = u32(record0, 0x54);
    const quint32 titleLength = u32(record0, 0x58);
    const QByteArray titleInFile = record0.mid(titleOffset, titleLength);
    quint32 headerLength = u32(record0, 0x14);
    if (headerLength == 0xe4) {
        // Room for the PrimaryINDX.
        record0[0x17] = char(0xe8);
        put32(&record0, 0xf4, 0xffffffff);
        headerLength = 0xe8;
    }
    if (0x10 + qsizetype(headerLength) > record0.size()) {
        *error = QObject::tr("The book's MOBI header is damaged.");
        return false;
    }
    put32(&record0, 0x80, flags | 0x40);
    const quint32 encryption = u16(record0, 12);
    QByteArray drm;
    if (encryption != 0) {
        const quint32 drmOffset = u32(record0, 0xa8);
        const quint32 drmCount = u32(record0, 0xac);
        drm = record0.mid(drmOffset, qsizetype(drmCount) * 48);
        put32(&record0, 0xa8, 0x10 + headerLength + quint32(exth.size()));
        put32(&record0, 0xb0, quint32(drm.size()));
        put32(&record0, 0x54, 0x10 + headerLength + quint32(exth.size() + drm.size()));
    } else {
        put32(&record0, 0x54, 0x10 + headerLength + quint32(exth.size()));
    }
    if (!newTitle.isEmpty())
        put32(&record0, 0x58, quint32(newTitle.size()));
    QByteArray rebuilt = record0.left(0x10 + headerLength) + exth + drm + (newTitle.isEmpty() ? titleInFile : newTitle);
    // At least a byte of padding to four, then 8 KB more.
    rebuilt += QByteArray(4 - rebuilt.size() % 4, '\0');
    rebuilt += QByteArray(1024 * 8, '\0');
    replaceRecord0(book, rebuilt);
    return true;
}

} // namespace

Options Options::load(const QString &calibreConfigDirectory)
{
    Options options;
    QFile file(QDir(calibreConfigDirectory).filePath(QStringLiteral("conversion/mobi_output.py")));
    if (calibreConfigDirectory.isEmpty() || !file.open(QIODevice::ReadOnly))
        return options;
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    options.preferAuthorSort = saved.value(QLatin1String("prefer_author_sort")).toBool(false);
    options.personalDoc = saved.value(QLatin1String("personal_doc")).toString();
    options.shareNotSync = saved.value(QLatin1String("share_not_sync")).toBool(false);
    return options;
}

bool update(QByteArray *data, const Metadata &metadata, const Options &options, QString *error, const QString &uuid4)
{
    if (data->mid(60, 8) != "BOOKMOBI") {
        *error = QObject::tr("Setting metadata is only supported for MOBI files of type BOOK.");
        return false;
    }
    Book book{ data, u16(*data, 76) };
    if (book.count < 1) {
        *error = QObject::tr("The book has no records.");
        return false;
    }
    QByteArray record0 = book.record(0);
    if (!u32(record0, 0x14)) {
        *error = QObject::tr("Non-standard file format: convert it to MOBI with Calibre first.");
        return false;
    }
    const bool utf8 = u32(record0, 28) == 65001;
    const quint32 flags = u32(record0, 128);
    const quint32 imageBase = u32(record0, 108);
    if (u16(record0, 12) != 0 && !(flags & 0x40)) {
        *error = QObject::tr("Unable to set metadata on a DRM file without an EXTH header.");
        return false;
    }
    if (!(flags & 0x40)) {
        // An empty EXTH block, then on as with one.
        const QByteArray empty = QByteArray("EXTH") + pack32(12) + pack32(0) + QByteArray(4, '\0');
        if (!createExth(&book, empty, {}, flags, error))
            return false;
    }
    Exth exth = readExth(book);
    if (!exth.found) {
        *error = QObject::tr("No existing EXTH record. Cannot update metadata.");
        return false;
    }

    QList<QPair<quint32, QByteArray>> records;
    const auto add = [&records, &exth](quint32 id, const QByteArray &content) {
        records.append({ id, content });
        exth.records.erase(id);
    };
    const QString title = normalize(metadata.title);
    bool added501 = false;
    if (!metadata.authorSort.isEmpty() && options.preferAuthorSort) {
        for (const QString &author : metadata.authorSort.split(QStringLiteral(" & ")))
            add(100, encode(normalize(author), utf8));
    } else {
        for (const QString &author : metadata.authors)
            add(100, encode(normalize(author), utf8));
    }
    if (!metadata.publisher.isEmpty())
        add(101, encode(normalize(metadata.publisher), utf8));
    if (!metadata.comments.isEmpty()) {
        // Without the reader's annotations Calibre once kept there.
        QString comments = metadata.comments;
        const qsizetype annotations = comments.indexOf(QLatin1String("<div class=\"user_annotations\">"));
        const qsizetype divider = comments.indexOf(QLatin1String("<hr class=\"annotations_divider\" />"));
        if (annotations >= 0)
            comments.truncate(annotations);
        if (divider >= 0 && divider < comments.size())
            comments.truncate(divider);
        add(103, encode(normalize(comments), utf8));
    }
    if (!metadata.isbn.isEmpty())
        add(104, encode(metadata.isbn, utf8));
    if (!metadata.tags.isEmpty()) {
        add(105, encode(normalize(metadata.tags.join(QStringLiteral("; "))), utf8));
        if (!options.personalDoc.isEmpty()
            && (options.personalDoc == QLatin1String("*") || metadata.tags.contains(options.personalDoc))) {
            added501 = true;
            add(501, "PDOC");
        }
    }
    // The database always has a publication date, its undefined one at
    // worst, which is truthy.
    add(106, encode(pythonDate(metadata.pubdate.isValid() ? metadata.pubdate : CalibreDb::undefinedDate()), utf8));
    if (exth.coverIndex) {
        add(201, pack32(*exth.coverIndex));
        add(203, pack32(0));
    }
    if (exth.thumbnailIndex)
        add(202, pack32(*exth.thumbnailIndex));
    // An ASIN, without which Amazon would not sync the book.
    const auto original501 = exth.records.find(501);
    if (!exth.records.contains(113) && original501 != exth.records.end() && original501->second == "EBOK" && !added501
        && !options.shareNotSync)
        add(113, encode(uuid4.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : uuid4, utf8));
    if (!metadata.uuid.isEmpty())
        add(112, encode(QStringLiteral("calibre:") + metadata.uuid, utf8));
    if (exth.records.contains(503))
        add(503, encode(title, utf8));
    // is_null('language'): none, or "und".
    const bool hasLanguage = !metadata.languages.isEmpty() && metadata.languages.first() != QLatin1String("und");
    if (hasLanguage) {
        const QString language = CalibreText::canonicalizeLanguage(metadata.languages.first());
        const QString code = CalibreText::languageAsIso6391(language);
        const QString written = !code.isEmpty() ? code : language;
        if (!written.isEmpty())
            add(524, encode(written, utf8));
    }
    // The other records as they were, then all by id, those of the same id
    // in their order.
    for (const auto &[id, content] : exth.records)
        records.append({ id, content });
    std::stable_sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    QByteArray body;
    for (const auto &[id, content] : std::as_const(records))
        body += pack32(id) + pack32(quint32(content.size() + 8)) + content;
    const QByteArray block = QByteArray("EXTH") + pack32(quint32(body.size() + 12)) + pack32(quint32(records.size())) + body
        + QByteArray(4 - body.size() % 4, '\0');

    if (hasLanguage) {
        const QByteArray code = CalibreText::mobiLanguageCode(metadata.languages.first());
        data->replace(book.offset(0) + 92, 4, code);
    }
    if (!createExth(&book, block, encode(title, utf8), flags, error))
        return false;

    // The cover and its thumbnail, where they fit.
    if (!metadata.cover.isEmpty()) {
        const Exth written = readExth(book);
        const auto replace = [&](std::optional<quint32> index, bool thumbnail) {
            if (!index || int(*index + imageBase) >= book.count)
                return;
            const int n = int(*index + imageBase);
            const QByteArray current = book.record(n);
            if (!isImage(current))
                return;
            QByteArray image = thumbnail ? rescale(metadata.cover, current.size(), thumbWidth, thumbHeight)
                                         : rescale(metadata.cover, current.size());
            if (image.isEmpty() || image.size() > current.size())
                return;
            image += QByteArray(current.size() - image.size(), '\0');
            data->replace(book.offset(n), current.size(), image);
        };
        replace(written.coverIndex, false);
        replace(written.thumbnailIndex, true);
    }
    return true;
}

QList<QPair<quint32, QByteArray>> exthRecords(const QByteArray &data)
{
    QList<QPair<quint32, QByteArray>> records;
    if (data.mid(60, 8) != "BOOKMOBI")
        return records;
    QByteArray copy = data;
    const Book book{ &copy, u16(data, 76) };
    const QByteArray record0 = book.record(0);
    const qsizetype start = qsizetype(u32(record0, 20)) + 16;
    if (!(u32(record0, 128) & 0x40) || record0.mid(start, 4) != "EXTH")
        return records;
    const quint32 items = u32(record0, start + 8);
    qsizetype pos = start + 12;
    for (quint32 i = 0; i < items && pos + 8 <= record0.size(); ++i) {
        const quint32 size = u32(record0, pos + 4);
        if (size < 8)
            break;
        records.append({ u32(record0, pos), record0.mid(pos + 8, size - 8) });
        pos += size;
    }
    return records;
}

QString thumbnailName(const QByteArray &data)
{
    // EXTHHeader: the last 113 is the ASIN, the last 501 the cdetype.
    QString asin;
    QString cdetype;
    for (const auto &[id, content] : exthRecords(data)) {
        if (id == 113)
            asin = QString::fromLatin1(content);
        else if (id == 501)
            cdetype = QString::fromLatin1(content);
    }
    if (asin.isEmpty() || cdetype.isEmpty())
        return {};
    return QStringLiteral("thumbnail_%1_%2_portrait.jpg").arg(asin, cdetype);
}

QByteArray apnx(const QByteArray &data, const QString &guid, QString *error)
{
    if (data.mid(60, 8) != "BOOKMOBI") {
        *error = QObject::tr("Not a valid MOBI file.");
        return {};
    }
    // The PDB name, the MOBI version, the ASIN and cdetype.
    QByteArray rawName = data.left(32);
    rawName.replace('\0', QByteArray());
    static const QRegularExpression unsafe(QStringLiteral("[^-A-Za-z0-9 ]+"));
    const QString name = QString::fromLatin1(rawName).replace(unsafe, QStringLiteral("_"));
    QByteArray copy = data;
    const Book book{ &copy, u16(data, 76) };
    const QByteArray record0 = book.record(0);
    const QString format = u32(record0, 0x68) == 8 ? QStringLiteral("MOBI_8") : QStringLiteral("MOBI_7");
    QString asin;
    QString cdetype;
    for (const auto &[id, content] : exthRecords(data)) {
        if (id == 113)
            asin = QString::fromLatin1(content);
        else if (id == 501)
            cdetype = QString::fromLatin1(content);
    }
    if (cdetype.isEmpty())
        cdetype = QStringLiteral("EBOK");
    // FastPageGenerator: the uncompressed text's length, from the PalmDOC
    // header.
    const quint32 textLength = u32(record0, 4);
    QList<quint32> pages;
    for (quint32 count = 0; count < textLength; count += 2300)
        pages.append(count);
    if (pages.isEmpty()) {
        *error = QObject::tr("Could not generate page mapping.");
        return {};
    }
    const QByteArray content = (format == QLatin1String("MOBI_8")
                                    ? QStringLiteral("{\"contentGuid\":\"%1\",\"asin\":\"%2\",\"cdeType\":\"%3\",\"format\":\"%4\","
                                                     "\"fileRevisionId\":\"1\",\"acr\":\"%5\"}")
                                          .arg(guid, asin, cdetype, format, name)
                                    : QStringLiteral("{\"contentGuid\":\"%1\",\"asin\":\"%2\",\"cdeType\":\"%3\",\"fileRevisionId\":\"1\"}")
                                          .arg(guid, asin, cdetype))
                                   .toUtf8();
    const QByteArray pageHeader = QStringLiteral("{\"asin\":\"%1\",\"pageMap\":\"(1,a,1)\"}").arg(asin).toUtf8();
    const auto pack16 = [](quint16 value) {
        QByteArray bytes(2, '\0');
        qToBigEndian(value, bytes.data());
        return bytes;
    };
    QByteArray result = pack32(65537) + pack32(quint32(12 + content.size())) + pack32(quint32(content.size())) + content
        + pack16(1) + pack16(quint16(pageHeader.size())) + pack16(quint16(pages.size())) + pack16(32) + pageHeader;
    for (const quint32 location : std::as_const(pages))
        result += pack32(location);
    return result;
}

} // namespace MobiMetadata
