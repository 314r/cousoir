#pragma once

#include <QList>
#include <QString>

// The books' text, searched in the index Calibre keeps of it when its full
// text search is turned on: full-text-search.db beside metadata.db, FTS5
// tables tokenized by Calibre's tokenizer (db/fts/connect.py). Read only,
// with nothing written beside the file.
namespace BookTextSearch {

struct Status
{
    // Whether Calibre made an index for the library.
    bool exists = false;
    // The books' formats indexed, and those waiting to be.
    int indexed = 0;
    int pending = 0;
};

struct Hit
{
    int bookId = 0;
    QString format;
    // The passage around the words found, each marked with \x1d before and
    // after, as Calibre's snippet() makes it.
    QString snippet;
};

QString indexPath(const QString &libraryPath);
Status status(const QString &libraryPath);

// The formats whose text has what `query` asks, in FTS5's query language as
// Calibre's full text search takes it, best first, `limit` at most; by the
// words' stems with `stemming`, then as written (Calibre may have indexed in
// another language). Calibre's tokenizer
// splits and folds words as `interfaceLanguage` does. On a query FTS5
// cannot read, nothing, with the error.
QList<Hit> find(const QString &libraryPath, const QString &query, bool stemming, const QString &interfaceLanguage,
                int limit, QString *error);

} // namespace BookTextSearch
