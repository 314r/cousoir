// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/conversion/plugins/mobi_output.py: AZW3Output.convert(),
//   remove_html_cover()
// - src/calibre/ebooks/conversion/plugins/epub_input.py: the HTML cover of
//   rationalize_cover2() and rationalize_cover3(), process_encryption()
// - src/calibre/ebooks/oeb/transforms/data_url.py: DataURL
// - src/calibre/ebooks/mobi/writer2/resources.py: Resources,
//   process_jpegs_for_amazon(), PLACEHOLDER_GIF
// - src/calibre/ebooks/mobi/writer8/cleanup.py: remove_duplicate_anchors()
// - src/calibre/ebooks/mobi/writer8/toc.py: TOCAdder
// - src/calibre/ebooks/mobi/writer8/main.py: KF8Writer
// - src/calibre/ebooks/mobi/writer8/skeleton.py: Chunker.remove_namespaces()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kf8_writer.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "book.h"
#include "calibre_text.h"
#include "epub_book.h"
#include "epub_package.h"
#include "html_soup.h"
#include "kf8_book.h"
#include "kf8_index.h"
#include "kf8_skeleton.h"
#include "mobi_format.h"
#include "xml_tree.h"

namespace Kf8 {

using EpubPackage::resolve;
using Kf8Skeleton::Element;
using MobiFormat::toBase32;
using XmlTree::Node;

namespace {

const QString xhtmlNamespace = QStringLiteral("http://www.w3.org/1999/xhtml");
const QString svgNamespace = QStringLiteral("http://www.w3.org/2000/svg");

// Of Resources, a picture of one transparent pixel. The strings apart where
// a hexadecimal escape would take the next letter.
const QByteArray placeholderGif("GIF89a\x01\x00\x01\x00\xf0\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00!\xfe"
                                " calibre-placeholder-gif-for-azw3\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02"
                                "D\x01\x00;",
                                79);

struct Item
{
    QString id;
    QString name;
    QString mediaType;
    QStringList properties;
};

struct Page
{
    QString name;
    std::unique_ptr<Node> document;
};

struct GuideReference
{
    QString type;
    QString title;
    QString name;
    QString fragment;
};

struct TocNode
{
    QString title;
    QString name;
    QString fragment;
    int depth = 0;
    int parent = -1;
    QList<int> children;
};

bool isDocument(const QString &mediaType)
{
    return mediaType == QLatin1String("application/xhtml+xml") || mediaType == QLatin1String("text/html")
        || mediaType == QLatin1String("application/x-dtbook+xml");
}

QString localPart(QStringView qualifiedName)
{
    const qsizetype colon = qualifiedName.lastIndexOf(u':');
    return (colon < 0 ? qualifiedName : qualifiedName.mid(colon + 1)).toString();
}

bool isXhtml(const Node &node, const char *name)
{
    return node.isElement(name) && (node.namespaceUri.isEmpty() || node.namespaceUri == xhtmlNamespace);
}

QList<Node *> elements(Node *root, const std::function<bool(const Node &)> &matches)
{
    return XmlTree::findAll(root, [&matches](const Node &node) { return node.kind == Node::Kind::Element && matches(node); });
}

// The text of an element's own text and CDATA.
QString ownText(const Node &node)
{
    QString text;
    for (const auto &child : node.children) {
        if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata)
            text += child->text;
    }
    return text;
}

void setOwnText(Node *node, const QString &text)
{
    node->children.clear();
    node->append(XmlTree::textNode(text));
}

void remove(Node *node)
{
    if (Node *parent = node->parent)
        parent->take(parent->indexOf(node));
}

// What imghdr's what() names a picture.
QString imageFormat(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QByteArray format = QImageReader::imageFormat(&buffer);
    if (format == "jpg")
        format = "jpeg";
    return QString::fromLatin1(format);
}

// The JPEG markers before the picture: whether it has those PIL tells of
// (JFIF, Adobe, an ICC profile...), and whether one is EXIF.
void jpegMarkers(const QByteArray &data, bool *info, bool *exif)
{
    *info = false;
    *exif = false;
    qsizetype at = 2;
    while (at + 4 <= data.size() && quint8(data.at(at)) == 0xFF) {
        const quint8 marker = quint8(data.at(at + 1));
        const qsizetype length = MobiFormat::u16(data, at + 2);
        if (marker >= 0xE0 && marker <= 0xEF) {
            *info = true;
            if (marker == 0xE1 && data.mid(at + 4, 6) == QByteArray("Exif\0\0", 6))
                *exif = true;
        }
        // A progressive picture is told of too.
        if (marker == 0xC2)
            *info = true;
        if (marker == 0xDA || length < 2)
            break;
        at += 2 + length;
    }
}

// process_jpegs_for_amazon(): a JPEG without JFIF or with EXIF written
// again, turned as its EXIF says, which Kindles need.
QByteArray jpegForAmazon(const QByteArray &data)
{
    bool info = false;
    bool exif = false;
    jpegMarkers(data, &info, &exif);
    if (info && !exif)
        return data;
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return data;
    QByteArray bytes;
    QBuffer out(&bytes);
    out.open(QIODevice::WriteOnly);
    image.save(&out, "JPEG");
    return bytes;
}

QByteArray png(const QByteArray &data)
{
    const QImage image = QImage::fromData(data);
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer out(&bytes);
    out.open(QIODevice::WriteOnly);
    image.save(&out, "PNG");
    return bytes;
}

// The style sheet with each url() address given by `url`, and each @import
// one by `import`, as css_parser's replaceUrls() and the import rules'
// hrefs change them, written as css_parser writes them, url() without
// quotes, which the readers of KF8 books expect; the rest, comments and
// strings included, as written.
QString rewriteCss(const QString &css, const std::function<QString(const QString &)> &url,
                   const std::function<QString(const QString &)> &import)
{
    const qsizetype n = css.size();
    const auto readString = [&css, n](qsizetype at, qsizetype *end) {
        const QChar quote = css.at(at);
        QString value;
        qsizetype k = at + 1;
        while (k < n && css.at(k) != quote) {
            if (css.at(k) == u'\\' && k + 1 < n) {
                value += css.at(k + 1);
                k += 2;
                continue;
            }
            value += css.at(k++);
        }
        *end = std::min(k + 1, n);
        return value;
    };
    // After "url(": the address, its quote, and the end after ")".
    const auto readUrl = [&css, n, &readString](qsizetype at, qsizetype *end, QChar *quote) {
        qsizetype k = at;
        while (k < n && css.at(k).isSpace())
            ++k;
        QString value;
        *quote = QChar();
        if (k < n && (css.at(k) == u'"' || css.at(k) == u'\'')) {
            *quote = css.at(k);
            value = readString(k, &k);
        } else {
            const qsizetype start = k;
            while (k < n && css.at(k) != u')')
                ++k;
            value = css.mid(start, k - start).trimmed();
        }
        while (k < n && css.at(k) != u')')
            ++k;
        *end = std::min(k + 1, n);
        return value;
    };
    QString out;
    out.reserve(n);
    qsizetype i = 0;
    while (i < n) {
        const QChar c = css.at(i);
        if (c == u'/' && i + 1 < n && css.at(i + 1) == u'*') {
            const qsizetype close = css.indexOf(QLatin1String("*/"), i + 2);
            const qsizetype stop = close < 0 ? n : close + 2;
            out += QStringView(css).mid(i, stop - i);
            i = stop;
            continue;
        }
        if (c == u'"' || c == u'\'') {
            qsizetype end = i;
            readString(i, &end);
            out += QStringView(css).mid(i, end - i);
            i = end;
            continue;
        }
        if (c == u'@' && QStringView(css).mid(i, 7).compare(QLatin1String("@import"), Qt::CaseInsensitive) == 0) {
            qsizetype k = i + 7;
            while (k < n && css.at(k).isSpace())
                ++k;
            out += QStringView(css).mid(i, k - i);
            qsizetype end = k;
            QString href;
            if (k < n && (css.at(k) == u'"' || css.at(k) == u'\'')) {
                href = readString(k, &end);
            } else if (QStringView(css).mid(k, 4).compare(QLatin1String("url("), Qt::CaseInsensitive) == 0) {
                QChar quote;
                href = readUrl(k + 4, &end, &quote);
            }
            const QString replaced = import(href);
            out += replaced == href ? QStringView(css).mid(k, end - k).toString() : QStringLiteral("url(") + replaced + u')';
            i = end;
            continue;
        }
        if ((c == u'u' || c == u'U') && QStringView(css).mid(i, 4).compare(QLatin1String("url("), Qt::CaseInsensitive) == 0
            && (i == 0 || !(css.at(i - 1).isLetterOrNumber() || css.at(i - 1) == u'-' || css.at(i - 1) == u'_'))) {
            qsizetype end = i;
            QChar quote;
            const QString value = readUrl(i + 4, &end, &quote);
            const QString replaced = url(value);
            out += replaced == value ? QStringView(css).mid(i, end - i).toString() : QStringLiteral("url(") + replaced + u')';
            i = end;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// A subtree of a page as XML of its own, the namespaces it uses declared
// on it, as lxml writes it.
QByteArray serializeSubtree(const Node &node)
{
    std::unique_ptr<Node> copy = XmlTree::clone(node, false);
    QHash<QString, QString> declared;
    for (const QXmlStreamNamespaceDeclaration &declaration : std::as_const(copy->namespaces))
        declared.insert(declaration.prefix().toString(), declaration.namespaceUri().toString());
    const auto lookup = [&node](const QString &prefix) -> QString {
        for (const Node *at = &node; at; at = at->parent) {
            for (const QXmlStreamNamespaceDeclaration &declaration : at->namespaces) {
                if (declaration.prefix() == prefix)
                    return declaration.namespaceUri().toString();
            }
        }
        if (prefix == QLatin1String("xlink"))
            return QStringLiteral("http://www.w3.org/1999/xlink");
        if (prefix == QLatin1String("svg"))
            return svgNamespace;
        return {};
    };
    std::function<void(const Node &)> declare = [&](const Node &element) {
        if (element.kind != Node::Kind::Element)
            return;
        const QString prefix = element.prefix().chopped(element.prefix().isEmpty() ? 0 : 1);
        if (!declared.contains(prefix) && !element.namespaceUri.isEmpty()) {
            declared.insert(prefix, element.namespaceUri);
            copy->namespaces.append(QXmlStreamNamespaceDeclaration(prefix, element.namespaceUri));
        }
        for (const QXmlStreamAttribute &attribute : element.attributes) {
            const QString name = attribute.qualifiedName().toString();
            if (!name.contains(u':'))
                continue;
            const QString attributePrefix = name.section(u':', 0, 0);
            if (attributePrefix == QLatin1String("xml") || declared.contains(attributePrefix))
                continue;
            const QString uri = lookup(attributePrefix);
            if (!uri.isEmpty()) {
                declared.insert(attributePrefix, uri);
                copy->namespaces.append(QXmlStreamNamespaceDeclaration(attributePrefix, uri));
            }
        }
        for (const auto &child : element.children)
            declare(*child);
    };
    declare(*copy);
    Node document;
    document.append(std::move(copy));
    QByteArray xml = XmlTree::serialize(document);
    // Without the declaration serialize() puts first.
    return xml.mid(xml.indexOf('\n') + 1);
}

// Chunker.remove_namespaces(): the page with names and attributes without
// namespaces, comments and instructions left out, the root keeping only its
// language, class and style.
std::unique_ptr<Element> convert(const Node &node, bool root)
{
    auto element = std::make_unique<Element>();
    element->name = node.localName;
    if (root) {
        QString language;
        for (const QXmlStreamAttribute &attribute : node.attributes) {
            if (localPart(attribute.qualifiedName()) == QLatin1String("lang"))
                language = attribute.value().toString();
        }
        if (!language.isEmpty())
            element->setAttribute(QStringLiteral("lang"), language);
        for (const char *name : { "class", "style" }) {
            const QString key = QLatin1String(name);
            for (const QXmlStreamAttribute &attribute : node.attributes) {
                if (attribute.qualifiedName() == key)
                    element->setAttribute(key, attribute.value().toString());
            }
        }
    } else {
        for (const QXmlStreamAttribute &attribute : node.attributes)
            element->setAttribute(localPart(attribute.qualifiedName()), attribute.value().toString());
    }
    Element *last = nullptr;
    for (const auto &child : node.children) {
        switch (child->kind) {
        case Node::Kind::Text:
        case Node::Kind::Cdata:
            (last ? last->tail : element->text) += child->text;
            break;
        case Node::Kind::Element:
            last = element->append(convert(*child, false));
            break;
        default:
            break;
        }
    }
    return element;
}

// insert_aid_attributes(): an aid on the tags kindlegen gives one to and
// those with an id, a cid for those in a table, which the Kindle mishandles
// otherwise; `ids` the aid of each id, and of the page's body by "".
void insertAids(Element *element, bool inTable, const QString &page, quint64 base, quint64 *next, int *cid,
                QHash<QPair<QString, QString>, QString> *ids)
{
    for (const auto &child : element->children) {
        Element *tag = child.get();
        bool hasId = tag->hasAttribute(QStringLiteral("id"));
        QString id = tag->attribute(QStringLiteral("id"));
        if (!hasId && tag->name == QLatin1String("a") && tag->hasAttribute(QStringLiteral("name"))) {
            id = tag->attribute(QStringLiteral("name"));
            hasId = true;
            tag->setAttribute(QStringLiteral("id"), id);
        }
        const QString name = tag->name.toLower();
        const bool table = name == QLatin1String("table");
        if (hasId || Kf8Skeleton::aidAble(name)) {
            if (table || inTable) {
                if (!id.isEmpty()) {
                    const QString value = QStringLiteral("c%1").arg(++*cid);
                    ids->insert({ page, id }, value);
                    tag->setAttribute(QStringLiteral("cid"), value);
                }
            } else {
                const QString aid = toBase32(base + (*next)++);
                tag->setAttribute(QStringLiteral("aid"), aid);
                if (name == QLatin1String("body"))
                    ids->insert({ page, QString() }, aid);
                if (hasId)
                    ids->insert({ page, id }, aid);
            }
        }
        insertAids(tag, inTable || table, page, base, next, cid, ids);
    }
}

// The languages written right to left, for which a book turns its pages
// that way: set_page_progression_direction_if_needed().
bool rightToLeft(const QString &language)
{
    static const QSet<QString> languages{ QStringLiteral("ar"), QStringLiteral("he"), QStringLiteral("fa"),
                                          QStringLiteral("ur"), QStringLiteral("yi"), QStringLiteral("ps"),
                                          QStringLiteral("sd"), QStringLiteral("ug"), QStringLiteral("dv"),
                                          QStringLiteral("ku") };
    const QString code = CalibreText::languageAsIso6391(language);
    return languages.contains(code.isEmpty() ? language.left(2).toLower() : code);
}

// is_guide_ref_start().
bool startsTheBook(const GuideReference &reference)
{
    const QString type = reference.type.toLower();
    return reference.title.toLower() == QLatin1String("start") || type == QLatin1String("start")
        || type == QLatin1String("other.start") || type == QLatin1String("text");
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

class Writer
{
public:
    explicit Writer(const Options &options)
        : m_options(options)
    {
    }

    bool load(const QString &epub, QString *error);
    QByteArray write();

private:
    QString pointer(const QString &from, const QString &ref);
    void addResources();
    void removeHtmlCover();
    void addInlineToc();
    void readMetadata(Node *package);
    void undoFontObfuscation(const QString &uniqueIdentifier, const QStringList &identifiers, QString *error);

    Options m_options;
    QString m_epub;
    EpubPackage::Zip m_zip;
    QString m_opfName;
    QList<Item> m_manifest;
    QHash<QString, qsizetype> m_items;
    std::vector<Page> m_spine;
    QList<GuideReference> m_guide;
    QList<TocNode> m_toc;
    QString m_coverName;
    QString m_direction;

    // Resources: their records, by name the number of each from 1, and the
    // type of each picture.
    QList<QByteArray> m_records;
    QHash<QString, int> m_itemMap;
    QHash<QString, QString> m_mimeMap;
    QSet<int> m_imageIndices;
    QSet<int> m_usedImageIndices;
    QSet<QString> m_usedImages;
    std::optional<quint32> m_coverOffset;
    std::optional<quint32> m_thumbnailOffset;
    bool m_fonts = false;
};

void Writer::readMetadata(Node *package)
{
    // The EPUB's own metadata where the library gives none, as for a book
    // converted outside the library.
    MobiMetadata::Metadata &metadata = m_options.metadata;
    if (!metadata.title.isEmpty())
        return;
    Node *block = XmlTree::childElement(package, "metadata");
    if (!block)
        return;
    for (const auto &child : block->children) {
        if (child->kind != Node::Kind::Element)
            continue;
        const QString text = ownText(*child).trimmed();
        if (text.isEmpty())
            continue;
        if (child->isElement("title") && metadata.title.isEmpty())
            metadata.title = text;
        else if (child->isElement("creator"))
            metadata.authors.append(text);
        else if (child->isElement("publisher"))
            metadata.publisher = text;
        else if (child->isElement("description"))
            metadata.comments = text;
        else if (child->isElement("subject"))
            metadata.tags.append(text);
        else if (child->isElement("language"))
            metadata.languages.append(CalibreText::canonicalizeLanguage(text).isEmpty() ? text
                                                                                        : CalibreText::canonicalizeLanguage(text));
        else if (child->isElement("identifier")) {
            const QString lower = text.toLower();
            if (lower.startsWith(QLatin1String("urn:uuid:")) && metadata.uuid.isEmpty())
                metadata.uuid = text.mid(9);
            else if (lower.startsWith(QLatin1String("urn:isbn:")))
                metadata.isbn = text.mid(9);
            else if (child->attribute(QStringLiteral("opf:scheme")).compare(QLatin1String("isbn"), Qt::CaseInsensitive) == 0)
                metadata.isbn = text;
        }
    }
}

// process_encryption(): the fonts obfuscated by IDPF's or Adobe's method made
// whole again; a book with anything else encrypted is protected and refused.
void Writer::undoFontObfuscation(const QString &uniqueIdentifier, const QStringList &identifiers, QString *error)
{
    const std::unique_ptr<Node> encryption = XmlTree::parse(m_zip.data.value(QStringLiteral("META-INF/encryption.xml")));
    if (!encryption)
        return;
    for (Node *data : elements(encryption.get(), [](const Node &n) { return n.isElement("EncryptedData"); })) {
        QString algorithm;
        QString uri;
        for (Node *method : elements(data, [](const Node &n) { return n.isElement("EncryptionMethod"); }))
            algorithm = method->attribute(QStringLiteral("Algorithm"));
        for (Node *reference : elements(data, [](const Node &n) { return n.isElement("CipherReference"); }))
            uri = reference->attribute(QStringLiteral("URI"));
        const QString name = QDir::cleanPath(QUrl::fromPercentEncoding(uri.toUtf8()));
        if (!m_zip.data.contains(name))
            continue;
        QByteArray key;
        qsizetype extent = 0;
        if (algorithm == QLatin1String("http://www.idpf.org/2008/embedding")) {
            QString identifier = uniqueIdentifier;
            identifier.remove(u' ').remove(u'\t').remove(u'\r').remove(u'\n');
            key = QCryptographicHash::hash(identifier.toUtf8(), QCryptographicHash::Sha1);
            extent = 1040;
        } else if (algorithm == QLatin1String("http://ns.adobe.com/pdf/enc#RC")) {
            for (const QString &identifier : identifiers) {
                if (identifier.startsWith(QLatin1String("urn:uuid:"), Qt::CaseInsensitive))
                    key = QByteArray::fromHex(identifier.mid(9).remove(u'-').toLatin1());
            }
            extent = 1024;
        } else {
            *error = QObject::tr("%1 is protected by DRM and cannot be converted.").arg(QFileInfo(m_epub).fileName());
            return;
        }
        if (key.isEmpty())
            continue;
        QByteArray &font = m_zip.data[name];
        for (qsizetype i = 0; i < std::min(extent, font.size()); ++i)
            font[i] = char(font.at(i) ^ key.at(i % key.size()));
    }
}

bool Writer::load(const QString &epub, QString *error)
{
    m_epub = epub;
    if (!EpubPackage::readZip(epub, &m_zip, error))
        return false;
    m_opfName = EpubPackage::packageName(m_zip);
    std::unique_ptr<Node> opf = m_opfName.isEmpty() ? nullptr : XmlTree::parse(m_zip.data.value(m_opfName));
    Node *package = opf ? XmlTree::documentElement(opf.get()) : nullptr;
    Node *manifest = package ? XmlTree::childElement(package, "manifest") : nullptr;
    Node *spine = package ? XmlTree::childElement(package, "spine") : nullptr;
    if (!manifest || !spine) {
        *error = QObject::tr("%1 has no readable package document.").arg(QFileInfo(epub).fileName());
        return false;
    }
    readMetadata(package);

    // The identifiers, for the fonts.
    QString uniqueIdentifier;
    QStringList identifiers;
    QString coverId;
    if (Node *metadata = XmlTree::childElement(package, "metadata")) {
        const QString uniqueId = package->attribute(QStringLiteral("unique-identifier"));
        for (const auto &child : metadata->children) {
            if (child->isElement("identifier")) {
                identifiers.append(ownText(*child).trimmed());
                if (child->attribute(QStringLiteral("id")) == uniqueId)
                    uniqueIdentifier = ownText(*child);
            } else if (child->isElement("meta") && child->attribute(QStringLiteral("name")) == QLatin1String("cover")) {
                coverId = child->attribute(QStringLiteral("content"));
            }
        }
    }
    QString encryptionError;
    undoFontObfuscation(uniqueIdentifier, identifiers, &encryptionError);
    if (!encryptionError.isEmpty()) {
        *error = encryptionError;
        return false;
    }

    QHash<QString, QString> byId;
    for (const auto &child : manifest->children) {
        if (!child->isElement("item"))
            continue;
        Item item;
        item.id = child->attribute(QStringLiteral("id"));
        item.name = resolve(child->attribute(QStringLiteral("href")), m_opfName);
        item.mediaType = child->attribute(QStringLiteral("media-type")).toLower();
        item.properties = child->attribute(QStringLiteral("properties")).split(u' ', Qt::SkipEmptyParts);
        if (item.name.isEmpty() || !m_zip.data.contains(item.name) || m_items.contains(item.name))
            continue;
        byId.insert(item.id, item.name);
        m_items.insert(item.name, m_manifest.size());
        m_manifest.append(item);
        if (item.properties.contains(QStringLiteral("cover-image")))
            m_coverName = item.name;
    }
    if (m_coverName.isEmpty() && byId.contains(coverId)) {
        const QString name = byId.value(coverId);
        if (m_manifest.at(m_items.value(name)).mediaType.startsWith(QLatin1String("image/")))
            m_coverName = name;
    }
    m_direction = spine->attribute(QStringLiteral("page-progression-direction"));

    // The pages, as XML, else mended as a browser would.
    for (const auto &child : spine->children) {
        if (!child->isElement("itemref"))
            continue;
        const QString name = byId.value(child->attribute(QStringLiteral("idref")));
        if (name.isEmpty())
            continue;
        const Item &item = m_manifest.at(m_items.value(name));
        const QByteArray data = m_zip.data.value(name);
        std::unique_ptr<Node> document;
        if (item.mediaType == QLatin1String("image/svg+xml")) {
            // A picture as a page: in a page of its own.
            document = HtmlSoup::parse(QStringLiteral("<html><head></head><body></body></html>"));
            if (std::unique_ptr<Node> svg = XmlTree::parse(data); svg && XmlTree::documentElement(svg.get())) {
                Node *body = elements(document.get(), [](const Node &n) { return n.isElement("body"); }).value(0);
                Node *root = XmlTree::documentElement(svg.get());
                if (body)
                    body->append(svg->take(svg->indexOf(root)));
            }
        } else if (isDocument(item.mediaType)) {
            document = XmlTree::parse(data);
            Node *root = document ? XmlTree::documentElement(document.get()) : nullptr;
            if (!root || !root->isElement("html") || elements(root, [](const Node &n) { return n.isElement("body"); }).isEmpty())
                document = HtmlSoup::parse(XmlTree::xmlText(data));
        }
        if (document && XmlTree::documentElement(document.get()))
            m_spine.push_back({ name, std::move(document) });
    }
    if (m_spine.empty()) {
        *error = QObject::tr("%1 has no pages.").arg(QFileInfo(epub).fileName());
        return false;
    }

    if (Node *guide = XmlTree::childElement(package, "guide")) {
        for (const auto &child : guide->children) {
            if (!child->isElement("reference"))
                continue;
            const QString href = child->attribute(QStringLiteral("href"));
            const QString name = resolve(href, m_opfName);
            if (name.isEmpty())
                continue;
            m_guide.append({ child->attribute(QStringLiteral("type")), child->attribute(QStringLiteral("title")), name,
                             QUrl::fromPercentEncoding(href.section(u'#', 1).toUtf8()) });
        }
    }

    // The table of contents, its entries under their parents.
    QString tocError;
    if (const std::unique_ptr<EpubBook> book = EpubBook::open(epub, &tocError)) {
        QList<int> parents;
        for (const EpubBook::TocEntry &entry : book->toc()) {
            while (parents.size() > std::max(entry.depth, 0))
                parents.removeLast();
            TocNode node;
            node.title = entry.title;
            node.name = entry.name;
            node.fragment = entry.fragment;
            node.depth = int(parents.size());
            node.parent = parents.isEmpty() ? -1 : parents.back();
            const int index = int(m_toc.size());
            if (node.parent >= 0)
                m_toc[node.parent].children.append(index);
            m_toc.append(node);
            parents.append(index);
        }
    }
    return true;
}

// The address of a picture or a font as the KF8 has it, its record's number;
// the address as it was for anything else.
QString Writer::pointer(const QString &from, const QString &ref)
{
    const QString name = resolve(ref, from);
    const auto index = m_itemMap.constFind(name);
    if (name.isEmpty() || index == m_itemMap.cend())
        return ref;
    if (m_records.at(*index - 1).startsWith("FONT"))
        return QStringLiteral("kindle:embed:") + toBase32(quint64(*index), 4);
    m_usedImages.insert(name);
    return QStringLiteral("kindle:embed:%1?mime=%2").arg(toBase32(quint64(*index), 4), m_mimeMap.value(name));
}

// Resources.add_resources(): the pictures in the order of the manifest,
// the cover's thumbnail after it, then the fonts.
void Writer::addResources()
{
    static const QSet<QString> shown{ QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("gif") };
    for (const Item &item : std::as_const(m_manifest)) {
        if (!item.mediaType.startsWith(QLatin1String("image/")) || item.mediaType == QLatin1String("image/svg+xml"))
            continue;
        QByteArray data = m_zip.data.value(item.name);
        // WebP, AVIF and the others no Kindle shows, as PNG.
        if (!shown.contains(imageFormat(data)))
            data = png(data);
        const QString format = imageFormat(data);
        if (data.isEmpty() || format.isEmpty())
            continue;
        if (format == QLatin1String("jpeg"))
            data = jpegForAmazon(data);
        m_imageIndices.insert(int(m_records.size()));
        m_records.append(data);
        m_itemMap.insert(item.name, int(m_records.size()));
        m_mimeMap.insert(item.name, QStringLiteral("image/") + format);
        if (item.name == m_coverName) {
            m_coverOffset = quint32(m_records.size() - 1);
            m_usedImageIndices.insert(int(*m_coverOffset));
            const QByteArray thumbnail = MobiMetadata::thumbnail(data);
            if (!thumbnail.isEmpty()) {
                m_imageIndices.insert(int(m_records.size()));
                m_records.append(thumbnail);
                m_thumbnailOffset = quint32(m_records.size() - 1);
                m_usedImageIndices.insert(int(*m_thumbnailOffset));
            }
        }
    }
    for (const Item &item : std::as_const(m_manifest)) {
        const QString extension = item.name.section(u'.', -1).toLower();
        if (extension != QLatin1String("ttf") && extension != QLatin1String("otf"))
            continue;
        m_records.append(MobiFormat::fontRecord(m_zip.data.value(item.name)));
        m_itemMap.insert(item.name, int(m_records.size()));
        m_fonts = true;
    }
}

// remove_html_cover(), and the cover page of the EPUB's input: the first
// page shows the cover when the guide says so or the package names it the
// title page; the Kindle shows the cover itself.
void Writer::removeHtmlCover()
{
    if (m_coverName.isEmpty() || m_spine.size() < 2)
        return;
    QString titlePage;
    for (const Item &item : std::as_const(m_manifest)) {
        if (item.properties.contains(QStringLiteral("calibre:title-page")))
            titlePage = item.name;
    }
    for (const GuideReference &reference : std::as_const(m_guide)) {
        const QString type = reference.type.toLower();
        if (titlePage.isEmpty() && (type == QLatin1String("cover") || type == QLatin1String("titlepage"))
            && reference.name == m_spine.front().name)
            titlePage = reference.name;
    }
    // The page the guide calls the cover, wherever it is.
    for (const GuideReference &reference : std::as_const(m_guide)) {
        if (titlePage.isEmpty() && reference.type.toLower() == QLatin1String("cover")
            && std::any_of(m_spine.cbegin(), m_spine.cend(), [&reference](const Page &page) { return page.name == reference.name; }))
            titlePage = reference.name;
    }
    if (titlePage.isEmpty())
        return;
    std::erase_if(m_spine, [&titlePage](const Page &page) { return page.name == titlePage; });
    m_guide.removeIf([&titlePage](const GuideReference &reference) {
        const QString type = reference.type.toLower();
        return reference.name == titlePage || type == QLatin1String("cover");
    });
}

// TOCAdder: a page of the table of contents at the end of the book, when it
// has more than one entry and the guide names none with links.
void Writer::addInlineToc()
{
    const bool hasToc = m_toc.size() > 1;
    for (qsizetype i = 0; i < m_guide.size(); ++i) {
        if (m_guide.at(i).type.toLower() != QLatin1String("toc"))
            continue;
        const QString name = m_guide.at(i).name;
        const auto item = m_items.constFind(name);
        if (item != m_items.cend() && isDocument(m_manifest.at(*item).mediaType)) {
            const auto inSpine = std::find_if(m_spine.cbegin(), m_spine.cend(), [&name](const Page &page) { return page.name == name; });
            std::unique_ptr<Node> document = inSpine == m_spine.cend() ? XmlTree::parse(m_zip.data.value(name)) : nullptr;
            Node *root = inSpine != m_spine.cend() ? inSpine->document.get() : document.get();
            if (root && !elements(root, [](const Node &n) { return isXhtml(n, "a") && n.attributes.hasAttribute(QStringLiteral("href")); }).isEmpty()) {
                if (document)
                    m_spine.push_back({ name, std::move(document) });
                return;
            }
            if (!hasToc)
                return;
        }
        m_guide.removeAt(i);
        break;
    }
    if (!hasToc)
        return;
    const QString opfFolder = m_opfName.contains(u'/') ? m_opfName.section(u'/', 0, -2) + u'/' : QString();
    const QString name = EpubPackage::freeName(m_zip, opfFolder + QStringLiteral("contents.xhtml"));
    const QString title = escaped(m_options.tocTitle);
    std::function<QString(int, int)> entry = [&](int index, int level) {
        const TocNode &node = m_toc.at(index);
        QString href = node.name.isEmpty() ? QStringLiteral("#") : EpubPackage::relativeHref(node.name, name);
        if (!node.name.isEmpty() && !node.fragment.isEmpty())
            href += u'#' + QString::fromUtf8(QUrl::toPercentEncoding(node.fragment));
        QString li = QStringLiteral("<li><a href=\"%1\">%2</a>").arg(escaped(href), escaped(node.title));
        if (!node.children.isEmpty()) {
            li += QStringLiteral("\n%1<ul>\n%2").arg(QString(level, u'\t'), QString(level + 1, u'\t'));
            for (const int child : node.children)
                li += entry(child, level + 1);
            li += QStringLiteral("</ul>\n%1").arg(QString(level, u'\t'));
        }
        return li + QStringLiteral("</li>\n") + QString(level, u'\t');
    };
    QString list;
    for (qsizetype i = 0; i < m_toc.size(); ++i) {
        if (m_toc.at(i).parent < 0)
            list += entry(int(i), 0);
    }
    const QString page = QStringLiteral(
                             "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
                             "<head>\n"
                             "  <title>%1</title>\n"
                             "  <style type=\"text/css\">\n"
                             "  li { list-style-type: none }\n"
                             "  a { text-decoration: none }\n"
                             "  a:hover { color: red }\n"
                             "  </style>\n"
                             "</head>\n"
                             "<body id=\"calibre_generated_inline_toc\">\n"
                             "<h2>%1</h2>\n"
                             "<ul>\n\t%2</ul>\n"
                             "</body>\n"
                             "</html>\n")
                             .arg(title, list);
    std::unique_ptr<Node> document = XmlTree::parse(page.toUtf8());
    if (!document)
        return;
    m_zip.names.append(name);
    m_zip.data.insert(name, page.toUtf8());
    m_items.insert(name, m_manifest.size());
    m_manifest.append({ QStringLiteral("contents"), name, QStringLiteral("application/xhtml+xml"), {} });
    m_spine.push_back({ name, std::move(document) });
    m_guide.append({ QStringLiteral("toc"), QStringLiteral("Table of Contents"), name, {} });
}

QByteArray Writer::write()
{
    const QString opfFolder = m_opfName.contains(u'/') ? m_opfName.section(u'/', 0, -2) + u'/' : QString();
    // The library's cover in place of the book's, or added.
    if (const QByteArray &cover = m_options.metadata.cover; !cover.isEmpty() && !imageFormat(cover).isEmpty()) {
        if (m_coverName.isEmpty()) {
            m_coverName = EpubPackage::freeName(m_zip, opfFolder + QStringLiteral("cover.") + imageFormat(cover));
            m_zip.names.append(m_coverName);
            m_items.insert(m_coverName, m_manifest.size());
            m_manifest.append({ QStringLiteral("cover"), m_coverName, QStringLiteral("image/") + imageFormat(cover), {} });
        }
        m_zip.data.insert(m_coverName, cover);
        m_manifest[m_items.value(m_coverName)].mediaType = QStringLiteral("image/") + imageFormat(cover);
    }
    if (m_direction.isEmpty() && !m_options.metadata.languages.isEmpty() && rightToLeft(m_options.metadata.languages.first()))
        m_direction = QStringLiteral("rtl");

    // DataURL: pictures written into the pages as their own files.
    for (Page &page : m_spine) {
        for (Node *img : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "img"); })) {
            const QString src = img->attribute(QStringLiteral("src"));
            if (!src.startsWith(QLatin1String("data:image/")))
                continue;
            const QString header = src.section(u',', 0, 0);
            const QString encoded = src.section(u',', 1);
            const QByteArray data = header.contains(QLatin1String(";base64"))
                                        ? QByteArray::fromBase64(encoded.toLatin1().simplified().replace(' ', ""))
                                        : QByteArray::fromPercentEncoding(encoded.toLatin1());
            const QString format = imageFormat(data);
            if (format.isEmpty())
                continue;
            const QString name = EpubPackage::freeName(m_zip, opfFolder + QStringLiteral("data-url-image.") + format);
            m_zip.names.append(name);
            m_zip.data.insert(name, data);
            m_items.insert(name, m_manifest.size());
            m_manifest.append({ name, name, QStringLiteral("image/") + format, {} });
            img->setAttribute(QStringLiteral("src"), EpubPackage::relativeHref(name, page.name));
        }
    }

    // remove_duplicate_anchors(): an id or a name met before in the page
    // left out.
    for (Page &page : m_spine) {
        QSet<QString> seen;
        for (Node *tag : elements(page.document.get(), [](const Node &n) {
                 return n.attributes.hasAttribute(QStringLiteral("id")) || n.attributes.hasAttribute(QStringLiteral("name"));
             })) {
            for (const QString key : { QStringLiteral("id"), QStringLiteral("name") }) {
                if (!tag->attributes.hasAttribute(key))
                    continue;
                const QString anchor = tag->attribute(key);
                if (seen.contains(anchor))
                    tag->removeAttribute(key);
                else
                    seen.insert(anchor);
            }
        }
    }

    addResources();
    removeHtmlCover();
    addInlineToc();

    // cleanup_markup(): empty scripts out, and the aids and cids the writer
    // gives.
    for (Page &page : m_spine) {
        for (Node *script : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "script"); })) {
            if (ownText(*script).isEmpty() && !script->attributes.hasAttribute(QStringLiteral("src")))
                remove(script);
        }
        for (Node *tag : elements(page.document.get(), [](const Node &) { return true; })) {
            tag->removeAttribute(QStringLiteral("aid"));
            tag->removeAttribute(QStringLiteral("cid"));
        }
    }

    // replace_resource_links(): pictures and fonts by their records.
    QHash<QString, QString> styleSheets;
    std::map<QString, std::unique_ptr<Node>> svgFiles;
    const auto replaceInImages = [this](Node *root, const QString &from) {
        for (Node *tag : elements(root, [](const Node &n) {
                 return isXhtml(n, "img") || (n.isElement("image") && n.namespaceUri == svgNamespace);
             })) {
            for (QXmlStreamAttribute &attribute : tag->attributes) {
                const QString key = localPart(attribute.qualifiedName()).toLower();
                if (key == QLatin1String("src") || key == QLatin1String("href"))
                    attribute = QXmlStreamAttribute(attribute.qualifiedName().toString(), pointer(from, attribute.value().toString()));
            }
        }
    };
    const auto urls = [this](const QString &from) { return [this, from](const QString &url) { return pointer(from, url); }; };
    const auto unchanged = [](const QString &href) { return href; };
    for (Page &page : m_spine) {
        replaceInImages(page.document.get(), page.name);
        for (Node *style : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "style"); })) {
            const QString css = ownText(*style);
            if (!css.isEmpty())
                setOwnText(style, rewriteCss(css, urls(page.name), unchanged));
        }
    }
    for (const Item &item : std::as_const(m_manifest)) {
        if (item.mediaType == QLatin1String("text/css")) {
            styleSheets.insert(item.name, rewriteCss(QString::fromUtf8(m_zip.data.value(item.name)), urls(item.name), unchanged));
        } else if (item.mediaType == QLatin1String("image/svg+xml")) {
            if (std::unique_ptr<Node> svg = XmlTree::parse(m_zip.data.value(item.name)); svg && XmlTree::documentElement(svg.get())) {
                replaceInImages(svg.get(), item.name);
                svgFiles.emplace(item.name, std::move(svg));
            }
        }
    }

    // extract_css_into_flows(): each style sheet a flow, the page's styles
    // too, each once.
    QList<QByteArray> flows{ QByteArray() };
    QHash<QString, int> sheets;
    QList<QString> sheetNames;
    for (const Item &item : std::as_const(m_manifest)) {
        if (styleSheets.contains(item.name)) {
            sheets.insert(item.name, int(flows.size()));
            sheetNames.append(item.name);
            flows.append(QByteArray());
        }
    }
    const auto flowHref = [](int flow, const char *mime) { return QStringLiteral("kindle:flow:%1?mime=%2").arg(toBase32(quint64(flow), 4), QLatin1String(mime)); };
    const auto imports = [&sheets, &flowHref](const QString &from) {
        return [&sheets, &flowHref, from](const QString &href) {
            const auto flow = sheets.constFind(resolve(href, from));
            return flow == sheets.cend() ? href : flowHref(*flow, "text/css");
        };
    };
    const auto noUrls = [](const QString &url) { return url; };
    QList<QString> inlineSheets;
    QHash<QString, QList<Node *>> inlineLinks;
    for (Page &page : m_spine) {
        for (Node *link : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "link") && n.attributes.hasAttribute(QStringLiteral("href")); })) {
            const auto flow = sheets.constFind(resolve(link->attribute(QStringLiteral("href")), page.name));
            if (flow != sheets.cend())
                link->setAttribute(QStringLiteral("href"), flowHref(*flow, "text/css"));
        }
        for (Node *style : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "style"); })) {
            const QString raw = ownText(*style);
            if (raw.trimmed().isEmpty()) {
                remove(style);
                continue;
            }
            const QString css = rewriteCss(raw, noUrls, imports(page.name));
            Node *parent = style->parent;
            const size_t at = parent->indexOf(style);
            auto link = XmlTree::element(*style, QStringLiteral("link"));
            link->setAttribute(QStringLiteral("type"), QStringLiteral("text/css"));
            link->setAttribute(QStringLiteral("rel"), QStringLiteral("stylesheet"));
            parent->take(at);
            Node *added = parent->insert(at, std::move(link));
            parent->insert(at + 1, XmlTree::textNode(QStringLiteral("\n")));
            if (!inlineLinks.contains(css))
                inlineSheets.append(css);
            inlineLinks[css].append(added);
        }
    }
    for (const QString &css : std::as_const(inlineSheets)) {
        const int flow = int(flows.size());
        flows.append(css.toUtf8());
        for (Node *link : inlineLinks.value(css))
            link->setAttribute(QStringLiteral("href"), flowHref(flow, "text/css"));
    }
    for (const QString &name : std::as_const(sheetNames))
        flows[sheets.value(name)] = rewriteCss(styleSheets.value(name), noUrls, imports(name)).toUtf8();

    // extract_svg_into_flows(): the SVG files and the pages' pictures in SVG
    // as flows, shown by an image.
    QHash<QString, int> svgFlows;
    for (const Item &item : std::as_const(m_manifest)) {
        if (const auto svg = svgFiles.find(item.name); svg != svgFiles.end()) {
            svgFlows.insert(item.name, int(flows.size()));
            flows.append(XmlTree::serialize(*svg->second));
        }
    }
    for (Page &page : m_spine) {
        Node *root = XmlTree::documentElement(page.document.get());
        // The outermost ones, whose subtrees go whole.
        QList<Node *> pictures;
        for (Node *svg : elements(page.document.get(), [](const Node &n) { return n.isElement("svg") && n.namespaceUri == svgNamespace; })) {
            bool nested = false;
            for (const Node *at = svg->parent; at && !nested; at = at->parent)
                nested = at->kind == Node::Kind::Element && at->isElement("svg") && at->namespaceUri == svgNamespace;
            if (!nested)
                pictures.append(svg);
        }
        for (Node *svg : std::as_const(pictures)) {
            const int flow = int(flows.size());
            flows.append(serializeSubtree(*svg));
            auto img = XmlTree::element(*root, QStringLiteral("img"));
            img->setAttribute(QStringLiteral("src"), flowHref(flow, "image/svg+xml"));
            Node *parent = svg->parent;
            const size_t at = parent->indexOf(svg);
            parent->take(at);
            parent->insert(at, std::move(img));
        }
        for (Node *img : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "img") && n.attributes.hasAttribute(QStringLiteral("src")); })) {
            const auto flow = svgFlows.constFind(resolve(img->attribute(QStringLiteral("src")), page.name));
            if (flow != svgFlows.cend())
                img->setAttribute(QStringLiteral("src"), flowHref(*flow, "image/svg+xml"));
        }
    }

    // replace_internal_links_with_placeholders(): a link to a page of the
    // book a placeholder, set once the chunks tell where its target is.
    QSet<QString> spineNames;
    for (const Page &page : std::as_const(m_spine))
        spineNames.insert(page.name);
    QHash<QString, QPair<QString, QString>> linkTargets;
    quint64 count = 0;
    for (Page &page : m_spine) {
        for (Node *a : elements(page.document.get(), [](const Node &n) { return isXhtml(n, "a") && n.attributes.hasAttribute(QStringLiteral("href")); })) {
            ++count;
            const QString href = a->attribute(QStringLiteral("href"));
            const QString name = resolve(href, page.name);
            if (!spineNames.contains(name))
                continue;
            const QString placeholder = QStringLiteral("kindle:pos:fid:0000:off:") + toBase32(count, 10);
            linkTargets.insert(placeholder, { name, QUrl::fromPercentEncoding(href.section(u'#', 1).toUtf8()) });
            a->setAttribute(QStringLiteral("href"), placeholder);
        }
    }

    // The pages without namespaces, with their aids.
    QHash<QPair<QString, QString>, QString> ids;
    std::vector<std::unique_ptr<Element>> pages;
    int cid = 0;
    for (size_t i = 0; i < m_spine.size(); ++i) {
        std::unique_ptr<Element> root = convert(*XmlTree::documentElement(m_spine.at(i).document.get()), true);
        quint64 next = 0;
        insertAids(root.get(), false, m_spine.at(i).name, quint64(i) * 1000000, &next, &cid, &ids);
        pages.push_back(std::move(root));
    }
    // chunk_it_up(): the aid each placeholder goes to, else the page's body.
    QHash<QString, QString> links;
    for (auto target = linkTargets.cbegin(); target != linkTargets.cend(); ++target) {
        const QString aid = ids.value(target.value(), ids.value({ target.value().first, QString() }));
        if (!aid.isEmpty())
            links.insert(target.key(), aid);
    }
    const Kf8Skeleton::Text text = Kf8Skeleton::chunk(pages, links);
    flows[0] = text.text;

    // The place of an aid, or of the page's body, or the first chunk.
    const auto placeOf = [&ids, &text](const QString &name, const QString &fragment) -> std::optional<Kf8Skeleton::Text::Place> {
        const QString aid = ids.value({ name, fragment }, ids.value({ name, QString() }));
        if (aid.isEmpty() || !text.places.contains(aid))
            return std::nullopt;
        return text.places.value(aid);
    };

    // create_text_records() and create_fdst_records().
    QByteArray all;
    for (const QByteArray &flow : std::as_const(flows))
        all += flow;
    QList<qsizetype> lengths;
    QList<QByteArray> records = MobiFormat::textRecords(all, &lengths);
    QByteArray fdst = QByteArray("FDST") + MobiFormat::pack32(12) + MobiFormat::pack32(quint32(flows.size()));
    quint32 flowStart = 0;
    for (const QByteArray &flow : std::as_const(flows)) {
        fdst += MobiFormat::pack32(flowStart) + MobiFormat::pack32(flowStart + quint32(flow.size()));
        flowStart += quint32(flow.size());
    }

    // create_indices(): the table of contents by depth then place, as the
    // Kindle needs it to go from a section to the next.
    Kf8Book::Parts parts;
    QList<Kf8Index::NcxEntry> ncx;
    for (qsizetype i = 0; i < m_toc.size(); ++i) {
        const TocNode &node = m_toc.at(i);
        Kf8Index::NcxEntry entry;
        entry.index = int(i);
        entry.label = node.title.isEmpty() ? m_options.unknown : node.title;
        entry.depth = node.depth;
        if (const std::optional<Kf8Skeleton::Text::Place> place = placeOf(node.name, node.fragment)) {
            entry.position = place->position;
            entry.fidOffset = place->offset;
            entry.offset = place->textOffset;
        } else {
            entry.offset = text.chunks.isEmpty() ? 0 : text.chunks.first().insertPosition;
        }
        ncx.append(entry);
    }
    if (!ncx.isEmpty()) {
        QList<Kf8Index::NcxEntry> linear = ncx;
        std::stable_sort(linear.begin(), linear.end(), [](const auto &a, const auto &b) {
            return a.depth != b.depth ? a.depth < b.depth : a.offset < b.offset;
        });
        QHash<int, int> newIndex;
        for (qsizetype i = 0; i < linear.size(); ++i)
            newIndex.insert(linear.at(i).index, int(i));
        for (Kf8Index::NcxEntry &entry : linear) {
            const TocNode &node = m_toc.at(entry.index);
            if (!node.children.isEmpty()) {
                entry.firstChild = newIndex.value(node.children.first());
                entry.lastChild = newIndex.value(node.children.last());
            }
            if (node.parent >= 0)
                entry.parent = newIndex.value(node.parent);
            entry.index = newIndex.value(entry.index);
        }
        for (Kf8Index::NcxEntry &entry : linear) {
            quint32 next = quint32(flows.first().size());
            for (const Kf8Index::NcxEntry &other : std::as_const(linear)) {
                if (other.depth <= entry.depth && other.offset > entry.offset)
                    next = std::min(next, other.offset);
            }
            entry.length = next - entry.offset;
        }
        const QList<QByteArray> sequences = Kf8Index::trailingByteSequences(linear, lengths);
        for (qsizetype i = 0; i < records.size() && i < sequences.size(); ++i)
            records[i] += sequences.at(i);
        parts.trailingByteSequences = true;
        parts.ncxIndex = Kf8Index::ncxIndex(linear);
    }

    // create_guide().
    QList<Kf8Index::GuideReference> guide;
    for (const GuideReference &reference : std::as_const(m_guide)) {
        const std::optional<Kf8Skeleton::Text::Place> place = placeOf(reference.name, reference.fragment);
        if (!place)
            continue;
        if (startsTheBook(reference))
            parts.start = place->textOffset;
        guide.append({ reference.title.isEmpty() ? m_options.unknown : reference.title, reference.type, place->position, place->offset });
    }
    std::stable_sort(guide.begin(), guide.end(), [](const auto &a, const auto &b) { return a.type < b.type; });

    // Resources.serialize(): the pictures no page shows as a placeholder.
    QSet<int> used = m_usedImageIndices;
    for (const QString &name : std::as_const(m_usedImages))
        used.insert(m_itemMap.value(name) - 1);
    for (const int index : std::as_const(m_imageIndices)) {
        if (!used.contains(index))
            m_records[index] = placeholderGif;
    }

    parts.text = records;
    parts.textLength = all.size();
    parts.chunkIndex = Kf8Index::chunkIndex(text.chunks);
    parts.skeletonIndex = Kf8Index::skeletonIndex(text.skeletons);
    if (!guide.isEmpty())
        parts.guideIndex = Kf8Index::guideIndex(guide);
    parts.resources = m_records;
    parts.fonts = m_fonts;
    parts.cover = m_coverOffset;
    parts.thumbnail = m_thumbnailOffset;
    parts.fdst = fdst;
    parts.flows = int(flows.size());
    parts.pageProgressionDirection = m_direction;
    // Without it, the Kindle turns no pages right to left.
    if (m_direction == QLatin1String("rtl"))
        parts.primaryWritingMode = QStringLiteral("horizontal-rl");
    parts.metadata = m_options.metadata;
    parts.options = m_options.mobi;
    parts.unknown = m_options.unknown;
    return Kf8Book::write(parts);
}

} // namespace

Options optionsFor(const Book &book)
{
    QByteArray cover;
    QFile file(book.coverFile);
    if (!book.coverFile.isEmpty() && file.open(QIODevice::ReadOnly))
        cover = file.readAll();
    Options options;
    options.metadata = MobiMetadata::metadataFor(book, cover);
    return options;
}

bool write(const QString &epub, const QString &azw3, const Options &options, QString *error)
{
    Writer writer(options);
    if (!writer.load(epub, error))
        return false;
    const QByteArray book = writer.write();
    QSaveFile file(azw3);
    if (!file.open(QIODevice::WriteOnly) || file.write(book) != book.size() || !file.commit()) {
        *error = QObject::tr("Cannot write %1.").arg(azw3);
        return false;
    }
    return true;
}

} // namespace Kf8
