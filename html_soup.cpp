// SPDX-License-Identifier: GPL-3.0-only

#include "html_soup.h"

#include <QRegularExpression>
#include <QSet>

#include "calibre_text.h"

namespace HtmlSoup {

using XmlTree::Node;

namespace {

const QString xhtml = QStringLiteral("http://www.w3.org/1999/xhtml");

const QSet<QString> &voidElements()
{
    static const QSet<QString> names{
        QStringLiteral("area"), QStringLiteral("base"), QStringLiteral("br"),    QStringLiteral("col"),
        QStringLiteral("embed"), QStringLiteral("hr"),  QStringLiteral("img"),   QStringLiteral("input"),
        QStringLiteral("link"), QStringLiteral("meta"), QStringLiteral("param"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("wbr"),
    };
    return names;
}

// The blocks that close an open paragraph.
const QSet<QString> &closesParagraph()
{
    static const QSet<QString> names{
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"),  QStringLiteral("blockquote"),
        QStringLiteral("center"),  QStringLiteral("dd"),      QStringLiteral("div"),    QStringLiteral("dl"),
        QStringLiteral("dt"),      QStringLiteral("fieldset"), QStringLiteral("figure"), QStringLiteral("footer"),
        QStringLiteral("form"),    QStringLiteral("h1"),      QStringLiteral("h2"),     QStringLiteral("h3"),
        QStringLiteral("h4"),      QStringLiteral("h5"),      QStringLiteral("h6"),     QStringLiteral("header"),
        QStringLiteral("hr"),      QStringLiteral("li"),      QStringLiteral("nav"),    QStringLiteral("ol"),
        QStringLiteral("p"),       QStringLiteral("pre"),     QStringLiteral("section"), QStringLiteral("table"),
        QStringLiteral("ul"),
    };
    return names;
}

// Where looking for an element to close stops.
const QSet<QString> &scopes()
{
    static const QSet<QString> names{ QStringLiteral("body"), QStringLiteral("td"), QStringLiteral("th"),
                                      QStringLiteral("table"), QStringLiteral("blockquote"), QStringLiteral("div"),
                                      QStringLiteral("li") };
    return names;
}

const QSet<QString> &headElements()
{
    static const QSet<QString> names{ QStringLiteral("title"), QStringLiteral("meta"), QStringLiteral("link"),
                                      QStringLiteral("style"), QStringLiteral("base") };
    return names;
}

class Builder
{
public:
    Builder()
    {
        m_root = std::make_unique<Node>();
        auto html = std::make_unique<Node>();
        html->qualifiedName = html->localName = QStringLiteral("html");
        html->namespaceUri = xhtml;
        html->namespaces.append(QXmlStreamNamespaceDeclaration(QString(), xhtml));
        m_html = m_root->append(std::move(html));
        m_head = m_html->append(make(QStringLiteral("head")));
        m_body = m_html->append(make(QStringLiteral("body")));
        m_stack.append(m_body);
    }

    std::unique_ptr<Node> take() { return std::move(m_root); }

    void start(const QString &name, const QXmlStreamAttributes &attributes, bool selfClosing)
    {
        if (name == QLatin1String("html") || name == QLatin1String("head")) {
            return;
        }
        if (name == QLatin1String("body")) {
            for (const QXmlStreamAttribute &attribute : attributes) {
                if (!m_body->attributes.hasAttribute(attribute.qualifiedName()))
                    m_body->attributes.append(attribute);
            }
            return;
        }
        if (headElements().contains(name) && m_stack.size() == 1 && !m_bodyStarted) {
            auto element = make(name);
            element->attributes = attributes;
            Node *added = m_head->append(std::move(element));
            if (name == QLatin1String("title") || name == QLatin1String("style"))
                m_headText = added;
            return;
        }
        m_bodyStarted = true;
        if (closesParagraph().contains(name))
            closeWithin(QStringLiteral("p"));
        if (name == QLatin1String("li"))
            closeWithin(QStringLiteral("li"), { QStringLiteral("ul"), QStringLiteral("ol") });
        else if (name == QLatin1String("dt") || name == QLatin1String("dd")) {
            closeWithin(QStringLiteral("dt"), { QStringLiteral("dl") });
            closeWithin(QStringLiteral("dd"), { QStringLiteral("dl") });
        } else if (name == QLatin1String("tr")) {
            closeWithin(QStringLiteral("tr"), { QStringLiteral("table") });
        } else if (name == QLatin1String("td") || name == QLatin1String("th")) {
            closeWithin(QStringLiteral("td"), { QStringLiteral("tr"), QStringLiteral("table") });
            closeWithin(QStringLiteral("th"), { QStringLiteral("tr"), QStringLiteral("table") });
        }
        auto element = make(name);
        element->attributes = attributes;
        Node *added = m_stack.last()->append(std::move(element));
        if (!selfClosing && !voidElements().contains(name))
            m_stack.append(added);
    }

    void end(const QString &name)
    {
        if (name == QLatin1String("title") || name == QLatin1String("style")) {
            m_headText = nullptr;
            if (m_stack.size() == 1 && !m_bodyStarted)
                return;
        }
        // The nearest one open, and all open inside it.
        for (qsizetype i = m_stack.size() - 1; i > 0; --i) {
            if (m_stack.at(i)->localName == name) {
                m_stack.resize(i);
                return;
            }
        }
    }

    void text(const QString &text)
    {
        if (m_headText) {
            m_headText->append(XmlTree::textNode(text));
            return;
        }
        if (!m_bodyStarted && text.trimmed().isEmpty())
            return;
        m_bodyStarted = true;
        Node *parent = m_stack.last();
        if (!parent->children.empty() && parent->children.back()->kind == Node::Kind::Text)
            parent->children.back()->text += text;
        else
            parent->append(XmlTree::textNode(text));
    }

private:
    static std::unique_ptr<Node> make(const QString &name)
    {
        auto node = std::make_unique<Node>();
        node->qualifiedName = node->localName = name;
        node->namespaceUri = xhtml;
        return node;
    }

    // Closes an open `name`, and what is open inside it, unless a scope
    // or one of `stops` comes first.
    void closeWithin(const QString &name, const QSet<QString> &stops = {})
    {
        for (qsizetype i = m_stack.size() - 1; i > 0; --i) {
            const QString &open = m_stack.at(i)->localName;
            if (open == name) {
                m_stack.resize(i);
                return;
            }
            if (stops.contains(open) || (scopes().contains(open) && open != name))
                return;
        }
    }

    std::unique_ptr<Node> m_root;
    Node *m_html = nullptr;
    Node *m_head = nullptr;
    Node *m_body = nullptr;
    Node *m_headText = nullptr;
    QList<Node *> m_stack;
    bool m_bodyStarted = false;
};

QXmlStreamAttributes attributesOf(QStringView raw)
{
    static const QRegularExpression attribute(
        QStringLiteral("([^\\s\"'>/=]+)(?:\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+)))?"));
    static const QRegularExpression xmlName(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$"));
    QXmlStreamAttributes attributes;
    for (QRegularExpressionMatchIterator it = attribute.globalMatchView(raw); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString name = match.captured(1).toLower();
        // Another namespace's, or no name XML takes.
        if (!xmlName.match(name).hasMatch() || attributes.hasAttribute(name))
            continue;
        QString value = match.captured(2);
        if (match.capturedStart(3) >= 0)
            value = match.captured(3);
        else if (match.capturedStart(4) >= 0)
            value = match.captured(4);
        attributes.append(name, CalibreText::replaceEntities(value));
    }
    return attributes;
}

} // namespace

std::unique_ptr<Node> parse(const QString &html)
{
    Builder builder;
    static const QRegularExpression tagName(QStringLiteral("^(/?)([A-Za-z][A-Za-z0-9:_-]*)"));
    qsizetype pos = 0;
    while (pos < html.size()) {
        const qsizetype lt = html.indexOf(u'<', pos);
        if (lt < 0) {
            builder.text(CalibreText::replaceEntities(html.mid(pos)));
            break;
        }
        if (lt > pos)
            builder.text(CalibreText::replaceEntities(html.mid(pos, lt - pos)));
        if (html.mid(lt, 4) == QLatin1String("<!--")) {
            const qsizetype close = html.indexOf(QLatin1String("-->"), lt + 4);
            pos = close < 0 ? html.size() : close + 3;
            continue;
        }
        if (html.mid(lt, 2) == QLatin1String("<!") || html.mid(lt, 2) == QLatin1String("<?")) {
            const qsizetype close = html.indexOf(u'>', lt);
            pos = close < 0 ? html.size() : close + 1;
            continue;
        }
        const QRegularExpressionMatch name = tagName.matchView(QStringView(html).mid(lt + 1));
        const qsizetype gt = html.indexOf(u'>', lt);
        if (!name.hasMatch() || gt < 0) {
            builder.text(QStringLiteral("<"));
            pos = lt + 1;
            continue;
        }
        const bool closing = !name.captured(1).isEmpty();
        const QString tag = name.captured(2).toLower();
        QStringView rest = QStringView(html).mid(lt + 1 + name.capturedLength(), gt - lt - 1 - name.capturedLength());
        const bool selfClosing = rest.endsWith(u'/');
        pos = gt + 1;
        // Scripts left out whole, with what they hold.
        if (!closing && tag == QLatin1String("script")) {
            const qsizetype close = html.indexOf(QLatin1String("</script"), pos, Qt::CaseInsensitive);
            const qsizetype after = close < 0 ? -1 : html.indexOf(u'>', close);
            pos = after < 0 ? html.size() : after + 1;
            continue;
        }
        // Another namespace's (mbp:, o:…): its content kept, the tag not.
        if (tag.contains(u':'))
            continue;
        if (closing)
            builder.end(tag);
        else
            builder.start(tag, attributesOf(selfClosing ? rest.chopped(1) : rest), selfClosing);
    }
    return builder.take();
}

} // namespace HtmlSoup
