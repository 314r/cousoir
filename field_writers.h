#pragma once

#include <QDateTime>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>

#include <optional>

#include "book_edit.h"
#include "calibre_date.h"
#include "calibre_text.h"

class SqliteDb;

// Calibre's field writers, ported from src/calibre/db/write.py and
// Cache.set_field() of Calibre 9.16, on a connection prepared like Calibre's.
// Each value goes through Calibre's adapter, a value equal to the stored one
// changes nothing, and items no book uses any more are removed, as Calibre
// removes them.
namespace FieldWriters {

struct Context
{
    CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    // Calibre's interface language: title sorts, "Unknown".
    QString interfaceLanguage;
    // last_modified of the books a change touches.
    QDateTime now;
    // Items with a note in .calnotes, as (field, id): Cousoir keeps them when
    // no book uses them any more, where Calibre would retire the note.
    QSet<QPair<QString, int>> notes;
    // Whether a value differing from an item only by case renames the item,
    // as the editor does; set_metadata() for a new book keeps the item's.
    bool allowCaseChange = true;
    // set_metadata(ignore_errors=True) of a new book: a custom column's
    // value that its adapter rejects is left out.
    bool ignoreCustomErrors = false;
};

struct Result
{
    // The books whose metadata changed, the edited one and those of an item
    // whose case changed.
    QSet<int> dirtied;
    // The sort Calibre holds for an author renamed by a case change, which
    // its books' author sort follow while the authors table keeps the old
    // one. Kept across the edits of one change.
    QHash<int, QString> authorSorts;
};

// Writes `edit` in Calibre's set_metadata() order, adding to `result`.
// Returns false with `error` when a statement fails; the caller rolls the
// transaction back.
bool apply(SqliteDb &db, const BookEdit &edit, const Context &context, Result *result, QString *error);

// mark_as_dirty(): last_modified to `now` and the books queued for Calibre
// to write their metadata.opf again.
bool markDirtied(SqliteDb &db, const QSet<int> &books, const QDateTime &now, QString *error);

// update_last_modified(): last_modified to `now`, for books whose formats
// changed, which Calibre does not mark dirty.
bool touch(SqliteDb &db, const QSet<int> &books, const QDateTime &now, QString *error);

// sqlite_datetime(): "2015-07-07 18:41:43.605980+00:00", without the
// fraction when it is 0.
QString sqliteDate(const QDateTime &date);

// parse_only_date(): a date without time, at midnight UTC, moved off the
// first and last days of the month so that it reads the same month in every
// timezone. A year alone takes the current month; a month alone the 15th.
// Empty for text that is no date.
std::optional<QDateTime> parseOnlyDate(const QString &text, const QDateTime &now);
// A date with its time, "1947-03-20T22:00:00+00:00" as the library stores
// it: taken as it is, to the microsecond, for putting a date back or adding
// a book. Empty for any other text.
std::optional<CalibreDate> parseExactDate(const QString &text);

} // namespace FieldWriters
