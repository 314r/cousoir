#pragma once

#include <QList>
#include <QString>

#include "calibre_search.h"

// What the command palette finds in a library for a few letters typed.
namespace Palette {

struct Hit
{
    // "book", or the section of an item: "authors", "series", "tags",
    // "publishers".
    QString kind;
    int id = 0;
    QString title;
    // A book's authors, an item's number of books.
    QString detail;
    int score = 0;
};

// The authors, series, tags and publishers whose name has every word typed,
// and the books whose title or authors have them, the closest first: a
// name that is what was typed, then one that starts with it, items before
// books, shorter before longer. Words match as the search matches them,
// without case, accents or punctuation. At most `limit`.
// `commands`, the window's actions and sections, kind "action" or
// "section", are found by their title and come first: all of them, in
// their order, when nothing is typed.
QList<Hit> search(const CalibreSearch::Index &index, const QString &query, int limit,
                  const QList<Hit> &commands = {});

} // namespace Palette
