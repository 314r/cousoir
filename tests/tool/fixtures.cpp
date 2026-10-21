// The book files of the fixtures, which tools/make_fixture_library.sh and
// tools/make_import_fixtures.sh then hand to Calibre.

#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

#include "common.h"

namespace Tool {

namespace {

// xml.sax.saxutils.escape().
QString escape(QString text)
{
    return text.replace(u'&', QStringLiteral("&amp;")).replace(u'<', QStringLiteral("&lt;")).replace(u'>', QStringLiteral("&gt;"));
}

const char container[] = R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
)";

// An EPUB of tests/data/import: the package file, then the other files, in
// their order. Its container lists a missing package file first.
bool writeImportEpub(const QString &path, const QString &opf, const ZipEntries &files)
{
    const QString container = QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/missing.opf" media-type="application/oebps-package+xml"/>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
)");
    ZipEntries entries{
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"), container.toUtf8() },
        { QStringLiteral("OEBPS/content.opf"), opf.toUtf8() },
    };
    entries.append(files);
    return writeZip(path, entries, { QStringLiteral("mimetype") });
}

QByteArray page(const QString &title, const QString &body = QString())
{
    return QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>%1</title></head>
<body><h1>%1</h1>%2</body>
</html>
)")
        .arg(title, body)
        .toUtf8();
}

// A PDF text string: ASCII as a literal string, anything else in UTF-16
// with a byte order mark, as PDF text strings beyond PDFDocEncoding are.
QByteArray pdfString(QString text)
{
    if (std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() < 128; }))
        return '(' + text.replace(u'\\', QStringLiteral("\\\\")).replace(u'(', QStringLiteral("\\(")).replace(u')', QStringLiteral("\\)")).toLatin1()
               + ')';
    QByteArray utf16;
    const QString marked = QChar(0xFEFF) + text;
    for (const QChar c : marked) {
        utf16.append(char(c.unicode() >> 8));
        utf16.append(char(c.unicode() & 0xFF));
    }
    return '<' + utf16.toHex().toUpper() + '>';
}

} // namespace

// Writes a minimal valid EPUB 3 for the fixture library:
//
//   make-epub OUT TITLE AUTHOR LANGUAGE PARAGRAPH…
int makeEpub(const QStringList &arguments)
{
    const QString &title = arguments.at(1);
    const QString &author = arguments.at(2);
    const QString &language = arguments.at(3);
    // uuid.NAMESPACE_URL.
    const QUuid id = QUuid::createUuidV5(QUuid(QStringLiteral("6ba7b811-9dad-11d1-80b4-00c04fd430c8")),
                                         QStringLiteral("cousoir-fixture:") + title);
    const QString opf = QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:%1</dc:identifier>
    <dc:title>%2</dc:title>
    <dc:creator>%3</dc:creator>
    <dc:language>%4</dc:language>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="text"/>
  </spine>
</package>
)")
                              .arg(id.toString(QUuid::WithoutBraces), escape(title), escape(author), language);
    const QString nav = QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>%1</title></head>
<body><nav epub:type="toc"><ol><li><a href="text.xhtml">%1</a></li></ol></nav></body>
</html>
)")
                              .arg(escape(title));
    QString body;
    for (const QString &paragraph : arguments.mid(4))
        body += QStringLiteral("<p>") + escape(paragraph) + QStringLiteral("</p>");
    const QString text = QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>%1</title></head>
<body><h1>%1</h1>%2</body>
</html>
)")
                               .arg(escape(title), body);
    const ZipEntries entries{
        { QStringLiteral("mimetype"), "application/epub+zip" },
        { QStringLiteral("META-INF/container.xml"), container },
        { QStringLiteral("OEBPS/content.opf"), opf.toUtf8() },
        { QStringLiteral("OEBPS/nav.xhtml"), nav.toUtf8() },
        { QStringLiteral("OEBPS/text.xhtml"), text.toUtf8() },
    };
    return writeZip(arguments.at(0), entries, { QStringLiteral("mimetype") }) ? 0 : 1;
}

// Writes the EPUBs of tests/data/import, each with a package file that tries
// one corner of Calibre's OPF readers:
//
//   make-import-epubs FOLDER COVER.jpg
int makeImportEpubs(const QStringList &arguments)
{
    const QDir folder(arguments.at(0));
    const std::optional<QByteArray> cover = readFile(arguments.at(1));
    if (!cover) {
        err() << "Cannot read " << arguments.at(1) << Qt::endl;
        return 1;
    }

    // EPUB 3: refines, a no-break space in a name, an editor, a subtitle, a
    // collection, a fractional date, a cover-image item and identifiers.
    const bool vicomte = writeImportEpub(folder.filePath(QStringLiteral("Le Vicomte - Inconnu.epub")), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid"
         prefix="calibre: https://calibre-ebook.com">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:2a7d1c3e-8b2f-4a39-9e65-0f4c1d2b3a4f</dc:identifier>
    <dc:identifier id="isbn">urn:isbn:978-2-07-036024-5</dc:identifier>
    <dc:identifier>doi:10.1000/182</dc:identifier>
    <dc:title id="main">Le  Vicomte)" " " R"(de Bragelonne</dc:title>
    <meta refines="#main" property="title-type">main</meta>
    <meta refines="#main" property="file-as">Vicomte de Bragelonne, Le</meta>
    <dc:title id="sub">Tome premier</dc:title>
    <meta refines="#sub" property="title-type">subtitle</meta>
    <dc:creator id="a1">Alexandre)" " " R"(Dumas</dc:creator>
    <meta refines="#a1" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#a1" property="file-as">Dumas, Alexandre</meta>
    <meta refines="#a1" property="display-seq">2</meta>
    <dc:creator id="a2">Auguste Maquet</dc:creator>
    <meta refines="#a2" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#a2" property="display-seq">1</meta>
    <dc:creator id="e1">Jean Éditeur</dc:creator>
    <meta refines="#e1" property="role" scheme="marc:relators">edt</meta>
    <dc:contributor id="p1">Atelier Typographique</dc:contributor>
    <meta refines="#p1" property="role" scheme="marc:relators">bkp</meta>
    <dc:language>fr</dc:language>
    <dc:language>en</dc:language>
    <dc:date>1847-10-20T11:30:15.123456+01:00</dc:date>
    <dc:publisher>Michel Lévy</dc:publisher>
    <dc:publisher>Second Publisher</dc:publisher>
    <dc:subject>Roman historique</dc:subject>
    <dc:subject>  Cape   et épée </dc:subject>
    <dc:subject>roman historique</dc:subject>
    <dc:description>&lt;p&gt;Le dernier volet.&lt;/p&gt;</dc:description>
    <meta property="belongs-to-collection" id="c1">Les Mousquetaires</meta>
    <meta refines="#c1" property="collection-type">series</meta>
    <meta refines="#c1" property="group-position">3.5</meta>
    <meta property="calibre:rating">8</meta>
    <meta property="calibre:timestamp" scheme="dcterms:W3CDTF">2020-02-02T10:00:00+00:00</meta>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="text" href="text.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>
  </manifest>
  <spine><itemref idref="text"/></spine>
</package>
)"),
                                         {
                                             { QStringLiteral("OEBPS/nav.xhtml"), page(QStringLiteral("Nav")) },
                                             { QStringLiteral("OEBPS/text.xhtml"), page(QStringLiteral("Le Vicomte")) },
                                             { QStringLiteral("OEBPS/images/cover.jpg"), *cover },
                                         });

    // EPUB 2: opf:file-as and opf:role, Calibre's meta tags, an offset
    // QTimeZone refuses, two dates, comma-separated subjects, a bare ISBN,
    // and a cover meta whose percent-encoded href Calibre does not decode, so
    // it renders the first page instead.
    const bool black = writeImportEpub(folder.filePath(QStringLiteral("Black Company 2.epub")), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:identifier id="uuid_id" opf:scheme="uuid">0d3f2c1b-1111-4222-8333-944455556666</dc:identifier>
    <dc:identifier opf:scheme="calibre">0d3f2c1b-1111-4222-8333-944455556666</dc:identifier>
    <dc:identifier>9780812521399</dc:identifier>
    <dc:identifier opf:scheme="GOODREADS">40307</dc:identifier>
    <dc:title>Shadows   Linger</dc:title>
    <dc:creator opf:role="aut" opf:file-as="Cook, Glen">Glen Cook &amp; Someone Else</dc:creator>
    <dc:creator opf:role="ill">An Illustrator</dc:creator>
    <dc:contributor opf:role="bkp">calibre (9.16.0) [https://calibre-ebook.com]</dc:contributor>
    <dc:date>1984-05-20T19:59:00-17:00</dc:date>
    <dc:date opf:event="original-publication">1983-01-01</dc:date>
    <dc:language>eng</dc:language>
    <dc:subject>Fantasy, Military</dc:subject>
    <dc:subject>Dark fantasy</dc:subject>
    <dc:publisher>Tor</dc:publisher>
    <dc:description>The second book.</dc:description>
    <meta name="calibre:series" content="Chronicles of the Black Company"/>
    <meta name="calibre:series_index" content="2"/>
    <meta name="calibre:rating" content="6"/>
    <meta name="calibre:title_sort" content="Shadows Linger"/>
    <meta name="calibre:timestamp" content="2019-03-04T05:06:07+00:00"/>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="text" href="text.html" media-type="application/xhtml+xml"/>
    <item id="cover-image" href="cover%20image.jpg" media-type="image/jpeg"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx"><itemref idref="text"/></spine>
</package>
)"),
                                       {
                                           { QStringLiteral("OEBPS/text.html"), page(QStringLiteral("Shadows Linger")) },
                                           { QStringLiteral("OEBPS/cover image.jpg"), *cover },
                                       });

    // No title and no author: the file's name gives them.
    const bool lost = writeImportEpub(folder.filePath(QStringLiteral("The Lost Title - Anne Onyme & Bob Builder.epub")),
                                      QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:7b0c9d8e-2222-4333-8444-a55566667777</dc:identifier>
    <dc:language>und</dc:language>
  </metadata>
  <manifest><item id="text" href="text.html" media-type="application/xhtml+xml"/></manifest>
  <spine><itemref idref="text"/></spine>
</package>
)"),
                                      {
                                          { QStringLiteral("OEBPS/text.html"),
                                            page(QStringLiteral("Lost"), QStringLiteral(R"(<p><img src="cover.jpg" alt="cover"/></p>)")) },
                                          { QStringLiteral("OEBPS/cover.jpg"), *cover },
                                      });

    // A manga-style book whose first spine item is its cover image.
    const bool manga = writeImportEpub(folder.filePath(QStringLiteral("manga.epub")), QStringLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="id">urn:uuid:9c8b7a6f-3333-4444-8555-b66677778888</dc:identifier>
    <dc:title>Manga</dc:title>
    <dc:creator>Mangaka</dc:creator>
    <dc:language>ja</dc:language>
  </metadata>
  <manifest>
    <item id="p1" href="p1.jpg" media-type="image/jpeg"/>
    <item id="text" href="text.html" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="p1"/><itemref idref="text"/></spine>
</package>
)"),
                                       {
                                           { QStringLiteral("OEBPS/p1.jpg"), *cover },
                                           { QStringLiteral("OEBPS/text.html"), page(QStringLiteral("Manga")) },
                                       });
    return vicomte && black && lost && manga ? 0 : 1;
}

// Writes a one-page PDF with a line of text, and optionally an info
// dictionary, from a JSON object of strings, and an XMP packet:
//
//   make-pdf OUT TEXT [INFO.json [PACKET.xml]]
int makePdf(const QStringList &arguments)
{
    // The info dictionary's entries in the order of the file.
    QList<std::pair<QString, QString>> info;
    if (arguments.size() > 2) {
        const std::optional<QByteArray> raw = readFile(arguments.at(2));
        const std::optional<QJsonValue> read = readJson(arguments.at(2));
        if (!raw || !read)
            return 1;
        const QJsonObject object = read->toObject();
        const QString text = QString::fromUtf8(*raw);
        QList<std::pair<qsizetype, QString>> keys;
        for (const QString &key : object.keys())
            keys.append({ text.indexOf(QRegularExpression(u'"' + QRegularExpression::escape(key) + QStringLiteral("\"\\s*:"))), key });
        std::sort(keys.begin(), keys.end());
        for (const auto &[position, key] : std::as_const(keys))
            info.append({ key, object.value(key).toString() });
    }
    std::optional<QByteArray> xmp;
    if (arguments.size() > 3) {
        xmp = readFile(arguments.at(3));
        if (!xmp) {
            err() << "Cannot read " << arguments.at(3) << Qt::endl;
            return 1;
        }
    }

    const QByteArray stream = (QStringLiteral("BT /F1 18 Tf 72 720 Td (") + arguments.at(1) + QStringLiteral(") Tj ET")).toLatin1();
    QList<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Pages 2 0 R") + (xmp ? " /Metadata 6 0 R" : "") + " >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R"
        " /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    if (xmp)
        objects.append("<< /Type /Metadata /Subtype /XML /Length " + QByteArray::number(xmp->size()) + " >>\nstream\n" + *xmp
                       + "\nendstream");
    if (!info.isEmpty()) {
        QList<QByteArray> entries;
        for (const auto &[key, value] : std::as_const(info))
            entries.append('/' + key.toLatin1() + ' ' + pdfString(value));
        objects.append("<< " + entries.join(' ') + " >>");
    }
    QByteArray pdf("%PDF-1.4\n");
    QList<qsizetype> offsets;
    for (qsizetype number = 1; number <= objects.size(); ++number) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(number) + " 0 obj\n" + objects.at(number - 1) + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : std::as_const(offsets))
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    QByteArray trailer = "/Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R";
    if (!info.isEmpty())
        trailer += " /Info " + QByteArray::number(objects.size()) + " 0 R";
    pdf += "trailer\n<< " + trailer + " >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return writeFile(arguments.at(0), pdf) ? 0 : 1;
}

} // namespace Tool
