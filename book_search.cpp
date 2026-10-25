// SPDX-License-Identifier: GPL-3.0-only

#include "book_search.h"

#include <QMutexLocker>
#include <QTextBoundaryFinder>

#include <optional>

#include "calibre_text.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "highlight_json.h"
#include "xml_tree.h"

namespace {

// A character folded as the search folds text: an ASCII letter at once,
// others through CalibreText::foldForSearch(), which may make it longer
// ("œ", "oe") or nothing (a combining accent).
void appendFolded(QChar c, QString *folded)
{
    const char16_t code = c.unicode();
    if (code < 0x80) {
        folded->append(code >= u'A' && code <= u'Z' ? QChar(code + 32) : c);
        return;
    }
    folded->append(CalibreText::foldForSearch(QString(c)));
}

// A query as the texts are folded: white space as one space.
QString foldQuery(const QString &query)
{
    QString folded;
    for (const QChar c : query.simplified()) {
        if (!EpubCfi::invisible(c))
            appendFolded(c, &folded);
    }
    return folded;
}

// Some words before or after a place, cut at a word.
QString around(const QString &text, qsizetype from, qsizetype to, bool before)
{
    from = std::max<qsizetype>(0, from);
    to = std::min(text.size(), to);
    if (from >= to)
        return {};
    QString part = text.mid(from, to - from);
    if (before && from > 0) {
        const qsizetype space = part.indexOf(u' ');
        part = (space >= 0 && space < part.size() - 1 ? part.mid(space + 1) : part).prepend(QStringLiteral("…"));
    } else if (!before && to < text.size()) {
        const qsizetype space = part.lastIndexOf(u' ');
        part = (space > 0 ? part.left(space) : part) + QStringLiteral("…");
    }
    return part;
}

} // namespace

// A spine item's text folded, and for each of its characters the one of
// the page's text it comes from.
struct BookSearch::Item
{
    QString folded;
    QList<qsizetype> origins;
};

BookSearch::BookSearch(std::shared_ptr<const EpubBook> book) : m_book(std::move(book))
{
    m_items.resize(m_book ? m_book->spine().size() : 0);
}

BookSearch::~BookSearch() = default;

QList<BookSearch::Match> BookSearch::find(const QString &query, int limit, bool *more)
{
    const QMutexLocker locker(&m_lock);
    if (more)
        *more = false;
    QList<Match> matches;
    const QString needle = foldQuery(query);
    if (!m_book || needle.isEmpty())
        return matches;
    for (int index = 0; index < m_items.size(); ++index) {
        // A page is parsed for its places only when it has some.
        std::unique_ptr<XmlTree::Node> root;
        std::optional<EpubCfi::PageText> page;
        std::shared_ptr<Item> &item = m_items[index];
        const auto parse = [&] {
            if (!root)
                root = XmlTree::parse(m_book->read(m_book->spine().at(index).name));
            if (root && !page)
                page = EpubCfi::PageText::of(*root, true);
            return root != nullptr;
        };
        if (!item) {
            item = std::make_shared<Item>();
            if (parse()) {
                item->folded.reserve(page->text.size());
                item->origins.reserve(page->text.size());
                for (qsizetype i = 0; i < page->text.size(); ++i) {
                    const qsizetype before = item->folded.size();
                    appendFolded(page->text.at(i), &item->folded);
                    for (qsizetype k = before; k < item->folded.size(); ++k)
                        item->origins.append(i);
                }
            }
        }
        qsizetype at = item->folded.indexOf(needle);
        if (at < 0)
            continue;
        if (!parse())
            continue;
        const EpubCfi::Document document(*root);
        for (; at >= 0; at = item->folded.indexOf(needle, at + needle.size())) {
            if (matches.size() >= limit) {
                if (more)
                    *more = true;
                return matches;
            }
            const qsizetype first = item->origins.at(at);
            const qsizetype last = item->origins.at(at + needle.size() - 1);
            const EpubCfi::Point start = page->starts.at(first);
            const EpubCfi::Point end = page->ends.at(last);
            Match match;
            match.spineIndex = index;
            match.start = document.encode(*start.node, start.offset);
            match.end = document.encode(*end.node, end.offset);
            match.text = page->text.mid(first, last - first + 1);
            match.before = around(page->text, first - 50, first, true);
            match.after = around(page->text, last + 1, last + 1 + 80, false);
            const int order = page->order.value(start.node, -1);
            const QStringList family = HighlightJson::tocFamily(*m_book, index, [&](const QString &fragment) {
                const XmlTree::Node *target = document.element(fragment);
                return target && page->order.value(target, -1) <= order;
            });
            match.chapter = family.isEmpty() ? QString() : family.last();
            matches.append(match);
        }
    }
    return matches;
}

struct BookSentences::Page
{
    std::unique_ptr<XmlTree::Node> root;
    std::optional<EpubCfi::Document> document;
    EpubCfi::PageText text;
};

BookSentences::BookSentences(const EpubBook &book, int spineIndex) : m_page(std::make_unique<Page>())
{
    if (spineIndex < 0 || spineIndex >= book.spine().size())
        return;
    m_page->root = XmlTree::parse(book.read(book.spine().at(spineIndex).name));
    if (!m_page->root)
        return;
    m_page->document.emplace(*m_page->root);
    m_page->text = EpubCfi::PageText::of(*m_page->root, true);
    const EpubCfi::PageText &page = m_page->text;
    // Each block on its own: from one break between blocks to the next.
    QList<qsizetype> ends = page.breaks;
    ends.append(page.text.size());
    qsizetype blockStart = 0;
    for (const qsizetype blockEnd : std::as_const(ends)) {
        const QString block = page.text.mid(blockStart, blockEnd - blockStart);
        QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, block);
        qsizetype from = 0;
        for (qsizetype to = finder.toNextBoundary(); to > 0; to = finder.toNextBoundary()) {
            // Without the white space around it.
            qsizetype first = from;
            qsizetype last = to - 1;
            while (first <= last && block.at(first).isSpace())
                ++first;
            while (last >= first && block.at(last).isSpace())
                --last;
            from = to;
            if (first > last)
                continue;
            const QString text = block.mid(first, last - first + 1);
            // Nothing to say in a line of punctuation or a lone number.
            if (std::none_of(text.cbegin(), text.cend(), [](QChar c) { return c.isLetter(); }))
                continue;
            const qsizetype at = blockStart + first;
            const qsizetype end = blockStart + last;
            const EpubCfi::Point startPoint = page.starts.at(at);
            const EpubCfi::Point endPoint = page.ends.at(end);
            m_sentences.append({ m_page->document->encode(*startPoint.node, startPoint.offset),
                                 m_page->document->encode(*endPoint.node, endPoint.offset), text, at });
        }
        // Past the space between the blocks.
        blockStart = blockEnd + 1;
    }
}

BookSentences::~BookSentences() = default;

int BookSentences::sentenceAt(const QString &cfi) const
{
    if (!m_page->document || cfi.isEmpty())
        return 0;
    const EpubCfi::Point point = m_page->document->decode(cfi);
    const qsizetype at = point.node ? m_page->text.indexOf(point) : -1;
    if (at < 0)
        return 0;
    for (int i = 0; i < m_sentences.size(); ++i) {
        const Sentence &sentence = m_sentences.at(i);
        if (sentence.at + sentence.text.size() > at)
            return i;
    }
    return std::max(0, int(m_sentences.size()) - 1);
}

QString BookSentences::cfiAt(qsizetype at) const
{
    if (!m_page->document || at < 0 || at >= m_page->text.starts.size())
        return {};
    const EpubCfi::Point point = m_page->text.starts.at(at);
    return m_page->document->encode(*point.node, point.offset);
}

