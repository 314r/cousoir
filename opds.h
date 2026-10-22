#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

struct LibraryData;

// The library as an OPDS 1.2 catalog, which e-readers and reading apps
// browse (KOReader, Thorium, Moon+ Reader…), and as a web page for a
// browser: Atom feeds of navigation and of books, each book with its cover
// and its files to download, and an OpenSearch description for searching
// with Calibre's search language. Only text: LibraryServer serves it.
//
// The addresses:
//   /opds                     the catalog's start
//   /opds/books?q=&sort=&title=&offset=
//                             books a search finds, sorted: "title",
//                             "timestamp" (the newest first), "authors" or
//                             "series"
//   /opds/authors, /opds/series, /opds/tags, /opds/publishers,
//   /opds/languages           each item, leading to its books
//   /opds/search.xml          the OpenSearch description
//   /get/<id>/<FORMAT>        a book's file
//   /cover/<id>, /thumb/<id>  its cover, whole or small
//   /?q=&offset=              the web page
namespace Opds {

// The books of a feed or page at a time.
constexpr int pageSize = 50;

// The catalog's start: the ways to browse.
QByteArray rootFeed(const LibraryData &data, const QString &libraryName);

// The books of `rows`, indexes into the library's books, from `offset`:
// `path` is the feed's own address without its offset, for the links to
// the pages before and after.
QByteArray booksFeed(const LibraryData &data, const QList<qsizetype> &rows, const QString &title, const QString &path,
                     int offset);

// The items of a kind, "authors", "series", "tags", "publishers" or
// "languages", each leading to its books.
QByteArray categoryFeed(const LibraryData &data, const QString &kind, const QString &interfaceLanguage);

QByteArray openSearch(const QString &libraryName);

// A book file's media type, by Calibre's format name.
QString mimeType(const QString &format);

// The web page: a search box, then the books of `rows` from `offset` with
// their covers and files.
QByteArray webPage(const LibraryData &data, const QList<qsizetype> &rows, const QString &query, int offset,
                   const QString &libraryName, const QString &error = {});

} // namespace Opds
