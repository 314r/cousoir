#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

#include "calibre_date.h"

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
    // When the book was added: only for books Reliure adds or restores.
    std::optional<CalibreDate> timestamp;

    bool isEmpty() const;
    // Whether the book's folder and file names may change.
    bool movesFiles() const;

    // From the keys QML and `reliure-cli apply` use: title, sort, authors,
    // authorSort, series, seriesIndex, tags, rating, publisher, languages,
    // identifiers ({ type: value } or "isbn:…, goodreads:…"), pubdate,
    // comments, coverFile (a path to read) and removeCover. Lists may be
    // arrays or text: authors split on "&", the others on ",". Returns false
    // with `error` for a value of the wrong kind or an unknown key.
    static bool fromVariantMap(int bookId, const QVariantMap &changes, BookEdit *edit, QString *error);
};
