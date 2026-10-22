#pragma once

#include <QBitArray>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

#include "book.h"
#include "calibre_text.h"

// Calibre's search language, ported from src/calibre/utils/search_query_parser.py
// and src/calibre/db/search.py of Calibre 9.16: `field:value`, `=exact`,
// `~regex`, `^accents`, `and`, `or`, `not`, parentheses, comparisons of
// numbers, ratings, sizes and dates, `true` and `false`, counts with `#`,
// identifiers, saved searches (`search:`), virtual libraries (`vl:`), grouped
// search terms and user categories (`@`). What Calibre's ICU collators
// decide, whether "ecume" is in "L'Écume", is folded once by a table taken
// from Calibre itself, calibre_search.inc, and checked against Calibre by
// tests/calibre_search_test.cpp.
namespace CalibreSearch {

// Text as Calibre's primary collators compare it: lower case, without
// accents, "ae" for "æ", "は" for "パ", and without spaces and punctuation
// unless they are kept. UTF-8.
struct Folded
{
    QByteArray text;
    // The places, in bytes, inside a character folded to several, "æ" to
    // "ae": a match can neither start nor end there. Most texts have none.
    QList<int> inside;
};

Folded fold(QStringView text, bool keepPunctuation = false);
// primary_no_punc_contains(query, text), or primary_contains() with both
// folded keeping punctuation: whether `query` is in `text`. An empty query
// is in every text.
bool contains(const Folded &text, const Folded &query);

// What a search reads besides the books: the library's own preferences, as
// Calibre keeps them in metadata.db.
struct Preferences
{
    // Name and query.
    QMap<QString, QString> savedSearches;
    QMap<QString, QString> virtualLibraries;
    // Name and the fields it searches, "authors", "tags", "#genre".
    QMap<QString, QStringList> groupedSearchTerms;
    // Name, "Favorites" or "Genres.SF", and its items: name and field.
    QMap<QString, QList<QPair<QString, QString>>> userCategories;
    // The virtual library Calibre opens the library with.
    QString virtualLibraryOnStartup;
    // Whether a yes-or-no custom column has a third value, none, or none is
    // no.
    bool boolsAreTristate = true;

    // From the preferences table's rows, key and JSON value.
    static Preferences fromRows(const QHash<QString, QString> &rows);
};

// The books of a library, with what a search compares of them folded once:
// made with each load, on the worker thread.
class Index
{
public:
    Index() = default;
    Index(const LibraryData &data, const Preferences &preferences, const CalibreText::Tweaks &tweaks,
          const QString &interfaceLanguage);

    const LibraryData &data() const { return m_data; }
    const Preferences &preferences() const { return m_preferences; }
    qsizetype size() const { return m_data.books.size(); }
    // The folded titles, by book row, and names of the library's authors,
    // series, tags and publishers, in the order of its lists.
    const QList<Folded> &titles() const { return m_titles; }
    const QList<Folded> &authorNames() const { return m_authorNames; }
    const QList<Folded> &seriesNames() const { return m_seriesNames; }
    const QList<Folded> &tagNames() const { return m_tagNames; }
    const QList<Folded> &publisherNames() const { return m_publisherNames; }
    // The row of an author in the library's list, -1 when unknown.
    qsizetype authorRow(int id) const { return m_authorRows.value(id, -1); }

private:
    friend class Search;

    LibraryData m_data;
    Preferences m_preferences;
    CalibreText::Tweaks m_tweaks;
    QString m_interfaceLanguage;
    // By book row.
    QList<Folded> m_titles;
    QList<Folded> m_comments;
    QList<Folded> m_authorSorts;
    QList<Folded> m_sorts;
    QList<Folded> m_uuids;
    // By item, in the order of the library's lists, and the row of each id.
    QList<Folded> m_authorNames;
    QList<Folded> m_tagNames;
    QList<Folded> m_seriesNames;
    QList<Folded> m_publisherNames;
    QHash<int, qsizetype> m_authorRows;
    QHash<int, qsizetype> m_tagRows;
    QHash<int, qsizetype> m_seriesRows;
    QHash<int, qsizetype> m_publisherRows;
    // The custom columns' search terms, "#genre", and "#saga_index" for a
    // series column's numbers.
    QStringList m_customTerms;
};

// What a search found: the rows of the books it matches, one bit per book
// of the index; or why it could not run, Calibre's reasons.
struct Result
{
    QBitArray rows;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Searches the books for `query`, among those `restriction` finds when it
// is set, as Calibre's search() with a search restriction: a virtual
// library's search, for instance. An empty query finds the restriction's
// books, all of them without one.
Result search(const Index &index, const QString &query, const QString &restriction = {});
// The same among `universe`, the rows a restriction found already.
Result search(const Index &index, const QString &query, const QBitArray &universe);

// The search terms the parser knows as fields, "title", "author", "#genre",
// "vl", "search", and the library's grouped search terms and user
// categories.
QStringList locations(const Index &index);

} // namespace CalibreSearch
