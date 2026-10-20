// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/opf3.py: read_metadata() and the readers it
//   calls, parse_prefixes(), expand_prefix(), parse_identifier() and
//   find_main_title()
// - src/calibre/ebooks/metadata/opf2.py: the OPF class's readers,
//   first_spine_item() and the EPUB 3 raster cover
// - src/calibre/ebooks/metadata/utils.py: parse_opf_version()
// - src/calibre/ebooks/metadata/book/base.py: set_identifier()
// - src/calibre/utils/date.py: fix_only_date()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "opf_reader.h"

#include <QDomDocument>
#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>

#include "calibre_text.h"

namespace {

const QString opfNamespace = QStringLiteral("http://www.idpf.org/2007/opf");
const QString calibrePrefix = QStringLiteral("https://calibre-ebook.com");

// normalize_whitespace()
QString collapse(const QString &text)
{
    static const QRegularExpression spaces(CalibreText::pythonSpace + u'+');
    return CalibreText::strip(QString(text).replace(spaces, QStringLiteral(" ")));
}

QList<QDomElement> children(const QDomElement &parent)
{
    QList<QDomElement> result;
    for (QDomElement child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        result.append(child);
    return result;
}

void collectDescendants(const QDomElement &parent, QList<QDomElement> *result)
{
    for (QDomElement child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        result->append(child);
        collectDescendants(child, result);
    }
}

QList<QDomElement> descendants(const QDomElement &parent)
{
    QList<QDomElement> result;
    collectDescendants(parent, &result);
    return result;
}

// An attribute by its local name, whatever its namespace prefix.
QString attribute(const QDomElement &element, const QString &name)
{
    const QDomNamedNodeMap attributes = element.attributes();
    for (int index = 0; index < attributes.count(); ++index) {
        const QDomAttr attr = attributes.item(index).toAttr();
        const QString local = attr.localName().isEmpty() ? attr.name().section(u':', -1) : attr.localName();
        if (local == name)
            return attr.value();
    }
    return {};
}

bool hasAttribute(const QDomElement &element, const QString &name)
{
    const QDomNamedNodeMap attributes = element.attributes();
    for (int index = 0; index < attributes.count(); ++index) {
        const QDomAttr attr = attributes.item(index).toAttr();
        const QString local = attr.localName().isEmpty() ? attr.name().section(u':', -1) : attr.localName();
        if (local == name)
            return true;
    }
    return false;
}

// The opf:-namespaced attribute only, as OPF('scheme') looks it up.
QString opfAttribute(const QDomElement &element, const QString &name)
{
    return element.attributeNS(opfNamespace, name);
}

// The element's name as XPath's name() spells it: prefix and local name.
QString qualifiedName(const QDomElement &element)
{
    return element.tagName();
}

QString localName(const QDomElement &element)
{
    return element.localName().isEmpty() ? element.tagName().section(u':', -1) : element.localName();
}

bool isDc(const QDomElement &element, const QString &name)
{
    return element.namespaceURI() == QLatin1String("http://purl.org/dc/elements/1.1/") && localName(element) == name;
}

bool isOpf(const QDomElement &element, const QString &name)
{
    return element.namespaceURI() == opfNamespace && localName(element) == name;
}

QString text(const QDomElement &element)
{
    return element.text();
}

QString pythonStrip(const QString &text)
{
    return CalibreText::strip(text);
}

std::optional<double> toNumber(const QString &text)
{
    bool ok = false;
    const double value = pythonStrip(text).toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

// --- OPF 3 -----------------------------------------------------------------

using Refines = QHash<QString, QList<QDomElement>>;

const QHash<QString, QString> &reservedPrefixes()
{
    static const QHash<QString, QString> prefixes{
        { QStringLiteral("dcterms"), QStringLiteral("http://purl.org/dc/terms/") },
        { QStringLiteral("epubsc"), QStringLiteral("http://idpf.org/epub/vocab/sc/#") },
        { QStringLiteral("marc"), QStringLiteral("http://id.loc.gov/vocabulary/") },
        { QStringLiteral("media"), QStringLiteral("http://www.idpf.org/epub/vocab/overlays/#") },
        { QStringLiteral("onix"), QStringLiteral("http://www.editeur.org/ONIX/book/codelists/current.html#") },
        { QStringLiteral("rendition"), QStringLiteral("http://www.idpf.org/vocab/rendition/#") },
        { QStringLiteral("schema"), QStringLiteral("http://schema.org/") },
        { QStringLiteral("xsd"), QStringLiteral("http://www.w3.org/2001/XMLSchema#") },
    };
    return prefixes;
}

QHash<QString, QString> readPrefixes(const QDomElement &root)
{
    QHash<QString, QString> prefixes = reservedPrefixes();
    using CalibreText::pythonNonSpace, CalibreText::pythonSpace;
    static const QRegularExpression pattern(u'(' + pythonNonSpace + QStringLiteral("+): ") + pythonSpace + u"*(" + pythonNonSpace + u"+)");
    auto it = pattern.globalMatch(root.attribute(QStringLiteral("prefix")));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        prefixes.insert(match.captured(1), match.captured(2));
    }
    return prefixes;
}

// expand_prefix(): "calibre:rating" as its full IRI.
QString expandPrefix(const QString &raw, const QHash<QString, QString> &prefixes)
{
    using CalibreText::pythonNonSpace, CalibreText::pythonSpace;
    static const QRegularExpression pattern(u'(' + pythonNonSpace + u"+)" + pythonSpace + u"*:" + pythonSpace + u"*(" + pythonNonSpace + u"+)");
    QString result;
    qsizetype last = 0;
    auto it = pattern.globalMatch(raw);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += raw.mid(last, match.capturedStart() - last);
        result += prefixes.value(match.captured(1), match.captured(1)) + u':' + match.captured(2);
        last = match.capturedEnd();
    }
    return result + raw.mid(last);
}

QHash<QString, QString> propertiesFor(const QString &id, const Refines &refines)
{
    QHash<QString, QString> result;
    if (id.isEmpty())
        return result;
    for (const QDomElement &element : refines.value(id)) {
        const QString key = element.attribute(QStringLiteral("property"));
        const QString value = text(element).trimmed();
        if (!key.isEmpty() && !value.isEmpty())
            result.insert(key, value);
    }
    return result;
}

struct Scheme
{
    QString namespaceIri;
    QString scheme;
    QString value;
};

QHash<QString, QList<Scheme>> propertiesWithScheme(const QString &id, const QHash<QString, QString> &prefixes,
                                                   const Refines &refines)
{
    QHash<QString, QList<Scheme>> result;
    if (id.isEmpty())
        return result;
    for (const QDomElement &element : refines.value(id)) {
        const QString key = element.attribute(QStringLiteral("property"));
        const QString value = text(element).trimmed();
        if (key.isEmpty() || value.isEmpty())
            continue;
        QString scheme = element.attribute(QStringLiteral("scheme"));
        QString iri;
        if (!scheme.isEmpty()) {
            const QString prefix = scheme.section(u':', 0, 0);
            const QString rest = scheme.section(u':', 1);
            if (scheme.contains(u':') && !prefix.isEmpty() && !rest.isEmpty() && prefixes.contains(prefix)) {
                iri = prefixes.value(prefix);
                scheme = rest;
            }
        }
        result[key].append({ iri, scheme, value });
    }
    return result;
}

bool isRelatorsRole(const QHash<QString, QList<Scheme>> &properties, const QString &role)
{
    // A role without a scheme Cousoir can resolve counts, as do MARC
    // relators.
    for (const Scheme &scheme : properties.value(QStringLiteral("role"))) {
        if (scheme.value.toLower() == role
            && (scheme.namespaceIri.isEmpty()
                || (scheme.namespaceIri == reservedPrefixes().value(QStringLiteral("marc"))
                    && scheme.scheme == QLatin1String("relators"))))
            return true;
    }
    return false;
}

// parse_identifier(): "isbn" and its digits from opf:scheme, or from
// "urn:isbn:…" or "isbn:…" in the value.
QPair<QString, QString> parseIdentifier(const QDomElement &element, QString value)
{
    const auto finalize = [](QString scheme, QString value, bool fromValue) -> QPair<QString, QString> {
        if (scheme.isEmpty() || value.isEmpty())
            return {};
        scheme = scheme.toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
            return fromValue ? QPair<QString, QString>{ QStringLiteral("url"), scheme + u':' + value } : QPair<QString, QString>{};
        if (scheme.startsWith(QLatin1String("isbn")))
            scheme = QStringLiteral("isbn");
        if (scheme == QLatin1String("isbn")) {
            value = CalibreText::checkIsbn(value.section(u':', -1));
            if (value.isEmpty())
                return {};
        }
        return { scheme, value };
    };
    const QString scheme = opfAttribute(element, QStringLiteral("scheme"));
    if (!scheme.isEmpty() && !value.toLower().startsWith(QLatin1String("urn:")))
        return finalize(scheme, value, false);
    if (value.toLower().startsWith(QLatin1String("urn:")))
        value = value.mid(4);
    return finalize(value.section(u':', 0, 0), value.contains(u':') ? value.section(u':', 1) : QString(), true);
}

QDomElement mainTitle(const QList<QDomElement> &metadata, const Refines &refines)
{
    QDomElement first;
    for (const QDomElement &title : metadata) {
        if (!isDc(title, QStringLiteral("title")) || text(title).trimmed().isEmpty())
            continue;
        if (first.isNull())
            first = title;
        if (propertiesFor(title.attribute(QStringLiteral("id")), refines).value(QStringLiteral("title-type"))
            == QLatin1String("main"))
            return title;
    }
    return first;
}

// --- dates -------------------------------------------------------------------

std::optional<CalibreDate> parseIsoDate(const QString &text, bool assumeUtc, bool *hasTime)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^\\s*(\\d{4})(?:-(\\d{1,2})(?:-(\\d{1,2}))?)?(?:[T ](\\d{1,2}):(\\d{2})(?::(\\d{2})(?:[.,](\\d+))?)?)?"
        "\\s*(Z|[+-]\\d{2}(?::?\\d{2})?)?\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    const QDate today = assumeUtc ? QDateTime::currentDateTimeUtc().date() : QDate::currentDate();
    const int year = match.captured(1).toInt();
    const int month = match.captured(2).isEmpty() ? today.month() : match.captured(2).toInt();
    const int day = match.captured(3).isEmpty() ? 15 : match.captured(3).toInt();
    const QDate date(year, month, day);
    const QTime time(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt());
    if (!date.isValid() || !time.isValid())
        return std::nullopt;
    if (hasTime)
        *hasTime = !match.captured(4).isEmpty();
    CalibreDate result;
    // Microseconds: the first six digits of the fraction.
    result.microsecond = match.captured(7).left(6).leftJustified(6, u'0').toInt();
    const QString zone = match.captured(8);
    if (zone == QLatin1String("Z")) {
        result.dateTime = QDateTime(date, time, QTimeZone::UTC);
    } else if (!zone.isEmpty()) {
        // Any offset, beyond the ±14 hours QTimeZone takes.
        const QString digits = QString(zone).remove(u':');
        const int offset = (digits.mid(1, 2).toInt() * 60 + (digits.size() > 3 ? digits.mid(3, 2).toInt() : 0)) * 60;
        result.dateTime = QDateTime(date, time, QTimeZone::UTC).addSecs(digits.startsWith(u'-') ? offset : -offset);
    } else {
        result.dateTime = QDateTime(date, time, assumeUtc ? QTimeZone(QTimeZone::UTC) : QTimeZone::systemTimeZone()).toUTC();
    }
    return result;
}

} // namespace

void ImportedMetadata::setIdentifier(const QString &type, const QString &value)
{
    // Metadata.set_identifier(): ck() and cv(), a value moved last, an empty
    // one removed.
    const QString key = CalibreText::strip(type.toLower()).remove(u':').remove(u',');
    const QString cleaned = CalibreText::strip(value).replace(u',', u'|');
    if (key.isEmpty())
        return;
    identifiers.removeIf([&key](const auto &identifier) { return identifier.first == key; });
    if (!cleaned.isEmpty())
        identifiers.append({ key, cleaned });
}

namespace OpfReader {

std::optional<CalibreDate> parseDate(const QString &text, bool assumeUtc)
{
    return parseIsoDate(text, assumeUtc, nullptr);
}

QDateTime fixOnlyDate(const QDateTime &date)
{
    QDateTime result = date;
    if (result.addDays(1).date().month() > result.date().month())
        result = result.addDays(-1);
    if (result.date().day() == 1)
        result = result.addDays(1);
    return result;
}

bool read(const QByteArray &opf, Package *package, QString *error)
{
    QDomDocument document;
    const QDomDocument::ParseResult parsed = document.setContent(opf, QDomDocument::ParseOption::UseNamespaceProcessing);
    if (!parsed) {
        if (error)
            *error = QObject::tr("The OPF cannot be read: %1").arg(parsed.errorMessage);
        return false;
    }
    const QDomElement root = document.documentElement();
    Package result;
    // parse_opf_version(): 2.0 unless it says otherwise.
    const QString version = root.attribute(QStringLiteral("version"));
    bool versionOk = false;
    result.version = version.section(u'.', 0, 0).toInt(&versionOk);
    if (!versionOk || result.version < 1)
        result.version = 2;
    ImportedMetadata &mi = result.metadata;

    QDomElement metadataElement;
    QDomElement manifest;
    QDomElement spine;
    for (const QDomElement &child : children(root)) {
        const QString name = localName(child).toLower();
        if (name == QLatin1String("metadata") && metadataElement.isNull())
            metadataElement = child;
        else if (name == QLatin1String("manifest") && manifest.isNull())
            manifest = child;
        else if (name == QLatin1String("spine") && spine.isNull())
            spine = child;
    }
    if (metadataElement.isNull()) {
        // Old OPFs have their metadata deeper down.
        for (const QDomElement &element : descendants(root)) {
            if (localName(element).toLower() == QLatin1String("metadata")) {
                metadataElement = element;
                break;
            }
        }
    }
    QHash<QString, QDomElement> manifestById;
    QList<QDomElement> manifestItems;
    for (const QDomElement &item : children(manifest)) {
        if (localName(item).toLower() == QLatin1String("item")) {
            manifestItems.append(item);
            if (!manifestById.contains(item.attribute(QStringLiteral("id"))))
                manifestById.insert(item.attribute(QStringLiteral("id")), item);
        }
    }
    QStringList spineIds;
    for (const QDomElement &itemref : children(spine)) {
        if (localName(itemref).toLower() == QLatin1String("itemref"))
            spineIds.append(itemref.attribute(QStringLiteral("idref")));
    }
    // first_spine_item(): the first itemref's item.
    for (const QString &id : std::as_const(spineIds)) {
        for (const QDomElement &item : std::as_const(manifestItems)) {
            if (item.attribute(QStringLiteral("id")) == id) {
                result.firstSpineItem = item.attribute(QStringLiteral("href"));
                break;
            }
        }
        break;
    }
    const auto imageHref = [](const QDomElement &item) -> QString {
        const QString type = item.attribute(QStringLiteral("media-type"));
        if (!type.isEmpty() && !type.contains(QLatin1String("xml")) && !type.contains(QLatin1String("html")))
            return item.attribute(QStringLiteral("href"));
        return {};
    };

    // epub3_raster_cover of opf2.py, which a KEPUB's cover takes first: the
    // cover-image item, else a first spine item that is an image, as in
    // manga.
    for (const QDomElement &item : std::as_const(manifestItems)) {
        const QStringList properties = item.attribute(QStringLiteral("properties")).toLower().split(
            QRegularExpression(CalibreText::pythonSpace + u'+'), Qt::SkipEmptyParts);
        if (properties.contains(QLatin1String("cover-image")) && !imageHref(item).isEmpty()) {
            result.epub3RasterCover = imageHref(item);
            break;
        }
    }
    if (result.epub3RasterCover.isEmpty() && !spineIds.isEmpty()) {
        static const QStringList images{ QStringLiteral("image/jpeg"), QStringLiteral("image/jpg"),
                                         QStringLiteral("image/png"), QStringLiteral("image/webp") };
        for (const QDomElement &item : std::as_const(manifestItems)) {
            if (item.attribute(QStringLiteral("id")) == spineIds.first()) {
                if (images.contains(item.attribute(QStringLiteral("media-type")).toLower()))
                    result.epub3RasterCover = item.attribute(QStringLiteral("href"));
                break;
            }
        }
    }

    if (result.version >= 3) {
        // read_metadata() of opf3.py: the elements directly in metadata.
        const QList<QDomElement> metadata = children(metadataElement);
        const QHash<QString, QString> prefixes = readPrefixes(root);
        Refines refines;
        for (const QDomElement &meta : metadata) {
            const QString target = meta.attribute(QStringLiteral("refines"));
            if (isOpf(meta, QStringLiteral("meta")) && target.startsWith(u'#'))
                refines[target.mid(1)].append(meta);
        }
        for (const QDomElement &element : metadata) {
            if (!isDc(element, QStringLiteral("identifier")))
                continue;
            const QString value = text(element).trimmed();
            if (value.isEmpty())
                continue;
            const auto [scheme, parsedValue] = parseIdentifier(element, value);
            if (scheme.isEmpty() || parsedValue.isEmpty())
                continue;
            if (scheme == QLatin1String("uuid")) {
                if (mi.uuid.isEmpty())
                    mi.uuid = parsedValue;
            } else if (scheme != QLatin1String("calibre")) {
                // The first value of each scheme.
                if (std::none_of(mi.identifiers.cbegin(), mi.identifiers.cend(),
                                 [&](const auto &id) { return id.first == scheme; }))
                    mi.identifiers.append({ scheme, parsedValue });
            }
        }
        const QDomElement title = mainTitle(metadata, refines);
        if (!title.isNull()) {
            // read_title() and read_title_sort() are simple_text():
            // whitespace normalized.
            mi.title = pythonStrip(text(title));
            for (const QDomElement &subtitle : metadata) {
                if (!isDc(subtitle, QStringLiteral("title")) || text(subtitle).trimmed().isEmpty())
                    continue;
                const QString type = propertiesFor(subtitle.attribute(QStringLiteral("id")), refines)
                                         .value(QStringLiteral("title-type"));
                if (type.contains(QLatin1String("subtitle")) || type.contains(QLatin1String("sub-title"))) {
                    if (subtitle != title)
                        mi.title += QStringLiteral(": ") + pythonStrip(text(subtitle));
                    break;
                }
            }
            mi.title = collapse(mi.title);
            mi.titleSort = collapse(propertiesFor(title.attribute(QStringLiteral("id")), refines).value(QStringLiteral("file-as")));
        }
        if (mi.titleSort.isEmpty()) {
            for (const QDomElement &meta : metadata) {
                if (isOpf(meta, QStringLiteral("meta")) && meta.attribute(QStringLiteral("name")) == QLatin1String("calibre:title_sort")
                    && !meta.attribute(QStringLiteral("content")).isEmpty()) {
                    mi.titleSort = collapse(meta.attribute(QStringLiteral("content")));
                    break;
                }
            }
        }
        for (const QDomElement &language : metadata) {
            if (!isDc(language, QStringLiteral("language")))
                continue;
            const QString code = CalibreText::canonicalizeLanguage(text(language).trimmed());
            if (!code.isEmpty() && !mi.languages.contains(code) && code != QLatin1String("und"))
                mi.languages.append(code);
        }

        // read_authors(): the creators with role "aut", else those with no
        // role, else the editors, by display-seq.
        struct Author
        {
            QString name;
            QString sort;
            int seq = 0;
            bool operator==(const Author &) const = default;
        };
        QList<Author> roled, unroled, editors;
        for (const QDomElement &creator : metadata) {
            if (!isDc(creator, QStringLiteral("creator")))
                continue;
            const QString value = text(creator).trimmed();
            if (value.isEmpty())
                continue;
            const auto properties = propertiesWithScheme(creator.attribute(QStringLiteral("id")), prefixes, refines);
            const auto author = [&] {
                QString sort;
                if (properties.contains(QStringLiteral("file-as")))
                    sort = properties.value(QStringLiteral("file-as")).first().value;
                else
                    sort = opfAttribute(creator, QStringLiteral("file-as"));
                int seq = 0;
                if (properties.contains(QStringLiteral("display-seq")))
                    seq = properties.value(QStringLiteral("display-seq")).first().value.toInt();
                return Author{ collapse(value), collapse(sort), seq };
            };
            const QString role = opfAttribute(creator, QStringLiteral("role"));
            if (properties.contains(QStringLiteral("role"))) {
                if (isRelatorsRole(properties, QStringLiteral("aut")))
                    roled.append(author());
                if (isRelatorsRole(properties, QStringLiteral("edt"))) {
                    const Author editor = author();
                    editors.removeIf([&](const Author &a) { return a.name == editor.name; });
                    editors.append(editor);
                }
            } else if (!role.isEmpty()) {
                if (role.toLower() == QLatin1String("aut"))
                    roled.append(author());
            } else {
                unroled.append(author());
            }
        }
        QList<Author> authors = !roled.isEmpty() ? roled : !unroled.isEmpty() ? unroled : editors;
        QList<Author> unique;
        for (const Author &author : std::as_const(authors)) {
            if (!unique.contains(author))
                unique.append(author);
        }
        std::stable_sort(unique.begin(), unique.end(), [](const Author &a, const Author &b) { return a.seq < b.seq; });
        QStringList sorts;
        for (const Author &author : std::as_const(unique)) {
            mi.authors.append(author.name);
            sorts.append(author.sort);
        }
        mi.authorSort = CalibreText::authorsToString(sorts);

        for (const QDomElement &contributor : metadata) {
            if (!isDc(contributor, QStringLiteral("contributor")) || text(contributor).trimmed().isEmpty())
                continue;
            const auto properties = propertiesWithScheme(contributor.attribute(QStringLiteral("id")), prefixes, refines);
            const bool producer = properties.contains(QStringLiteral("role"))
                ? isRelatorsRole(properties, QStringLiteral("bkp"))
                : opfAttribute(contributor, QStringLiteral("role")).toLower() == QLatin1String("bkp");
            if (producer) {
                mi.bookProducer = collapse(text(contributor));
                break;
            }
        }
        // read_pubdate(): the first dc:date that parses, in UTC, a date
        // alone moved off the month's edges.
        for (const QDomElement &date : metadata) {
            if (!isDc(date, QStringLiteral("date")) || text(date).trimmed().isEmpty())
                continue;
            bool hasTime = false;
            std::optional<CalibreDate> parsed = parseIsoDate(text(date).trimmed(), true, &hasTime);
            if (!parsed)
                continue;
            if (!hasTime && parsed->dateTime.time() == QTime(0, 0))
                parsed->dateTime = fixOnlyDate(parsed->dateTime);
            if (parsed->dateTime.date().year() > 101)
                mi.pubdate = parsed;
            break;
        }
        const QString calibreTimestamp = calibrePrefix + QStringLiteral(":timestamp");
        const QString calibreRating = calibrePrefix + QStringLiteral(":rating");
        for (const QDomElement &meta : metadata) {
            if (!isOpf(meta, QStringLiteral("meta")) || !hasAttribute(meta, QStringLiteral("property")))
                continue;
            const QString property = expandPrefix(meta.attribute(QStringLiteral("property")), prefixes).toLower();
            const QString value = text(meta).trimmed();
            if (value.isEmpty())
                continue;
            if (property == calibreTimestamp && !mi.timestamp)
                mi.timestamp = parseIsoDate(value, true, nullptr);
            else if (property == calibreRating && !mi.rating)
                mi.rating = toNumber(value);
        }
        for (const QDomElement &meta : metadata) {
            if (!isOpf(meta, QStringLiteral("meta")))
                continue;
            const QString name = meta.attribute(QStringLiteral("name"));
            const QString content = meta.attribute(QStringLiteral("content"));
            if (content.isEmpty())
                continue;
            if (name == QLatin1String("calibre:timestamp") && !mi.timestamp)
                mi.timestamp = parseIsoDate(content, true, nullptr);
            else if (name == QLatin1String("calibre:rating") && !mi.rating)
                mi.rating = toNumber(content);
        }
        QStringList comments;
        for (const QDomElement &description : metadata) {
            if (isDc(description, QStringLiteral("description")) && !text(description).isEmpty())
                comments.append(text(description).trimmed());
        }
        mi.comments = comments.join(u'\n').trimmed();
        for (const QDomElement &publisher : metadata) {
            if (isDc(publisher, QStringLiteral("publisher")) && !text(publisher).isEmpty()) {
                mi.publisher = collapse(text(publisher));
                break;
            }
        }
        for (const QDomElement &subject : metadata) {
            if (!isDc(subject, QStringLiteral("subject")) || text(subject).isEmpty())
                continue;
            for (const QString &tag : text(subject).split(u',')) {
                const QString cleaned = collapse(tag);
                if (!cleaned.isEmpty() && !mi.tags.contains(cleaned))
                    mi.tags.append(cleaned);
            }
        }
        // read_series(): a collection of type series, else calibre:series.
        bool foundSeries = false;
        for (const QDomElement &meta : metadata) {
            if (!isOpf(meta, QStringLiteral("meta")) || meta.attribute(QStringLiteral("property")) != QLatin1String("belongs-to-collection")
                || meta.attribute(QStringLiteral("id")).isEmpty() || text(meta).trimmed().isEmpty())
                continue;
            const auto properties = propertiesFor(meta.attribute(QStringLiteral("id")), refines);
            if (properties.value(QStringLiteral("collection-type")) == QLatin1String("series")) {
                mi.series = collapse(text(meta));
                mi.seriesIndex = toNumber(properties.value(QStringLiteral("group-position"))).value_or(1.0);
                foundSeries = true;
                break;
            }
        }
        if (!foundSeries) {
            double index = 1.0;
            for (const QDomElement &meta : metadata) {
                if (isOpf(meta, QStringLiteral("meta")) && meta.attribute(QStringLiteral("name")) == QLatin1String("calibre:series_index")
                    && hasAttribute(meta, QStringLiteral("content"))) {
                    if (const std::optional<double> value = toNumber(meta.attribute(QStringLiteral("content")))) {
                        index = *value;
                        break;
                    }
                }
            }
            for (const QDomElement &meta : metadata) {
                if (isOpf(meta, QStringLiteral("meta")) && meta.attribute(QStringLiteral("name")) == QLatin1String("calibre:series")) {
                    const QString series = collapse(meta.attribute(QStringLiteral("content")));
                    if (!series.isEmpty()) {
                        mi.series = series;
                        mi.seriesIndex = index;
                        break;
                    }
                }
            }
        }
        // read_raster_cover()
        for (const QDomElement &item : std::as_const(manifestItems)) {
            const QStringList properties = item.attribute(QStringLiteral("properties")).toLower().split(u' ', Qt::SkipEmptyParts);
            if (properties.contains(QStringLiteral("cover-image")) && !imageHref(item).isEmpty()) {
                result.rasterCover = imageHref(item);
                break;
            }
        }
        if (result.rasterCover.isEmpty()) {
            for (const QDomElement &meta : metadata) {
                if (isOpf(meta, QStringLiteral("meta")) && meta.attribute(QStringLiteral("name")) == QLatin1String("cover")) {
                    const QDomElement item = manifestById.value(meta.attribute(QStringLiteral("content")));
                    if (!item.isNull() && item.hasAttribute(QStringLiteral("href")) && item.hasAttribute(QStringLiteral("media-type"))
                        && !imageHref(item).isEmpty()) {
                        result.rasterCover = imageHref(item);
                        break;
                    }
                }
            }
        }
        if (result.rasterCover.isEmpty() && !spineIds.isEmpty()) {
            const QDomElement item = manifestById.value(spineIds.first());
            const QString type = item.attribute(QStringLiteral("media-type"));
            if (!item.isNull() && (type == QLatin1String("image/jpeg") || type == QLatin1String("image/webp")
                                   || type == QLatin1String("image/png")))
                result.rasterCover = imageHref(item);
        }
    } else {
        // OPF 2: Calibre's OPF class, lenient about names and namespaces.
        const QList<QDomElement> metadata = descendants(metadataElement);
        const auto nameContains = [](const QDomElement &element, const QString &part) {
            return qualifiedName(element).contains(part, Qt::CaseInsensitive);
        };
        const auto content = [](const QDomElement &element) {
            // get_text(): the content attribute, else the text.
            const QString value = element.attribute(QStringLiteral("content"));
            return value.isEmpty() ? text(element) : value;
        };
        // MetadataField: the last element named <name> or meta calibre:<name>.
        const auto field = [&](const QString &name) -> std::optional<QString> {
            std::optional<QString> found;
            for (const QDomElement &element : metadata) {
                const QString qualified = qualifiedName(element);
                const bool byName = qualified.endsWith(name, Qt::CaseInsensitive);
                const bool byMeta = qualified.endsWith(QLatin1String("meta"), Qt::CaseInsensitive)
                    && element.attribute(QStringLiteral("name")).compare(QStringLiteral("calibre:") + name, Qt::CaseInsensitive) == 0;
                if (byName || byMeta)
                    found = pythonStrip(content(element));
            }
            return found;
        };

        for (const QDomElement &title : metadata) {
            if (nameContains(title, QStringLiteral("title")) && !content(title).trimmed().isEmpty()) {
                mi.title = collapse(content(title));
                break;
            }
        }
        const auto isAuthor = [](const QDomElement &element) {
            if (!qualifiedName(element).contains(QLatin1String("creator"), Qt::CaseInsensitive))
                return false;
            const QString role = element.attribute(QStringLiteral("role"));
            const QString opfRole = element.attributeNS(opfNamespace, QStringLiteral("role"));
            return role == QLatin1String("aut") || opfRole == QLatin1String("aut")
                || (!element.hasAttribute(QStringLiteral("role")) && opfRole.isEmpty()
                    && !element.hasAttributeNS(opfNamespace, QStringLiteral("role")));
        };
        const auto isEditor = [](const QDomElement &element) {
            return qualifiedName(element).contains(QLatin1String("creator"), Qt::CaseInsensitive)
                && (element.attribute(QStringLiteral("role")) == QLatin1String("edt")
                    || element.attributeNS(opfNamespace, QStringLiteral("role")) == QLatin1String("edt"));
        };
        QList<QDomElement> creators;
        for (const QDomElement &element : metadata) {
            if (isAuthor(element))
                creators.append(element);
        }
        if (creators.isEmpty()) {
            for (const QDomElement &element : metadata) {
                if (isEditor(element))
                    creators.append(element);
            }
        }
        for (const QDomElement &creator : std::as_const(creators))
            mi.authors += CalibreText::stringToAuthors(content(creator));
        for (const QDomElement &creator : std::as_const(creators)) {
            const QString sort = !creator.attributeNS(opfNamespace, QStringLiteral("file-as")).isEmpty()
                ? creator.attributeNS(opfNamespace, QStringLiteral("file-as")) : creator.attribute(QStringLiteral("file-as"));
            if (!sort.isEmpty()) {
                mi.authorSort = sort;
                break;
            }
        }
        // TitleSortField: calibre:title_sort, else a title's file-as.
        mi.titleSort = field(QStringLiteral("title_sort")).value_or(QString());
        if (mi.titleSort.isEmpty()) {
            for (const QDomElement &title : metadata) {
                if (!nameContains(title, QStringLiteral("title")))
                    continue;
                QString sort = title.attributeNS(opfNamespace, QStringLiteral("file-as"));
                if (sort.isEmpty())
                    sort = title.attribute(QStringLiteral("file-as"));
                if (!sort.isEmpty())
                    mi.titleSort = sort;
            }
            mi.titleSort = mi.titleSort.trimmed();
        }
        mi.publisher = field(QStringLiteral("publisher")).value_or(QString());
        mi.comments = field(QStringLiteral("description")).value_or(QString());
        mi.series = field(QStringLiteral("series")).value_or(QString());
        mi.seriesIndex = toNumber(field(QStringLiteral("series_index")).value_or(QString())).value_or(1.0);
        mi.rating = toNumber(field(QStringLiteral("rating")).value_or(QString()));
        if (const std::optional<QString> timestamp = field(QStringLiteral("timestamp")))
            mi.timestamp = parseIsoDate(*timestamp, false, nullptr);
        // The earliest of the dates.
        for (const QDomElement &date : metadata) {
            if (!nameContains(date, QStringLiteral("date")))
                continue;
            const std::optional<CalibreDate> parsed = parseIsoDate(pythonStrip(text(date)), false, nullptr);
            if (parsed && (!mi.pubdate || *parsed < *mi.pubdate))
                mi.pubdate = parsed;
        }
        for (const QDomElement &language : metadata) {
            if (localName(language) != QLatin1String("language") || text(language).trimmed().isEmpty())
                continue;
            const QString code = CalibreText::canonicalizeLanguage(text(language).trimmed());
            if (!code.isEmpty())
                mi.languages.append(code);
        }
        for (const QDomElement &subject : metadata) {
            if (!nameContains(subject, QStringLiteral("subject")) || content(subject).trimmed().isEmpty())
                continue;
            for (const QString &tag : content(subject).split(u','))
                mi.tags.append(tag.trimmed());
        }
        // get_identifiers()
        QStringList schemeless;
        for (const QDomElement &identifier : metadata) {
            if (localName(identifier) != QLatin1String("identifier") || text(identifier).isEmpty())
                continue;
            QString scheme;
            bool found = false;
            const QDomNamedNodeMap attributes = identifier.attributes();
            for (int index = 0; index < attributes.count(); ++index) {
                const QDomAttr attr = attributes.item(index).toAttr();
                if (attr.name().endsWith(QLatin1String("scheme"))) {
                    scheme = attr.value().toLower();
                    found = true;
                    break;
                }
            }
            QString value = text(identifier).trimmed();
            if (found) {
                if (scheme == QLatin1String("uuid")) {
                    mi.uuid = value;
                } else if (!value.isEmpty() && scheme != QLatin1String("calibre")) {
                    if (scheme == QLatin1String("isbn") && value.toLower().startsWith(QLatin1String("urn:isbn:")))
                        value = value.mid(9);
                    mi.setIdentifier(scheme, value);
                }
            } else if (value.toLower().startsWith(QLatin1String("urn:isbn:"))) {
                const QString isbn = CalibreText::checkIsbn(value.section(u':', -1));
                if (!isbn.isEmpty())
                    mi.setIdentifier(QStringLiteral("isbn"), isbn);
            } else {
                schemeless.append(value);
            }
        }
        if (std::none_of(mi.identifiers.cbegin(), mi.identifiers.cend(), [](const auto &id) { return id.first == QLatin1String("isbn"); })) {
            for (const QString &value : std::as_const(schemeless)) {
                if (!CalibreText::checkIsbn(value, true).isEmpty()) {
                    mi.setIdentifier(QStringLiteral("isbn"), CalibreText::checkIsbn(value));
                    break;
                }
            }
        }
        for (const QDomElement &contributor : metadata) {
            if (nameContains(contributor, QStringLiteral("contributor"))
                && (contributor.attribute(QStringLiteral("role")) == QLatin1String("bkp")
                    || contributor.attributeNS(opfNamespace, QStringLiteral("role")) == QLatin1String("bkp"))) {
                mi.bookProducer = content(contributor).trimmed();
                break;
            }
        }
        // raster_cover: the manifest item a meta name="cover" names.
        for (const QDomElement &meta : metadata) {
            if (!qualifiedName(meta).contains(QLatin1String("meta"), Qt::CaseInsensitive)
                || !meta.attribute(QStringLiteral("name")).contains(QLatin1String("cover"), Qt::CaseInsensitive)
                || !meta.hasAttribute(QStringLiteral("content")))
                continue;
            const QString id = meta.attribute(QStringLiteral("content"));
            for (const QDomElement &item : std::as_const(manifestItems)) {
                if (item.attribute(QStringLiteral("id")) == id && !imageHref(item).isEmpty()) {
                    result.rasterCover = imageHref(item);
                    break;
                }
            }
            if (result.rasterCover.isEmpty()) {
                for (const QDomElement &item : std::as_const(manifestItems)) {
                    if (item.attribute(QStringLiteral("href")) == id && !imageHref(item).isEmpty()) {
                        result.rasterCover = imageHref(item);
                        break;
                    }
                }
            }
            break;
        }
    }
    *package = result;
    return true;
}

} // namespace OpfReader
