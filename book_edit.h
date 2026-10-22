#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

#include "calibre_date.h"
#include "calibre_text.h"

struct LibraryData;

// A change to one book's metadata, a value per Calibre field. A field left
// unset keeps its value; the values are the user's, Calibre's writers clean
// them up.
struct BookEdit
{
    int bookId = 0;
    std::optional<QString> title;
    std::optional<QString> sort;
    std::optional<QStringList> authors;
    std::optional<QString> authorSort;
    // "Name", or "Name [2.5]" as Calibre's series field reads it; empty takes
    // the book out of its series.
    std::optional<QString> series;
    std::optional<double> seriesIndex;
    std::optional<QStringList> tags;
    // 0 to 10, two per star; 0 removes the rating.
    std::optional<int> rating;
    std::optional<QString> publisher;
    // ISO 639 codes, or names Calibre knows.
    std::optional<QStringList> languages;
    // Type and value; the list replaces the book's identifiers.
    std::optional<QList<QPair<QString, QString>>> identifiers;
    // "1947-03-20", "1947-03" or "1947"; empty clears the date.
    std::optional<QString> pubdate;
    // HTML; empty removes the description.
    std::optional<QString> comments;
    // An image file's contents; empty removes the cover.
    std::optional<QByteArray> cover;
    // When the book was added: only for books Cousoir adds or restores.
    std::optional<CalibreDate> timestamp;

    bool isEmpty() const;
    // Whether the book's folder and file names may change.
    bool movesFiles() const;

    // From the keys QML and `cousoir-cli apply` use: title, sort, authors,
    // authorSort, series, seriesIndex, tags, rating, publisher, languages,
    // identifiers ({ type: value } or "isbn:…, goodreads:…"), pubdate,
    // comments, coverFile (a path to read) and removeCover. Lists may be
    // arrays or text: authors split on "&", the others on ",". Returns false
    // with `error` for a value of the wrong kind or an unknown key.
    static bool fromVariantMap(int bookId, const QVariantMap &changes, BookEdit *edit, QString *error);
};

// A change to several books at once, as Calibre's bulk metadata edit,
// MetadataBulkDialog, makes it: one value for every book, tags added and
// removed from each book's own, series numbers given in the books' order.
// A field left unset keeps each book's value.
struct BulkEdit
{
    // In the order the list shows them, which numbers them in a series.
    QList<int> bookIds;
    std::optional<QStringList> authors;
    // 0 to 10; 0 removes the rating.
    std::optional<int> rating;
    // Empty removes the publisher, the series or the languages.
    std::optional<QString> publisher;
    std::optional<QString> series;
    // Numbers the books in the series: from `seriesStart` by
    // `seriesIncrement`, or without a start after the series' last number.
    // Unnumbered, each book's number becomes 1, unless the
    // series_index_auto_increment tweak is "no_change".
    bool numberSeries = false;
    std::optional<double> seriesStart;
    double seriesIncrement = 1;
    std::optional<QStringList> languages;
    // A date updateBook() takes; empty clears it.
    std::optional<QString> pubdate;
    bool removeAllTags = false;
    QStringList addTags;
    QStringList removeTags;

    bool isEmpty() const;

    // From the keys QML and `cousoir-cli apply` use: books, authors, rating,
    // publisher, series, numberSeries, seriesStart, seriesIncrement,
    // languages, pubdate, removeAllTags, addTags and removeTags. Returns
    // false with `error` for a value of the wrong kind or an unknown key.
    static bool fromVariantMap(const QVariantMap &changes, BulkEdit *edit, QString *error);

    // Each book's edit, in do_all()'s order: the authors, which their author
    // sort follows, the rating, publisher, series and number, publication
    // date, languages, then the tags by bulk_modify_tags(), which keeps the
    // case of a tag the library has. Books not in `data` are left out.
    QList<BookEdit> edits(const LibraryData &data, const CalibreText::Tweaks &tweaks) const;
};
