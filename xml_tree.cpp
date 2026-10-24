// SPDX-License-Identifier: GPL-3.0-only

#include "xml_tree.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

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

void write(QXmlStreamWriter &writer, const Node &node)
{
    switch (node.kind) {
    case Node::Kind::Element:
        writer.writeStartElement(node.qualifiedName);
        for (const QXmlStreamNamespaceDeclaration &declaration : node.namespaces) {
            const QString prefix = declaration.prefix().toString();
            writer.writeAttribute(prefix.isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:") + prefix,
                                  declaration.namespaceUri().toString());
        }
        for (const QXmlStreamAttribute &attribute : node.attributes)
            writer.writeAttribute(attribute.qualifiedName().toString(), attribute.value().toString());
        for (const auto &child : node.children)
            write(writer, *child);
        writer.writeEndElement();
        break;
    case Node::Kind::Text:
        writer.writeCharacters(node.text);
        break;
    case Node::Kind::Comment:
        writer.writeComment(node.text);
        break;
    case Node::Kind::Instruction:
        writer.writeProcessingInstruction(node.target, node.text);
        break;
    case Node::Kind::Cdata:
        writer.writeCDATA(node.text);
        break;
    case Node::Kind::Dtd:
        writer.writeDTD(node.text);
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

QString xmlText(const QByteArray &data)
{
    QStringDecoder decoder = QStringDecoder::decoderForHtml(data);
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

QByteArray serialize(const Node &root)
{
    QByteArray body;
    {
        QXmlStreamWriter writer(&body);
        for (const auto &child : root.children) {
            if (child->kind == Node::Kind::Element)
                write(writer, *child);
        }
    }
    body.replace("\xc2\xa0", "&#160;");
    // Calibre's declaration; the writer opens its array afresh.
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

std::unique_ptr<Node> clone(const Node &node, bool withOrigins)
{
    auto copy = std::make_unique<Node>();
    copy->kind = node.kind;
    copy->qualifiedName = node.qualifiedName;
    copy->localName = node.localName;
    copy->namespaceUri = node.namespaceUri;
    copy->attributes = node.attributes;
    copy->namespaces = node.namespaces;
    copy->text = node.text;
    copy->target = node.target;
    if (withOrigins)
        copy->origin = &node;
    for (const auto &child : node.children)
        copy->append(clone(*child, withOrigins));
    return copy;
}

} // namespace XmlTree
