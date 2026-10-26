// SPDX-License-Identifier: GPL-3.0-only

#include "kobo_placement.h"

#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <functional>
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

bool isText(const Node &node)
{
    return node.kind == Node::Kind::Text || node.kind == Node::Kind::Cdata;
}

// "span#kobo\.3\.1": the id of Kobo's span, "kobo.3.1".
QString spanIdOf(const QString &path)
{
    static const QRegularExpression span(QStringLiteral("kobo\\\\?\\.(\\d+)\\\\?\\.(\\d+)"));
    const QRegularExpressionMatch match = span.match(path);
    return match.hasMatch() ? QStringLiteral("kobo.%1.%2").arg(match.captured(1), match.captured(2)) : QString();
}

// The span of Kobo's a node is in, null for none.
const Node *spanOf(const Node *node)
{
    for (; node; node = node->parent) {
        if (node->isElement("span") && node->attribute(QStringLiteral("id")).startsWith(QLatin1String("kobo.")))
            return node;
    }
    return nullptr;
}

// How far into the text of `span` a place in one of its texts is.
qint64 offsetIn(const Node &span, const EpubCfi::Point &point)
{
    qint64 offset = 0;
    const std::function<bool(const Node &)> walk = [&](const Node &node) {
        if (&node == point.node) {
            offset += point.offset;
            return true;
        }
        if (isText(node)) {
            offset += node.text.size();
            return false;
        }
        for (const auto &child : node.children) {
            if (walk(*child))
                return true;
        }
        return false;
    };
    walk(span);
    return offset;
}

} // namespace

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
    // A highlight's may name the section of the file it is in: "#d14e4446".
    return QDir::cleanPath(parts.join(u'/').section(u'#', 0, 0));
}

QString spanPath(const QString &id)
{
    return QStringLiteral("span#") + QString(id).replace(u'.', QLatin1String("\\."));
}

QString folded(const QString &text)
{
    QString result;
    result.reserve(text.size());
    bool space = false;
    for (const QChar c : text) {
        if (EpubCfi::invisible(c))
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
    // The body's text, white space folded, and where each of its characters
    // is in the page.
    std::optional<EpubCfi::PageText> folded;
    // In a KEPUB, Kobo's spans of the page in order.
    std::optional<QList<Node *>> koboSpans;

    const QList<Node *> &spansInOrder()
    {
        if (!koboSpans)
            koboSpans = XmlTree::findAll(root.get(), [](const Node &node) { return spanOf(&node) == &node; });
        return *koboSpans;
    }

    const EpubCfi::PageText &text()
    {
        if (!folded)
            folded = EpubCfi::PageText::of(*root);
        return *folded;
    }

    // Where `needle` is in the page, the occurrence nearest `hint`; -1 for
    // none.
    qsizetype find(const QString &needle, qsizetype hint)
    {
        const QString &text = this->text().text;
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
    return spineIndexOfFile(fileOf(contentId));
}

int PlacementBook::spineIndexOfFile(const QString &name) const
{
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
                const Node *span = page->cfi->element(id);
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
            hint = page->text().indexOf(*start);
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
        if (hint < 0)
            hint = qsizetype(highlight.chapterProgress * double(page->text().text.size()));
        const qsizetype at = page->find(needle, hint);
        if (at >= 0) {
            start = page->text().starts.at(at);
            end = page->text().ends.at(at + needle.size() - 1);
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
            start = page->text().starts.at(bestAt);
            end = page->text().ends.at(bestAt + needle.size() - 1);
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
    const int startOrder = page->text().order.value(start->node, -1);
    placement.tocFamily = HighlightJson::tocFamily(*m_book, spineIndex, [&](const QString &fragment) {
        const Node *target = page->cfi->element(fragment);
        return target && page->text().order.value(target, -1) <= startOrder;
    });
    return placement;
}

Spot PlacementBook::placePoint(const QString &bookmark, double chapterProgress)
{
    Spot spot;
    const qsizetype hash = bookmark.indexOf(u'#');
    const int spineIndex = spineIndexOfFile(QDir::cleanPath(hash < 0 ? bookmark : bookmark.left(hash)));
    Page *page = this->page(spineIndex);
    if (!page)
        return spot;
    std::optional<EpubCfi::Point> point;
    const QString id = hash < 0 ? QString() : spanIdOf(bookmark.mid(hash + 1));
    if (!id.isEmpty()) {
        if (m_format == QLatin1String("KEPUB")) {
            const Node *span = page->cfi->element(id);
            if (span && !span->children.empty() && isText(*span->children.front()))
                point = EpubCfi::Point{ span->children.front().get(), 0 };
        } else {
            if (!page->spans)
                page->spans = Kepub::spanStarts(*page->root);
            const auto found = page->spans->constFind(id);
            if (found != page->spans->cend())
                point = EpubCfi::Point{ found->text, found->offset };
        }
        if (point)
            spot.method = Method::Spans;
    }
    // Without its span, as far into the file as the Kobo was; a file with
    // no text, a picture, at its start.
    if (!point && !page->text().text.isEmpty()) {
        const qsizetype length = page->text().text.size();
        point = page->text().starts.at(std::clamp(qsizetype(chapterProgress * double(length)), qsizetype(0), length - 1));
        spot.method = Method::Text;
    }
    if (!point) {
        const QList<Node *> bodies = XmlTree::findAll(page->root.get(), [](const Node &node) { return node.isElement("body"); });
        if (bodies.isEmpty())
            return spot;
        point = EpubCfi::Point{ bodies.first(), -1 };
        spot.method = Method::Text;
    }
    spot.spineIndex = spineIndex;
    spot.spineName = m_book->spine().at(spineIndex).name;
    spot.cfi = page->cfi->encode(*point->node, point->offset);
    return spot;
}

std::optional<TextSpot> PlacementBook::textAt(int spineIndex, const QString &startCfi, const QString &endCfi)
{
    Page *page = this->page(spineIndex);
    if (!page)
        return std::nullopt;
    const EpubCfi::Point start = page->cfi->decode(startCfi);
    if (!start.node)
        return std::nullopt;
    const EpubCfi::PageText &text = page->text();
    TextSpot spot;
    spot.spineName = m_book->spine().at(spineIndex).name;
    spot.length = text.text.size();
    spot.at = text.indexOf(start);
    if (endCfi.isEmpty()) {
        // Enough of what follows to be found again, some words; at the end
        // of the file, what comes before; in a file with no text, nothing.
        if (spot.at < 0)
            spot.at = std::max(qsizetype(0), spot.length - 80);
        spot.text = text.text.mid(spot.at, 80).trimmed();
        return spot;
    }
    const EpubCfi::Point end = page->cfi->decode(endCfi);
    if (spot.at < 0 || !end.node)
        return std::nullopt;
    spot.text = folded(page->cfi->textBetween(start, end));
    if (spot.text.isEmpty())
        return std::nullopt;
    return spot;
}

std::optional<SpanRange> PlacementBook::spansAt(const TextSpot &spot)
{
    if (spot.text.isEmpty())
        return startOf(spot.spineName);
    const int spineIndex = spineIndexOfFile(spot.spineName);
    Page *page = this->page(spineIndex);
    if (!page)
        return std::nullopt;
    const EpubCfi::PageText &text = page->text();
    const qsizetype hint = spot.length > 0 ? qsizetype(double(spot.at) * double(text.text.size()) / double(spot.length)) : 0;
    const qsizetype at = page->find(spot.text, hint);
    if (at < 0)
        return std::nullopt;
    const qsizetype last = at + spot.text.size() - 1;
    // A place in no span, white space between them, is the next span's.
    const auto spanFrom = [&](qsizetype index, qsizetype step, qsizetype stop) -> const Node * {
        for (qsizetype i = index; i != stop; i += step) {
            if (const Node *span = spanOf(text.starts.at(i).node))
                return span;
        }
        return nullptr;
    };
    const Node *startSpan = spanFrom(at, 1, last + 1);
    const Node *endSpan = spanFrom(last, -1, at - 1);
    if (!startSpan || !endSpan)
        return std::nullopt;
    // From the first character in a span to the last.
    qsizetype first = at;
    while (spanOf(text.starts.at(first).node) != startSpan)
        ++first;
    qsizetype final = last;
    while (spanOf(text.starts.at(final).node) != endSpan)
        --final;
    SpanRange range;
    range.spineName = m_book->spine().at(spineIndex).name;
    range.startSpan = startSpan->attribute(QStringLiteral("id"));
    range.startOffset = offsetIn(*startSpan, text.starts.at(first));
    range.endSpan = endSpan->attribute(QStringLiteral("id"));
    range.endOffset = offsetIn(*endSpan, text.ends.at(final));
    range.progress = text.text.isEmpty() ? 0 : double(at) / double(text.text.size());
    return range;
}

SpanRange PlacementBook::startOf(const QString &spineName)
{
    SpanRange range;
    const int spineIndex = spineIndexOfFile(spineName);
    Page *page = this->page(spineIndex);
    range.spineName = spineIndex < 0 ? spineName : m_book->spine().at(spineIndex).name;
    // As the Kobo has it where a file has no span of its.
    range.startSpan = page && !page->spansInOrder().isEmpty() ? page->spansInOrder().first()->attribute(QStringLiteral("id"))
                                                               : QStringLiteral("kobo.1.1");
    range.endSpan = range.startSpan;
    return range;
}

QDateTime changedAt(const KoboHighlights::Highlight &highlight)
{
    for (const QString &written : { highlight.modified, highlight.created }) {
        const QDateTime time = KoboHighlights::koboTime(written);
        if (time.isValid())
            return time;
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
