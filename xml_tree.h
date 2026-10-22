#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QXmlStreamAttributes>
#include <QXmlStreamNamespaceDeclarations>

#include <functional>
#include <memory>
#include <vector>

// A small XML tree, as lxml keeps one for Calibre: elements with their
// attributes and namespace declarations as written, and the text, comments
// and instructions between them, so that what Calibre's code does to a book's
// files can be done alike and written back as lxml writes it.
namespace XmlTree {

struct Node
{
    enum class Kind { Element, Text, Comment, Instruction, Cdata, Dtd };
    Kind kind = Kind::Element;
    // As written: "span", "h:span", "xml:lang".
    QString qualifiedName;
    QString localName;
    QString namespaceUri;
    QXmlStreamAttributes attributes;
    QXmlStreamNamespaceDeclarations namespaces;
    // Text, comment, CDATA, DTD, or a processing instruction's data.
    QString text;
    QString target;
    std::vector<std::unique_ptr<Node>> children;
    Node *parent = nullptr;

    QString attribute(const QString &name) const
    {
        for (const QXmlStreamAttribute &attribute : attributes) {
            if (attribute.qualifiedName() == name)
                return attribute.value().toString();
        }
        return {};
    }
    void setAttribute(const QString &name, const QString &value)
    {
        for (QXmlStreamAttribute &attribute : attributes) {
            if (attribute.qualifiedName() == name) {
                attribute = QXmlStreamAttribute(name, value);
                return;
            }
        }
        attributes.append(name, value);
    }
    void removeAttribute(const QString &name)
    {
        attributes.removeIf([&name](const QXmlStreamAttribute &attribute) { return attribute.qualifiedName() == name; });
    }
    bool isElement(const char *name) const
    {
        return kind == Kind::Element && localName.compare(QLatin1String(name), Qt::CaseInsensitive) == 0;
    }
    QString prefix() const
    {
        const qsizetype colon = qualifiedName.indexOf(u':');
        return colon < 0 ? QString() : qualifiedName.left(colon + 1);
    }
    Node *append(std::unique_ptr<Node> child)
    {
        child->parent = this;
        children.push_back(std::move(child));
        return children.back().get();
    }
    Node *insert(size_t at, std::unique_ptr<Node> child)
    {
        child->parent = this;
        return children.insert(children.begin() + qsizetype(at), std::move(child))->get();
    }
    size_t indexOf(const Node *child) const
    {
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i].get() == child)
                return i;
        }
        return children.size();
    }
    std::unique_ptr<Node> take(size_t at)
    {
        std::unique_ptr<Node> child = std::move(children[at]);
        children.erase(children.begin() + qsizetype(at));
        child->parent = nullptr;
        return child;
    }
};

// A new element named `localName` in the namespace and with the prefix of
// `like`.
std::unique_ptr<Node> element(const Node &like, const QString &localName);
std::unique_ptr<Node> textNode(const QString &text);
QList<Node *> findAll(Node *root, const std::function<bool(const Node &)> &matches);
// The text of a page as XML: its encoding by its byte order mark or UTF-8,
// HTML's named entities, which XML lacks, as character references.
QString xmlText(const QByteArray &data);
// A document: its top level nodes under a root that is no element of it;
// null when it is no XML.
std::unique_ptr<Node> parse(const QByteArray &data);
// The document's element alone, as lxml writes it for Calibre: no DOCTYPE;
// Calibre's declaration first unless `declaration` is false, as
// etree.tostring(root, encoding='utf-8') writes it; a no-break space as a
// reference, as Calibre writes the pages it changes, unless
// `nbspAsReference` is false.
QByteArray serialize(const Node &root, bool declaration = true, bool nbspAsReference = true);
Node *documentElement(Node *root);
Node *childElement(Node *parent, const char *name);

} // namespace XmlTree
