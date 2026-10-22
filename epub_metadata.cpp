#include "epub_metadata.h"

// Ported from Calibre 9.16, Copyright (C) Kovid Goyal, GPL v3:
// - src/calibre/ebooks/metadata/epub.py: set_metadata(),
//   serialize_cover_data();
// - src/calibre/ebooks/metadata/opf.py: set_metadata(),
//   set_metadata_opf3();
// - src/calibre/ebooks/metadata/opf3.py: apply_metadata() and the readers
//   and writers it calls, the prefixes and refines;
// - src/calibre/ebooks/metadata/utils.py: normalize_languages(),
//   ensure_unique(), create_manifest_item(), pretty_print_opf();
// - src/calibre/ebooks/oeb/polish/pretty.py: pretty_opf(),
//   pretty_xml_tree();
// - src/calibre/spell/__init__.py: parse_lang_code();
// - src/calibre/utils/img.py: save_cover_data_to(), as serialize_cover_data()
//   calls it;
// - src/calibre/utils/zipfile.py: safe_replace().

#include <QBuffer>
#include <QCollator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "book.h"
#include "book_zip.h"
#include "calibre_db.h"
#include "calibre_languages.h"
#include "calibre_text.h"
#include "opf.h"
#include "xml_tree.h"

using PythonJson::Value;
using XmlTree::Node;

namespace EpubMetadata {

namespace {

const QString opfNamespace = QStringLiteral("http://www.idpf.org/2007/opf");
const QString dcNamespace = QStringLiteral("http://purl.org/dc/elements/1.1/");
const QString calibrePrefix = QStringLiteral("https://calibre-ebook.com");

// --- The tree as lxml's elements -------------------------------------------

bool isText(const Node *node)
{
    return node->kind == Node::Kind::Text || node->kind == Node::Kind::Cdata;
}

bool isElement(const Node *node, const QString &namespaceUri, const char *localName)
{
    return node->kind == Node::Kind::Element && node->namespaceUri == namespaceUri
        && node->localName == QLatin1String(localName);
}

// An element's children as lxml counts them: elements, comments and
// instructions, text aside.
QList<Node *> kids(Node *parent)
{
    QList<Node *> result;
    for (const auto &child : parent->children) {
        if (!isText(child.get()))
            result.append(child.get());
    }
    return result;
}

QList<Node *> kids(Node *parent, const QString &namespaceUri, const char *localName)
{
    QList<Node *> result;
    for (const auto &child : parent->children) {
        if (isElement(child.get(), namespaceUri, localName))
            result.append(child.get());
    }
    return result;
}

// elem.text: the text before its first child.
QString textOf(const Node *element)
{
    QString text;
    for (const auto &child : element->children) {
        if (!isText(child.get()))
            break;
        text += child->text;
    }
    return text;
}

void setText(Node *element, const QString &text)
{
    while (!element->children.empty() && isText(element->children.front().get()))
        element->take(0);
    if (!text.isEmpty())
        element->insert(0, XmlTree::textNode(text));
}

// elem.tail: the text after it, up to its parent's next child.
QString tailOf(const Node *node)
{
    const Node *parent = node->parent;
    QString text;
    for (size_t i = parent->indexOf(node) + 1; i < parent->children.size(); ++i) {
        if (!isText(parent->children[i].get()))
            break;
        text += parent->children[i]->text;
    }
    return text;
}

void setTail(Node *node, const QString &text)
{
    Node *parent = node->parent;
    const size_t at = parent->indexOf(node) + 1;
    while (at < parent->children.size() && isText(parent->children[at].get()))
        parent->take(at);
    if (!text.isEmpty())
        parent->insert(at, XmlTree::textNode(text));
}

// parent.remove(child): the child goes with its tail.
std::vector<std::unique_ptr<Node>> takeWithTail(Node *node)
{
    Node *parent = node->parent;
    const size_t at = parent->indexOf(node);
    std::vector<std::unique_ptr<Node>> taken;
    taken.push_back(parent->take(at));
    while (at < parent->children.size() && isText(parent->children[at].get()))
        taken.push_back(parent->take(at));
    return taken;
}

void removeNode(Node *node)
{
    if (node->parent)
        takeWithTail(node);
}

// parent.insert(index, nodes): before its index-th child, as lxml counts.
void insertAt(Node *parent, qsizetype index, std::vector<std::unique_ptr<Node>> nodes)
{
    const QList<Node *> children = kids(parent);
    size_t at = index < children.size() ? parent->indexOf(children.at(index)) : parent->children.size();
    for (auto &node : nodes)
        parent->insert(at++, std::move(node));
}

Node *insertElement(Node *parent, qsizetype index, std::unique_ptr<Node> element)
{
    Node *raw = element.get();
    std::vector<std::unique_ptr<Node>> nodes;
    nodes.push_back(std::move(element));
    insertAt(parent, index, std::move(nodes));
    return raw;
}

Node *appendElement(Node *parent, std::unique_ptr<Node> element)
{
    return insertElement(parent, kids(parent).size(), std::move(element));
}

qsizetype indexOf(Node *parent, const Node *child)
{
    return kids(parent).indexOf(const_cast<Node *>(child));
}

// The prefix `namespaceUri` has where `node` is, "" for the default one.
std::optional<QString> prefixFor(const Node *node, const QString &namespaceUri)
{
    for (const Node *at = node; at; at = at->parent) {
        for (const QXmlStreamNamespaceDeclaration &declaration : at->namespaces) {
            if (declaration.namespaceUri() == namespaceUri)
                return declaration.prefix().toString();
        }
    }
    return std::nullopt;
}

std::optional<QString> namespaceFor(const Node *node, const QString &prefix)
{
    for (const Node *at = node; at; at = at->parent) {
        for (const QXmlStreamNamespaceDeclaration &declaration : at->namespaces) {
            if (declaration.prefix() == prefix)
                return declaration.namespaceUri().toString();
        }
    }
    return std::nullopt;
}

// makeelement(): an element of `namespaceUri` under `parent`, with the
// prefix that namespace has there.
std::unique_ptr<Node> makeElement(const Node *parent, const QString &namespaceUri, const QString &localName,
                                  const QList<QPair<QString, QString>> &attributes = {})
{
    auto element = std::make_unique<Node>();
    element->localName = localName;
    element->namespaceUri = namespaceUri;
    const std::optional<QString> prefix = prefixFor(parent, namespaceUri);
    if (prefix) {
        element->qualifiedName = prefix->isEmpty() ? localName : *prefix + u':' + localName;
    } else {
        const QString chosen = namespaceUri == dcNamespace ? QStringLiteral("dc") : QStringLiteral("opf");
        element->qualifiedName = chosen + u':' + localName;
        element->namespaces.append(QXmlStreamNamespaceDeclaration(chosen, namespaceUri));
    }
    for (const auto &[name, value] : attributes)
        element->setAttribute(name, value);
    return element;
}

// elem.get(OPF('name')): an attribute in OPF's namespace.
QString opfAttribute(const Node *element, const QString &localName)
{
    for (const QXmlStreamAttribute &attribute : element->attributes) {
        const QString name = attribute.qualifiedName().toString();
        const qsizetype colon = name.indexOf(u':');
        if (colon < 0 || name.mid(colon + 1) != localName)
            continue;
        if (namespaceFor(element, name.left(colon)) == opfNamespace)
            return attribute.value().toString();
    }
    return {};
}

bool hasAttribute(const Node *element, const QString &name)
{
    for (const QXmlStreamAttribute &attribute : element->attributes) {
        if (attribute.qualifiedName() == name)
            return true;
    }
    return false;
}

void collectAttribute(const Node *node, const QString &name, QSet<QString> *values)
{
    if (node->kind != Node::Kind::Element)
        return;
    if (hasAttribute(node, name))
        values->insert(node->attribute(name));
    for (const auto &child : node->children)
        collectAttribute(child.get(), name, values);
}

QString normalizeWhitespace(const QString &text)
{
    static const QRegularExpression space(QStringLiteral("\\s+"));
    return QString(text).replace(space, QStringLiteral(" ")).trimmed();
}

QStringList uniq(const QStringList &values)
{
    QStringList result;
    for (const QString &value : values) {
        if (!result.contains(value))
            result.append(value);
    }
    return result;
}

// ensure_unique()
QString ensureUnique(const QString &name, const QSet<QString> &existing)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    const QString base = dot > 0 && dot < name.size() - 1 ? name.left(dot) : name;
    const QString extension = dot > 0 && dot < name.size() - 1 ? name.mid(dot) : QString();
    QString candidate = name;
    for (int count = 1; existing.contains(candidate); ++count)
        candidate = QStringLiteral("%1-%2%3").arg(base).arg(count).arg(extension);
    return candidate;
}

// --- Prefixes and refines --------------------------------------------------

using Prefixes = QList<QPair<QString, QString>>;

const Prefixes &reservedPrefixes()
{
    static const Prefixes prefixes{
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

QString prefixValue(const Prefixes &prefixes, const QString &key)
{
    for (const auto &[name, value] : prefixes) {
        if (name == key)
            return value;
    }
    return {};
}

void setPrefix(Prefixes *prefixes, const QString &key, const QString &value)
{
    for (auto &[name, current] : *prefixes) {
        if (name == key) {
            current = value;
            return;
        }
    }
    prefixes->append({ key, value });
}

Prefixes readPrefixes(const Node *package)
{
    Prefixes prefixes = reservedPrefixes();
    static const QRegularExpression pair(QStringLiteral("(\\S+): \\s*(\\S+)"));
    for (auto it = pair.globalMatch(package->attribute(QStringLiteral("prefix"))); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        setPrefix(&prefixes, match.captured(1), match.captured(2));
    }
    return prefixes;
}

QString expandPrefix(const QString &raw, const Prefixes &prefixes)
{
    static const QRegularExpression prefixed(QStringLiteral("(\\S+)\\s*:\\s*(\\S+)"));
    QString result;
    qsizetype last = 0;
    for (auto it = prefixed.globalMatch(raw); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString expanded = prefixValue(prefixes, match.captured(1));
        result += raw.mid(last, match.capturedStart() - last)
            + (expanded.isEmpty() ? match.captured(1) : expanded) + u':' + match.captured(2);
        last = match.capturedEnd();
    }
    return result + raw.mid(last);
}

// ensure_prefix()
void ensurePrefix(Node *package, Prefixes *prefixes, const QString &prefix, const QString &value = {})
{
    setPrefix(prefixes, prefix, value.isEmpty() ? prefixValue(reservedPrefixes(), prefix) : value);
    QStringList declared;
    for (const auto &[name, uri] : std::as_const(*prefixes)) {
        if (prefixValue(reservedPrefixes(), name) != uri)
            declared.append(name + QStringLiteral(": ") + uri);
    }
    if (declared.isEmpty())
        package->removeAttribute(QStringLiteral("prefix"));
    else
        package->setAttribute(QStringLiteral("prefix"), declared.join(u' '));
}

using Refines = QHash<QString, QList<Node *>>;

struct Context
{
    Node *document = nullptr;
    Node *package = nullptr;
    Node *metadata = nullptr;
    Prefixes prefixes;
    Refines refines;
};

Refines readRefines(Node *metadata)
{
    Refines refines;
    for (Node *meta : kids(metadata, opfNamespace, "meta")) {
        const QString target = meta->attribute(QStringLiteral("refines"));
        if (hasAttribute(meta, QStringLiteral("refines")) && target.startsWith(u'#'))
            refines[target.mid(1)].append(meta);
    }
    return refines;
}

void removeRefines(Node *element, Refines *refines)
{
    const QString id = element->attribute(QStringLiteral("id"));
    for (Node *refine : refines->value(id))
        removeNode(refine);
    refines->remove(id);
}

void removeElement(Node *element, Refines *refines)
{
    removeRefines(element, refines);
    removeNode(element);
}

QSet<QString> allValues(const Context &context, const QString &name)
{
    QSet<QString> values;
    for (const auto &child : context.document->children)
        collectAttribute(child.get(), name, &values);
    return values;
}

// ensure_id()
QString ensureId(const Context &context, Node *element)
{
    QString id = element->attribute(QStringLiteral("id"));
    if (id.isEmpty()) {
        id = ensureUnique(QStringLiteral("id"), allValues(context, QStringLiteral("id")));
        element->setAttribute(QStringLiteral("id"), id);
    }
    return id;
}

struct Refine
{
    QString property;
    QString value;
    QString scheme;
};

// set_refines(): the element's refinements replaced, right after it.
void setRefines(Context &context, Node *element, const QList<Refine> &refines)
{
    const QString id = ensureId(context, element);
    removeRefines(element, &context.refines);
    Node *parent = element->parent;
    for (auto it = refines.crbegin(); it != refines.crend(); ++it) {
        auto refine = makeElement(element, opfNamespace, QStringLiteral("meta"),
                                  { { QStringLiteral("refines"), u'#' + id }, { QStringLiteral("property"), it->property } });
        refine->append(XmlTree::textNode(it->value.trimmed()));
        if (!it->scheme.isEmpty())
            refine->setAttribute(QStringLiteral("scheme"), it->scheme);
        insertElement(parent, indexOf(parent, element) + 1, std::move(refine));
    }
}

QHash<QString, QString> propertiesFor(const QString &id, const Refines &refines)
{
    QHash<QString, QString> properties;
    if (id.isEmpty())
        return properties;
    for (Node *refine : refines.value(id)) {
        const QString key = refine->attribute(QStringLiteral("property"));
        const QString value = textOf(refine).trimmed();
        if (!key.isEmpty() && !value.isEmpty())
            properties.insert(key, value);
    }
    return properties;
}

struct Scheme
{
    QString namespaceUri;
    QString scheme;
    QString value;
};

QHash<QString, QList<Scheme>> propertiesWithScheme(const QString &id, const Prefixes &prefixes, const Refines &refines)
{
    QHash<QString, QList<Scheme>> properties;
    if (id.isEmpty())
        return properties;
    for (Node *refine : refines.value(id)) {
        const QString key = refine->attribute(QStringLiteral("property"));
        const QString value = textOf(refine).trimmed();
        if (key.isEmpty() || value.isEmpty())
            continue;
        QString scheme = refine->attribute(QStringLiteral("scheme"));
        QString schemeNamespace;
        const qsizetype colon = scheme.indexOf(u':');
        if (colon > 0 && colon < scheme.size() - 1) {
            const QString uri = prefixValue(prefixes, scheme.left(colon));
            if (!uri.isEmpty()) {
                schemeNamespace = uri;
                scheme = scheme.mid(colon + 1);
            }
        }
        properties[key].append({ schemeNamespace, scheme, value });
    }
    return properties;
}

bool isRelatorsRole(const QHash<QString, QList<Scheme>> &properties, const QString &role)
{
    for (const Scheme &scheme : properties.value(QStringLiteral("role"))) {
        if (scheme.value.toLower() == role
            && (scheme.namespaceUri.isEmpty()
                || (scheme.namespaceUri == prefixValue(reservedPrefixes(), QStringLiteral("marc"))
                    && scheme.scheme == QLatin1String("relators"))))
            return true;
    }
    return false;
}

// --- The fields --------------------------------------------------------------

// parse_identifier()
std::optional<QPair<QString, QString>> parseIdentifier(const Node *identifier, QString value)
{
    const auto finalize = [](QString scheme, QString value, bool fromValue) -> std::optional<QPair<QString, QString>> {
        if (scheme.isEmpty() || value.isEmpty())
            return std::nullopt;
        scheme = scheme.toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
            if (fromValue)
                return QPair<QString, QString>(QStringLiteral("url"), scheme + u':' + value);
            return std::nullopt;
        }
        if (scheme.startsWith(QLatin1String("isbn")))
            scheme = QStringLiteral("isbn");
        if (scheme == QLatin1String("isbn")) {
            value = CalibreText::checkIsbn(value.section(u':', -1));
            if (value.isEmpty())
                return std::nullopt;
        }
        return QPair<QString, QString>(scheme, value);
    };
    const QString lower = value.toLower();
    const QString scheme = opfAttribute(identifier, QStringLiteral("scheme"));
    if (!scheme.isEmpty() && !lower.startsWith(QLatin1String("urn:")))
        return finalize(scheme, value, false);
    if (lower.startsWith(QLatin1String("urn:")))
        value = value.mid(4);
    const qsizetype colon = value.indexOf(u':');
    return colon < 0 ? finalize(value, QString(), true) : finalize(value.left(colon), value.mid(colon + 1), true);
}

void insertIdentifier(Context &context, Node *packageIdentifier, const QString &text)
{
    auto identifier = makeElement(context.metadata, dcNamespace, QStringLiteral("identifier"));
    identifier->append(XmlTree::textNode(text));
    if (!packageIdentifier) {
        appendElement(context.metadata, std::move(identifier));
    } else {
        Node *parent = packageIdentifier->parent;
        insertElement(parent, indexOf(parent, packageIdentifier), std::move(identifier));
    }
}

// set_identifiers()
void setIdentifiers(Context &context, const QList<QPair<QString, QString>> &identifiers)
{
    const bool hasUid = hasAttribute(context.package, QStringLiteral("unique-identifier"));
    const QString uid = context.package->attribute(QStringLiteral("unique-identifier"));
    QSet<QString> schemes;
    for (const auto &[scheme, value] : identifiers)
        schemes.insert(scheme);
    Node *packageIdentifier = nullptr;
    for (Node *identifier : kids(context.metadata, dcNamespace, "identifier")) {
        if (hasUid && uid == identifier->attribute(QStringLiteral("id")) && hasAttribute(identifier, QStringLiteral("id"))) {
            packageIdentifier = identifier;
            continue;
        }
        const QString value = textOf(identifier).trimmed();
        if (value.isEmpty()) {
            removeNode(identifier);
            continue;
        }
        const auto parsed = parseIdentifier(identifier, value);
        if (!parsed || schemes.contains(parsed->first))
            removeElement(identifier, &context.refines);
    }
    for (const auto &[scheme, value] : identifiers)
        insertIdentifier(context, packageIdentifier, scheme + u':' + value);
}

// identifier_writer(): calibre:ID, uuid:UUID.
void writeIdentifier(Context &context, const QString &name, const QString &value)
{
    const bool hasUid = hasAttribute(context.package, QStringLiteral("unique-identifier"));
    const QString uid = context.package->attribute(QStringLiteral("unique-identifier"));
    Node *packageIdentifier = nullptr;
    for (Node *identifier : kids(context.metadata, dcNamespace, "identifier")) {
        const bool isPackageId
            = hasUid && hasAttribute(identifier, QStringLiteral("id")) && uid == identifier->attribute(QStringLiteral("id"));
        if (isPackageId)
            packageIdentifier = identifier;
        const QString text = textOf(identifier).trimmed();
        if ((text.startsWith(name + u':') || opfAttribute(identifier, QStringLiteral("scheme")) == name) && !isPackageId)
            removeElement(identifier, &context.refines);
    }
    if (!value.isEmpty())
        insertIdentifier(context, packageIdentifier, name + u':' + value);
}

// find_main_title()
Node *findMainTitle(Context &context, bool removeBlanks)
{
    Node *firstTitle = nullptr;
    for (Node *title : kids(context.metadata, dcNamespace, "title")) {
        if (textOf(title).trimmed().isEmpty()) {
            if (removeBlanks)
                removeElement(title, &context.refines);
            continue;
        }
        if (!firstTitle)
            firstTitle = title;
        if (propertiesFor(title->attribute(QStringLiteral("id")), context.refines).value(QStringLiteral("title-type"))
            == QLatin1String("main"))
            return title;
    }
    return firstTitle;
}

Node *findSubtitle(Context &context)
{
    for (Node *title : kids(context.metadata, dcNamespace, "title")) {
        if (textOf(title).trimmed().isEmpty())
            continue;
        const QString type
            = propertiesFor(title->attribute(QStringLiteral("id")), context.refines).value(QStringLiteral("title-type"));
        if (type.contains(QLatin1String("subtitle")) || type.contains(QLatin1String("sub-title")))
            return title;
    }
    return nullptr;
}

QList<Node *> metasNamed(Context &context, const QString &name)
{
    QList<Node *> found;
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        if (hasAttribute(meta, QStringLiteral("name")) && meta->attribute(QStringLiteral("name")) == name)
            found.append(meta);
    }
    return found;
}

QList<Node *> metasWithProperty(Context &context, const QString &expanded)
{
    QList<Node *> found;
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        if (hasAttribute(meta, QStringLiteral("property"))
            && expandPrefix(meta->attribute(QStringLiteral("property")), context.prefixes).toLower() == expanded)
            found.append(meta);
    }
    return found;
}

// set_title()
void setTitle(Context &context, const QString &title, const QString &titleSort)
{
    Node *mainTitle = findMainTitle(context, true);
    if (Node *subtitle = findSubtitle(context)) {
        if (subtitle == mainTitle)
            mainTitle = nullptr;
        removeElement(subtitle, &context.refines);
    }
    if (!mainTitle)
        mainTitle = insertElement(context.metadata, 0, makeElement(context.metadata, dcNamespace, QStringLiteral("title")));
    setText(mainTitle, title);
    QList<Refine> refines{ { QStringLiteral("title-type"), QStringLiteral("main"), {} } };
    if (!titleSort.isEmpty())
        refines.append({ QStringLiteral("file-as"), titleSort, {} });
    setRefines(context, mainTitle, refines);
    for (Node *meta : metasNamed(context, QStringLiteral("calibre:title_sort")))
        removeElement(meta, &context.refines);
}

// parse_lang_code(): a language's canonical code and its country's.
std::optional<QPair<QString, QString>> parseLanguageCode(const QString &raw)
{
    QStringList parts = QString(raw).replace(u'_', u'-').split(u'-');
    const std::optional<QString> language = CalibreLanguages::canonicalize(parts.value(0));
    if (!language)
        return std::nullopt;
    parts.removeAll(QStringLiteral("Cyrl"));
    parts.removeAll(QStringLiteral("Latn"));
    QString country;
    if (parts.size() > 1) {
        const QString code = parts.at(1).toUpper();
        const QLocale::Territory territory = QLocale::codeToTerritory(code);
        if (territory != QLocale::AnyTerritory)
            country = QLocale::territoryToCode(territory);
    }
    return QPair<QString, QString>(*language, country);
}

// normalize_languages(): the library's languages, two letters where they
// have them, with the country the book gave each.
QStringList normalizeLanguages(const QStringList &bookLanguages, const QStringList &languages)
{
    QHash<QString, QString> countries;
    for (const QString &language : bookLanguages) {
        if (const auto parsed = parseLanguageCode(language))
            countries.insert(parsed->first, parsed->second);
    }
    QStringList result;
    for (const QString &language : languages) {
        const auto parsed = parseLanguageCode(language);
        if (!parsed)
            continue;
        const QString country = parsed->second.isEmpty() ? countries.value(parsed->first) : parsed->second;
        const QString shortCode = CalibreText::languageAsIso6391(parsed->first);
        QString code = shortCode.isEmpty() ? parsed->first : shortCode;
        if (!country.isEmpty())
            code += u'-' + country;
        result.append(code);
    }
    return result;
}

// set_languages()
void setLanguages(Context &context, const QStringList &languages)
{
    QStringList bookLanguages;
    for (Node *language : kids(context.metadata, dcNamespace, "language")) {
        const QString value = textOf(language).trimmed();
        removeElement(language, &context.refines);
        if (!value.isEmpty())
            bookLanguages.append(value);
    }
    QStringList normalized;
    for (const QString &language : normalizeLanguages(bookLanguages, languages)) {
        if (!language.isEmpty() && language != QLatin1String("und"))
            normalized.append(language);
    }
    if (normalized.isEmpty())
        normalized = { QStringLiteral("und") };
    for (const QString &language : uniq(normalized)) {
        auto element = makeElement(context.metadata, dcNamespace, QStringLiteral("language"));
        element->append(XmlTree::textNode(language));
        appendElement(context.metadata, std::move(element));
    }
}

// set_authors()
void setAuthors(Context &context, const QStringList &authors, const QString &authorSort)
{
    ensurePrefix(context.package, &context.prefixes, QStringLiteral("marc"));
    QList<Node *> removals;
    for (const QString role : { QStringLiteral("aut"), QStringLiteral("edt") }) {
        for (Node *creator : kids(context.metadata, dcNamespace, "creator")) {
            const auto properties
                = propertiesWithScheme(creator->attribute(QStringLiteral("id")), context.prefixes, context.refines);
            const QString opfRole = opfAttribute(creator, QStringLiteral("role"));
            if ((!opfRole.isEmpty() && opfRole.toLower() != role)
                || (!properties.value(QStringLiteral("role")).isEmpty() && !isRelatorsRole(properties, role)))
                continue;
            removals.append(creator);
        }
        if (!removals.isEmpty())
            break;
    }
    for (Node *creator : std::as_const(removals))
        removeElement(creator, &context.refines);
    const QStringList sorts = CalibreText::stringToAuthors(authorSort);
    for (qsizetype i = 0; i < authors.size(); ++i) {
        if (authors.at(i).isEmpty())
            continue;
        auto creator = makeElement(context.metadata, dcNamespace, QStringLiteral("creator"));
        // The new element counts among the document's ids, as lxml's does.
        const QString id = ensureUnique(QStringLiteral("id"), allValues(context, QStringLiteral("id")));
        creator->setAttribute(QStringLiteral("id"), id);
        creator->append(XmlTree::textNode(authors.at(i)));
        appendElement(context.metadata, std::move(creator));
        auto role = makeElement(context.metadata, opfNamespace, QStringLiteral("meta"),
                                { { QStringLiteral("refines"), u'#' + id },
                                  { QStringLiteral("property"), QStringLiteral("role") },
                                  { QStringLiteral("scheme"), QStringLiteral("marc:relators") } });
        role->append(XmlTree::textNode(QStringLiteral("aut")));
        appendElement(context.metadata, std::move(role));
        const QString sort = sorts.value(i);
        if (!sort.isEmpty()) {
            auto fileAs = makeElement(context.metadata, opfNamespace, QStringLiteral("meta"),
                                      { { QStringLiteral("refines"), u'#' + id },
                                        { QStringLiteral("property"), QStringLiteral("file-as") } });
            fileAs->append(XmlTree::textNode(sort));
            appendElement(context.metadata, std::move(fileAs));
        }
    }
}

void appendDc(Context &context, const QString &localName, const QString &text)
{
    auto element = makeElement(context.metadata, dcNamespace, localName);
    element->append(XmlTree::textNode(text));
    appendElement(context.metadata, std::move(element));
}

void removeDc(Context &context, const char *localName)
{
    for (Node *element : kids(context.metadata, dcNamespace, localName))
        removeElement(element, &context.refines);
}

// set_rating()
void setRating(Context &context, int rating)
{
    for (Node *meta : metasNamed(context, QStringLiteral("calibre:rating")))
        removeElement(meta, &context.refines);
    for (Node *meta : metasWithProperty(context, calibrePrefix + QStringLiteral(":rating")))
        removeElement(meta, &context.refines);
    if (rating) {
        ensurePrefix(context.package, &context.prefixes, QStringLiteral("calibre"), calibrePrefix);
        auto meta = makeElement(context.metadata, opfNamespace, QStringLiteral("meta"),
                                { { QStringLiteral("property"), QStringLiteral("calibre:rating") } });
        // f'{float(val):.2g}' of a whole rating.
        meta->append(XmlTree::textNode(QString::number(rating)));
        appendElement(context.metadata, std::move(meta));
    }
}

// set_series()
void setSeries(Context &context, const QString &series, double index)
{
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        const QString name = meta->attribute(QStringLiteral("name"));
        if (name == QLatin1String("calibre:series") || name == QLatin1String("calibre:series_index"))
            removeElement(meta, &context.refines);
    }
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        if (meta->attribute(QStringLiteral("property")) == QLatin1String("belongs-to-collection"))
            removeElement(meta, &context.refines);
    }
    if (series.isEmpty())
        return;
    auto meta = makeElement(context.metadata, opfNamespace, QStringLiteral("meta"),
                            { { QStringLiteral("property"), QStringLiteral("belongs-to-collection") } });
    meta->append(XmlTree::textNode(series));
    Node *collection = appendElement(context.metadata, std::move(meta));
    setRefines(context, collection,
               { { QStringLiteral("collection-type"), QStringLiteral("series"), {} },
                 { QStringLiteral("group-position"), CalibreText::seriesIndexText(index), {} } });
}

// read_user_metadata(): the custom columns the book carries, by key.
QList<QPair<QString, Value>> readUserMetadata(Context &context)
{
    QList<QPair<QString, Value>> result;
    for (Node *meta : metasWithProperty(context, calibrePrefix + QStringLiteral(":user_metadata"))) {
        const Value value = PythonJson::parse(textOf(meta).trimmed().toUtf8());
        if (value.kind == Value::Kind::Dict && !value.fields.isEmpty()) {
            for (const auto &field : value.fields)
                result.append(field);
            return result;
        }
    }
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        const QString name = meta->attribute(QStringLiteral("name"));
        if (!name.startsWith(QLatin1String("calibre:user_metadata:")))
            continue;
        const QString key = name.section(u':', 2);
        if (!key.startsWith(u'#'))
            continue;
        bool ok = false;
        const Value field = PythonJson::parse(meta->attribute(QStringLiteral("content")).toUtf8(), &ok);
        if (ok && field.kind == Value::Kind::Dict)
            result.append({ key, field });
    }
    return result;
}

bool isNullValue(const Value &value)
{
    return !PythonJson::truthy(value);
}

// apply_metadata()'s custom columns: those the book had, read before any
// change, that the library knows, then the library's own where it has a
// value.
void setUserMetadata(Context &context, const Metadata &metadata, const QList<QPair<QString, Value>> &bookUserMetadata)
{
    QList<QPair<QString, Value>> current;
    for (const auto &[key, field] : bookUserMetadata) {
        if (metadata.customKeys.contains(key))
            current.append({ key, field });
    }
    const auto put = [&current](const QString &key, const Value &field) {
        for (auto &[existing, value] : current) {
            if (existing == key) {
                value = field;
                return;
            }
        }
        current.append({ key, field });
    };
    for (const auto &[key, field] : metadata.userMetadata) {
        const QString type = field.text(QStringLiteral("datatype"));
        const Value *value = field.find(QStringLiteral("#value#"));
        const Value *multiple = field.find(QStringLiteral("is_multiple"));
        if (type == QLatin1String("text") && multiple && PythonJson::truthy(*multiple)) {
            if (value && PythonJson::truthy(*value))
                put(key, field);
        } else if (type == QLatin1String("int") || type == QLatin1String("float") || type == QLatin1String("bool")) {
            if (value && value->kind != Value::Kind::Null)
                put(key, field);
        } else if (value && !isNullValue(*value)) {
            put(key, field);
        }
    }
    for (Node *meta : kids(context.metadata, opfNamespace, "meta")) {
        if (meta->attribute(QStringLiteral("name")).startsWith(QLatin1String("calibre:user_metadata:")))
            removeElement(meta, &context.refines);
    }
    if (current.isEmpty())
        return;
    // dict_writer('user_metadata'), serialize_user_metadata()
    for (Node *meta : metasWithProperty(context, calibrePrefix + QStringLiteral(":user_metadata")))
        removeElement(meta, &context.refines);
    Value all = Value::dict();
    for (const auto &[key, field] : std::as_const(current)) {
        Value stored = field;
        const Value *display = field.find(QStringLiteral("display"));
        const Value *keep = display ? display->find(QStringLiteral("composite_store_template_value_in_opf")) : nullptr;
        if (field.text(QStringLiteral("datatype")) == QLatin1String("composite") && keep && !PythonJson::truthy(*keep))
            stored.insert(QStringLiteral("#value#"), Value::from(QString()));
        all.insert(key, stored);
    }
    ensurePrefix(context.package, &context.prefixes, QStringLiteral("calibre"), calibrePrefix);
    PythonJson::Format format;
    format.indent = 2;
    format.sortKeys = true;
    auto meta = makeElement(context.metadata, opfNamespace, QStringLiteral("meta"),
                            { { QStringLiteral("property"), QStringLiteral("calibre:user_metadata") } });
    meta->append(XmlTree::textNode(PythonJson::dumps(all, format)));
    appendElement(context.metadata, std::move(meta));
}

// items_with_property()
QList<Node *> itemsWithProperty(Context &context, const QString &property)
{
    QList<Node *> found;
    Node *manifest = nullptr;
    for (Node *child : kids(context.package, opfNamespace, "manifest")) {
        manifest = child;
        break;
    }
    if (!manifest)
        return found;
    for (Node *item : kids(manifest, opfNamespace, "item")) {
        if (!hasAttribute(item, QStringLiteral("properties")))
            continue;
        for (const QString &value : item->attribute(QStringLiteral("properties")).toLower().split(u' ', Qt::SkipEmptyParts)) {
            if (expandPrefix(value, context.prefixes) == property) {
                found.append(item);
                break;
            }
        }
    }
    return found;
}

QList<Node *> manifestItems(Context &context)
{
    QList<Node *> items;
    for (Node *manifest : kids(context.package, opfNamespace, "manifest"))
        items += kids(manifest, opfNamespace, "item");
    return items;
}

// read_raster_cover()
QString readRasterCover(Context &context)
{
    const auto hrefOf = [](const Node *item) {
        const QString type = item->attribute(QStringLiteral("media-type"));
        if (!type.isEmpty() && !type.contains(QLatin1String("xml")) && !type.contains(QLatin1String("html")))
            return item->attribute(QStringLiteral("href"));
        return QString();
    };
    for (Node *item : itemsWithProperty(context, QStringLiteral("cover-image"))) {
        if (const QString href = hrefOf(item); !href.isEmpty())
            return href;
    }
    QHash<QString, Node *> byId;
    for (Node *item : manifestItems(context)) {
        if (hasAttribute(item, QStringLiteral("id")) && hasAttribute(item, QStringLiteral("href"))
            && hasAttribute(item, QStringLiteral("media-type")))
            byId.insert(item->attribute(QStringLiteral("id")), item);
    }
    for (Node *meta : metasNamed(context, QStringLiteral("cover"))) {
        if (!hasAttribute(meta, QStringLiteral("content")))
            continue;
        if (Node *item = byId.value(meta->attribute(QStringLiteral("content")))) {
            if (const QString href = hrefOf(item); !href.isEmpty())
                return href;
        }
    }
    for (Node *spine : kids(context.package, opfNamespace, "spine")) {
        for (Node *itemref : kids(spine, opfNamespace, "itemref")) {
            if (!hasAttribute(itemref, QStringLiteral("idref")))
                continue;
            Node *item = byId.value(itemref->attribute(QStringLiteral("idref")));
            static const QStringList images{ QStringLiteral("image/jpeg"), QStringLiteral("image/webp"),
                                             QStringLiteral("image/png") };
            if (item && images.contains(item->attribute(QStringLiteral("media-type"))))
                return hrefOf(item);
            return {};
        }
    }
    return {};
}

// guess_type() of the extensions a cover has.
QString mediaType(const QString &name)
{
    const QString extension = QFileInfo(name).suffix().toLower();
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg"))
        return QStringLiteral("image/jpeg");
    return QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
}

// create_manifest_item() and ensure_is_only_raster_cover(): the cover
// added, the only one of the manifest.
bool addCover(Context &context, const QString &name)
{
    Node *manifest = nullptr;
    for (Node *child : kids(context.package, opfNamespace, "manifest")) {
        manifest = child;
        break;
    }
    if (!manifest)
        return false;
    const QString href = ensureUnique(name, allValues(context, QStringLiteral("href")));
    const QString id = ensureUnique(QStringLiteral("cover"), allValues(context, QStringLiteral("id")));
    appendElement(manifest, makeElement(manifest, opfNamespace, QStringLiteral("item"),
                                        { { QStringLiteral("href"), href },
                                          { QStringLiteral("id"), id },
                                          { QStringLiteral("media-type"), mediaType(name) } }));
    for (Node *meta : metasNamed(context, QStringLiteral("cover")))
        removeElement(meta, &context.refines);
    // set_unique_property()
    for (Node *item : itemsWithProperty(context, QStringLiteral("cover-image"))) {
        const QString rest = normalizeWhitespace(item->attribute(QStringLiteral("properties")).replace(QLatin1String("cover-image"), QString()));
        if (rest.isEmpty())
            item->removeAttribute(QStringLiteral("properties"));
        else
            item->setAttribute(QStringLiteral("properties"), rest);
    }
    for (Node *item : manifestItems(context)) {
        if (item->attribute(QStringLiteral("href")) == name)
            item->setAttribute(QStringLiteral("properties"),
                               normalizeWhitespace(item->attribute(QStringLiteral("properties")) + QStringLiteral(" cover-image")));
    }
    return true;
}

// --- The layout --------------------------------------------------------------

void collectElements(Node *node, const QString &namespaceUri, const char *localName, QList<Node *> *found)
{
    if (isElement(node, namespaceUri, localName))
        found->append(node);
    for (const auto &child : node->children)
        collectElements(child.get(), namespaceUri, localName, found);
}

// pretty_opf(): Dublin Core first, title and authors at their head; the
// manifest by kind, pages in the spine's order.
void prettyOpf(Node *package)
{
    QList<Node *> blocks;
    collectElements(package, opfNamespace, "metadata", &blocks);
    for (Node *metadata : std::as_const(blocks)) {
        QList<Node *> dc;
        for (Node *child : kids(metadata)) {
            if (child->kind == Node::Kind::Element && child->namespaceUri == dcNamespace)
                dc.append(child);
        }
        const auto key = [](const Node *node) {
            return node->localName == QLatin1String("title") ? 0 : node->localName == QLatin1String("creator") ? 1 : 2;
        };
        std::stable_sort(dc.begin(), dc.end(), [&key](const Node *a, const Node *b) { return key(a) < key(b); });
        for (auto it = dc.crbegin(); it != dc.crend(); ++it)
            insertAt(metadata, 0, takeWithTail(*it));
    }

    QHash<QString, int> spine;
    QList<Node *> spines;
    collectElements(package, opfNamespace, "spine", &spines);
    for (Node *element : std::as_const(spines)) {
        for (Node *itemref : kids(element, opfNamespace, "itemref")) {
            if (hasAttribute(itemref, QStringLiteral("idref")) && !spine.contains(itemref->attribute(QStringLiteral("idref"))))
                spine.insert(itemref->attribute(QStringLiteral("idref")), int(spine.size()));
        }
    }
    static const QSet<QString> documents{ QStringLiteral("application/xhtml+xml"), QStringLiteral("text/html"),
                                          QStringLiteral("text/x-oeb1-document"), QStringLiteral("text/x-oeb-document") };
    static const QSet<QString> styles{ QStringLiteral("text/css"), QStringLiteral("text/x-oeb1-css"),
                                       QStringLiteral("text/x-oeb-css"), QStringLiteral("xhtml/css") };
    QCollator collator;
    QList<Node *> manifests;
    collectElements(package, opfNamespace, "manifest", &manifests);
    for (Node *manifest : std::as_const(manifests)) {
        QList<Node *> items = kids(manifest);
        // A comment among them: left as it is.
        if (std::any_of(items.cbegin(), items.cend(), [](const Node *item) { return item->kind != Node::Kind::Element; }))
            continue;
        struct Key
        {
            int category;
            int position;
            QCollatorSortKey name;
        };
        QHash<const Node *, std::shared_ptr<Key>> keys;
        for (const Node *item : std::as_const(items)) {
            const QString type = item->attribute(QStringLiteral("media-type"));
            const QString href = item->attribute(QStringLiteral("href"));
            const QString extension = href.section(u'.', -1).toLower();
            int category = 1000;
            if (documents.contains(type))
                category = 0;
            else if (type == QLatin1String("application/x-dtbncx+xml"))
                category = 1;
            else if (styles.contains(type))
                category = 2;
            else if (type.startsWith(QLatin1String("image/")))
                category = 3;
            else if (QStringList{ "otf", "ttf", "woff", "woff2" }.contains(extension))
                category = 4;
            else if (type.startsWith(QLatin1String("audio/")))
                category = 5;
            else if (type.startsWith(QLatin1String("video/")))
                category = 6;
            const int position = category == 0 ? spine.value(item->attribute(QStringLiteral("id")), 1000000000) : 0;
            keys.insert(item, std::make_shared<Key>(Key{ category, position, collator.sortKey(href) }));
        }
        std::stable_sort(items.begin(), items.end(), [&keys](const Node *a, const Node *b) {
            const Key &x = *keys.value(a);
            const Key &y = *keys.value(b);
            if (x.category != y.category)
                return x.category < y.category;
            if (x.category == 0)
                return x.position < y.position;
            return x.name.compare(y.name) < 0;
        });
        for (auto it = items.crbegin(); it != items.crend(); ++it)
            insertAt(manifest, 0, takeWithTail(*it));
    }
}

bool isSpace(const QString &text)
{
    for (const QChar c : text) {
        if (c != u'\t' && c != u'\n' && c != u'\f' && c != u'\r' && c != u' ')
            return false;
    }
    return true;
}

// pretty_xml_tree()
void prettyXmlTree(Node *element, int level = 0)
{
    const QString indent = QStringLiteral("  ");
    const QList<Node *> children = kids(element);
    const QString text = textOf(element);
    if ((text.isEmpty() && !children.isEmpty()) || (!text.isEmpty() && isSpace(text)))
        setText(element, u'\n' + indent.repeated(level + 1));
    for (qsizetype i = 0; i < children.size(); ++i) {
        Node *child = children.at(i);
        if (child->kind == Node::Kind::Element)
            prettyXmlTree(child, level + 1);
        const QString tail = tailOf(child);
        if (tail.isEmpty() || isSpace(tail))
            setTail(child, u'\n' + indent.repeated(i == children.size() - 1 ? level : level + 1));
    }
}

bool isNullTitle(const QString &title, const QString &interfaceLanguage)
{
    return title.isEmpty() || title == CalibreText::unknownText(interfaceLanguage);
}

// serialize_cover_data(): the library's cover as the book's file holds its
// own, kept as it is when it is that format already.
QByteArray coverAs(const QByteArray &cover, const QString &path)
{
    QString format = QFileInfo(path).suffix().toLower();
    if (format == QLatin1String("jpg"))
        format = QStringLiteral("jpeg");
    QBuffer buffer;
    buffer.setData(cover);
    QImageReader reader(&buffer);
    QString original = QString::fromLatin1(reader.format()).toLower();
    if (original == QLatin1String("jpg"))
        original = QStringLiteral("jpeg");
    QImage image = reader.read();
    if (image.isNull())
        return cover;
    bool changed = format != original;
    if (image.hasAlphaChannel()) {
        // Transparency blended with white.
        QImage blended(image.size(), QImage::Format_RGB32);
        blended.fill(Qt::white);
        QPainter painter(&blended);
        painter.drawImage(0, 0, image);
        painter.end();
        image = blended;
        changed = true;
    }
    if (!changed)
        return cover;
    QByteArray data;
    QBuffer out(&data);
    out.open(QIODevice::WriteOnly);
    image.save(&out, format.toLatin1().constData(), 90);
    return data;
}

// The package's path, by META-INF/container.xml.
QString packagePath(const BookZip::Zip &zip)
{
    std::unique_ptr<Node> container = XmlTree::parse(zip.data.value(QStringLiteral("META-INF/container.xml")));
    if (!container)
        return {};
    for (const Node *rootfile : XmlTree::findAll(container.get(), [](const Node &node) { return node.isElement("rootfile"); })) {
        if (rootfile->attribute(QStringLiteral("media-type")) == QLatin1String("application/oebps-package+xml"))
            return rootfile->attribute(QStringLiteral("full-path"));
    }
    return {};
}

} // namespace

Metadata fromBook(const LibraryData &data, const Book &book, const QString &interfaceLanguage)
{
    Metadata metadata;
    metadata.title = book.title;
    metadata.titleSort = book.sort;
    metadata.authors = book.authors;
    metadata.authorSort = book.authorSort;
    metadata.languages = book.languages;
    metadata.pubdate = book.pubdate;
    metadata.comments = book.comments;
    metadata.publisher = book.publisher;
    metadata.tags = book.tags;
    metadata.rating = book.rating;
    metadata.series = book.series;
    metadata.seriesIndex = book.seriesIndex;
    metadata.identifiers = book.identifiers;
    metadata.applicationId = book.id;
    metadata.uuid = book.uuid;
    for (const auto &[key, json] : Opf::userMetadata(data, book))
        metadata.userMetadata.append({ key, PythonJson::parse(json.toUtf8()) });
    for (const CustomColumn &column : data.customColumns)
        metadata.customKeys.append(u'#' + column.label);
    QFile cover(book.coverFile);
    if (!book.coverFile.isEmpty() && cover.open(QIODevice::ReadOnly))
        metadata.cover = cover.readAll();
    metadata.interfaceLanguage = interfaceLanguage;
    return metadata;
}

std::optional<Package> apply(const QByteArray &opf, const Metadata &metadata, const QString &coverPrefix)
{
    std::unique_ptr<Node> document = XmlTree::parse(opf);
    Node *package = document ? XmlTree::documentElement(document.get()) : nullptr;
    if (!package || package->namespaceUri != opfNamespace || package->localName != QLatin1String("package"))
        return std::nullopt;
    // parse_opf_version(): OPF 2 is Calibre's other writer's.
    bool ok = false;
    const int major = package->attribute(QStringLiteral("version")).section(u'.', 0, 0).toInt(&ok);
    if (!ok || major < 3)
        return std::nullopt;
    Context context;
    context.document = document.get();
    context.package = package;
    const QList<Node *> blocks = kids(package, opfNamespace, "metadata");
    if (blocks.isEmpty())
        return std::nullopt;
    context.metadata = blocks.constFirst();
    context.prefixes = readPrefixes(package);
    context.refines = readRefines(context.metadata);
    // read_metadata(root), before anything changes.
    const QList<QPair<QString, Value>> bookUserMetadata = readUserMetadata(context);

    // apply_metadata(), what the library holds of the book, field by field.
    if (!metadata.identifiers.isEmpty())
        setIdentifiers(context, metadata.identifiers);
    if (!isNullTitle(metadata.title, metadata.interfaceLanguage))
        setTitle(context, metadata.title, metadata.titleSort);
    if (!metadata.languages.isEmpty())
        setLanguages(context, metadata.languages);
    if (!metadata.authors.isEmpty())
        setAuthors(context, metadata.authors, metadata.authorSort);
    // set_pubdate(): the book's dates go; the library's comes when it has one.
    removeDc(context, "date");
    if (metadata.pubdate.isValid() && !CalibreDb::isUndefinedDate(metadata.pubdate))
        appendDc(context, QStringLiteral("date"), CalibreDb::isoFormat(metadata.pubdate));
    if (!metadata.comments.trimmed().isEmpty()) {
        removeDc(context, "description");
        appendDc(context, QStringLiteral("description"), metadata.comments.trimmed());
    }
    if (!metadata.publisher.trimmed().isEmpty()) {
        removeDc(context, "publisher");
        appendDc(context, QStringLiteral("publisher"), normalizeWhitespace(metadata.publisher));
    }
    if (!metadata.tags.isEmpty()) {
        removeDc(context, "subject");
        for (const QString &tag : uniq(metadata.tags)) {
            if (!normalizeWhitespace(tag).isEmpty())
                appendDc(context, QStringLiteral("subject"), normalizeWhitespace(tag));
        }
    }
    if (metadata.rating > 0)
        setRating(context, metadata.rating);
    if (!metadata.series.isEmpty())
        setSeries(context, metadata.series, metadata.seriesIndex);
    if (metadata.applicationId)
        writeIdentifier(context, QStringLiteral("calibre"), QString::number(metadata.applicationId));
    if (!metadata.uuid.isEmpty())
        writeIdentifier(context, QStringLiteral("uuid"), metadata.uuid);
    setUserMetadata(context, metadata, bookUserMetadata);

    Package result;
    result.rasterCover = readRasterCover(context);
    if (result.rasterCover.isEmpty() && !metadata.cover.isEmpty()) {
        // The cover's name from the package's folder, as Calibre gives it,
        // which puts the folder twice in its path.
        QString prefix = coverPrefix;
        if (!prefix.isEmpty() && !prefix.endsWith(u'/'))
            prefix += u'/';
        const QString name = prefix + QStringLiteral("cover.jpg");
        if (addCover(context, name))
            result.rasterCover = name;
    }
    prettyOpf(package);
    prettyXmlTree(package);
    result.opf = XmlTree::serialize(*document, false, false);
    return result;
}

bool write(const QString &source, const QString &target, const Metadata &metadata, QString *error)
{
    BookZip::Zip zip;
    if (!BookZip::readZip(source, &zip, error))
        return false;
    const QString opfPath = packagePath(zip);
    const QString folder = opfPath.contains(u'/') ? opfPath.section(u'/', 0, -2) : QString();
    const std::optional<Package> package
        = opfPath.isEmpty() ? std::nullopt : apply(zip.data.value(opfPath), metadata, folder);
    if (!package) {
        // As when Calibre's writer fails: the book as it is.
        QFile::remove(target);
        if (!QFile::copy(source, target)) {
            *error = QObject::tr("Cannot write %1.").arg(target);
            return false;
        }
        return true;
    }
    // safe_replace(): the package and the cover in their places, or last.
    const auto replace = [&zip](const QString &name, const QByteArray &bytes) {
        if (!zip.data.contains(name))
            zip.names.append(name);
        zip.data.insert(name, bytes);
    };
    replace(opfPath, package->opf);
    if (!metadata.cover.isEmpty() && !package->rasterCover.isEmpty()) {
        const QString coverPath = folder.isEmpty() ? package->rasterCover : folder + u'/' + package->rasterCover;
        const QString extension = QFileInfo(coverPath).suffix().toLower();
        static const QStringList replaceable{ QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg") };
        const bool encrypted = zip.data.value(QStringLiteral("META-INF/encryption.xml")).contains(coverPath.toUtf8());
        if (replaceable.contains(extension) && !encrypted)
            replace(coverPath, coverAs(metadata.cover, coverPath));
    }
    return BookZip::writeZip(target, zip, error);
}

} // namespace EpubMetadata
