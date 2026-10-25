// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/pyj/read_book/cfi.pyj: encode(), decode(), adjust_node_for_text_offset(),
//   increment_index_for_child(), node_at_index(), node_for_path_step(),
//   node_for_text_offset(), id_is_unique(), escape_for_cfi() and
//   unescape_from_cfi(), without the highlight wrappers a page in the
//   viewer has and a book has not
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "epub_cfi.h"

#include <QRegularExpression>
#include <QSet>

#include <functional>

namespace EpubCfi {

using XmlTree::Node;

namespace {

bool isText(const Node &node)
{
    return node.kind == Node::Kind::Text || node.kind == Node::Kind::Cdata;
}

bool isElement(const Node &node)
{
    return node.kind == Node::Kind::Element;
}

// The node types the walk back of adjust_node_for_text_offset() goes past:
// all up to COMMENT_NODE, which leaves out a DOCTYPE alone.
bool walkedPast(const Node &node)
{
    return node.kind != Node::Kind::Dtd;
}

void countIds(const Node &node, QHash<QString, int> *ids, QHash<QString, const Node *> *first)
{
    if (isElement(node)) {
        const QString id = node.attribute(QStringLiteral("id"));
        if (!id.isEmpty()) {
            ++(*ids)[id];
            if (!first->contains(id))
                first->insert(id, &node);
        }
    }
    for (const auto &child : node.children)
        countIds(*child, ids, first);
}

} // namespace

QString escape(const QString &raw)
{
    static const QRegularExpression special(QStringLiteral("([\\[\\],^();~@!])"));
    QString escaped = raw;
    return escaped.replace(special, QStringLiteral("^\\1"));
}

QString unescape(const QString &raw)
{
    static const QRegularExpression escaped(QStringLiteral("\\^(.)"));
    QString text = raw;
    return text.replace(escaped, QStringLiteral("\\1"));
}

Document::Document(const Node &root)
    : m_root(root)
{
    countIds(root, &m_ids, &m_firstById);
}

QString Document::encode(const Node &target, qsizetype offset) const
{
    const Node *node = &target;
    QString cfi;
    // An element and an offset: the child there, when an element.
    if (isElement(*node) && offset >= 0 && size_t(offset) < node->children.size()
        && isElement(*node->children[size_t(offset)]))
        node = node->children[size_t(offset)].get();
    if (isText(*node)) {
        offset = qMax<qsizetype>(offset, 0);
        // adjust_node_for_text_offset(): from the first of the siblings
        // before, elements and comments included, with the text they hold
        // themselves.
        if (const Node *parent = node->parent) {
            for (size_t i = parent->indexOf(node); i > 0 && walkedPast(*parent->children[i - 1]); --i) {
                const Node &previous = *parent->children[i - 1];
                if (isText(previous))
                    offset += previous.text.size();
                node = &previous;
            }
        }
        cfi = u':' + QString::number(offset);
    }
    while (node != &m_root && node->parent) {
        const Node *parent = node->parent;
        int index = 0;
        for (const auto &child : parent->children) {
            index |= 1;
            if (isElement(*child))
                index += 1;
            if (child.get() == node)
                break;
        }
        QString idSpec;
        if (isElement(*node)) {
            const QString id = node->attribute(QStringLiteral("id"));
            if (!id.isEmpty() && m_ids.value(id) < 2)
                idSpec = u'[' + escape(id) + u']';
        }
        cfi = u'/' + QString::number(index) + idSpec + cfi;
        node = parent;
    }
    return cfi;
}

Point Document::decode(const QString &cfi) const
{
    static const QRegularExpression step(QStringLiteral("^/(\\d+)(\\[[^\\]]*\\])?"));
    static const QRegularExpression textOffset(QStringLiteral("^:(\\d+)"));
    const Node *node = &m_root;
    QStringView rest(cfi);
    for (;;) {
        const QRegularExpressionMatch match = step.matchView(rest);
        if (!match.hasMatch())
            break;
        const int target = match.captured(1).toInt();
        const Node *found = nullptr;
        // node_for_path_step(): a unique id first.
        if (match.hasCaptured(2)) {
            const QString id = unescape(match.captured(2).mid(1, match.capturedLength(2) - 2));
            if (m_ids.value(id) == 1)
                found = m_firstById.value(id);
        }
        if (!found) {
            const bool element = target % 2 == 0;
            int wanted = target / 2;
            if (element && wanted > 0)
                wanted -= 1;
            int index = 0;
            for (const auto &child : node->children) {
                if (element ? !isElement(*child) : !isText(*child))
                    continue;
                if (index == wanted) {
                    found = child.get();
                    break;
                }
                ++index;
            }
        }
        if (found)
            node = found;
        else if (target)
            return {};
        rest = rest.mid(match.capturedLength());
    }
    Point point{ node, 0 };
    const QRegularExpressionMatch offsetMatch = textOffset.matchView(rest);
    if (offsetMatch.hasMatch() && node->parent) {
        // node_for_text_offset(): the text the offset falls in, counting
        // from the node named.
        qsizetype offset = offsetMatch.captured(1).toLongLong();
        const Node *lastText = nullptr;
        bool seenFirst = false;
        for (const auto &child : node->parent->children) {
            if (!seenFirst) {
                if (child.get() != node)
                    continue;
                seenFirst = true;
            }
            if (!isText(*child))
                continue;
            if (offset <= child->text.size())
                return { child.get(), offset };
            lastText = child.get();
            offset -= child->text.size();
        }
        if (lastText)
            point.node = lastText;
        point.offset = offset;
    }
    return point;
}

QString Document::textBetween(Point start, Point end) const
{
    if (!start.node || !end.node)
        return {};
    // The texts in document order, from the start's to the end's.
    QString text;
    bool inside = false;
    const std::function<bool(const Node &)> walk = [&](const Node &node) {
        if (&node == start.node) {
            inside = true;
            if (isText(node)) {
                if (&node == end.node) {
                    text += node.text.mid(start.offset, end.offset - start.offset);
                    return true;
                }
                text += node.text.mid(start.offset);
                return false;
            }
        }
        if (&node == end.node) {
            if (inside && isText(node))
                text += node.text.left(end.offset);
            return true;
        }
        if (inside && isText(node))
            text += node.text;
        for (const auto &child : node.children) {
            if (walk(*child))
                return true;
        }
        return false;
    };
    walk(m_root);
    return text;
}

const Node *Document::element(const QString &id) const
{
    return m_firstById.value(id);
}

bool invisible(QChar c)
{
    return c == QChar(0x00ad) || c == QChar(0x200b) || c == QChar(0x200c) || c == QChar(0x200d) || c == QChar(0xfeff);
}

PageText PageText::of(const Node &root, bool blockBreaks)
{
    static const QSet<QString> blocks{
        QStringLiteral("address"), QStringLiteral("article"), QStringLiteral("aside"), QStringLiteral("blockquote"),
        QStringLiteral("br"), QStringLiteral("dd"), QStringLiteral("div"), QStringLiteral("dl"), QStringLiteral("dt"),
        QStringLiteral("figcaption"), QStringLiteral("figure"), QStringLiteral("footer"), QStringLiteral("h1"),
        QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("header"), QStringLiteral("hr"), QStringLiteral("li"), QStringLiteral("nav"),
        QStringLiteral("ol"), QStringLiteral("p"), QStringLiteral("pre"), QStringLiteral("section"),
        QStringLiteral("table"), QStringLiteral("td"), QStringLiteral("th"), QStringLiteral("tr"), QStringLiteral("ul"),
    };
    PageText page;
    const Node *html = XmlTree::documentElement(const_cast<Node *>(&root));
    const Node *body = html ? XmlTree::childElement(const_cast<Node *>(html), "body") : nullptr;
    bool space = false;
    // Where the white space began; none for a break between blocks.
    Point spaceStart;
    // A block ended or began since the last character.
    bool broken = false;
    const auto blockBreak = [&](const Node &node) {
        if (!blockBreaks || !isElement(node) || !blocks.contains(node.localName.toLower()))
            return;
        broken = true;
        if (!space) {
            space = true;
            spaceStart = {};
        }
    };
    const std::function<void(const Node &)> walk = [&](const Node &node) {
        page.order.insert(&node, int(page.order.size()));
        if (node.isElement("script") || node.isElement("style"))
            return;
        blockBreak(node);
        if (isText(node)) {
            for (qsizetype i = 0; i < node.text.size(); ++i) {
                const QChar c = node.text.at(i);
                if (invisible(c))
                    continue;
                if (c.isSpace()) {
                    if (!space)
                        spaceStart = { &node, i };
                    space = true;
                    continue;
                }
                if (space && !page.text.isEmpty()) {
                    if (broken)
                        page.breaks.append(page.text.size());
                    page.text += u' ';
                    page.starts.append(spaceStart.node ? spaceStart : Point{ &node, i });
                    page.ends.append({ &node, i });
                }
                space = false;
                broken = false;
                page.text += c;
                page.starts.append({ &node, i });
                page.ends.append({ &node, i + 1 });
            }
        }
        for (const auto &child : node.children)
            walk(*child);
        blockBreak(node);
    };
    walk(body ? *body : root);
    return page;
}

qsizetype PageText::indexOf(const Point &point) const
{
    for (qsizetype i = 0; i < starts.size(); ++i) {
        if (starts.at(i).node == point.node && starts.at(i).offset >= point.offset)
            return i;
    }
    return -1;
}

} // namespace EpubCfi
