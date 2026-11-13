#include "text_book_epub.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUuid>

#include <memory>

#include "epub_package.h"
#include "text_book_metadata.h"
#include "xml_tree.h"

namespace TextBookEpub {

namespace {

using Node = XmlTree::Node;

const QString xlinkNamespace = QStringLiteral("http://www.w3.org/1999/xlink");

QString escaped(const QString &text)
{
    QString out = text;
    return out.replace(u'&', QStringLiteral("&amp;"))
        .replace(u'<', QStringLiteral("&lt;"))
        .replace(u'>', QStringLiteral("&gt;"))
        .replace(u'"', QStringLiteral("&quot;"));
}

const char css[] = "body { margin: 0 4%; line-height: 1.45; }\n"
                   "h1, h2, h3, h4, h5, h6 { text-align: center; margin: 1.5em 0 0.8em; }\n"
                   "p { margin: 0; text-indent: 1.5em; }\n"
                   "p.empty { text-indent: 0; }\n"
                   "p.subtitle { text-align: center; text-indent: 0; margin: 1em 0; }\n"
                   "blockquote { margin: 1em 2em; font-style: italic; }\n"
                   "div.poem { margin: 1em 2em; }\n"
                   "div.stanza { margin: 0 0 1em; }\n"
                   "p.v, p.text-author { text-indent: 0; }\n"
                   "p.text-author { text-align: right; }\n"
                   "div.image { text-align: center; text-indent: 0; margin: 1em 0; }\n"
                   "div.cover { text-align: center; margin: 0; }\n"
                   "img { max-width: 100%; }\n"
                   "div.cover img { max-height: 95vh; }\n"
                   "a.note { vertical-align: super; font-size: 0.75em; }\n";

QString page(const QString &title, const QString &language, const QString &body)
{
    return QStringLiteral("<?xml version='1.0' encoding='utf-8'?>\n"
                          "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"%2\">\n"
                          "<head><title>%1</title><link rel=\"stylesheet\" type=\"text/css\" href=\"../style.css\"/></head>\n"
                          "<body>\n%3</body>\n</html>\n")
        .arg(escaped(title), escaped(language), body);
}

struct NavPoint
{
    QString title;
    QString href;
    QList<NavPoint> children;
};

// The package around pages: the OPF, the NCX, the container, the style.
bool writeBook(const QString &epub, const QString &title, const QStringList &authors, const QString &language,
               const QList<QPair<QString, QString>> &pages, const QHash<QString, QPair<QByteArray, QString>> &images,
               const QList<NavPoint> &toc, const QString &coverImage, QString *error)
{
    EpubPackage::Zip zip;
    const auto add = [&zip](const QString &name, const QByteArray &data) {
        zip.names.append(name);
        zip.data.insert(name, data);
    };
    add(QStringLiteral("mimetype"), "application/epub+zip");
    add(QStringLiteral("META-INF/container.xml"),
        "<?xml version=\"1.0\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "<rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
        "</container>\n");
    add(QStringLiteral("style.css"), css);
    QString manifest = QStringLiteral("<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
                                      "<item id=\"style\" href=\"style.css\" media-type=\"text/css\"/>\n");
    QString spine;
    for (qsizetype i = 0; i < pages.size(); ++i) {
        add(pages.at(i).first, pages.at(i).second.toUtf8());
        manifest += QStringLiteral("<item id=\"page%1\" href=\"%2\" media-type=\"application/xhtml+xml\"/>\n")
                        .arg(i)
                        .arg(escaped(pages.at(i).first));
        spine += QStringLiteral("<itemref idref=\"page%1\"/>\n").arg(i);
    }
    int number = 0;
    for (auto it = images.cbegin(); it != images.cend(); ++it) {
        add(it.key(), it.value().first);
        manifest += QStringLiteral("<item id=\"image%1\" href=\"%2\" media-type=\"%3\"/>\n")
                        .arg(number++)
                        .arg(escaped(it.key()), escaped(it.value().second));
    }
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString metadata = QStringLiteral("<dc:title>%1</dc:title>\n<dc:language>%2</dc:language>\n"
                                      "<dc:identifier id=\"id\">urn:uuid:%3</dc:identifier>\n")
                           .arg(escaped(title), escaped(language), uuid);
    for (const QString &author : authors)
        metadata += QStringLiteral("<dc:creator>%1</dc:creator>\n").arg(escaped(author.trimmed()));
    if (!coverImage.isEmpty())
        metadata += QStringLiteral("<meta name=\"cover\" content=\"image%1\"/>\n")
                        .arg(std::distance(images.cbegin(), images.constFind(coverImage)));
    add(QStringLiteral("content.opf"),
        QStringLiteral("<?xml version='1.0' encoding='utf-8'?>\n"
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"id\">\n"
                       "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n%1</metadata>\n"
                       "<manifest>\n%2</manifest>\n<spine toc=\"ncx\">\n%3</spine>\n</package>\n")
            .arg(metadata, manifest, spine)
            .toUtf8());
    QString points;
    int order = 0;
    const std::function<void(const QList<NavPoint> &)> navPoints = [&](const QList<NavPoint> &entries) {
        for (const NavPoint &entry : entries) {
            ++order;
            points += QStringLiteral("<navPoint id=\"nav%1\" playOrder=\"%1\"><navLabel><text>%2</text></navLabel>"
                                     "<content src=\"%3\"/>\n")
                          .arg(order)
                          .arg(escaped(entry.title), escaped(entry.href));
            navPoints(entry.children);
            points += QStringLiteral("</navPoint>\n");
        }
    };
    navPoints(toc);
    add(QStringLiteral("toc.ncx"),
        QStringLiteral("<?xml version='1.0' encoding='utf-8'?>\n"
                       "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
                       "<head><meta name=\"dtb:uid\" content=\"urn:uuid:%1\"/></head>\n"
                       "<docTitle><text>%2</text></docTitle>\n<navMap>\n%3</navMap>\n</ncx>\n")
            .arg(uuid, escaped(title), points)
            .toUtf8());
    return EpubPackage::writeZip(epub, zip, error);
}

// --- FB2 ---------------------------------------------------------------------

class Fb2
{
public:
    Fb2(Node *root, const QString &language)
        : m_language(language)
    {
        m_book = XmlTree::documentElement(root);
        m_namespace = m_book ? m_book->namespaceUri : QString();
    }

    bool is(const Node *node, const char *name) const
    {
        return node && node->kind == Node::Kind::Element && node->localName == QLatin1String(name)
            && (node->namespaceUri == m_namespace || node->namespaceUri.isEmpty());
    }

    QList<Node *> children(const Node *node, const char *name) const
    {
        QList<Node *> found;
        for (const auto &child : node->children) {
            if (is(child.get(), name))
                found.append(child.get());
        }
        return found;
    }

    static QString xlinkHref(const Node &node)
    {
        for (const QXmlStreamAttribute &attribute : node.attributes) {
            const QString qualified = attribute.qualifiedName().toString();
            if (qualified == QLatin1String("href") || qualified.endsWith(QLatin1String(":href")))
                return attribute.value().toString();
        }
        return {};
    }

    static QString plainText(const Node &node)
    {
        QString text;
        for (const auto &child : node.children) {
            if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata)
                text += child->text;
            else if (child->kind == Node::Kind::Element)
                text += (text.isEmpty() || text.endsWith(u' ') ? QString() : QStringLiteral(" ")) + plainText(*child);
        }
        return text.simplified();
    }

    // The pictures, as files of the EPUB.
    void readImages()
    {
        for (Node *binary : children(m_book, "binary")) {
            const QString id = binary->attribute(QStringLiteral("id"));
            QString type = binary->attribute(QStringLiteral("content-type"));
            if (id.isEmpty() || !type.startsWith(QLatin1String("image/")))
                continue;
            QString encoded;
            for (const auto &child : binary->children) {
                if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata)
                    encoded += child->text;
            }
            const QString extension = type == QLatin1String("image/png") ? QStringLiteral("png")
                : type == QLatin1String("image/gif") ? QStringLiteral("gif") : QStringLiteral("jpg");
            const QString name = QStringLiteral("images/%1.%2").arg(m_images.size()).arg(extension);
            m_images.insert(name, { QByteArray::fromBase64(encoded.toLatin1()), type });
            m_imageOf.insert(id, name);
        }
    }

    // Which page each id is on, before any page is written, for the links.
    void notePage(const Node &node, const QString &name)
    {
        const QString id = node.attribute(QStringLiteral("id"));
        if (!id.isEmpty())
            m_pageOf.insert(id, name);
        for (const auto &child : node.children) {
            if (child->kind == Node::Kind::Element)
                notePage(*child, name);
        }
    }

    QString imageTag(const Node &node, bool block) const
    {
        const QString href = xlinkHref(node);
        const QString name = m_imageOf.value(href.startsWith(u'#') ? href.mid(1) : href);
        if (name.isEmpty())
            return {};
        const QString img = QStringLiteral("<img src=\"../%1\" alt=\"\"/>").arg(escaped(name));
        return block ? QStringLiteral("<div class=\"image\">%1</div>\n").arg(img) : img;
    }

    QString inlineContent(const Node &node, const QString &page) const
    {
        QString out;
        for (const auto &child : node.children) {
            if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata) {
                out += escaped(child->text);
                continue;
            }
            if (child->kind != Node::Kind::Element)
                continue;
            const QString name = child->localName;
            const QString inner = inlineContent(*child, page);
            if (name == QLatin1String("emphasis"))
                out += QStringLiteral("<em>%1</em>").arg(inner);
            else if (name == QLatin1String("strong"))
                out += QStringLiteral("<strong>%1</strong>").arg(inner);
            else if (name == QLatin1String("strikethrough"))
                out += QStringLiteral("<s>%1</s>").arg(inner);
            else if (name == QLatin1String("sub") || name == QLatin1String("sup") || name == QLatin1String("code"))
                out += QStringLiteral("<%1>%2</%1>").arg(name, inner);
            else if (name == QLatin1String("image"))
                out += imageTag(*child, false);
            else if (name == QLatin1String("a")) {
                const QString href = xlinkHref(*child);
                QString target = href;
                if (href.startsWith(u'#')) {
                    const QString where = m_pageOf.value(href.mid(1));
                    target = where.isEmpty() ? href : (where == page ? href : QStringLiteral("../%1%2").arg(where, href));
                }
                const bool note = child->attribute(QStringLiteral("type")) == QLatin1String("note");
                out += QStringLiteral("<a href=\"%1\"%2>%3</a>")
                           .arg(escaped(target), note ? QStringLiteral(" class=\"note\"") : QString(), inner);
            } else {
                out += inner;
            }
        }
        return out;
    }

    QString idAttribute(const Node &node) const
    {
        const QString id = node.attribute(QStringLiteral("id"));
        return id.isEmpty() ? QString() : QStringLiteral(" id=\"%1\"").arg(escaped(id));
    }

    QString blocks(const Node &node, int depth, const QString &page) const
    {
        QString out;
        for (const auto &child : node.children) {
            if (child->kind != Node::Kind::Element)
                continue;
            const Node &element = *child;
            const QString name = element.localName;
            const QString id = idAttribute(element);
            if (name == QLatin1String("title")) {
                QStringList lines;
                for (Node *line : children(&element, "p"))
                    lines.append(inlineContent(*line, page));
                const int level = std::min(depth, 6);
                out += QStringLiteral("<h%1%2>%3</h%1>\n").arg(level).arg(id, lines.join(QStringLiteral("<br/>")));
            } else if (name == QLatin1String("p")) {
                out += QStringLiteral("<p%1>%2</p>\n").arg(id, inlineContent(element, page));
            } else if (name == QLatin1String("empty-line")) {
                out += QStringLiteral("<p class=\"empty\"><br/></p>\n");
            } else if (name == QLatin1String("subtitle")) {
                out += QStringLiteral("<p class=\"subtitle\"%1><strong>%2</strong></p>\n").arg(id, inlineContent(element, page));
            } else if (name == QLatin1String("text-author")) {
                out += QStringLiteral("<p class=\"text-author\"%1>%2</p>\n").arg(id, inlineContent(element, page));
            } else if (name == QLatin1String("v")) {
                out += QStringLiteral("<p class=\"v\"%1>%2</p>\n").arg(id, inlineContent(element, page));
            } else if (name == QLatin1String("image")) {
                out += imageTag(element, true);
            } else if (name == QLatin1String("section")) {
                out += QStringLiteral("<div%1>\n%2</div>\n").arg(id, blocks(element, depth + 1, page));
            } else if (name == QLatin1String("epigraph") || name == QLatin1String("cite")) {
                out += QStringLiteral("<blockquote%1>\n%2</blockquote>\n").arg(id, blocks(element, depth, page));
            } else if (name == QLatin1String("poem") || name == QLatin1String("stanza")) {
                out += QStringLiteral("<div class=\"%1\"%2>\n%3</div>\n").arg(name, id, blocks(element, depth + 1, page));
            } else if (name == QLatin1String("table")) {
                QString rows;
                for (Node *row : children(&element, "tr")) {
                    QString cells;
                    for (const auto &cell : row->children) {
                        if (cell->kind == Node::Kind::Element)
                            cells += QStringLiteral("<%1>%2</%1>").arg(cell->localName == QLatin1String("th") ? QStringLiteral("th") : QStringLiteral("td"),
                                                                       inlineContent(*cell, page));
                    }
                    rows += QStringLiteral("<tr>%1</tr>\n").arg(cells);
                }
                out += QStringLiteral("<table%1>\n%2</table>\n").arg(id, rows);
            }
        }
        return out;
    }

    Node *book() const { return m_book; }
    const QHash<QString, QPair<QByteArray, QString>> &images() const { return m_images; }
    QString imageOf(const QString &id) const { return m_imageOf.value(id); }

private:
    Node *m_book = nullptr;
    QString m_namespace;
    QString m_language;
    QHash<QString, QPair<QByteArray, QString>> m_images;
    QHash<QString, QString> m_imageOf;
    QHash<QString, QString> m_pageOf;
};

} // namespace

bool fb2ToEpub(const QString &fb2, const QString &epub, QString *error)
{
    const QByteArray data = TextBookMetadata::fb2Data(fb2, error);
    if (data.isEmpty())
        return false;
    std::unique_ptr<Node> root = XmlTree::parse(data);
    ImportedMetadata metadata;
    TextBookMetadata::readFb2(fb2, &metadata, error);
    const QString language = metadata.languages.value(0, QStringLiteral("und"));
    Fb2 book(root.get(), language);
    if (!root || !book.book()) {
        *error = QObject::tr("%1 is not an FB2 book Cousoir can read.").arg(QFileInfo(fb2).fileName());
        return false;
    }
    book.readImages();
    const QString title = metadata.title.isEmpty() ? QFileInfo(fb2).completeBaseName() : metadata.title;

    // The pages: the cover, the main body's sections one a page, then the
    // notes.
    struct Part
    {
        QString name;
        QList<Node *> nodes;
        bool notes = false;
    };
    QList<Part> parts;
    QList<NavPoint> toc;
    QString cover;
    for (Node *description : book.children(book.book(), "description")) {
        for (Node *info : book.children(description, "title-info")) {
            for (Node *coverpage : book.children(info, "coverpage")) {
                for (Node *image : book.children(coverpage, "image")) {
                    const QString href = Fb2::xlinkHref(*image);
                    if (cover.isEmpty())
                        cover = book.imageOf(href.startsWith(u'#') ? href.mid(1) : href);
                }
            }
        }
    }
    for (Node *body : book.children(book.book(), "body")) {
        const QString kind = body->attribute(QStringLiteral("name"));
        const bool notes = kind == QLatin1String("notes") || kind == QLatin1String("comments");
        if (notes) {
            parts.append({ QStringLiteral("text/notes%1.xhtml").arg(parts.size()), { body }, true });
            continue;
        }
        // What comes before the first section: the body's title, epigraphs.
        QList<Node *> lead;
        for (const auto &child : body->children) {
            if (child->kind != Node::Kind::Element)
                continue;
            if (book.is(child.get(), "section")) {
                const QString name = QStringLiteral("text/part%1.xhtml").arg(parts.size(), 4, 10, QChar(u'0'));
                parts.append({ name, lead + QList<Node *>{ child.get() }, false });
                lead.clear();
                // The section's title and its sections', for the contents.
                NavPoint point;
                for (Node *heading : book.children(child.get(), "title"))
                    point.title = Fb2::plainText(*heading);
                point.href = name;
                int number = 0;
                for (Node *sub : book.children(child.get(), "section")) {
                    QString subTitle;
                    for (Node *heading : book.children(sub, "title"))
                        subTitle = Fb2::plainText(*heading);
                    if (subTitle.isEmpty())
                        continue;
                    if (sub->attribute(QStringLiteral("id")).isEmpty())
                        sub->setAttribute(QStringLiteral("id"), QStringLiteral("cousoir-%1-%2").arg(parts.size()).arg(++number));
                    point.children.append({ subTitle, name + u'#' + sub->attribute(QStringLiteral("id")), {} });
                }
                if (!point.title.isEmpty())
                    toc.append(point);
                else
                    toc += point.children;
            } else {
                lead.append(child.get());
            }
        }
        if (!lead.isEmpty())
            parts.append({ QStringLiteral("text/part%1.xhtml").arg(parts.size(), 4, 10, QChar(u'0')), lead, false });
    }
    if (parts.isEmpty()) {
        *error = QObject::tr("%1 has no text.").arg(QFileInfo(fb2).fileName());
        return false;
    }
    for (const Part &part : std::as_const(parts)) {
        for (const Node *node : part.nodes)
            book.notePage(*node, part.name);
    }
    QList<QPair<QString, QString>> pages;
    if (!cover.isEmpty())
        pages.append({ QStringLiteral("text/cover.xhtml"),
                       page(title, language, QStringLiteral("<div class=\"cover\"><img src=\"../%1\" alt=\"\"/></div>\n")
                                                 .arg(escaped(cover))) });
    for (const Part &part : std::as_const(parts)) {
        QString body;
        for (const Node *node : part.nodes) {
            if (part.notes || book.is(node, "section")) {
                // A section's own blocks, at the top level.
                body += part.notes ? book.blocks(*node, 1, part.name)
                                   : QStringLiteral("<div%1>\n%2</div>\n").arg(book.idAttribute(*node), book.blocks(*node, 1, part.name));
            } else {
                // A block before the first section: the body's title, an
                // epigraph, a picture.
                QString one;
                const QString name = node->localName;
                if (name == QLatin1String("title")) {
                    QStringList lines;
                    for (Node *line : book.children(node, "p"))
                        lines.append(book.inlineContent(*line, part.name));
                    one = QStringLiteral("<h1>%1</h1>\n").arg(lines.join(QStringLiteral("<br/>")));
                } else if (name == QLatin1String("image")) {
                    one = book.imageTag(*node, true);
                } else if (name == QLatin1String("epigraph")) {
                    one = QStringLiteral("<blockquote>\n%1</blockquote>\n").arg(book.blocks(*node, 1, part.name));
                } else if (name == QLatin1String("p")) {
                    one = QStringLiteral("<p>%1</p>\n").arg(book.inlineContent(*node, part.name));
                }
                body += one;
            }
        }
        pages.append({ part.name, page(title, language, body) });
    }
    if (toc.isEmpty())
        toc.append({ title, pages.first().first, {} });
    return writeBook(epub, title, metadata.authors, language, pages, book.images(), toc, cover, error);
}

bool txtToEpub(const QString &txt, const QString &epub, QString *error)
{
    QFile file(txt);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cannot read %1.").arg(txt);
        return false;
    }
    const QByteArray data = file.readAll();
    // UTF-8, else the old Western encoding.
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(data);
    if (utf8.hasError())
        text = QStringDecoder(QStringDecoder::System).decode(data);
    if (text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n');
    ImportedMetadata metadata;
    TextBookMetadata::readTxt(txt, &metadata, error);
    const QString title = metadata.title.isEmpty() ? QFileInfo(txt).completeBaseName() : metadata.title;
    // A title and its authors before the text, as Calibre's reader finds
    // them, are the book's metadata, not its first paragraph.
    static const QRegularExpression header(
        QStringLiteral("^[ ]*[^\\n]+[ ]*\\n{3}[ ]*[^\\n]+[ ]*\\n"));
    const QRegularExpressionMatch heading = header.match(text);
    if (heading.hasMatch())
        text = text.mid(heading.capturedEnd());
    // Paragraphs by the blank lines between them, their lines run together;
    // else each line a paragraph.
    static const QRegularExpression blank(QStringLiteral("\\n[ \\t]*\\n"));
    QStringList paragraphs = text.contains(blank) ? text.split(blank) : text.split(u'\n');
    QList<QPair<QString, QString>> pages;
    QString body;
    qsizetype size = 0;
    const auto flush = [&] {
        if (body.isEmpty())
            return;
        pages.append({ QStringLiteral("text/part%1.xhtml").arg(pages.size(), 4, 10, QChar(u'0')),
                       page(title, QStringLiteral("und"), body) });
        body.clear();
        size = 0;
    };
    for (const QString &paragraph : std::as_const(paragraphs)) {
        const QString line = paragraph.simplified();
        if (line.isEmpty())
            continue;
        body += QStringLiteral("<p>%1</p>\n").arg(escaped(line));
        size += line.size();
        // Pages of about 100,000 characters, which the reader lays out fast.
        if (size > 100000)
            flush();
    }
    flush();
    if (pages.isEmpty())
        pages.append({ QStringLiteral("text/part0000.xhtml"), page(title, QStringLiteral("und"), QString()) });
    return writeBook(epub, title, metadata.authors, QStringLiteral("und"), pages, {}, { { title, pages.first().first, {} } },
                     QString(), error);
}

} // namespace TextBookEpub
