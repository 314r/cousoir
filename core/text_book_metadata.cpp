// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre 9.17, https://calibre-ebook.com, GPL v3:
// - src/calibre/ebooks/metadata/fb2.py, Copyright (C) 2011 Roman Mukhin,
//   2008 Anatoly Shipitsin: get_fb2_data(), get_metadata() and its _parse_*
//   functions, ensure_namespace()
// - src/calibre/ebooks/metadata/txt.py, Copyright (C) 2009 John Schember:
//   get_metadata()

#include "text_book_metadata.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSet>

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "calibre_text.h"
#include "field_writers.h"
#include "xml_tree.h"

namespace TextBookMetadata {

namespace {

const QString fb2Namespace = QStringLiteral("http://www.gribuser.ru/xml/fictionbook/2.0");
const QString xlinkNamespace = QStringLiteral("http://www.w3.org/1999/xlink");

using Node = XmlTree::Node;

// get_fb2_data(): the book, or the first .fb2 of a ZIP, else its first file.
QByteArray readFb2Data(const QString &path, QString *error)
{
    int code = 0;
    zip_t *zip = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &code);
    if (!zip) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QObject::tr("Cannot read %1.").arg(path);
            return {};
        }
        return file.readAll();
    }
    const std::unique_ptr<zip_t, void (*)(zip_t *)> closer(zip, [](zip_t *z) { zip_discard(z); });
    QStringList names;
    const zip_int64_t count = zip_get_num_entries(zip, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        if (const char *name = zip_get_name(zip, zip_uint64_t(i), 0))
            names.append(QString::fromUtf8(name));
    }
    QStringList fb2;
    for (const QString &name : std::as_const(names)) {
        if (name.toLower().endsWith(QLatin1String(".fb2")))
            fb2.append(name);
    }
    const QString chosen = (fb2.isEmpty() ? names : fb2).value(0);
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (chosen.isEmpty() || zip_stat(zip, chosen.toUtf8().constData(), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE)) {
        *error = QObject::tr("%1 holds no FB2 book.").arg(QFileInfo(path).fileName());
        return {};
    }
    zip_file_t *file = zip_fopen(zip, chosen.toUtf8().constData(), 0);
    if (!file) {
        *error = QObject::tr("Cannot read %1.").arg(path);
        return {};
    }
    QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
    const zip_int64_t read = zip_fread(file, data.data(), stat.size);
    zip_fclose(file);
    if (read < 0 || zip_uint64_t(read) != stat.size) {
        *error = QObject::tr("Cannot read %1.").arg(path);
        return {};
    }
    return data;
}

// os.path.splitext(name)[0]: the name without its last extension.
QString stem(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot > 0 ? name.left(dot) : name;
}

// XPath's normalize-space().
QString normalizeSpace(const QString &text)
{
    QString out;
    bool space = false;
    for (const QChar c : text) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') {
            space = !out.isEmpty();
        } else {
            if (space)
                out += u' ';
            out += c;
            space = false;
        }
    }
    return out;
}

// The document's elements and text nodes in document order.
void walk(const Node &node, const std::function<void(const Node &)> &visit)
{
    for (const auto &child : node.children) {
        visit(*child);
        if (child->kind == Node::Kind::Element)
            walk(*child, visit);
    }
}

class Book
{
public:
    explicit Book(Node *root)
        : m_root(root)
    {
        const Node *top = XmlTree::documentElement(root);
        // Context: the namespace of the root's prefix, else FB2 2.0's.
        m_namespace = top && !top->namespaceUri.isEmpty() ? top->namespaceUri : fb2Namespace;
        // ensure_namespace(): a description or body written xmlns="" counts
        // as in the book's namespace.
        if (top) {
            for (const auto &child : top->children) {
                if (child->kind == Node::Kind::Element && child->namespaceUri.isEmpty()
                    && (child->localName == QLatin1String("description") || child->localName == QLatin1String("body")))
                    m_bare = true;
            }
        }
    }

    bool is(const Node *node, const char *name) const
    {
        return node && node->kind == Node::Kind::Element && node->localName == QLatin1String(name)
            && (node->namespaceUri == m_namespace || (m_bare && node->namespaceUri.isEmpty()));
    }

    // //fb:<parent>/fb:<name>, in document order; any parent when null.
    QList<const Node *> all(const char *parent, const char *name) const
    {
        QList<const Node *> found;
        walk(*m_root, [&](const Node &node) {
            if (is(&node, name) && (!parent || is(node.parent, parent)))
                found.append(&node);
        });
        return found;
    }

    // The text nodes of `nodes`' children, in document order.
    static QStringList texts(const QList<const Node *> &nodes)
    {
        QStringList found;
        for (const Node *node : nodes) {
            for (const auto &child : node->children) {
                if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata)
                    found.append(child->text);
            }
        }
        return found;
    }

    // string(<path>/text()): the first text node's.
    QString firstText(const char *parent, const char *name) const { return texts(all(parent, name)).value(0); }

    // A child element's first text node, normalized.
    QString childText(const Node &node, const char *name) const
    {
        for (const auto &child : node.children) {
            if (is(child.get(), name)) {
                const QStringList found = texts({ child.get() });
                if (!found.isEmpty())
                    return normalizeSpace(found.first());
            }
        }
        return {};
    }

    // An attribute in the XLink namespace, its prefix declared on the
    // element or above it.
    static QString xlinkAttribute(const Node &node, const QString &name)
    {
        for (const QXmlStreamAttribute &attribute : node.attributes) {
            const QString qualified = attribute.qualifiedName().toString();
            if (!qualified.endsWith(u':' + name))
                continue;
            const QString prefix = qualified.section(u':', 0, 0);
            for (const Node *scope = &node; scope; scope = scope->parent) {
                const auto declared = std::find_if(scope->namespaces.cbegin(), scope->namespaces.cend(),
                                                   [&prefix](const QXmlStreamNamespaceDeclaration &declaration) {
                                                       return declaration.prefix() == prefix;
                                                   });
                if (declared != scope->namespaces.cend()) {
                    if (declared->namespaceUri() == xlinkNamespace)
                        return attribute.value().toString();
                    break;
                }
            }
        }
        return {};
    }

    const Node *root() const { return m_root; }

private:
    Node *m_root;
    QString m_namespace;
    bool m_bare = false;
};

// _parse_author(): the first, middle and last names, else the nickname.
QString author(const Book &book, const Node &element)
{
    QString name = book.childText(element, "first-name");
    const QString middle = book.childText(element, "middle-name");
    const QString last = book.childText(element, "last-name");
    if (!middle.isEmpty())
        name = (name + u' ' + middle).trimmed();
    if (!last.isEmpty())
        name = (name + u' ' + last).trimmed();
    if (name.isEmpty())
        name = book.childText(element, "nickname");
    return name;
}

// tostring(element, method='text'): its text and its tail.
void textOf(const Node &node, QString *out)
{
    for (const auto &child : node.children) {
        if (child->kind == Node::Kind::Text || child->kind == Node::Kind::Cdata)
            out->append(child->text);
        else if (child->kind == Node::Kind::Element)
            textOf(*child, out);
    }
}

QString textWithTail(const Node &node)
{
    QString text;
    textOf(node, &text);
    const Node *parent = node.parent;
    if (!parent)
        return text;
    bool after = false;
    for (const auto &sibling : parent->children) {
        if (after) {
            if (sibling->kind != Node::Kind::Text && sibling->kind != Node::Kind::Cdata)
                break;
            text += sibling->text;
        } else if (sibling.get() == &node) {
            after = true;
        }
    }
    return text;
}

// XPath's number(): a decimal number, spaces around it, else NaN.
double xpathNumber(const QString &text)
{
    static const QRegularExpression number(QStringLiteral("^\\s*(-?([0-9]+(\\.[0-9]*)?|\\.[0-9]+))\\s*$"));
    const QRegularExpressionMatch match = number.match(text);
    return match.hasMatch() ? match.captured(1).toDouble() : std::nan("");
}

bool isImageType(const QString &type)
{
    static const QSet<QString> known{ QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
                                      QStringLiteral("image/gif"),  QStringLiteral("image/bmp"),
                                      QStringLiteral("image/webp"), QStringLiteral("image/tiff"),
                                      QStringLiteral("image/svg+xml") };
    return known.contains(type.toLower());
}

} // namespace

bool readFb2(const QString &path, ImportedMetadata *metadata, QString *error)
{
    const QByteArray data = readFb2Data(path, error);
    if (data.isEmpty())
        return false;
    std::unique_ptr<Node> root = XmlTree::parse(data);
    if (!root || !XmlTree::documentElement(root.get())) {
        *error = QObject::tr("%1 is not an FB2 book Cousoir can read.").arg(QFileInfo(path).fileName());
        return false;
    }
    const Book book(root.get());
    ImportedMetadata mi;

    // _parse_book_title(): the first of the title-info's, the publish-info's
    // and the src-title-info's, in the book's order; else the file's name.
    QString title;
    walk(*book.root(), [&](const Node &node) {
        if (!title.isNull() || (node.kind != Node::Kind::Text && node.kind != Node::Kind::Cdata))
            return;
        const Node *parent = node.parent;
        if (book.is(parent, "book-title")
            && (book.is(parent->parent, "title-info") || book.is(parent->parent, "publish-info")
                || book.is(parent->parent, "src-title-info")))
            title = node.text;
    });
    mi.title = normalizeSpace(title);
    if (mi.title.isEmpty())
        mi.title = stem(QFileInfo(path).fileName());

    // _parse_authors(): those of the first section that has one, as its last
    // author found says.
    for (const char *section : { "title-info", "src-title-info", "document-info" }) {
        QString last;
        for (const Node *element : book.all(section, "author")) {
            last = author(book, *element);
            if (!last.isEmpty())
                mi.authors.append(last);
        }
        if (!last.isEmpty())
            break;
    }

    // _parse_cover(): the image the cover page links to.
    const QList<const Node *> images = book.all("coverpage", "image");
    const QString href = images.isEmpty() ? QString() : Book::xlinkAttribute(*images.first(), QStringLiteral("href"));
    const QString id = href.section(u'#', 1);
    if (href.contains(u'#') && !id.isEmpty()) {
        for (const Node *binary : book.all(nullptr, "binary")) {
            if (binary->attribute(QStringLiteral("id")) != id)
                continue;
            const QString type = binary->attribute(QStringLiteral("content-type")).isEmpty()
                ? QStringLiteral("image/jpeg") : binary->attribute(QStringLiteral("content-type"));
            const QString byName = QStringLiteral("image/") + id.section(u'.', -1).toLower().replace(QStringLiteral("jpg"),
                                                                                                    QStringLiteral("jpeg"));
            if (isImageType(type) || (type.startsWith(QLatin1String("image/")) && id.contains(u'.') && isImageType(byName))) {
                const QString encoded = Book::texts({ binary }).value(0).trimmed();
                if (!encoded.isEmpty())
                    mi.cover = QByteArray::fromBase64(encoded.toLatin1());
            }
            break;
        }
    }

    // _parse_comments(): the annotation, as text.
    for (const char *section : { "title-info", "src-title-info" }) {
        const QList<const Node *> annotations = book.all(section, "annotation");
        if (!annotations.isEmpty()) {
            mi.comments = textWithTail(*annotations.first());
            break;
        }
    }

    // _parse_tags(): the genres of the first section that has some.
    for (const char *section : { "title-info", "src-title-info" }) {
        const QStringList tags = Book::texts(book.all(section, "genre"));
        if (!tags.isEmpty()) {
            mi.tags = tags;
            break;
        }
    }

    // _parse_series(): the first sequence of the title-info or the
    // publish-info, its number of up to two parts.
    const Node *sequence = nullptr;
    walk(*book.root(), [&](const Node &node) {
        if (sequence || !book.is(&node, "sequence")
            || !(book.is(node.parent, "title-info") || book.is(node.parent, "publish-info")))
            return;
        for (const auto &sibling : node.parent->children) {
            if (book.is(sibling.get(), "sequence")) {
                if (sibling.get() == &node)
                    sequence = &node;
                break;
            }
        }
    });
    if (sequence && !sequence->attribute(QStringLiteral("name")).isEmpty()) {
        mi.series = sequence->attribute(QStringLiteral("name"));
        const QStringList parts = sequence->attribute(QStringLiteral("number")).split(QRegularExpression(QStringLiteral("\\s+")),
                                                                                     Qt::SkipEmptyParts);
        bool ok = false;
        const double index = parts.mid(0, 2).join(u'.').toDouble(&ok);
        if (ok)
            mi.seriesIndex = index;
    }

    // _parse_isbn(): the first, when it is an ISBN.
    QString isbn = normalizeSpace(book.firstText("publish-info", "isbn"));
    if (isbn.contains(u','))
        isbn = isbn.left(isbn.indexOf(u','));
    if (!isbn.isEmpty() && !CalibreText::checkIsbn(isbn).isEmpty())
        mi.setIdentifier(QStringLiteral("isbn"), isbn);

    mi.publisher = book.firstText("publish-info", "publisher");

    // _parse_pubdate(): a year alone.
    const double year = xpathNumber(book.firstText("publish-info", "year"));
    if (!std::isnan(year) && year == std::floor(year)) {
        if (const std::optional<QDateTime> date
            = FieldWriters::parseOnlyDate(QString::number(qint64(year)), QDateTime::currentDateTimeUtc()))
            mi.pubdate = CalibreDate::fromDateTime(*date);
    }

    const QString language = CalibreText::canonicalizeLanguage(book.firstText("title-info", "lang"));
    if (!language.isEmpty())
        mi.languages = { language };

    *metadata = mi;
    return true;
}

QByteArray fb2Data(const QString &path, QString *error)
{
    return readFb2Data(path, error);
}

bool readTxt(const QString &path, ImportedMetadata *metadata, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cannot read %1.").arg(path);
        return false;
    }
    ImportedMetadata mi;
    // The name without its extension, else Unknown, which the name's own
    // metadata stands in for.
    const QString name = QFileInfo(path).fileName();
    mi.title = name.lastIndexOf(u'.') > 0 ? stem(name) : QString();
    QString head;
    for (int line = 0; line < 4; ++line) {
        const QByteArray read = file.readLine();
        if (read.isEmpty())
            break;
        head += QString::fromUtf8(read);
    }
    head = head.left(1024);
    static const QRegularExpression pattern(
        QStringLiteral("^[ ]*(?<title>[^\\n]+)[ ]*(\\n{3}|(\\r\\n){3}|\\r{3})[ ]*(?<author>[^\\n]+)[ ]*(\\n|\\r\\n|\\r)$"));
    const QRegularExpressionMatch match = pattern.match(head);
    if (match.hasMatch()) {
        mi.title = match.captured(QStringLiteral("title"));
        mi.authors = match.captured(QStringLiteral("author")).split(u',');
    }
    *metadata = mi;
    return true;
}

} // namespace TextBookMetadata
