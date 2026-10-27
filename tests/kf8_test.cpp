#include <QtTest>

#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstring>

#include "epub_book.h"
#include "epub_package.h"
#include "html_soup.h"
#include "kf8_index.h"
#include "kf8_writer.h"
#include "mobi_book.h"
#include "mobi_format.h"
#include "mobi_metadata.h"
#include "test_support.h"
#include "xml_tree.h"

namespace {

QJsonObject oracle()
{
    return QJsonDocument::fromJson(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kf8/oracle.json"))).object();
}

QList<QByteArray> base64Records(const QJsonValue &value)
{
    QList<QByteArray> records;
    for (const QJsonValue &record : value.toArray())
        records.append(QByteArray::fromBase64(record.toString().toLatin1()));
    return records;
}

QString digest(const QList<QByteArray> &records)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QByteArray &record : records)
        hash.addData(record);
    return QString::fromLatin1(hash.result().toHex());
}

// The oracle's table of contents: index, offset, length, depth, parent,
// first and last child (-1 for none), position, offset in it, label.
QList<Kf8Index::NcxEntry> ncxOf(const QJsonValue &value)
{
    QList<Kf8Index::NcxEntry> entries;
    for (const QJsonValue &row : value.toArray()) {
        const QJsonArray item = row.toArray();
        Kf8Index::NcxEntry entry;
        entry.index = item.at(0).toInt();
        entry.offset = quint32(item.at(1).toInteger());
        entry.length = quint32(item.at(2).toInteger());
        entry.depth = item.at(3).toInt();
        if (item.at(4).toInt() >= 0)
            entry.parent = item.at(4).toInt();
        if (item.at(5).toInt() >= 0)
            entry.firstChild = item.at(5).toInt();
        if (item.at(6).toInt() >= 0)
            entry.lastChild = item.at(6).toInt();
        entry.position = item.at(7).toInt();
        entry.fidOffset = item.at(8).toInt();
        entry.label = item.at(9).toString();
        entries.append(entry);
    }
    return entries;
}

// cpalmdoc_compress() as Calibre writes it, its search back from each
// place unbounded, for the faster one to be checked against.
QByteArray palmdocAsCalibre(const QByteArray &input)
{
    const auto *data = reinterpret_cast<const quint8 *>(input.constData());
    const qsizetype length = input.size();
    const auto literal = [](quint8 c) { return c == 0 || (c > 8 && c < 0x80); };
    QByteArray out;
    qsizetype i = 0;
    while (i < length) {
        const quint8 c = data[i];
        if (i > 10 && length - i > 10) {
            bool found = false;
            for (int chunk = 10; chunk > 2 && !found; --chunk) {
                qsizetype j = i;
                for (qsizetype k = i - chunk; k > -1; --k) {
                    if (std::memcmp(data + k, data + i, size_t(chunk)) == 0) {
                        j = k;
                        break;
                    }
                }
                if (j < i && i - j <= 2047) {
                    const quint32 compound = quint32(((i - j) << 3) + chunk - 3);
                    out.append(char(0x80 + (compound >> 8)));
                    out.append(char(compound & 0xFF));
                    i += chunk;
                    found = true;
                }
            }
            if (found)
                continue;
        }
        ++i;
        if (c == 32 && i < length && data[i] >= 0x40 && data[i] <= 0x7F) {
            out.append(char(data[i] ^ 0x80));
            ++i;
            continue;
        }
        if (literal(c)) {
            out.append(char(c));
        } else {
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

// The words of an EPUB's pages, in the order of its spine: the text of
// their bodies, without pictures in SVG, styles or scripts.
QStringList wordsOf(const QString &epub)
{
    QString error;
    const std::unique_ptr<EpubBook> book = EpubBook::open(epub, &error);
    if (!book)
        return {};
    QString text;
    std::function<void(const XmlTree::Node &, bool)> collect = [&](const XmlTree::Node &node, bool inBody) {
        if (node.kind == XmlTree::Node::Kind::Text || node.kind == XmlTree::Node::Kind::Cdata) {
            if (inBody)
                text += node.text;
            return;
        }
        if (node.kind != XmlTree::Node::Kind::Element)
            return;
        if (node.isElement("svg") || node.isElement("style") || node.isElement("script") || node.isElement("head"))
            return;
        // The pieces of a long text a Kindle book keeps apart are one.
        if (node.isElement("p") || node.isElement("div") || node.isElement("h1") || node.isElement("h2") || node.isElement("li")
            || node.isElement("td") || node.isElement("br"))
            text += u' ';
        for (const auto &child : node.children)
            collect(*child, inBody || node.isElement("body"));
        if (node.isElement("p") || node.isElement("div") || node.isElement("li") || node.isElement("td"))
            text += u' ';
    };
    for (const EpubBook::SpineItem &item : book->spine()) {
        const QByteArray data = book->read(item.name);
        std::unique_ptr<XmlTree::Node> page = XmlTree::parse(data);
        if (!page)
            page = HtmlSoup::parse(XmlTree::xmlText(data));
        if (page)
            collect(*page, false);
        text += u' ';
    }
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

// Of an EPUB, every file by name.
EpubPackage::Zip filesOf(const QString &epub)
{
    EpubPackage::Zip zip;
    QString error;
    EpubPackage::readZip(epub, &zip, &error);
    return zip;
}

QByteArray image(const char *format, const QColor &color, int width = 40, int height = 60)
{
    QImage picture(width, height, QImage::Format_RGB32);
    picture.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    picture.save(&buffer, format);
    return bytes;
}

const QString uniqueIdentifier = QStringLiteral("urn:uuid:2b6c4d2e-0000-4000-8000-00000000kf8a");

// A font's bytes, as a TrueType file starts.
QByteArray font()
{
    QByteArray bytes("\0\1\0\0", 4);
    QRandomGenerator random(1040);
    for (int i = 0; i < 3000; ++i)
        bytes.append(char(random.bounded(256)));
    return bytes;
}

QString longText()
{
    QString text;
    for (int i = 0; i < 1400; ++i)
        text += QStringLiteral("mot%1 été ").arg(i);
    return text;
}

// An EPUB of what a converter meets: an HTML cover the guide names, a font
// obfuscated as IDPF says, a picture written into its page, one in WebP and
// one in SVG, styles that import others and embed the font, an SVG drawing
// in a page, a table with ids, a paragraph longer than a chunk, links to
// the other page and into its table.
bool makeRichEpub(const QString &path, const QString &encryptionAlgorithm = QStringLiteral("http://www.idpf.org/2008/embedding"))
{
    EpubPackage::Zip zip;
    const auto add = [&zip](const QString &name, const QByteArray &data) {
        zip.names.append(name);
        zip.data.insert(name, data);
    };
    add(QStringLiteral("mimetype"), "application/epub+zip");
    add(QStringLiteral("META-INF/container.xml"),
        "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    QByteArray obfuscated = font();
    const QByteArray key = QCryptographicHash::hash(uniqueIdentifier.toUtf8(), QCryptographicHash::Sha1);
    for (int i = 0; i < 1040; ++i)
        obfuscated[i] = char(obfuscated.at(i) ^ key.at(i % key.size()));
    add(QStringLiteral("META-INF/encryption.xml"),
        QStringLiteral("<?xml version=\"1.0\"?><encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                       "xmlns:enc=\"http://www.w3.org/2001/04/xmlenc#\"><enc:EncryptedData><enc:EncryptionMethod Algorithm=\"%1\"/>"
                       "<enc:CipherData><enc:CipherReference URI=\"OEBPS/fonts/f.ttf\"/></enc:CipherData></enc:EncryptedData></encryption>")
            .arg(encryptionAlgorithm)
            .toUtf8());
    add(QStringLiteral("OEBPS/content.opf"),
        QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">"
                       "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"uid\">%1</dc:identifier>"
                       "<dc:title>Le Livre riche</dc:title><dc:creator>Anne Exemple</dc:creator><dc:language>fr</dc:language></metadata>"
                       "<manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
                       "<item id=\"cover-page\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"c1\" href=\"text/chapter%201.xhtml\" media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"c2\" href=\"text/chapter2.xhtml\" media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"css\" href=\"styles/main.css\" media-type=\"text/css\"/>"
                       "<item id=\"more\" href=\"styles/more.css\" media-type=\"text/css\"/>"
                       "<item id=\"font\" href=\"fonts/f.ttf\" media-type=\"application/x-font-truetype\"/>"
                       "<item id=\"cover\" href=\"images/cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>"
                       "<item id=\"webp\" href=\"images/sky.webp\" media-type=\"image/webp\"/>"
                       "<item id=\"unused\" href=\"images/unused.png\" media-type=\"image/png\"/>"
                       "<item id=\"drawing\" href=\"images/drawing.svg\" media-type=\"image/svg+xml\"/></manifest>"
                       "<spine><itemref idref=\"cover-page\"/><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>"
                       "<guide><reference type=\"cover\" title=\"Couverture\" href=\"cover.xhtml\"/>"
                       "<reference type=\"text\" title=\"Début\" href=\"text/chapter%201.xhtml\"/></guide></package>")
            .arg(uniqueIdentifier)
            .toUtf8());
    add(QStringLiteral("OEBPS/nav.xhtml"),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
        "<head><title>Nav</title></head><body><nav epub:type=\"toc\"><ol>"
        "<li><a href=\"text/chapter%201.xhtml\">Premier</a><ol><li><a href=\"text/chapter%201.xhtml#long\">Le long</a></li></ol></li>"
        "<li><a href=\"text/chapter2.xhtml#target\">Second</a></li></ol></nav></body></html>");
    add(QStringLiteral("OEBPS/cover.xhtml"),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Cover</title></head>"
        "<body><img src=\"images/cover.jpg\" alt=\"couverture\"/></body></html>");
    add(QStringLiteral("OEBPS/styles/main.css"),
        "@import \"more.css\";\n@font-face { font-family: \"Rich\"; src: url(../fonts/f.ttf); }\n"
        "/* url(not-a-picture.png) */ body { font-family: Rich; }\n.sky { background: url('../images/sky.webp'); }\n");
    add(QStringLiteral("OEBPS/styles/more.css"), "p.more-css-marker { color: #345; }\n");
    add(QStringLiteral("OEBPS/fonts/f.ttf"), obfuscated);
    add(QStringLiteral("OEBPS/images/cover.jpg"), image("JPEG", Qt::darkBlue, 300, 450));
    add(QStringLiteral("OEBPS/images/sky.webp"), image("WEBP", Qt::cyan));
    add(QStringLiteral("OEBPS/images/unused.png"), image("PNG", Qt::red));
    add(QStringLiteral("OEBPS/images/drawing.svg"),
        "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><circle r=\"4\" cx=\"5\" cy=\"5\"/></svg>");
    const QString dataImage = QString::fromLatin1(image("PNG", Qt::green).toBase64());
    add(QStringLiteral("OEBPS/text/chapter 1.xhtml"),
        QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" "
                       "xmlns:xlink=\"http://www.w3.org/1999/xlink\" xml:lang=\"fr\">"
                       "<head><title>Un</title><link rel=\"stylesheet\" type=\"text/css\" href=\"../styles/main.css\"/>"
                       "<style>.inline { background: url(../images/sky.webp) }</style></head>"
                       "<body><section epub:type=\"chapter\"><h1 id=\"top\">Premier chapitre</h1>"
                       "<p class=\"more-css-marker\">Une image <img src=\"data:image/png;base64,%1\" alt=\"verte\"/> écrite dans la page."
                       " <a href=\"chapter2.xhtml#target\">Vers la cible</a>, <a href=\"chapter2.xhtml#cell\">vers la case</a>.</p>"
                       "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><image xlink:href=\"../images/sky.webp\" "
                       "width=\"20\" height=\"20\"/><text>Dessin</text></svg>"
                       "<p id=\"long\">%2</p><p>Fin &amp; suite &lt;ici&gt;.</p></section></body></html>")
            .arg(dataImage, longText())
            .toUtf8());
    add(QStringLiteral("OEBPS/text/chapter2.xhtml"),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Deux</title></head>"
        "<body><h1>Second chapitre</h1><p id=\"target\">La cible des liens.</p>"
        "<table><tr><td id=\"cell\">Une case</td><td>Une autre</td></tr></table>"
        "<p><img src=\"../images/drawing.svg\" alt=\"dessin\"/> <img src=\"../images/sky.webp\" alt=\"ciel\"/></p>"
        "<p><a href=\"chapter%201.xhtml#top\">Retour</a></p></body></html>");
    QString error;
    return EpubPackage::writeZip(path, zip, &error);
}

// The target of each link of the EPUB's pages to another of its pages, by
// the text of the link: whether the page it names has the id it names.
QHash<QString, bool> linksOf(const QString &epub)
{
    const EpubPackage::Zip zip = filesOf(epub);
    QHash<QString, bool> links;
    static const QRegularExpression link(QStringLiteral("<a [^>]*href=\"([^\"#]+)#([^\"]+)\"[^>]*>([^<]*)</a>"));
    for (const QString &name : zip.names) {
        if (!name.endsWith(QLatin1String("html")))
            continue;
        const QString page = QString::fromUtf8(zip.data.value(name));
        QRegularExpressionMatchIterator matches = link.globalMatch(page);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString target = EpubPackage::resolve(match.captured(1), name);
            const QString text = QString::fromUtf8(zip.data.value(target));
            links.insert(match.captured(3), text.contains(QStringLiteral("id=\"%1\"").arg(match.captured(2))));
        }
    }
    return links;
}

} // namespace

class Kf8Test : public QObject
{
    Q_OBJECT

private slots:
    void compressesAsCalibre();
    void compressesAsCalibresSearch();
    void encodesNumbersAsCalibre();
    void cutsTextRecordsAsCalibre();
    void writesIndicesAsCalibre();
    void writesTrailingByteSequencesAsCalibre();
    void roundTripsBooks_data();
    void roundTripsBooks();
    void convertsWhatBooksHold();
    void writesTheLibrarysMetadata();
    void refusesProtectedBooks();
    void survivesDamagedBooks();
};

void Kf8Test::compressesAsCalibre()
{
    for (const QJsonValue &value : oracle().value(QLatin1String("palmdoc")).toArray()) {
        const QByteArray input = QByteArray::fromBase64(value[QLatin1String("input")].toString().toLatin1());
        const QByteArray expected = QByteArray::fromBase64(value[QLatin1String("output")].toString().toLatin1());
        const QByteArray compressed = MobiFormat::palmdocCompress(input);
        QCOMPARE(compressed.toHex(), expected.toHex());
        QCOMPARE(MobiFormat::palmdocDecompress(compressed), input);
    }
}

// The search of earlier copies by their first bytes finds what Calibre's
// search back from each place finds.
void Kf8Test::compressesAsCalibresSearch()
{
    QRandomGenerator random(4096);
    const QList<QByteArray> words{ "lantern ", "été ", "<p aid=\"", "\">", "</p>\n", "a", "ab", "abc", "\x01\x02", " " };
    for (int round = 0; round < 60; ++round) {
        QByteArray text;
        const int size = 1 + random.bounded(4096);
        while (text.size() < size) {
            if (round % 3 == 0)
                text.append(char(random.bounded(256)));
            else
                text += words.at(random.bounded(int(words.size())));
        }
        text.truncate(size);
        QCOMPARE(MobiFormat::palmdocCompress(text).toHex(), palmdocAsCalibre(text).toHex());
    }
}

void Kf8Test::encodesNumbersAsCalibre()
{
    for (const QJsonValue &value : oracle().value(QLatin1String("encint")).toArray()) {
        const quint64 number = quint64(value[QLatin1String("value")].toInteger());
        QCOMPARE(MobiFormat::encint(number).toHex(), value[QLatin1String("forward")].toString().toLatin1());
        QCOMPARE(MobiFormat::encint(number, false).toHex(), value[QLatin1String("backward")].toString().toLatin1());
        QCOMPARE(MobiFormat::trailingEntry(QByteArray(qsizetype(number % 300), '\x01')).toHex(),
                 value[QLatin1String("trailing")].toString().toLatin1());
        qsizetype consumed = 0;
        QCOMPARE(MobiFormat::decint(MobiFormat::encint(number), 0, &consumed), number);
    }
    QCOMPARE(MobiFormat::toBase32(0, 4), QStringLiteral("0000"));
    QCOMPARE(MobiFormat::toBase32(1000000), QStringLiteral("UGI0"));
    QCOMPARE(MobiFormat::base32(QStringLiteral("UGI0")), 1000000);
}

void Kf8Test::cutsTextRecordsAsCalibre()
{
    QString text;
    for (int i = 0; i < 4000; ++i)
        text += QStringLiteral("%1 lantern été 日本語 ").arg(i);
    QList<qsizetype> lengths;
    const QList<QByteArray> records = MobiFormat::textRecords(text.toUtf8(), &lengths);
    const QJsonObject expected = oracle();
    QCOMPARE(digest(records), expected.value(QLatin1String("text_records")).toString());
    QList<qsizetype> expectedLengths;
    for (const QJsonValue &length : expected.value(QLatin1String("text_lengths")).toArray())
        expectedLengths.append(qsizetype(length.toInteger()));
    QCOMPARE(lengths, expectedLengths);
}

void Kf8Test::writesIndicesAsCalibre()
{
    const QJsonObject expected = oracle();
    QList<Kf8Index::Skeleton> skeletons;
    for (const QJsonValue &value : expected.value(QLatin1String("skeletons")).toArray()) {
        skeletons.append({ value[QLatin1String("file_number")].toInt(), value[QLatin1String("chunk_count")].toInt(),
                           quint32(value[QLatin1String("start_pos")].toInteger()), quint32(value[QLatin1String("length")].toInteger()) });
    }
    QCOMPARE(Kf8Index::skeletonIndex(skeletons), base64Records(expected.value(QLatin1String("skeleton_index"))));

    QList<Kf8Index::Chunk> chunks;
    for (const QJsonValue &value : expected.value(QLatin1String("chunks")).toArray()) {
        chunks.append({ quint32(value[QLatin1String("insert_pos")].toInteger()), value[QLatin1String("selector")].toString(),
                        value[QLatin1String("file_number")].toInt(), value[QLatin1String("sequence_number")].toInt(),
                        quint32(value[QLatin1String("start_pos")].toInteger()), quint32(value[QLatin1String("length")].toInteger()) });
    }
    QCOMPARE(Kf8Index::chunkIndex(chunks), base64Records(expected.value(QLatin1String("chunk_index"))));

    QList<Kf8Index::GuideReference> guide;
    for (const QJsonValue &value : expected.value(QLatin1String("guide")).toArray()) {
        const QJsonArray position = value[QLatin1String("pos_fid")].toArray();
        guide.append({ value[QLatin1String("title")].toString(), value[QLatin1String("type")].toString(), position.at(0).toInt(),
                       position.at(1).toInt() });
    }
    QCOMPARE(Kf8Index::guideIndex(guide), base64Records(expected.value(QLatin1String("guide_index"))));

    QCOMPARE(Kf8Index::ncxIndex(ncxOf(expected.value(QLatin1String("ncx_small")))),
             base64Records(expected.value(QLatin1String("ncx_small_index"))));

    // Past 64 KB, of index and of strings: 825 chapters of three parts.
    QList<Kf8Index::NcxEntry> large;
    for (int t = 0; t < 825; ++t) {
        Kf8Index::NcxEntry entry;
        entry.index = t;
        entry.label = QStringLiteral("Chapter %1").arg(t);
        entry.offset = quint32(t * 1000);
        entry.length = 1000;
        entry.position = t;
        entry.firstChild = 825 + 3 * t;
        entry.lastChild = 825 + 3 * t + 2;
        large.append(entry);
    }
    for (int t = 0; t < 825; ++t) {
        for (int k = 0; k < 3; ++k) {
            Kf8Index::NcxEntry entry;
            entry.index = 825 + 3 * t + k;
            entry.label = QStringLiteral("Part %1 of chapter %2, by the lantern and the river").arg(k).arg(t);
            entry.offset = quint32(t * 1000 + 100 + 300 * k);
            entry.length = 300;
            entry.depth = 1;
            entry.position = t;
            entry.fidOffset = 10 * k;
            entry.parent = t;
            large.append(entry);
        }
    }
    const QList<QByteArray> records = Kf8Index::ncxIndex(large);
    QCOMPARE(records.size(), expected.value(QLatin1String("ncx_large_records")).toInt());
    QCOMPARE(digest(records), expected.value(QLatin1String("ncx_large_index")).toString());
}

void Kf8Test::writesTrailingByteSequencesAsCalibre()
{
    for (const QJsonValue &value : oracle().value(QLatin1String("tbs")).toArray()) {
        QList<qsizetype> lengths;
        for (const QJsonValue &length : value[QLatin1String("record_lengths")].toArray())
            lengths.append(qsizetype(length.toInteger()));
        QCOMPARE(Kf8Index::trailingByteSequences(ncxOf(value[QLatin1String("ncx")]), lengths),
                 base64Records(value[QLatin1String("trailing")]));
    }
}

void Kf8Test::roundTripsBooks_data()
{
    QTest::addColumn<QString>("epub");
    QTest::addColumn<int>("tocEntries");
    QTest::newRow("voyage") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub") << 8;
    QTest::newRow("notes") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/notes.epub") << -1;
    QTest::newRow("cfi") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/cfi.epub") << -1;
    QTest::newRow("fixed") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/fixed.epub") << -1;
    QTest::newRow("kepub") << QStringLiteral(COUSOIR_TEST_DATA_DIR "/kepub/rich.epub") << -1;
}

// What an AZW3 made of an EPUB holds, read again by Cousoir's Kindle
// reader: every word of the pages, in order, then the table of contents
// added, and the table of contents itself.
void Kf8Test::roundTripsBooks()
{
    QFETCH(QString, epub);
    QFETCH(int, tocEntries);
    QTemporaryDir dir;
    const QString azw3 = dir.filePath(QStringLiteral("book.azw3"));
    const QString back = dir.filePath(QStringLiteral("back.epub"));
    QString error;
    QVERIFY2(Kf8::write(epub, azw3, Kf8::Options(), &error), qPrintable(error));
    QVERIFY2(MobiBook::toEpub(azw3, back, &error), qPrintable(error));

    const QStringList before = wordsOf(epub);
    const QStringList after = wordsOf(back);
    QVERIFY(!before.isEmpty());
    QCOMPARE(after.mid(0, before.size()), before);

    std::unique_ptr<EpubBook> source = EpubBook::open(epub, &error);
    std::unique_ptr<EpubBook> book = EpubBook::open(back, &error);
    QVERIFY2(source && book, qPrintable(error));
    QStringList titles;
    for (const EpubBook::TocEntry &entry : source->toc())
        titles.append(entry.title);
    QStringList titlesAfter;
    for (const EpubBook::TocEntry &entry : book->toc())
        titlesAfter.append(entry.title);
    if (tocEntries >= 0)
        QCOMPARE(titles.size(), tocEntries);
    QCOMPARE(titlesAfter, titles);
    // Each entry leads to its place, a heading alone aside.
    for (const EpubBook::TocEntry &entry : book->toc()) {
        if (entry.name.isEmpty() || entry.fragment.isEmpty())
            continue;
        QVERIFY2(QString::fromUtf8(book->read(entry.name)).contains(QStringLiteral("id=\"%1\"").arg(entry.fragment)),
                 qPrintable(entry.title));
    }
}

void Kf8Test::convertsWhatBooksHold()
{
    QTemporaryDir dir;
    const QString epub = dir.filePath(QStringLiteral("rich.epub"));
    QVERIFY(makeRichEpub(epub));
    const QString azw3 = dir.filePath(QStringLiteral("rich.azw3"));
    const QString back = dir.filePath(QStringLiteral("back.epub"));
    QString error;
    QVERIFY2(Kf8::write(epub, azw3, Kf8::Options(), &error), qPrintable(error));
    QVERIFY2(MobiBook::toEpub(azw3, back, &error), qPrintable(error));
    const EpubPackage::Zip files = filesOf(back);

    // The HTML cover left out, the Kindle showing the cover itself; the two
    // chapters, then the table of contents.
    std::unique_ptr<EpubBook> book = EpubBook::open(back, &error);
    QVERIFY2(book, qPrintable(error));
    QCOMPARE(book->spine().size(), 3);
    const QStringList words = wordsOf(back);
    QCOMPARE(words.first(), QStringLiteral("Premier"));
    QVERIFY(words.join(u' ').contains(longText().simplified()));
    QVERIFY(words.join(u' ').contains(QStringLiteral("Fin & suite <ici>.")));

    // The font as it was before IDPF's obfuscation, the styles that embed
    // and import, the pictures as a Kindle shows them.
    bool fontFound = false;
    bool importFound = false;
    bool fontFace = false;
    int pngs = 0;
    for (const QString &name : files.names) {
        const QByteArray data = files.data.value(name);
        fontFound = fontFound || data == font();
        if (name.endsWith(QLatin1String(".css"))) {
            importFound = importFound || data.contains("more-css-marker");
            fontFace = fontFace || data.contains("@font-face");
            QVERIFY2(!data.contains("kindle:") && !data.contains("('\""), data.constData());
        }
        QVERIFY2(!name.endsWith(QLatin1String(".webp")), qPrintable(name));
        if (data.startsWith("\x89PNG"))
            ++pngs;
    }
    QVERIFY(fontFound);
    QVERIFY(importFound);
    QVERIFY(fontFace);
    // The WebP and the picture of the page as PNGs; the one no page shows
    // Calibre's placeholder, a picture too.
    QCOMPARE(pngs, QImageWriter::supportedImageFormats().contains("webp") ? 2 : 1);
    bool placeholder = false;
    for (const QString &name : files.names) {
        const QByteArray data = files.data.value(name);
        if (data.contains("calibre-placeholder-gif-for-azw3")) {
            placeholder = true;
            QCOMPARE(data.size(), 79);
            QCOMPARE(QImage::fromData(data).size(), QSize(1, 1));
        }
    }
    QVERIFY(placeholder);

    // The links lead where they did, into the table too.
    const QHash<QString, bool> links = linksOf(back);
    QVERIFY2(links.value(QStringLiteral("Vers la cible")), qPrintable(QStringList(links.keys()).join(u',')));
    QVERIFY(links.value(QStringLiteral("vers la case")));
    QVERIFY(links.value(QStringLiteral("Retour")));
    // epub:type without its namespace.
    bool typed = false;
    for (const QString &name : files.names)
        typed = typed || files.data.value(name).contains("type=\"chapter\"");
    QVERIFY(typed);

    // Calibre's MOBI writer and the APNX of a send accept it.
    QByteArray bytes = readAll(azw3);
    QVERIFY2(MobiMetadata::update(&bytes, MobiMetadata::Metadata{ QStringLiteral("Titre") }, MobiMetadata::Options(), &error),
             qPrintable(error));
    QVERIFY(!MobiMetadata::apnx(bytes, QStringLiteral("0123abcd"), &error).isEmpty());
}

void Kf8Test::writesTheLibrarysMetadata()
{
    QTemporaryDir dir;
    const QString epub = dir.filePath(QStringLiteral("rich.epub"));
    QVERIFY(makeRichEpub(epub));
    Kf8::Options options;
    options.metadata.title = QStringLiteral("Le Petit Prince");
    options.metadata.authors = { QStringLiteral("Antoine de Saint-Exupéry"), QStringLiteral("Second  Auteur") };
    options.metadata.publisher = QStringLiteral("Gallimard");
    options.metadata.comments = QStringLiteral("<p>Un livre.</p>");
    options.metadata.isbn = QStringLiteral("9782070612758");
    options.metadata.tags = { QStringLiteral("Conte"), QStringLiteral("Classique") };
    options.metadata.pubdate = QDateTime(QDate(1943, 4, 6), QTime(0, 0), QTimeZone::UTC);
    options.metadata.uuid = QStringLiteral("8c1e5f0a-0000-4000-8000-000000000001");
    options.metadata.languages = { QStringLiteral("fra") };
    options.metadata.cover = image("JPEG", Qt::darkRed, 600, 900);
    const QString azw3 = dir.filePath(QStringLiteral("book.azw3"));
    QString error;
    QVERIFY2(Kf8::write(epub, azw3, options, &error), qPrintable(error));
    const QByteArray book = readAll(azw3);

    QMultiHash<quint32, QByteArray> exth;
    for (const auto &[id, content] : MobiMetadata::exthRecords(book))
        exth.insert(id, content);
    QCOMPARE(exth.values(100).size(), 2);
    QVERIFY(exth.values(100).contains("Second Auteur"));
    QCOMPARE(exth.value(101), QByteArray("Gallimard"));
    QCOMPARE(exth.value(103), QByteArray("<p>Un livre.</p>"));
    QCOMPARE(exth.value(104), QByteArray("9782070612758"));
    QCOMPARE(exth.values(105).size(), 2);
    QCOMPARE(exth.value(106), QByteArray("1943-04-06T00:00:00+00:00"));
    QCOMPARE(exth.value(112), QByteArray("calibre:8c1e5f0a-0000-4000-8000-000000000001"));
    QCOMPARE(exth.value(113), QByteArray("8c1e5f0a-0000-4000-8000-000000000001"));
    QCOMPARE(exth.value(501), QByteArray("EBOK"));
    QCOMPARE(exth.value(503), QString(QStringLiteral("Le Petit Prince")).toUtf8());
    QCOMPARE(exth.value(524), QByteArray("fr"));
    QVERIFY(exth.contains(201) && exth.contains(202) && exth.contains(129));
    // Where the guide says the text starts.
    QVERIFY(exth.contains(116));
    QCOMPARE(MobiMetadata::thumbnailName(book),
             QStringLiteral("thumbnail_8c1e5f0a-0000-4000-8000-000000000001_EBOK_portrait.jpg"));

    // The cover the library's, the HTML cover gone, the title in the header.
    const QString back = dir.filePath(QStringLiteral("back.epub"));
    QVERIFY2(MobiBook::toEpub(azw3, back, &error), qPrintable(error));
    std::unique_ptr<EpubBook> read = EpubBook::open(back, &error);
    QVERIFY2(read, qPrintable(error));
    QCOMPARE(read->title(), QStringLiteral("Le Petit Prince"));
}

void Kf8Test::refusesProtectedBooks()
{
    QTemporaryDir dir;
    const QString epub = dir.filePath(QStringLiteral("locked.epub"));
    QVERIFY(makeRichEpub(epub, QStringLiteral("http://www.w3.org/2001/04/xmlenc#aes128-cbc")));
    QString error;
    QVERIFY(!Kf8::write(epub, dir.filePath(QStringLiteral("locked.azw3")), Kf8::Options(), &error));
    QVERIFY2(error.contains(QLatin1String("DRM")), qPrintable(error));
    QVERIFY(!Kf8::write(dir.filePath(QStringLiteral("none.epub")), dir.filePath(QStringLiteral("none.azw3")), Kf8::Options(), &error));
}

// EPUBs damaged every which way inside: their pages, package and styles.
// Made or refused, never a crash; an AZW3 made is read again.
void Kf8Test::survivesDamagedBooks()
{
    QTemporaryDir dir;
    const QString epub = dir.filePath(QStringLiteral("rich.epub"));
    QVERIFY(makeRichEpub(epub));
    const EpubPackage::Zip original = filesOf(epub);
    const int rounds = qEnvironmentVariableIntValue("COUSOIR_FUZZ_ROUNDS") > 0 ? qEnvironmentVariableIntValue("COUSOIR_FUZZ_ROUNDS") : 60;
    QRandomGenerator random(quint32(qEnvironmentVariableIntValue("COUSOIR_FUZZ_SEED")) + 20261027);
    int made = 0;
    for (int round = 0; round < rounds; ++round) {
        EpubPackage::Zip zip = original;
        const QStringList names = zip.names.filter(QRegularExpression(QStringLiteral("\\.(xhtml|opf|css|svg|ncx)$")));
        for (int n = 0, count = 1 + random.bounded(6); n < count; ++n) {
            QByteArray &data = zip.data[names.at(random.bounded(int(names.size())))];
            if (data.isEmpty())
                continue;
            switch (round % 3) {
            case 0:
                data[random.bounded(int(data.size()))] = "<>&\"/ \x80\xff"[random.bounded(8)];
                break;
            case 1:
                data.truncate(random.bounded(int(data.size())));
                break;
            default:
                data.insert(random.bounded(int(data.size())), "<div><p>");
            }
        }
        const QString damaged = dir.filePath(QStringLiteral("damaged.epub"));
        QString error;
        QVERIFY(EpubPackage::writeZip(damaged, zip, &error));
        const QString azw3 = dir.filePath(QStringLiteral("damaged.azw3"));
        if (!Kf8::write(damaged, azw3, Kf8::Options(), &error))
            continue;
        ++made;
        QVERIFY2(MobiBook::toEpub(azw3, dir.filePath(QStringLiteral("back.epub")), &error), qPrintable(error));
    }
    QVERIFY(made > rounds / 3);
}

QTEST_GUILESS_MAIN(Kf8Test)
#include "kf8_test.moc"
