// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/mobi/writer8/skeleton.py: aid_able_tags,
//   close_self_closing_tags(), Chunk, Skeleton, Chunker
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "kf8_skeleton.h"

#include <QRegularExpression>
#include <QSet>

#include "mobi_format.h"

namespace Kf8Skeleton {

namespace {

constexpr qsizetype chunkSize = 8192;

const QSet<QString> &aidAbleTags()
{
    static const QSet<QString> tags{
        QStringLiteral("a"),        QStringLiteral("abbr"),     QStringLiteral("address"),  QStringLiteral("article"),
        QStringLiteral("aside"),    QStringLiteral("audio"),    QStringLiteral("b"),        QStringLiteral("bdo"),
        QStringLiteral("blockquote"), QStringLiteral("body"),   QStringLiteral("button"),   QStringLiteral("cite"),
        QStringLiteral("code"),     QStringLiteral("dd"),       QStringLiteral("del"),      QStringLiteral("details"),
        QStringLiteral("dfn"),      QStringLiteral("div"),      QStringLiteral("dl"),       QStringLiteral("dt"),
        QStringLiteral("em"),       QStringLiteral("fieldset"), QStringLiteral("figcaption"), QStringLiteral("figure"),
        QStringLiteral("footer"),   QStringLiteral("h1"),       QStringLiteral("h2"),       QStringLiteral("h3"),
        QStringLiteral("h4"),       QStringLiteral("h5"),       QStringLiteral("h6"),       QStringLiteral("header"),
        QStringLiteral("hgroup"),   QStringLiteral("i"),        QStringLiteral("ins"),      QStringLiteral("kbd"),
        QStringLiteral("label"),    QStringLiteral("legend"),   QStringLiteral("li"),       QStringLiteral("map"),
        QStringLiteral("mark"),     QStringLiteral("meter"),    QStringLiteral("nav"),      QStringLiteral("ol"),
        QStringLiteral("output"),   QStringLiteral("p"),        QStringLiteral("pre"),      QStringLiteral("progress"),
        QStringLiteral("q"),        QStringLiteral("rp"),       QStringLiteral("rt"),       QStringLiteral("samp"),
        QStringLiteral("section"),  QStringLiteral("select"),   QStringLiteral("small"),    QStringLiteral("span"),
        QStringLiteral("strong"),   QStringLiteral("sub"),      QStringLiteral("summary"),  QStringLiteral("sup"),
        QStringLiteral("textarea"), QStringLiteral("time"),     QStringLiteral("ul"),       QStringLiteral("var"),
        QStringLiteral("video"),
    };
    return tags;
}

// The tags close_self_closing_tags() writes with an end tag when empty.
bool closedWhenEmpty(const QString &name)
{
    const QString lower = name.toLower();
    return aidAbleTags().contains(lower) || lower == QLatin1String("script") || lower == QLatin1String("style")
        || lower == QLatin1String("title") || lower == QLatin1String("head");
}

// What lxml escapes in a text: &, < and >.
QByteArray escapedText(const QString &text)
{
    QString escaped = text;
    escaped.replace(u'&', QLatin1String("&amp;")).replace(u'<', QLatin1String("&lt;")).replace(u'>', QLatin1String("&gt;"));
    return escaped.toUtf8();
}

QByteArray escapedAttribute(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar c : value) {
        switch (c.unicode()) {
        case '&': escaped += QLatin1String("&amp;"); break;
        case '<': escaped += QLatin1String("&lt;"); break;
        case '>': escaped += QLatin1String("&gt;"); break;
        case '"': escaped += QLatin1String("&quot;"); break;
        case '\n': escaped += QLatin1String("&#10;"); break;
        case '\r': escaped += QLatin1String("&#13;"); break;
        case '\t': escaped += QLatin1String("&#9;"); break;
        default: escaped += c;
        }
    }
    return escaped.toUtf8();
}

void write(QByteArray *out, const Element &element, bool withTail)
{
    out->append('<');
    out->append(element.name.toUtf8());
    for (const auto &[key, value] : element.attributes) {
        out->append(' ');
        out->append(key.toUtf8());
        out->append("=\"");
        out->append(escapedAttribute(value));
        out->append('"');
    }
    if (element.text.isEmpty() && element.children.empty()) {
        if (closedWhenEmpty(element.name)) {
            out->append("></");
            out->append(element.name.toUtf8());
            out->append('>');
        } else {
            out->append("/>");
        }
    } else {
        out->append('>');
        out->append(escapedText(element.text));
        for (const auto &child : element.children)
            write(out, *child, true);
        out->append("</");
        out->append(element.name.toUtf8());
        out->append('>');
    }
    if (withTail)
        out->append(escapedText(element.tail));
}

bool blank(const QString &text)
{
    return text.trimmed().isEmpty();
}

// A chunk of a skeleton, and the tags with an aid it starts and ends.
struct Piece
{
    QByteArray raw;
    QString selector;
    QStringList startsTags;
    QStringList endsTags;
    quint32 insertPosition = 0;
};

class Chunker
{
public:
    void stepIntoTag(Element *tag, QList<Piece> *pieces);
    QList<Piece> chunkUpText(const QString &text) const;

private:
    QString m_selector;
};

// Chunker.step_into_tag(): the tag's content as chunks of at most 8 KB, those
// of its children too big for one, with an aid, taken from inside them; the
// tag's text and the children taken out of the skeleton.
void Chunker::stepIntoTag(Element *tag, QList<Piece> *pieces)
{
    const QString aid = tag->attribute(QStringLiteral("aid"));
    m_selector = QStringLiteral("P-//*[@aid='%1']").arg(aid);
    const qsizetype first = pieces->size();
    // White space alone stays in the skeleton.
    if (!blank(tag->text)) {
        *pieces += chunkUpText(tag->text);
        tag->text.clear();
    }
    for (size_t i = 0; i < tag->children.size();) {
        Element *child = tag->children[i].get();
        const QByteArray raw = serialize(*child, false);
        if (raw.size() > chunkSize && child->hasAttribute(QStringLiteral("aid"))
            && (!blank(child->text) || !child->children.empty())) {
            stepIntoTag(child, pieces);
            if (!blank(child->tail)) {
                *pieces += chunkUpText(child->tail);
                child->tail.clear();
            }
            ++i;
        } else {
            pieces->append({ raw, m_selector, {}, {}, 0 });
            if (!child->tail.isEmpty())
                *pieces += chunkUpText(child->tail);
            tag->children.erase(tag->children.begin() + qsizetype(i));
        }
    }
    if (pieces->size() > first) {
        (*pieces)[first].startsTags.append(aid);
        pieces->back().endsTags.append(aid);
    }
    m_selector = QStringLiteral("S-//*[@aid='%1']").arg(aid);
}

// Chunker.chunk_up_text(): the text escaped, in pieces of at most 8 KB cut
// between characters, those after the first in a span of their own.
QList<Piece> Chunker::chunkUpText(const QString &text) const
{
    QByteArray rest = escapedText(text);
    QList<Piece> pieces;
    bool firstPiece = true;
    while (firstPiece || !rest.isEmpty()) {
        qsizetype cut = rest.size();
        if (cut > chunkSize) {
            cut = chunkSize;
            // Between characters, and outside an entity.
            while (cut > 0 && (quint8(rest.at(cut)) & 0xC0) == 0x80)
                --cut;
            const qsizetype ampersand = rest.lastIndexOf('&', cut - 1);
            if (ampersand >= 0 && ampersand > cut - 6 && rest.indexOf(';', ampersand) >= cut)
                cut = ampersand;
        }
        const QByteArray start = rest.left(cut);
        rest = rest.mid(cut);
        pieces.append({ firstPiece ? start : QByteArray("<span class=\"AmznBigTextBlock\">") + start + "</span>", m_selector, {}, {}, 0 });
        firstPiece = false;
    }
    return pieces;
}

// Chunker.merge_small_chunks(): neighbours joined up to 8 KB, but not across
// the start or the end of a tag of the skeleton.
QList<Piece> mergeSmallPieces(const QList<Piece> &pieces)
{
    QList<Piece> merged;
    for (const Piece &piece : pieces) {
        if (merged.isEmpty() || !piece.startsTags.isEmpty() || piece.raw.size() + merged.back().raw.size() > chunkSize
            || !merged.back().endsTags.isEmpty()) {
            merged.append(piece);
        } else {
            merged.back().raw += piece.raw;
            merged.back().endsTags = piece.endsTags;
        }
    }
    return merged;
}

Element *findBody(Element *element)
{
    if (element->name.compare(QLatin1String("body"), Qt::CaseInsensitive) == 0)
        return element;
    for (const auto &child : element->children) {
        if (Element *body = findBody(child.get()))
            return body;
    }
    return nullptr;
}

void collectAids(const Element &element, QList<const Element *> *found)
{
    if (element.hasAttribute(QStringLiteral("aid")))
        found->append(&element);
    for (const auto &child : element.children)
        collectAids(*child, found);
}

struct Page
{
    QByteArray skeleton;
    QList<Piece> pieces;
    quint32 start = 0;

    qsizetype size() const
    {
        qsizetype size = skeleton.size();
        for (const Piece &piece : pieces)
            size += piece.raw.size();
        return size;
    }
};

// Skeleton: the page's markup, then where each chunk goes into it, by the
// lengths of the start and end tags around it.
Page makePage(Element *root, QList<Piece> pieces)
{
    Page page;
    page.pieces = std::move(pieces);
    QByteArray rendered("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    rendered += serialize(*root, true);
    const qsizetype html = rendered.indexOf("<html");
    if (html >= 0)
        rendered.insert(html + 5, " xmlns=\"http://www.w3.org/1999/xhtml\"");
    page.skeleton = rendered;
    // calculate_metrics(): the length of each tag's start with its text, and
    // of its end with its tail.
    struct Metric
    {
        qsizetype start = 0;
        qsizetype end = 0;
    };
    QHash<QString, Metric> metrics;
    QList<const Element *> tagged;
    collectAids(*root, &tagged);
    for (const Element *tag : std::as_const(tagged)) {
        const QByteArray raw = serialize(*tag, true);
        const QByteArray text = escapedText(tag->text);
        const qsizetype startEnd = raw.indexOf('>');
        const qsizetype endStart = raw.lastIndexOf('<');
        metrics.insert(tag->attribute(QStringLiteral("aid")),
                       { (startEnd < 0 ? raw.size() : startEnd) + text.size() + 1, raw.size() - endStart });
    }
    // calculate_insert_positions().
    qsizetype position = std::max<qsizetype>(page.skeleton.indexOf("<body"), 0);
    for (Piece &piece : page.pieces) {
        for (const QString &aid : std::as_const(piece.startsTags))
            position += metrics.value(aid).start;
        piece.insertPosition = quint32(position);
        position += piece.raw.size();
        for (const QString &aid : std::as_const(piece.endsTags))
            position += metrics.value(aid).end;
    }
    return page;
}

QByteArray rebuild(const Page &page)
{
    QByteArray text = page.skeleton;
    for (const Piece &piece : page.pieces)
        text.insert(piece.insertPosition, piece.raw);
    return text;
}

} // namespace

QString Element::attribute(const QString &key) const
{
    for (const auto &[name, value] : attributes) {
        if (name == key)
            return value;
    }
    return {};
}

bool Element::hasAttribute(const QString &key) const
{
    return std::any_of(attributes.cbegin(), attributes.cend(), [&key](const auto &attribute) { return attribute.first == key; });
}

void Element::setAttribute(const QString &key, const QString &value)
{
    for (auto &attribute : attributes) {
        if (attribute.first == key) {
            attribute.second = value;
            return;
        }
    }
    attributes.append({ key, value });
}

void Element::removeAttribute(const QString &key)
{
    attributes.removeIf([&key](const auto &attribute) { return attribute.first == key; });
}

Element *Element::append(std::unique_ptr<Element> child)
{
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

bool aidAble(const QString &name)
{
    return aidAbleTags().contains(name.toLower());
}

QByteArray serialize(const Element &element, bool withTail)
{
    QByteArray out;
    write(&out, element, withTail);
    return out;
}

Text chunk(std::vector<std::unique_ptr<Element>> &pages, const QHash<QString, QString> &links)
{
    Text result;
    QList<Page> skeletons;
    for (const auto &root : pages) {
        Element *body = findBody(root.get());
        QList<Piece> pieces;
        if (body) {
            body->tail = QStringLiteral("\n");
            Chunker chunker;
            chunker.stepIntoTag(body, &pieces);
            pieces = mergeSmallPieces(pieces);
        }
        root->tail = QStringLiteral("\n");
        skeletons.append(makePage(root.get(), std::move(pieces)));
    }

    // create_tables().
    quint32 start = 0;
    for (qsizetype i = 0; i < skeletons.size(); ++i) {
        Page &page = skeletons[i];
        page.start = start;
        start += quint32(page.size());
        result.skeletons.append({ int(i), int(page.pieces.size()), page.start, quint32(page.skeleton.size()) });
    }
    int sequence = 0;
    for (qsizetype i = 0; i < skeletons.size(); ++i) {
        const Page &page = skeletons.at(i);
        quint32 within = 0;
        for (const Piece &piece : page.pieces) {
            result.chunks.append({ piece.insertPosition + page.start, piece.selector, int(i), sequence++, within, quint32(piece.raw.size()) });
            within += quint32(piece.raw.size());
        }
    }

    QByteArray text;
    QByteArray rebuilt;
    for (const Page &page : std::as_const(skeletons)) {
        text += page.skeleton;
        for (const Piece &piece : page.pieces)
            text += piece.raw;
        rebuilt += rebuild(page);
    }

    // set_internal_links(): each aid's chunk and offset from its start, in
    // the text with the chunks in place.
    static const QRegularExpression tagWithAid(QStringLiteral("<[^>]+? [ac]id=['\"]([cA-Z0-9]+)['\"]"));
    const QString markup = QString::fromLatin1(rebuilt);
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator matches = tagWithAid.globalMatch(markup);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const qsizetype offset = match.capturedStart();
        Text::Place place{ 0, 0, quint32(offset) };
        // The chunks before the last one found end before this aid.
        while (cursor < result.chunks.size()) {
            const Kf8Index::Chunk &chunk = result.chunks.at(cursor);
            if (qsizetype(chunk.insertPosition) <= offset && offset < qsizetype(chunk.insertPosition + chunk.length)) {
                place = { chunk.sequenceNumber, int(offset - chunk.insertPosition), quint32(offset) };
                break;
            }
            // In the skeleton: the chunk after it.
            if (qsizetype(chunk.insertPosition) > offset) {
                place = { chunk.sequenceNumber, 0, quint32(offset) };
                break;
            }
            if (cursor == result.chunks.size() - 1) {
                place = { chunk.sequenceNumber, int(offset - chunk.insertPosition), quint32(offset) };
                break;
            }
            ++cursor;
        }
        result.places.insert(match.captured(1), place);
    }

    // The placeholders of the links made into the place of their aid, at
    // the same length: "kindle:pos:fid:XXXX:off:YYYYYYYYYY".
    static const QByteArray placeholder("kindle:pos:fid:0000:off:");
    for (qsizetype at = text.indexOf(placeholder); at >= 0; at = text.indexOf(placeholder, at + 1)) {
        const QString key = QString::fromLatin1(text.mid(at, placeholder.size() + 10));
        const auto aid = links.constFind(key);
        if (aid == links.cend() || !result.places.contains(*aid))
            continue;
        const Text::Place place = result.places.value(*aid);
        const QByteArray target = (MobiFormat::toBase32(quint64(place.position), 4) + QStringLiteral(":off:")
                                   + MobiFormat::toBase32(quint64(place.offset), 10)).toLatin1();
        text.replace(at + 15, 19, target);
    }
    result.text = text;
    return result;
}

} // namespace Kf8Skeleton
