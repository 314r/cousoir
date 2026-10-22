#include "palette.h"

#include <QObject>
#include <QStringList>

#include <algorithm>

namespace {

using CalibreSearch::Folded;

// How well a folded name answers the words: 0 when one is missing; more for
// a name that is the whole query, or starts with it.
int score(const Folded &name, const QList<Folded> &words, const Folded &whole)
{
    for (const Folded &word : words) {
        if (!CalibreSearch::contains(name, word))
            return 0;
    }
    if (name.text == whole.text)
        return 30;
    if (name.text.startsWith(whole.text))
        return 20;
    return 10;
}

} // namespace

namespace Palette {

QList<Hit> search(const CalibreSearch::Index &index, const QString &query, int limit, const QList<Hit> &commands)
{
    QList<Folded> words;
    for (const QString &word : query.split(u' ', Qt::SkipEmptyParts)) {
        const Folded folded = CalibreSearch::fold(word);
        if (!folded.text.isEmpty())
            words.append(folded);
    }
    if (limit <= 0)
        return {};
    if (words.isEmpty())
        return commands.mid(0, limit);
    const Folded whole = CalibreSearch::fold(query);
    const LibraryData &data = index.data();
    QList<Hit> hits;

    // The window's commands before anything of the library as close.
    for (const Hit &command : commands) {
        const int found = score(CalibreSearch::fold(command.title), words, whole);
        if (found > 0) {
            Hit hit = command;
            hit.score = found + 9;
            hits.append(hit);
        }
    }

    const auto items = [&](const QString &kind, const QList<FacetItem> &list, const QList<Folded> &names) {
        for (qsizetype row = 0; row < list.size() && row < names.size(); ++row) {
            const int found = score(names.at(row), words, whole);
            if (found > 0) {
                const FacetItem &item = list.at(row);
                // Items before books of the same closeness.
                hits.append({ kind, item.id, item.name,
                              item.count == 1 ? QObject::tr("1 book") : QObject::tr("%1 books").arg(item.count),
                              found + 5 });
            }
        }
    };
    items(QStringLiteral("authors"), data.authors, index.authorNames());
    items(QStringLiteral("series"), data.series, index.seriesNames());
    items(QStringLiteral("tags"), data.tags, index.tagNames());
    items(QStringLiteral("publishers"), data.publishers, index.publisherNames());

    // A book by its title, or by its title and authors together: every word
    // in one or the other.
    const QList<Folded> &titles = index.titles();
    for (qsizetype row = 0; row < data.books.size() && row < titles.size(); ++row) {
        const Book &book = data.books.at(row);
        int found = score(titles.at(row), words, whole);
        if (found == 0) {
            bool all = true;
            for (const Folded &word : words) {
                bool here = CalibreSearch::contains(titles.at(row), word);
                for (qsizetype i = 0; !here && i < book.authorIds.size(); ++i) {
                    const qsizetype author = index.authorRow(book.authorIds.at(i));
                    here = author >= 0 && CalibreSearch::contains(index.authorNames().at(author), word);
                }
                if (!here) {
                    all = false;
                    break;
                }
            }
            found = all ? 8 : 0;
        }
        if (found > 0)
            hits.append({ QStringLiteral("book"), book.id, book.title, book.authors.join(QStringLiteral(" & ")), found });
    }

    const auto closer = [](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.title.size() != b.title.size())
            return a.title.size() < b.title.size();
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    };
    if (hits.size() > limit) {
        std::partial_sort(hits.begin(), hits.begin() + limit, hits.end(), closer);
        hits.resize(limit);
    } else {
        std::sort(hits.begin(), hits.end(), closer);
    }
    return hits;
}

} // namespace Palette
