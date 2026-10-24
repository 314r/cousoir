// SPDX-License-Identifier: GPL-3.0-only

#include "kobo_placement.h"

#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>

#include <optional>

#include "book.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "highlight_json.h"
#include "kepub.h"
#include "xml_tree.h"

namespace KoboPlacement {

using XmlTree::Node;

namespace {

bool skippedCharacter(QChar c)
{
    return c == QChar(0x00ad) || c == QChar(0x200b) || c == QChar(0x200c) || c == QChar(0x200d) || c == QChar(0xfeff);
}

bool isText(const Node &node)
{
    return node.kind == Node::Kind::Text || node.kind == Node::Kind::Cdata;
}

// The file of the book a Kobo's id names: "OEBPS/Text/c1.xhtml" of a KEPUB's
// "…/book.kepub.epub!OEBPS!Text/c1.xhtml", of an EPUB's
// "…/book.epub#(3)OEBPS/Text/c1.xhtml".
QString fileOf(const QString &contentId)
{
    const qsizetype open = contentId.indexOf(QLatin1String("#("));
    if (open >= 0) {
        const qsizetype close = contentId.indexOf(u')', open);
        return close < 0 ? QString() : contentId.mid(close + 1);
    }
    QStringList parts = contentId.split(u'!');
    if (parts.size() < 3)
        return {};
    parts.removeFirst();
    parts.removeAll(QString());
    return QDir::cleanPath(parts.join(u'/'));
}

// "span#kobo\.3\.1": the id of Kobo's span, "kobo.3.1".
QString spanIdOf(const QString &path)
{
    static const QRegularExpression span(QStringLiteral("kobo\\\\?\\.(\\d+)\\\\?\\.(\\d+)"));
    const QRegularExpressionMatch match = span.match(path);
    return match.hasMatch() ? QStringLiteral("kobo.%1.%2").arg(match.captured(1), match.captured(2)) : QString();
}

const Node *elementById(const Node &node, const QString &id)
{
    if (node.kind == Node::Kind::Element && node.attribute(QStringLiteral("id")) == id)
        return &node;
    for (const auto &child : node.children) {
        if (const Node *found = elementById(*child, id))
            return found;
    }
    return nullptr;
}

} // namespace

QString folded(const QString &text)
{
    QString result;
    result.reserve(text.size());
    bool space = false;
    for (const QChar c : text) {
        if (skippedCharacter(c))
            continue;
        if (c.isSpace()) {
            space = true;
            continue;
        }
        if (space && !result.isEmpty())
            result += u' ';
        space = false;
        result += c;
    }
    return result;
}

QString methodName(Method method)
{
    switch (method) {
    case Method::Spans:
        return QStringLiteral("spans");
    case Method::Text:
        return QStringLiteral("text");
    case Method::Book:
        return QStringLiteral("book");
    case Method::None:
        break;
    }
    return QStringLiteral("none");
}

QString targetFormat(const Book &book)
{
    for (const char *wanted : { "EPUB", "KEPUB" }) {
        for (const BookFormat &format : book.formats) {
            if (format.format == QLatin1String(wanted) && !format.file.isEmpty())
                return format.format;
        }
    }
    return {};
}

// A page of the book, parsed, and what placing in it takes, made as needed.
struct PlacementBook::Page
{
    std::unique_ptr<Node> root;
    std::unique_ptr<EpubCfi::Document> cfi;
    std::optional<QHash<QString, Kepub::SpanStart>> spans;
    // The body's text folded, and where each of its characters starts and
    // ends in the page.
    bool foldedMade = false;
    QString text;
    QList<EpubCfi::Point> starts;
    QList<EpubCfi::Point> ends;
    // The nodes in document order.
    QHash<const Node *, int> order;

    void fold()
    {
        if (foldedMade)
            return;
        foldedMade = true;
        Node *html = XmlTree::documentElement(root.get());
        const Node *body = html ? XmlTree::childElement(html, "body") : nullptr;
        bool space = false;
        EpubCfi::Point spaceStart;
        const std::function<void(const Node &)> walk = [&](const Node &node) {
            order.insert(&node, int(order.size()));
            if (node.isElement("script") || node.isElement("style"))
                return;
            if (isText(node)) {
                for (qsizetype i = 0; i < node.text.size(); ++i) {
                    const QChar c = node.text.at(i);
                    if (skippedCharacter(c))
                        continue;
                    if (c.isSpace()) {
                        if (!space)
                            spaceStart = { &node, i };
                        space = true;
                        continue;
                    }
                    if (space && !text.isEmpty()) {
                        text += u' ';
                        starts.append(spaceStart);
                        ends.append({ &node, i });
                    }
                    space = false;
                    text += c;
                    starts.append({ &node, i });
                    ends.append({ &node, i + 1 });
                }
            }
            for (const auto &child : node.children)
                walk(*child);
        };
        walk(body ? *body : *root);
    }

    // The folded index of a place, -1 for none.
    qsizetype indexOf(const EpubCfi::Point &point)
    {
        fold();
        for (qsizetype i = 0; i < starts.size(); ++i) {
            if (starts.at(i).node == point.node && starts.at(i).offset >= point.offset)
                return i;
        }
        return -1;
    }

    // Where `needle` is in the page, the occurrence nearest `hint`; -1 for
    // none.
    qsizetype find(const QString &needle, qsizetype hint)
    {
        fold();
        for (const Qt::CaseSensitivity sensitivity : { Qt::CaseSensitive, Qt::CaseInsensitive }) {
            qsizetype best = -1;
            for (qsizetype at = text.indexOf(needle, 0, sensitivity); at >= 0;
                 at = text.indexOf(needle, at + 1, sensitivity)) {
                if (best < 0 || qAbs(at - hint) < qAbs(best - hint))
                    best = at;
            }
            if (best >= 0)
                return best;
        }
        return -1;
    }
};

PlacementBook::PlacementBook() = default;
PlacementBook::~PlacementBook() = default;

std::unique_ptr<PlacementBook> PlacementBook::open(const QString &path, const QString &format, QString *error)
{
    std::unique_ptr<EpubBook> book = EpubBook::open(path, error);
    if (!book)
        return nullptr;
    std::unique_ptr<PlacementBook> placement(new PlacementBook);
    placement->m_book = std::move(book);
    placement->m_format = format.toUpper();
    return placement;
}

PlacementBook::Page *PlacementBook::page(int spineIndex)
{
    if (spineIndex < 0 || spineIndex >= m_book->spine().size())
        return nullptr;
    auto found = m_pages.find(spineIndex);
    if (found == m_pages.end()) {
        auto page = std::make_unique<Page>();
        page->root = XmlTree::parse(m_book->read(m_book->spine().at(spineIndex).name));
        if (page->root)
            page->cfi = std::make_unique<EpubCfi::Document>(*page->root);
        found = m_pages.emplace(spineIndex, std::move(page)).first;
    }
    return found->second->root ? found->second.get() : nullptr;
}

int PlacementBook::spineIndexOf(const QString &contentId) const
{
    const QString name = fileOf(contentId);
    if (name.isEmpty())
        return -1;
    for (const QString &candidate : { name, QUrl::fromPercentEncoding(name.toUtf8()) }) {
        const int index = m_book->spineIndex(candidate);
        if (index >= 0)
            return index;
    }
    // The same file in another folder: a book made again, its files moved.
    const QString fileName = name.section(u'/', -1);
    for (int index = 0; index < m_book->spine().size(); ++index) {
        if (m_book->spine().at(index).name.section(u'/', -1).compare(fileName, Qt::CaseInsensitive) == 0)
            return index;
    }
    return -1;
}

Placement PlacementBook::place(const KoboHighlights::Highlight &highlight)
{
    Placement placement;
    const QString needle = folded(highlight.text);
    if (needle.isEmpty()) {
        placement.reason = QStringLiteral("note without highlighted text");
        return placement;
    }
    const int expected = spineIndexOf(highlight.contentId);
    std::optional<EpubCfi::Point> start;
    std::optional<EpubCfi::Point> end;
    int spineIndex = expected;
    qsizetype hint = -1;

    // Kobo's spans, made again of the page as kepubify makes them, or the
    // KEPUB's own.
    Page *page = this->page(expected);
    const QString startSpan = spanIdOf(highlight.startPath);
    const QString endSpan = spanIdOf(highlight.endPath);
    if (page && !startSpan.isEmpty() && !endSpan.isEmpty()) {
        const auto pointOf = [&](const QString &id, qint64 offset) -> std::optional<EpubCfi::Point> {
            if (m_format == QLatin1String("KEPUB")) {
                const Node *span = elementById(*page->root, id);
                if (!span || span->children.empty() || !isText(*span->children.front())
                    || offset > span->children.front()->text.size())
                    return std::nullopt;
                return EpubCfi::Point{ span->children.front().get(), qsizetype(offset) };
            }
            if (!page->spans)
                page->spans = Kepub::spanStarts(*page->root);
            const auto found = page->spans->constFind(id);
            if (found == page->spans->cend() || offset > found->length)
                return std::nullopt;
            return EpubCfi::Point{ found->text, found->offset + qsizetype(offset) };
        };
        start = pointOf(startSpan, highlight.startOffset);
        end = pointOf(endSpan, highlight.endOffset);
        if (start)
            hint = page->indexOf(*start);
        if (start && end && folded(page->cfi->textBetween(*start, *end)) == needle) {
            placement.method = Method::Spans;
        } else {
            start.reset();
            end.reset();
        }
    }

    // The text, in its file, then in the others, nearest where the Kobo has
    // it.
    if (placement.method == Method::None && page) {
        if (hint < 0) {
            page->fold();
            hint = qsizetype(highlight.chapterProgress * double(page->text.size()));
        }
        const qsizetype at = page->find(needle, hint);
        if (at >= 0) {
            start = page->starts.at(at);
            end = page->ends.at(at + needle.size() - 1);
            placement.method = Method::Text;
        }
    }
    if (placement.method == Method::None) {
        int best = -1;
        qsizetype bestAt = -1;
        for (int index = 0; index < m_book->spine().size(); ++index) {
            Page *candidate = index == expected ? nullptr : this->page(index);
            if (!candidate)
                continue;
            const qsizetype at = candidate->find(needle, 0);
            if (at >= 0 && (best < 0 || (expected >= 0 && qAbs(index - expected) < qAbs(best - expected)))) {
                best = index;
                bestAt = at;
            }
        }
        if (best >= 0) {
            page = this->page(best);
            spineIndex = best;
            start = page->starts.at(bestAt);
            end = page->ends.at(bestAt + needle.size() - 1);
            placement.method = Method::Book;
        }
    }
    if (placement.method == Method::None) {
        placement.reason = expected < 0 && fileOf(highlight.contentId).isEmpty() ? QStringLiteral("file not named")
                                                                               : QStringLiteral("text not found");
        return placement;
    }

    placement.spineIndex = spineIndex;
    placement.spineName = m_book->spine().at(spineIndex).name;
    placement.startCfi = page->cfi->encode(*start->node, start->offset);
    placement.endCfi = page->cfi->encode(*end->node, end->offset);
    page->fold();
    const int startOrder = page->order.value(start->node, -1);
    placement.tocFamily = HighlightJson::tocFamily(*m_book, spineIndex, [&](const QString &fragment) {
        const Node *target = elementById(*page->root, fragment);
        return target && page->order.value(target, -1) <= startOrder;
    });
    return placement;
}

QDateTime changedAt(const KoboHighlights::Highlight &highlight)
{
    static const QRegularExpression zoned(QStringLiteral("(Z|[+-]\\d\\d:?\\d\\d)$"));
    for (const QString &written : { highlight.modified, highlight.created }) {
        const QString text = written.trimmed();
        QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!time.isValid())
            continue;
        if (!zoned.match(text).hasMatch())
            time = QDateTime(time.date(), time.time(), QTimeZone::UTC);
        return time.toUTC();
    }
    return {};
}

QJsonObject annotation(const KoboHighlights::Highlight &highlight, const Placement &placement)
{
    const QDateTime changed = changedAt(highlight);
    const QStringList family = placement.tocFamily.isEmpty() && !highlight.chapterTitle.isEmpty()
        ? QStringList{ highlight.chapterTitle }
        : placement.tocFamily;
    return HighlightJson::highlight(highlight.id,
                                    HighlightJson::isoTimestamp(changed.isValid() ? changed : QDateTime::currentDateTimeUtc()),
                                    highlight.text, placement.startCfi, placement.endCfi,
                                    KoboHighlights::calibreColor(highlight.color), placement.spineName, placement.spineIndex,
                                    family, highlight.note);
}

} // namespace KoboPlacement
