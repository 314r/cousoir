#include "xml_tree.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QXmlStreamReader>

#include "calibre_text.h"

namespace XmlTree {

namespace {

void collect(Node *node, const std::function<bool(const Node &)> &matches, QList<Node *> *found)
{
    if (matches(*node))
        found->append(node);
    for (const auto &child : node->children)
        collect(child.get(), matches, found);
}

// libxml2's escaping, as lxml writes: in text &, < and > and carriage
// returns; in attributes quotes and line breaks too.
void escape(QString *out, const QString &text, bool attribute)
{
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'&':
            *out += QLatin1String("&amp;");
            break;
        case u'<':
            *out += QLatin1String("&lt;");
            break;
        case u'>':
            *out += QLatin1String("&gt;");
            break;
        case u'\r':
            *out += QLatin1String("&#13;");
            break;
        case u'"':
            *out += attribute ? QLatin1String("&quot;") : QLatin1String("\"");
            break;
        case u'\n':
            *out += attribute ? QLatin1String("&#10;") : QLatin1String("\n");
            break;
        case u'\t':
            *out += attribute ? QLatin1String("&#9;") : QLatin1String("\t");
            break;
        default:
            *out += c;
        }
    }
}

void write(QString *out, const Node &node)
{
    switch (node.kind) {
    case Node::Kind::Element:
        *out += u'<' + node.qualifiedName;
        for (const QXmlStreamNamespaceDeclaration &declaration : node.namespaces) {
            const QString prefix = declaration.prefix().toString();
            *out += prefix.isEmpty() ? QStringLiteral(" xmlns=\"") : QStringLiteral(" xmlns:") + prefix + QStringLiteral("=\"");
            escape(out, declaration.namespaceUri().toString(), true);
            *out += u'"';
        }
        for (const QXmlStreamAttribute &attribute : node.attributes) {
            *out += u' ' + attribute.qualifiedName().toString() + QStringLiteral("=\"");
            escape(out, attribute.value().toString(), true);
            *out += u'"';
        }
        if (node.children.empty()) {
            *out += QLatin1String("/>");
            break;
        }
        *out += u'>';
        for (const auto &child : node.children)
            write(out, *child);
        *out += QStringLiteral("</") + node.qualifiedName + u'>';
        break;
    case Node::Kind::Text:
        escape(out, node.text, false);
        break;
    case Node::Kind::Comment:
        *out += QStringLiteral("<!--") + node.text + QStringLiteral("-->");
        break;
    case Node::Kind::Instruction:
        *out += QStringLiteral("<?") + node.target + (node.text.isEmpty() ? QString() : u' ' + node.text) + QStringLiteral("?>");
        break;
    case Node::Kind::Cdata:
        *out += QStringLiteral("<![CDATA[") + node.text + QStringLiteral("]]>");
        break;
    case Node::Kind::Dtd:
        *out += node.text;
        break;
    }
}

} // namespace

std::unique_ptr<Node> element(const Node &like, const QString &localName)
{
    auto node = std::make_unique<Node>();
    node->localName = localName;
    node->qualifiedName = like.prefix() + localName;
    node->namespaceUri = like.namespaceUri;
    return node;
}

std::unique_ptr<Node> textNode(const QString &text)
{
    auto node = std::make_unique<Node>();
    node->kind = Node::Kind::Text;
    node->text = text;
    return node;
}

QList<Node *> findAll(Node *root, const std::function<bool(const Node &)> &matches)
{
    QList<Node *> found;
    collect(root, matches, &found);
    return found;
}

// The text of a page as XML: its encoding by its byte order mark or UTF-8,
// HTML's named entities, which XML lacks, as character references.
QString xmlText(const QByteArray &data)
{
    // xml_to_unicode(): a byte order mark, else the encoding the XML
    // declaration names, else what an HTML page says, else UTF-8.
    QStringDecoder decoder;
    const bool bom = data.startsWith("\xef\xbb\xbf") || data.startsWith("\xff\xfe") || data.startsWith("\xfe\xff");
    static const QRegularExpression declared(QStringLiteral("^\\s*<\\?xml[^>]*encoding\\s*=\\s*[\"']([-\\w.:]+)[\"']"));
    const QRegularExpressionMatch encoding = declared.match(QString::fromLatin1(data.left(256)));
    if (!bom && encoding.hasMatch())
        decoder = QStringDecoder(encoding.captured(1).toLatin1().constData());
    if (!decoder.isValid())
        decoder = QStringDecoder::decoderForHtml(data);
    QString text = decoder.isValid() ? decoder.decode(data) : QString::fromUtf8(data);
    static const QRegularExpression entity(QStringLiteral("&([A-Za-z][A-Za-z0-9]*);"));
    static const QSet<QString> xmlEntities{ QStringLiteral("amp"), QStringLiteral("lt"), QStringLiteral("gt"),
                                            QStringLiteral("quot"), QStringLiteral("apos") };
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = entity.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (xmlEntities.contains(match.captured(1)))
            continue;
        const QString decoded = CalibreText::replaceEntities(match.captured());
        if (decoded == match.captured())
            continue;
        result += QStringView(text).mid(last, match.capturedStart() - last);
        for (const char32_t code : decoded.toUcs4())
            result += QStringLiteral("&#%1;").arg(uint(code));
        last = match.capturedEnd();
    }
    return last == 0 ? text : result + QStringView(text).mid(last);
}

// A document: its top level nodes under a root that is no element of it.
std::unique_ptr<Node> parse(const QByteArray &data)
{
    QXmlStreamReader reader(xmlText(data));
    auto root = std::make_unique<Node>();
    Node *current = root.get();
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::DTD: {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Dtd;
            node->text = reader.text().toString();
            current->append(std::move(node));
            break;
        }
        case QXmlStreamReader::StartElement: {
            auto node = std::make_unique<Node>();
            node->qualifiedName = reader.qualifiedName().toString();
            node->localName = reader.name().toString();
            node->namespaceUri = reader.namespaceUri().toString();
            node->namespaces = reader.namespaceDeclarations();
            // The declarations are written back as they were.
            for (const QXmlStreamAttribute &attribute : reader.attributes())
                node->attributes.append(attribute.qualifiedName().toString(), attribute.value().toString());
            current = current->append(std::move(node));
            break;
        }
        case QXmlStreamReader::EndElement:
            current = current->parent;
            break;
        case QXmlStreamReader::Characters: {
            const Node::Kind kind = reader.isCDATA() ? Node::Kind::Cdata : Node::Kind::Text;
            if (kind == Node::Kind::Text && !current->children.empty() && current->children.back()->kind == Node::Kind::Text) {
                current->children.back()->text += reader.text();
            } else {
                auto node = std::make_unique<Node>();
                node->kind = kind;
                node->text = reader.text().toString();
                current->append(std::move(node));
            }
            break;
        }
        case QXmlStreamReader::Comment: {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Comment;
            node->text = reader.text().toString();
            current->append(std::move(node));
            break;
        }
        case QXmlStreamReader::ProcessingInstruction: {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Instruction;
            node->target = reader.processingInstructionTarget().toString();
            node->text = reader.processingInstructionData().toString();
            current->append(std::move(node));
            break;
        }
        default:
            break;
        }
    }
    if (reader.hasError())
        return nullptr;
    return root;
}

QByteArray serialize(const Node &root, bool declaration, bool nbspAsReference)
{
    // The document's element alone, as lxml writes it for Calibre: no
    // DOCTYPE.
    QString text;
    for (const auto &child : root.children) {
        if (child->kind == Node::Kind::Element)
            write(&text, *child);
    }
    QByteArray body = text.toUtf8();
    if (nbspAsReference)
        body.replace("\xc2\xa0", "&#160;");
    if (!declaration)
        return body;
    return QByteArrayLiteral("<?xml version='1.0' encoding='utf-8'?>\n") + body;
}

Node *documentElement(Node *root)
{
    for (const auto &child : root->children) {
        if (child->kind == Node::Kind::Element)
            return child.get();
    }
    return nullptr;
}

Node *childElement(Node *parent, const char *name)
{
    for (const auto &child : parent->children) {
        if (child->isElement(name))
            return child.get();
    }
    return nullptr;
}

} // namespace XmlTree
