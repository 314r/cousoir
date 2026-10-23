// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/oeb/polish/kepubify.py: kepubify_container(),
//   add_kobo_spans(), wrap_body_contents(), add_style_and_script(),
//   remove_kobo_markup_from_html(), add_dummy_title_page(),
//   is_probably_a_title_page()
// - resources/templates/kobo.js, the script the book carries
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kepub.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QUrl>

#include <memory>

#include <zip.h>

#include "book.h"
#include "xml_tree.h"

namespace Kepub {

using namespace XmlTree;

namespace {

const QString xhtmlNamespace = QStringLiteral("http://www.w3.org/1999/xhtml");
const QString koboCssId = QStringLiteral("kobostylehacks");
const QString koboSpanStyleId = QStringLiteral("koboSpanStyle");
const QString koboCss = QStringLiteral("div#book-inner { margin-top: 0; margin-bottom: 0; }");
const QString koboSpanCss = QStringLiteral(".koboSpan { -webkit-text-combine: inherit; }");
const QString outerDivId = QStringLiteral("book-columns");
const QString innerDivId = QStringLiteral("book-inner");
const QString koboSpanClass = QStringLiteral("koboSpan");
const QString dummyTitlePage = QStringLiteral("kobo-title-page-generated-by-calibre");
const QString dummyCoverImage = QStringLiteral("kobo-cover-image-generated-by-calibre");

// --- Kobo's markup of a page ------------------------------------------------

// Takes `node` out of its parent, its children in its place.
void unwrap(Node *node)
{
    Node *parent = node->parent;
    const size_t at = parent->indexOf(node);
    std::unique_ptr<Node> taken = parent->take(at);
    size_t to = at;
    for (auto &child : taken->children)
        parent->insert(to++, std::move(child));
    // Texts that now meet are one text.
    for (size_t i = 1; i < parent->children.size();) {
        if (parent->children[i]->kind == Node::Kind::Text && parent->children[i - 1]->kind == Node::Kind::Text) {
            parent->children[i - 1]->text += parent->children[i]->text;
            parent->children[i - 1]->origin = nullptr;
            parent->take(i);
        } else {
            ++i;
        }
    }
}

// remove_kobo_markup_from_html(): what an earlier kepubify left.
void removeKoboMarkup(Node *root)
{
    for (Node *node : findAll(root, [](const Node &n) {
             return (n.isElement("style") && (n.attribute(QStringLiteral("id")) == koboCssId
                                              || n.attribute(QStringLiteral("id")) == koboSpanStyleId
                                              || n.attribute(QStringLiteral("id")) == QLatin1String("kepubify-extra-css")))
                 || (n.isElement("script") && n.attribute(QStringLiteral("src")).section(u'/', -1) == QLatin1String("kobo.js"));
         })) {
        node->parent->take(node->parent->indexOf(node));
    }
    for (Node *node : findAll(root, [](const Node &n) {
             return n.isElement("span") && n.attribute(QStringLiteral("class")) == koboSpanClass
                 && n.attribute(QStringLiteral("id")).startsWith(QLatin1String("kobo."));
         })) {
        unwrap(node);
    }
    for (Node *node : findAll(root, [](const Node &n) {
             return n.isElement("div") && (n.attribute(QStringLiteral("id")) == innerDivId);
         })) {
        unwrap(node);
    }
    for (Node *node : findAll(root, [](const Node &n) {
             return n.isElement("div") && (n.attribute(QStringLiteral("id")) == outerDivId);
         })) {
        unwrap(node);
    }
}

void addStyleAndScript(Node *html, const QString &scriptHref)
{
    Node *parent = nullptr;
    for (const auto &child : html->children) {
        if (child->isElement("head"))
            parent = child.get();
    }
    if (!parent) {
        for (const auto &child : html->children) {
            if (child->isElement("body"))
                parent = child.get();
        }
    }
    if (!parent)
        return;
    const auto style = [&](const QString &id, const QString &css) {
        auto node = element(*parent, QStringLiteral("style"));
        node->setAttribute(QStringLiteral("type"), QStringLiteral("text/css"));
        node->setAttribute(QStringLiteral("id"), id);
        node->append(textNode(css));
        parent->append(std::move(node));
    };
    style(koboCssId, koboCss);
    style(koboSpanStyleId, koboSpanCss);
    auto script = element(*parent, QStringLiteral("script"));
    script->setAttribute(QStringLiteral("type"), QStringLiteral("text/javascript"));
    script->setAttribute(QStringLiteral("src"), scriptHref);
    parent->append(std::move(script));
}

// wrap_body_contents(): the body's content in book-columns > book-inner.
Node *wrapBody(Node *body)
{
    for (Node *node : findAll(body, [](const Node &n) {
             return n.kind == Node::Kind::Element
                 && (n.attribute(QStringLiteral("id")) == outerDivId || n.attribute(QStringLiteral("id")) == innerDivId);
         })) {
        node->removeAttribute(QStringLiteral("id"));
    }
    // Calibre's parser drops the spaces the body starts with.
    if (!body->children.empty() && body->children.front()->kind == Node::Kind::Text
        && body->children.front()->text.trimmed().isEmpty())
        body->take(0);
    auto outer = element(*body, QStringLiteral("div"));
    outer->setAttribute(QStringLiteral("id"), outerDivId);
    auto inner = element(*body, QStringLiteral("div"));
    inner->setAttribute(QStringLiteral("id"), innerDivId);
    for (auto &child : body->children)
        inner->append(std::move(child));
    body->children.clear();
    Node *innerNode = outer->append(std::move(inner));
    body->append(std::move(outer));
    return innerNode;
}

// add_kobo_spans(): each sentence of the text in a span numbered by its
// paragraph, each image in a span of its own.
class Spanner
{
public:
    void run(Node *inner) { visit(inner); }

private:
    int m_paragraph = 0;
    int m_segment = 0;
    bool m_nextParagraph = true;

    std::unique_ptr<Node> span(const Node &parent)
    {
        ++m_segment;
        auto node = element(parent, QStringLiteral("span"));
        node->setAttribute(QStringLiteral("class"), koboSpanClass);
        node->setAttribute(QStringLiteral("id"), QStringLiteral("kobo.%1.%2").arg(m_paragraph).arg(m_segment));
        return node;
    }

    static bool skipped(const Node &node)
    {
        static const QSet<QString> tags{ QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("atom"),
                                         QStringLiteral("pre"),    QStringLiteral("audio"), QStringLiteral("video"),
                                         QStringLiteral("svg"),    QStringLiteral("math") };
        return node.localName.isEmpty() || tags.contains(node.localName.toLower());
    }

    static bool block(const Node &node)
    {
        static const QSet<QString> tags{ QStringLiteral("p"),  QStringLiteral("ol"), QStringLiteral("ul"),
                                         QStringLiteral("table"), QStringLiteral("h1"), QStringLiteral("h2"),
                                         QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
                                         QStringLiteral("h6") };
        return tags.contains(node.localName.toLower());
    }

    // An image in a span, a paragraph of its own.
    void wrapChild(Node *child)
    {
        m_nextParagraph = false;
        ++m_paragraph;
        m_segment = 0;
        Node *parent = child->parent;
        const size_t at = parent->indexOf(child);
        auto wrapper = span(*parent);
        wrapper->append(parent->take(at));
        parent->insert(at, std::move(wrapper));
    }

    // The text at `at` of `parent` in spans; the index of the last node
    // put there.
    size_t wrapText(Node *parent, size_t at)
    {
        const QString text = parent->children[at]->text;
        if (m_nextParagraph) {
            ++m_paragraph;
            m_segment = 0;
            m_nextParagraph = false;
        }
        qsizetype leading = 0;
        while (leading < text.size() && text.at(leading).isSpace())
            ++leading;
        if (leading == text.size()) {
            // A block with only spaces has them in a span.
            if (at == 0 && parent->children.size() == 1) {
                auto wrapper = span(*parent);
                wrapper->append(parent->take(at));
                parent->insert(at, std::move(wrapper));
            }
            return at;
        }
        const std::unique_ptr<Node> taken = parent->take(at);
        // Each piece keeps where it was in the text it was cut from.
        const auto piece = [&taken](qsizetype start, qsizetype length) {
            auto node = textNode(taken->text.mid(start, length));
            node->origin = taken->origin;
            node->originOffset = taken->originOffset + start;
            return node;
        };
        QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, text);
        qsizetype start = 0;
        size_t to = at;
        for (qsizetype end = finder.toNextBoundary(); end > 0; end = finder.toNextBoundary()) {
            auto wrapper = span(*parent);
            wrapper->append(piece(start, end - start));
            parent->insert(to++, std::move(wrapper));
            start = end;
        }
        if (start < text.size()) {
            auto wrapper = span(*parent);
            wrapper->append(piece(start, -1));
            parent->insert(to++, std::move(wrapper));
        }
        return to - 1;
    }

    void visit(Node *node)
    {
        if (node->isElement("img")) {
            wrapChild(node);
            return;
        }
        if (!m_nextParagraph && block(*node))
            m_nextParagraph = true;
        for (size_t i = 0; i < node->children.size(); ++i) {
            Node *child = node->children[i].get();
            if (child->kind == Node::Kind::Text)
                i = wrapText(node, i);
            else if (child->kind == Node::Kind::Element && !skipped(*child))
                visit(child);
        }
    }
};

void addKoboMarkup(Node *root, const QString &scriptHref)
{
    Node *html = documentElement(root);
    if (!html)
        return;
    removeKoboMarkup(root);
    addStyleAndScript(html, scriptHref);
    for (const auto &child : html->children) {
        if (child->isElement("body"))
            Spanner().run(wrapBody(child.get()));
    }
}

// The text of a page, its title left out.
QString pageText(const Node &node)
{
    if (node.kind == Node::Kind::Text || node.kind == Node::Kind::Cdata)
        return node.text;
    if (node.isElement("title") || node.isElement("script") || node.isElement("style"))
        return {};
    QString text;
    for (const auto &child : node.children)
        text += pageText(*child);
    return text;
}

// is_probably_a_title_page(): a cover's title, or one image and little text.
bool isProbablyTitlePage(Node *root)
{
    for (Node *title : findAll(root, [](const Node &n) { return n.isElement("title"); })) {
        QString text;
        for (const auto &child : title->children)
            text += child->text;
        if (text.toLower().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).contains(QStringLiteral("cover")))
            return true;
    }
    QString text = pageText(*root);
    text.remove(QRegularExpression(QStringLiteral("\\s+")));
    const qsizetype images = findAll(root, [](const Node &n) { return n.isElement("img") || n.isElement("svg"); }).size();
    return (images == 1 && text.size() <= 10) || (text.size() <= 50 && images < 1);
}

// --- The book ----------------------------------------------------------------

struct Zip
{
    // The book's files, in their order.
    QStringList names;
    QHash<QString, QByteArray> data;
};

bool readZip(const QString &path, Zip *zip, QString *error)
{
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &code);
    if (!archive) {
        *error = QObject::tr("%1 is not a ZIP archive.").arg(QFileInfo(path).fileName());
        return false;
    }
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive, zip_uint64_t(i), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_NAME))
            continue;
        const QString name = QString::fromUtf8(stat.name);
        if (name.endsWith(u'/'))
            continue;
        zip_file_t *file = zip_fopen_index(archive, zip_uint64_t(i), 0);
        if (!file)
            continue;
        QByteArray bytes(qsizetype(stat.size), Qt::Uninitialized);
        const zip_int64_t read = zip_fread(file, bytes.data(), stat.size);
        zip_fclose(file);
        if (read < 0 || zip_uint64_t(read) != stat.size) {
            zip_discard(archive);
            *error = QObject::tr("Cannot read %1 in %2.").arg(name, QFileInfo(path).fileName());
            return false;
        }
        zip->names.append(name);
        zip->data.insert(name, bytes);
    }
    zip_discard(archive);
    return true;
}

bool writeZip(const QString &path, const Zip &zip, QString *error)
{
    QFile::remove(path);
    int code = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &code);
    if (!archive) {
        *error = QObject::tr("Cannot write %1.").arg(path);
        return false;
    }
    // The mimetype first and stored, as EPUB wants.
    QStringList names = zip.names;
    if (names.removeAll(QStringLiteral("mimetype")) > 0)
        names.prepend(QStringLiteral("mimetype"));
    for (const QString &name : std::as_const(names)) {
        const QByteArray &bytes = zip.data[name];
        zip_source_t *source = zip_source_buffer(archive, bytes.constData(), zip_uint64_t(bytes.size()), 0);
        const zip_int64_t index = source ? zip_file_add(archive, name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            if (source)
                zip_source_free(source);
            zip_discard(archive);
            *error = QObject::tr("Cannot write %1.").arg(path);
            return false;
        }
        const bool store = name == QLatin1String("mimetype");
        zip_set_file_compression(archive, zip_uint64_t(index), store ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    }
    if (zip_close(archive) != 0) {
        *error = QObject::tr("Cannot write %1: %2").arg(path, QString::fromUtf8(zip_strerror(archive)));
        zip_discard(archive);
        return false;
    }
    return true;
}

// A name's address from another name of the book, both from its root.
QString relativeHref(const QString &target, const QString &from)
{
    const QString base = from.contains(u'/') ? from.section(u'/', 0, -2) : QString();
    const QString relative = QDir(QStringLiteral("/") + base).relativeFilePath(QStringLiteral("/") + target);
    return QString::fromUtf8(QUrl::toPercentEncoding(relative, "/-._~"));
}

// A name not taken yet: `name`, else name-1, name-2...
QString freeName(const Zip &zip, const QString &name)
{
    if (!zip.data.contains(name))
        return name;
    const QString base = name.section(u'.', 0, -2);
    const QString extension = name.section(u'.', -1);
    for (int i = 1;; ++i) {
        const QString candidate = QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(extension);
        if (!zip.data.contains(candidate))
            return candidate;
    }
}

QString escapeXml(const QString &text)
{
    return text.toHtmlEscaped();
}

// The library's metadata in the package's: its title, authors, languages,
// publisher, tags, description, identifiers and series, those of the book
// before removed, as Calibre's metadata writer replaces them.
void embedMetadata(Node *package, const Metadata &metadata)
{
    Node *block = childElement(package, "metadata");
    if (!block)
        return;
    const QString dc = QStringLiteral("http://purl.org/dc/elements/1.1/");
    // The prefix the package gives Dublin Core, "dc:" mostly.
    QString prefix = QStringLiteral("dc:");
    for (const auto &child : block->children) {
        if (child->kind == Node::Kind::Element && child->namespaceUri == dc) {
            prefix = child->prefix();
            break;
        }
    }
    const QString uniqueId = package->attribute(QStringLiteral("unique-identifier"));
    const QSet<QString> replaced{ QStringLiteral("title"),   QStringLiteral("creator"), QStringLiteral("language"),
                                  QStringLiteral("publisher"), QStringLiteral("subject"), QStringLiteral("description") };
    for (size_t i = 0; i < block->children.size();) {
        const Node &child = *block->children[i];
        const QString name = child.attribute(QStringLiteral("name"));
        const QString refines = child.attribute(QStringLiteral("refines"));
        const bool dcReplaced = child.kind == Node::Kind::Element && child.namespaceUri == dc
            && replaced.contains(child.localName);
        const bool series = child.isElement("meta")
            && (name == QLatin1String("calibre:series") || name == QLatin1String("calibre:series_index"));
        // OPF 3 refinements of what goes: file-as, role, display-seq.
        const bool refinement = child.isElement("meta") && refines.startsWith(u'#') && refines.mid(1) != uniqueId;
        if (dcReplaced || series || refinement)
            block->take(i);
        else
            ++i;
    }
    const auto add = [&](const QString &localName, const QString &text, const QList<QPair<QString, QString>> &attributes = {}) {
        auto node = std::make_unique<Node>();
        node->localName = localName;
        node->qualifiedName = prefix + localName;
        node->namespaceUri = dc;
        for (const auto &[name, value] : attributes)
            node->setAttribute(name, value);
        if (!text.isEmpty())
            node->append(textNode(text));
        block->append(textNode(QStringLiteral("\n    ")));
        block->append(std::move(node));
    };
    const auto meta = [&](const QString &name, const QString &content) {
        auto node = element(*block, QStringLiteral("meta"));
        // The package's own namespace, OPF's, unprefixed mostly.
        node->namespaceUri = package->namespaceUri;
        node->qualifiedName = package->prefix() + QStringLiteral("meta");
        node->setAttribute(QStringLiteral("name"), name);
        node->setAttribute(QStringLiteral("content"), content);
        block->append(textNode(QStringLiteral("\n    ")));
        block->append(std::move(node));
    };
    const bool opf2 = package->attribute(QStringLiteral("version")).startsWith(u'2');
    add(QStringLiteral("title"), metadata.title);
    for (const QString &author : metadata.authors) {
        // OPF 2 names the role and sort on the creator.
        if (opf2)
            add(QStringLiteral("creator"), author,
                { { QStringLiteral("opf:role"), QStringLiteral("aut") }, { QStringLiteral("opf:file-as"), metadata.authorSort } });
        else
            add(QStringLiteral("creator"), author);
    }
    for (const QString &language : metadata.languages)
        add(QStringLiteral("language"), language);
    if (!metadata.publisher.isEmpty())
        add(QStringLiteral("publisher"), metadata.publisher);
    for (const QString &tag : metadata.tags)
        add(QStringLiteral("subject"), tag);
    if (!metadata.comments.isEmpty())
        add(QStringLiteral("description"), metadata.comments);
    if (!metadata.series.isEmpty()) {
        meta(QStringLiteral("calibre:series"), metadata.series);
        QString index = QString::number(metadata.seriesIndex, 'f', 2);
        while (index.endsWith(u'0'))
            index.chop(1);
        if (index.endsWith(u'.'))
            index.chop(1);
        meta(QStringLiteral("calibre:series_index"), index);
    }
    // The ISBN the library has, beside the book's own identifiers.
    for (const auto &[type, value] : metadata.identifiers) {
        if (type.compare(QLatin1String("isbn"), Qt::CaseInsensitive) == 0)
            add(QStringLiteral("identifier"), QStringLiteral("urn:isbn:") + value);
    }
    block->append(textNode(QStringLiteral("\n  ")));
}

} // namespace

Options optionsFor(const Book &book)
{
    Options options;
    options.title = book.title;
    options.authors = book.authors.join(QStringLiteral(" & "));
    QFile cover(book.coverFile);
    if (!book.coverFile.isEmpty() && cover.open(QIODevice::ReadOnly))
        options.cover = cover.readAll();
    Metadata metadata;
    metadata.title = book.title;
    metadata.authors = book.authors;
    metadata.authorSort = book.authorSort;
    metadata.publisher = book.publisher;
    metadata.languages = book.languages;
    metadata.tags = book.tags;
    metadata.series = book.series;
    metadata.seriesIndex = book.seriesIndex;
    metadata.comments = book.comments;
    metadata.identifiers = book.identifiers;
    options.metadata = metadata;
    return options;
}

std::optional<QByteArray> kepubifyPage(const QByteArray &xhtml, const QString &scriptHref)
{
    std::unique_ptr<Node> root = parse(xhtml);
    if (!root || !documentElement(root.get()))
        return std::nullopt;
    addKoboMarkup(root.get(), scriptHref);
    return serialize(*root);
}

bool kepubify(const QString &epub, const QString &kepub, const Options &options, QString *error)
{
    Zip zip;
    if (!readZip(epub, &zip, error))
        return false;
    // The package, by META-INF/container.xml.
    std::unique_ptr<Node> container = parse(zip.data.value(QStringLiteral("META-INF/container.xml")));
    QString opfName;
    if (container) {
        // The first package there.
        for (const Node *rootfile : findAll(container.get(), [](const Node &n) { return n.isElement("rootfile"); })) {
            const QString name = rootfile->attribute(QStringLiteral("full-path"));
            if (zip.data.contains(name)) {
                opfName = name;
                break;
            }
        }
    }
    std::unique_ptr<Node> opf = opfName.isEmpty() ? nullptr : parse(zip.data.value(opfName));
    Node *package = opf ? documentElement(opf.get()) : nullptr;
    Node *manifest = package ? childElement(package, "manifest") : nullptr;
    Node *spine = package ? childElement(package, "spine") : nullptr;
    if (!manifest || !spine) {
        *error = QObject::tr("%1 has no readable package document.").arg(QFileInfo(epub).fileName());
        return false;
    }
    const QString opfFolder = opfName.contains(u'/') ? opfName.section(u'/', 0, -2) + u'/' : QString();
    const auto nameOf = [&opfFolder](const QString &href) {
        return QDir::cleanPath(opfFolder + QUrl::fromPercentEncoding(href.section(u'#', 0, 0).toUtf8()));
    };
    const auto hrefOf = [&opfName](const QString &name) { return relativeHref(name, opfName); };
    QHash<QString, Node *> items;
    for (const auto &child : manifest->children) {
        if (child->isElement("item"))
            items.insert(child->attribute(QStringLiteral("id")), child.get());
    }
    const auto addItem = [&](const QString &name, const QString &id, const QString &mediaType) {
        // In the manifest's own namespace and prefix.
        auto item = element(*manifest, QStringLiteral("item"));
        QString unique = id;
        for (int i = 1; items.contains(unique); ++i)
            unique = id + QStringLiteral("-%1").arg(i);
        item->setAttribute(QStringLiteral("id"), unique);
        item->setAttribute(QStringLiteral("href"), hrefOf(name));
        item->setAttribute(QStringLiteral("media-type"), mediaType);
        Node *added = manifest->append(std::move(item));
        items.insert(unique, added);
        return added;
    };

    // A cover image: the package's, else the library's.
    Node *coverItem = nullptr;
    for (Node *item : std::as_const(items)) {
        if (item->attribute(QStringLiteral("properties")).split(u' ').contains(QStringLiteral("cover-image")))
            coverItem = item;
    }
    Node *metadata = childElement(package, "metadata");
    if (!coverItem && metadata) {
        for (const auto &child : metadata->children) {
            if (child->isElement("meta") && child->attribute(QStringLiteral("name")) == QLatin1String("cover"))
                coverItem = items.value(child->attribute(QStringLiteral("content")));
        }
    }
    if (!coverItem && !options.cover.isEmpty()) {
        QBuffer buffer;
        buffer.setData(options.cover);
        const QByteArray format = QImageReader(&buffer).format();
        const QString extension = format == "png" ? QStringLiteral("png") : QStringLiteral("jpeg");
        const QString name = freeName(zip, dummyCoverImage + u'.' + extension);
        zip.names.append(name);
        zip.data.insert(name, options.cover);
        coverItem = addItem(name, QStringLiteral("cover-image"), QStringLiteral("image/") + extension);
    }
    // The library's cover in place of the book's.
    if (coverItem && !options.cover.isEmpty() && options.metadata) {
        const QString name = nameOf(coverItem->attribute(QStringLiteral("href")));
        if (zip.data.contains(name) && zip.data.value(name) != options.cover) {
            zip.data.insert(name, options.cover);
            QBuffer buffer;
            buffer.setData(options.cover);
            const QByteArray format = QImageReader(&buffer).format();
            if (!format.isEmpty())
                coverItem->setAttribute(QStringLiteral("media-type"),
                                        QStringLiteral("image/") + (format == "jpg" ? QStringLiteral("jpeg") : QString::fromLatin1(format)));
        }
    }
    if (options.metadata)
        embedMetadata(package, *options.metadata);
    if (coverItem) {
        // apply_unique_properties(): the cover is the only cover-image.
        for (Node *item : std::as_const(items)) {
            QStringList properties = item->attribute(QStringLiteral("properties")).split(u' ', Qt::SkipEmptyParts);
            const bool had = properties.removeAll(QStringLiteral("cover-image")) > 0;
            if (item == coverItem)
                properties.append(QStringLiteral("cover-image"));
            if (properties.isEmpty())
                item->removeAttribute(QStringLiteral("properties"));
            else if (had || item == coverItem)
                item->setAttribute(QStringLiteral("properties"), properties.join(u' '));
        }
    }

    // Kobo's script, at the root of the book.
    const QString scriptName = freeName(zip, QStringLiteral("kobo.js"));
    QFile script(QStringLiteral(":/cousoir/kobo/kobo.js"));
    if (!script.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Kobo's script is missing from Cousoir.");
        return false;
    }
    zip.names.append(scriptName);
    zip.data.insert(scriptName, script.readAll());
    addItem(scriptName, QStringLiteral("js-kobo.js"), QStringLiteral("application/javascript"));

    // A title page, when the book opens on none.
    QStringList spineNames;
    for (const auto &child : spine->children) {
        if (child->isElement("itemref") && child->attribute(QStringLiteral("linear")) != QLatin1String("no")) {
            if (Node *item = items.value(child->attribute(QStringLiteral("idref"))))
                spineNames.append(nameOf(item->attribute(QStringLiteral("href"))));
        }
    }
    bool coverPage = false;
    if (Node *guide = childElement(package, "guide")) {
        for (const auto &child : guide->children)
            coverPage = coverPage || (child->isElement("reference") && child->attribute(QStringLiteral("type")) == QLatin1String("cover"));
    }
    bool titlePage = false;
    if (!spineNames.isEmpty()) {
        const QString first = spineNames.first();
        if (first.contains(QLatin1String("cover")) || first.section(u'/', -1).contains(QLatin1String("title"))) {
            titlePage = true;
        } else if (std::unique_ptr<Node> page = parse(zip.data.value(first))) {
            titlePage = isProbablyTitlePage(page.get());
        }
    }
    if (!coverPage && !titlePage) {
        const QString name = freeName(zip, dummyTitlePage + QStringLiteral(".xhtml"));
        const QString content = coverItem
            ? QStringLiteral("<img src=\"%1\" alt=\"cover\" style=\"height: 100%\" />")
                  .arg(relativeHref(nameOf(coverItem->attribute(QStringLiteral("href"))), name))
            : QStringLiteral("<h1 style=\"text-align: center\">%1</h1>\n        <h3 style=\"text-align: center\">%2</h3>")
                  .arg(escapeXml(options.title), escapeXml(options.authors));
        const QString page = QStringLiteral(
            "<?xml version='1.0' encoding='utf-8'?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en\" xml:lang=\"en\">\n"
            "    <head>\n"
            "        <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"/>\n"
            "        <title>Dummy title page created by calibre</title>\n"
            "        <style type=\"text/css\">\n"
            "            @page { padding: 0pt; margin:0pt }\n"
            "            body { text-align: center; padding:0pt; margin: 0pt }\n"
            "            div { padding:0pt; margin: 0pt }\n"
            "            img { padding:0pt; margin: 0pt }\n"
            "        </style>\n"
            "    </head>\n"
            "    <body>\n"
            "    %1\n"
            "    </body>\n"
            "</html>\n").arg(content);
        zip.names.append(name);
        zip.data.insert(name, page.toUtf8());
        Node *item = addItem(name, QStringLiteral("kobo-title-page"), QStringLiteral("application/xhtml+xml"));
        auto itemref = element(*spine, QStringLiteral("itemref"));
        itemref->setAttribute(QStringLiteral("idref"), item->attribute(QStringLiteral("id")));
        size_t at = 0;
        while (at < spine->children.size() && !spine->children[at]->isElement("itemref"))
            ++at;
        spine->insert(at, std::move(itemref));
    }

    // Each page with Kobo's markup.
    for (Node *item : std::as_const(items)) {
        const QString type = item->attribute(QStringLiteral("media-type"));
        if (type != QLatin1String("application/xhtml+xml") && type != QLatin1String("text/html"))
            continue;
        const QString name = nameOf(item->attribute(QStringLiteral("href")));
        if (!zip.data.contains(name))
            continue;
        if (std::optional<QByteArray> page = kepubifyPage(zip.data.value(name), relativeHref(scriptName, name)))
            zip.data.insert(name, *page);
    }
    zip.data.insert(opfName, serialize(*opf));
    return writeZip(kepub, zip, error);
}

} // namespace Kepub
