#include "pdf_reader.h"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "calibre_text.h"

namespace {

// --- The tools ---------------------------------------------------------------

// Calibre's bundled poppler, which needs its own libraries, else the
// system's.
struct Tool
{
    QString program;
    QString libraryPath;
};

Tool findTool(const QString &name)
{
    for (const QString &folder : { QStringLiteral("/opt/calibre") }) {
        const QString program = folder + QStringLiteral("/bin/") + name;
        if (QFileInfo(program).isExecutable() && QFileInfo::exists(folder + QStringLiteral("/lib")))
            return { program, folder + QStringLiteral("/lib") };
    }
    return { QStandardPaths::findExecutable(name), {} };
}

// The tool's standard output, or nothing when it fails.
std::optional<QByteArray> run(const Tool &tool, const QStringList &arguments)
{
    if (tool.program.isEmpty())
        return std::nullopt;
    QProcess process;
    if (!tool.libraryPath.isEmpty()) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LD_LIBRARY_PATH"), tool.libraryPath);
        process.setProcessEnvironment(environment);
    }
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(tool.program, arguments);
    if (!process.waitForFinished(120000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::nullopt;
    return process.readAllStandardOutput();
}

// Python's bytes.strip(): ASCII whitespace.
QByteArray stripBytes(const QByteArray &bytes)
{
    qsizetype start = 0;
    qsizetype end = bytes.size();
    const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (start < end && space(bytes.at(start)))
        ++start;
    while (end > start && space(bytes.at(end - 1)))
        --end;
    return bytes.mid(start, end - start);
}

// str.splitlines()
QStringList splitLines(const QString &text)
{
    static const QRegularExpression breaks(QStringLiteral("\\r\\n|[\\n\\r\\x0b\\x0c\\x1c\\x1d\\x1e\\x85\\x{2028}\\x{2029}]"));
    QStringList lines = text.split(breaks);
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

// --- XMP ---------------------------------------------------------------------

const QString rdfNs = QStringLiteral("http://www.w3.org/1999/02/22-rdf-syntax-ns#");
const QString dcNs = QStringLiteral("http://purl.org/dc/elements/1.1/");
const QString pdfNs = QStringLiteral("http://ns.adobe.com/pdf/1.3/");
const QString pdfxNs = QStringLiteral("http://ns.adobe.com/pdfx/1.3/");
const QString xmpNs = QStringLiteral("http://ns.adobe.com/xap/1.0/");
const QString xmpidqNs = QStringLiteral("http://ns.adobe.com/xmp/Identifier/qual/1.0/");
const QString prismNs = QStringLiteral("http://prismstandard.org/namespaces/basic/2.0/");
const QString xmlNs = QStringLiteral("http://www.w3.org/XML/1998/namespace");
const QString calibreNs = QStringLiteral("http://calibre-ebook.com/xmp-namespace");
const QString calibreSiNs = QStringLiteral("http://calibre-ebook.com/xmp-namespace-series-index");

bool is(const QDomElement &element, const QString &ns, const QString &name)
{
    return element.namespaceURI() == ns && element.localName() == name;
}

void collect(const QDomElement &parent, const QString &ns, const QString &name, QList<QDomElement> *result)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (is(child, ns, name))
            result->append(child);
        collect(child, ns, name, result);
    }
}

// descendant::ns:name
QList<QDomElement> descendants(const QDomElement &parent, const QString &ns, const QString &name)
{
    QList<QDomElement> result;
    collect(parent, ns, name, &result);
    return result;
}

// //ns:name, the root among them.
QList<QDomElement> everywhere(const QDomDocument &document, const QString &ns, const QString &name)
{
    QList<QDomElement> result;
    const QDomElement root = document.documentElement();
    if (is(root, ns, name))
        result.append(root);
    collect(root, ns, name, &result);
    return result;
}

// lxml's .text: the text before the element's first child.
QString leadingText(const QDomElement &element)
{
    QString text;
    for (QDomNode node = element.firstChild(); !node.isNull(); node = node.nextSibling()) {
        if (!node.isText())
            break;
        text += node.toCharacterData().data();
    }
    return text;
}

// read_simple_property()
std::optional<QString> readSimpleProperty(const QDomElement &element)
{
    if (element.isNull())
        return std::nullopt;
    const QString text = leadingText(element);
    if (!text.isEmpty())
        return text;
    return element.attributeNS(rdfNs, QStringLiteral("resource"));
}

// read_lang_alt()
QDomElement readLangAlt(const QDomElement &parent)
{
    const QList<QDomElement> items = descendants(parent, rdfNs, QStringLiteral("li"));
    for (const QDomElement &item : items) {
        const QString lang = item.hasAttributeNS(xmlNs, QStringLiteral("lang"))
            ? item.attributeNS(xmlNs, QStringLiteral("lang")) : item.attribute(QStringLiteral("xml:lang"));
        if (lang == QLatin1String("x-default"))
            return item;
    }
    return items.isEmpty() ? QDomElement() : items.first();
}

// read_sequence()
QStringList readSequence(const QDomElement &parent)
{
    QStringList values;
    for (const QDomElement &item : descendants(parent, rdfNs, QStringLiteral("li")))
        values.append(readSimpleProperty(item).value_or(QString()));
    return values;
}

// multiple_sequences(): the distinct, non-empty values.
QStringList multipleSequences(const QDomDocument &document, const QString &ns, const QString &name)
{
    QStringList values;
    for (const QDomElement &item : everywhere(document, ns, name)) {
        for (const QString &value : readSequence(item)) {
            if (!value.isEmpty() && !values.contains(value))
                values.append(value);
        }
    }
    return values;
}

// first_alt()
QString firstAlt(const QDomDocument &document, const QString &ns, const QString &name)
{
    for (const QDomElement &item : everywhere(document, ns, name)) {
        const QString value = readSimpleProperty(readLangAlt(item)).value_or(QString());
        if (!value.isEmpty())
            return value;
    }
    return {};
}

// first_simple()
QString firstSimple(const QDomDocument &document, const QString &ns, const QString &name)
{
    for (const QDomElement &item : everywhere(document, ns, name)) {
        const QString value = readSimpleProperty(item).value_or(QString());
        if (!value.isEmpty())
            return value;
    }
    return {};
}

// first_sequence(): the first item of the first sequence, even empty.
std::optional<QString> firstSequence(const QDomDocument &document, const QString &ns, const QString &name)
{
    for (const QDomElement &item : everywhere(document, ns, name)) {
        const QStringList values = readSequence(item);
        if (!values.isEmpty())
            return values.first();
    }
    return std::nullopt;
}

// parse_xmp_packet(): a packet that says it is UTF-8 is read as such,
// without its XML declaration; any other as its declaration says.
bool parsePacket(const QByteArray &raw, QDomDocument *document)
{
    const QByteArray packet = stripBytes(raw);
    static const QRegularExpression begin(QStringLiteral("<?xpacket\\s+[^>]*?begin\\s*=\\s*['\"]([^'\"]*)['\"]"));
    const QRegularExpressionMatch match = begin.match(QString::fromLatin1(packet.left(1024)));
    const QDomDocument::ParseOptions options = QDomDocument::ParseOption::UseNamespaceProcessing;
    if (match.hasMatch() && (match.captured(1).isEmpty() || match.captured(1) == QLatin1String("\xef\xbb\xbf"))) {
        static const QRegularExpression declaration(QStringLiteral("<\\?xml[^<>]+encoding\\s*=\\s*['\"](.*?)['\"][^<>]*>"),
                                                    QRegularExpression::CaseInsensitiveOption);
        const QString text = QString::fromUtf8(packet).remove(declaration);
        return bool(document->setContent(QAnyStringView(text), options));
    }
    return bool(document->setContent(packet, options));
}

} // namespace

namespace PdfReader {

QString pdfinfo()
{
    return findTool(QStringLiteral("pdfinfo")).program;
}

QString pdftoppm()
{
    return findTool(QStringLiteral("pdftoppm")).program;
}

std::optional<Xmp> readXmp(const QByteArray &packet, const QString &interfaceLanguage)
{
    QDomDocument document;
    if (!parsePacket(packet, &document))
        return std::nullopt;
    Xmp xmp;
    ImportedMetadata &mi = xmp.metadata;
    const QString title = firstAlt(document, dcNs, QStringLiteral("title"));
    // Nitro PDF's corrupted packets.
    if (title.startsWith(QLatin1String("\\376\\377")))
        return std::nullopt;
    mi.title = title == CalibreText::unknownText(interfaceLanguage) ? QString() : title;
    for (const QString &creators : multipleSequences(document, dcNs, QStringLiteral("creator")))
        mi.authors += CalibreText::stringToAuthors(creators);
    mi.tags = multipleSequences(document, dcNs, QStringLiteral("subject"));
    if (mi.tags.isEmpty())
        mi.tags = multipleSequences(document, pdfNs, QStringLiteral("Keywords"));
    mi.comments = firstAlt(document, dcNs, QStringLiteral("description"));
    const QStringList publishers = multipleSequences(document, dcNs, QStringLiteral("publisher"));
    if (!publishers.isEmpty())
        mi.publisher = publishers.first();
    QString date = firstSequence(document, dcNs, QStringLiteral("date")).value_or(QString());
    if (date.isEmpty())
        date = firstSimple(document, xmpNs, QStringLiteral("CreateDate"));
    mi.pubdate = OpfReader::parseDate(date, false);
    mi.bookProducer = firstSimple(document, xmpNs, QStringLiteral("CreatorTool"));
    const std::optional<CalibreDate> metadataDate = OpfReader::parseDate(firstSimple(document, xmpNs, QStringLiteral("MetadataDate")), false);
    const std::optional<CalibreDate> modifyDate = OpfReader::parseDate(firstSimple(document, xmpNs, QStringLiteral("ModifyDate")), false);
    xmp.metadataDate = !metadataDate ? modifyDate : !modifyDate ? metadataDate : std::max(*metadataDate, *modifyDate);
    const QString rating = firstSimple(document, calibreNs, QStringLiteral("rating"));
    bool ok = false;
    const double ratingValue = CalibreText::strip(rating).toDouble(&ok);
    if (ok && ratingValue >= 0 && ratingValue <= 10)
        mi.rating = ratingValue;
    // read_series()
    for (const QDomElement &item : everywhere(document, calibreNs, QStringLiteral("series"))) {
        const QList<QDomElement> values = descendants(item, rdfNs, QStringLiteral("value"));
        if (values.isEmpty())
            continue;
        const QString series = leadingText(values.first());
        if (CalibreText::strip(series).isEmpty())
            continue;
        mi.series = series;
        mi.seriesIndex = 1.0;
        for (const QDomElement &index : descendants(item, calibreSiNs, QStringLiteral("series_index"))) {
            const double value = CalibreText::strip(leadingText(index)).toDouble(&ok);
            if (ok) {
                mi.seriesIndex = value;
                break;
            }
        }
        break;
    }
    for (const QDomElement &element : everywhere(document, calibreNs, QStringLiteral("title_sort"))) {
        const QString value = readSimpleProperty(element).value_or(QString());
        if (!value.isEmpty()) {
            mi.titleSort = value;
            break;
        }
    }
    for (const QDomElement &element : everywhere(document, calibreNs, QStringLiteral("author_sort"))) {
        const QString value = readSimpleProperty(element).value_or(QString());
        if (!value.isEmpty()) {
            mi.authorSort = value;
            break;
        }
    }
    for (const QString &language : multipleSequences(document, dcNs, QStringLiteral("language"))) {
        const QString code = CalibreText::canonicalizeLanguage(language);
        if (!code.isEmpty())
            mi.languages.append(code);
    }

    // Identifiers: xmp:Identifier's, then PRISM's and PDF/X's, then a DOI or
    // ISBN as dc:identifier, both kept as "doi" as Calibre does.
    QList<QPair<QString, QString>> identifiers;
    const auto has = [&identifiers](const QString &scheme) {
        return std::any_of(identifiers.cbegin(), identifiers.cend(), [&scheme](const auto &i) { return i.first == scheme; });
    };
    const auto set = [&identifiers](const QString &scheme, const QString &value) {
        for (auto &identifier : identifiers) {
            if (identifier.first == scheme) {
                identifier.second = value;
                return;
            }
        }
        identifiers.append({ scheme, value });
    };
    for (const QDomElement &parent : everywhere(document, xmpNs, QStringLiteral("Identifier"))) {
        // read_xmp_identifers(): ./rdf:Bag/rdf:li with a scheme.
        for (QDomElement bag = parent.firstChildElement(); !bag.isNull(); bag = bag.nextSiblingElement()) {
            if (!is(bag, rdfNs, QStringLiteral("Bag")))
                continue;
            for (QDomElement li = bag.firstChildElement(); !li.isNull(); li = li.nextSiblingElement()) {
                if (!is(li, rdfNs, QStringLiteral("li")))
                    continue;
                const QList<QDomElement> values = descendants(li, rdfNs, QStringLiteral("value"));
                const QList<QDomElement> schemes = descendants(li, xmpidqNs, QStringLiteral("Scheme"));
                if (values.isEmpty() || schemes.isEmpty())
                    continue;
                const QString scheme = leadingText(schemes.first());
                const QString value = leadingText(values.first());
                if (!scheme.isEmpty() && !value.isEmpty())
                    set(scheme.toLower(), value);
            }
        }
    }
    for (const QString &ns : { prismNs, pdfxNs }) {
        for (const QString &scheme : { QStringLiteral("isbn"), QStringLiteral("url"), QStringLiteral("doi") }) {
            if (has(scheme))
                continue;
            QString value = firstSimple(document, ns, scheme);
            if (scheme == QLatin1String("isbn"))
                value = CalibreText::checkIsbn(value);
            else if (scheme == QLatin1String("doi"))
                value = CalibreText::checkDoi(value);
            if (!value.isEmpty())
                set(scheme, value);
        }
    }
    const QString dcIdentifier = firstSimple(document, dcNs, QStringLiteral("identifier"));
    if (!has(QStringLiteral("doi"))) {
        const QString doi = CalibreText::checkDoi(dcIdentifier);
        if (!doi.isEmpty())
            set(QStringLiteral("doi"), doi);
    }
    if (!has(QStringLiteral("isbn"))) {
        const QString isbn = CalibreText::checkIsbn(dcIdentifier);
        if (!isbn.isEmpty())
            set(QStringLiteral("doi"), isbn);
    }
    for (const auto &identifier : std::as_const(identifiers))
        mi.setIdentifier(identifier.first, identifier.second);
    return xmp;
}

bool read(const QString &path, const QString &interfaceLanguage, ImportedMetadata *metadata, QString *error)
{
    // read_info(): pdfinfo's fields, in its order.
    const Tool info = findTool(QStringLiteral("pdfinfo"));
    if (info.program.isEmpty()) {
        if (error)
            *error = QObject::tr("Reliure needs Calibre or poppler's pdfinfo to read PDF files.");
        return false;
    }
    const QString file = QFileInfo(path).absoluteFilePath();
    const std::optional<QByteArray> output = run(info, { QStringLiteral("-enc"), QStringLiteral("UTF-8"),
                                                         QStringLiteral("-isodates"), file });
    if (!output) {
        if (error)
            *error = QObject::tr("pdfinfo could not read %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    QList<QPair<QString, QString>> fields;
    for (const QString &line : splitLines(QString::fromUtf8(*output))) {
        if (!line.contains(u':'))
            continue;
        const QString field = line.section(u':', 0, 0);
        const QString value = CalibreText::strip(line.section(u':', 1));
        if (field.isEmpty() || value.isEmpty())
            continue;
        const auto existing = std::find_if(fields.begin(), fields.end(), [&field](const auto &f) { return f.first == field; });
        if (existing != fields.end())
            existing->second = value;
        else
            fields.append({ field, value });
    }
    const auto value = [&fields](const QString &field) -> std::optional<QString> {
        for (const auto &f : std::as_const(fields)) {
            if (f.first == field)
                return f.second;
        }
        return std::nullopt;
    };
    const std::optional<QByteArray> xmpPacket = run(info, { QStringLiteral("-meta"), file });

    // get_metadata() of ebooks/metadata/pdf.py.
    const QString unknown = CalibreText::unknownText(interfaceLanguage);
    ImportedMetadata mi;
    mi.title = value(QStringLiteral("Title")).value_or(QString());
    if (mi.title == unknown)
        mi.title.clear();
    if (const std::optional<QString> author = value(QStringLiteral("Author")))
        mi.authors = CalibreText::stringToAuthors(*author);
    mi.bookProducer = value(QStringLiteral("Creator")).value_or(QString());
    if (const std::optional<QString> keywords = value(QStringLiteral("Keywords"))) {
        for (const QString &keyword : keywords->split(u','))
            mi.tags.append(CalibreText::strip(keyword));
        QString isbn;
        for (const QString &tag : std::as_const(mi.tags)) {
            isbn = CalibreText::checkIsbn(tag);
            if (!isbn.isEmpty())
                break;
        }
        if (!isbn.isEmpty()) {
            mi.setIdentifier(QStringLiteral("isbn"), isbn);
            mi.tags.removeIf([&isbn](const QString &tag) { return CalibreText::checkIsbn(tag) == isbn; });
        }
    }
    if (const std::optional<QString> subject = value(QStringLiteral("Subject")))
        mi.tags.prepend(*subject);

    // consolidate_metadata(): the XMP packet's, with replace_metadata, but
    // for the info dictionary's title, authors and tags when its
    // modification date is the later.
    const QByteArray packet = xmpPacket ? stripBytes(*xmpPacket) : QByteArray();
    if (!packet.isEmpty()) {
        if (const std::optional<Xmp> xmp = readXmp(packet, interfaceLanguage)) {
            const ImportedMetadata &other = xmp->metadata;
            const QString infoTitle = mi.title;
            const QStringList infoAuthors = mi.authors;
            const QStringList infoTags = mi.tags;
            if (!other.title.isEmpty()) {
                mi.title = other.title;
                mi.titleSort = other.titleSort;
            }
            const bool selfUnknown = mi.authors.isEmpty()
                || (mi.authors.size() == 1 && mi.authors.first() == unknown
                    && (mi.authorSort.isEmpty() || mi.authorSort == unknown));
            if (!other.authors.isEmpty() && (other.authors.first() != unknown || selfUnknown)) {
                mi.authors = other.authors;
                mi.authorSort = other.authorSort;
            }
            mi.rating = other.rating;
            mi.series = other.series;
            mi.seriesIndex = other.seriesIndex;
            mi.bookProducer = other.bookProducer;
            mi.timestamp = other.timestamp;
            mi.pubdate = other.pubdate;
            mi.uuid = other.uuid;
            mi.publisher = other.publisher;
            mi.tags = other.tags;
            if (!other.comments.isEmpty())
                mi.comments = other.comments;
            mi.identifiers = other.identifiers;
            if (!other.languages.isEmpty())
                mi.languages = other.languages;
            if (mi.series.isEmpty())
                mi.seriesIndex.reset();

            bool preferInfo = false;
            if (const std::optional<QString> modified = value(QStringLiteral("ModDate")); modified && xmp->metadataDate) {
                const std::optional<CalibreDate> date = OpfReader::parseDate(*modified, false);
                preferInfo = date && *xmp->metadataDate < *date;
            }
            if (preferInfo) {
                mi.title = infoTitle;
                mi.authors = infoAuthors;
                mi.tags = infoTags;
            } else {
                const bool xmpAuthorsNull = other.authors.isEmpty()
                    || (other.authors.size() == 1 && other.authors.first() == unknown);
                mi.authors = xmpAuthorsNull ? infoAuthors : other.authors;
                mi.tags = other.tags.isEmpty() ? infoTags : other.tags;
            }
        }
    }

    // A DOI or an ISBN anywhere in the info dictionary, when the XMP packet
    // had none.
    for (const QString &scheme : { QStringLiteral("doi"), QStringLiteral("isbn") }) {
        const bool present = std::any_of(mi.identifiers.cbegin(), mi.identifiers.cend(),
                                         [&scheme](const auto &i) { return i.first == scheme; });
        if (present)
            continue;
        for (const auto &field : std::as_const(fields)) {
            const QString found = scheme == QLatin1String("doi") ? CalibreText::checkDoi(field.second)
                                                                 : CalibreText::checkIsbn(field.second);
            if (!found.isEmpty()) {
                mi.setIdentifier(scheme, found);
                break;
            }
        }
    }

    // The first page, cropped, at pdftoppm's 150 dpi.
    QTemporaryDir folder;
    const Tool render = findTool(QStringLiteral("pdftoppm"));
    if (folder.isValid()
        && run(render, { QStringLiteral("-singlefile"), QStringLiteral("-jpeg"), QStringLiteral("-cropbox"), file,
                         folder.filePath(QStringLiteral("cover")) })) {
        QFile cover(folder.filePath(QStringLiteral("cover.jpg")));
        if (cover.open(QIODevice::ReadOnly))
            mi.cover = cover.readAll();
    }
    *metadata = mi;
    return true;
}

} // namespace PdfReader
