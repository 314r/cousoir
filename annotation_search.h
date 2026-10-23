#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "python_json.h"

class SqliteDb;
struct LibraryData;

// The highlights and bookmarks the library keeps, from Calibre's viewer and
// from Cousoir's reader, as Calibre's "Browse annotations" finds them
// (db/backend.py all_annotations() and search_annotations(),
// gui2/library/annotations.py): all of them newest first, or those a search
// of Calibre's full text index finds, best first, with the words found
// marked; each book's in their order in the book; and exported as text or
// Markdown as Calibre exports them.
namespace AnnotationSearch {

struct Query
{
    // FTS5's query language, as Calibre's search box takes it; empty for
    // all of them.
    QString text;
    // Words found by their stems, "read" finding "reading".
    bool stemming = true;
    // "highlight", "bookmark", or empty for both.
    QString type;
    // restrict_to_user: one user's alone, a reader's or the viewer's; all
    // when empty.
    QString userType;
    QString user;
};

struct Hit
{
    int id = 0;
    int bookId = 0;
    QString format;
    QString userType;
    QString user;
    // The annotation's JSON, as the viewer keeps it.
    PythonJson::Value annotation;
    // What shows: the text highlighted or the bookmark's title; for a
    // search, the snippet around the words found, marked with \x1d before
    // and after each.
    QString text;
};

// The marks a search puts around the words it finds.
constexpr QChar markStart = u'\x1d';
constexpr QChar markEnd = u'\x1d';

// The annotations `query` finds in `db`, a library's database on which
// Calibre's full text tokenizers are registered; the removed ones left
// out, 4096 at most without a search, as Calibre's browser asks for. On a
// query FTS5 cannot read, nothing, with the error.
QList<Hit> find(SqliteDb &db, const Query &query, QString *error);

// sorted_items(): a book's annotations in their order in the book, by
// their CFIs, those without one last.
QList<Hit> sortedInBook(QList<Hit> hits);

// The annotations grouped by book, the books in the order of their titles'
// sort, each book's sorted in the book.
QList<QPair<int, QList<Hit>>> groupedByBook(const QList<Hit> &hits, const LibraryData &data);

// exported_data(): each book's title, then its annotations under their
// chapters, as plain text or Markdown; the times in this computer's short
// format, an annotation without a chapter under Calibre's "Unknown
// chapter" in `interfaceLanguage`.
QString exportText(const QList<Hit> &hits, const LibraryData &data, bool markdown, const QString &interfaceLanguage);

// How many highlights and bookmarks the library keeps, the removed aside.
int count(SqliteDb &db);

// all_annotation_users(): the users the annotations are of, type and name.
QList<QPair<QString, QString>> users(SqliteDb &db);

} // namespace AnnotationSearch
