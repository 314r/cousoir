// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/write.py: the field writers, one_one_in_books(),
//   many_one(), many_many() and identifiers(), with get_db_id(),
//   change_case(), get_series_values(), clean_identifier() and the
//   adapt_*() functions
// - src/calibre/db/fields.py: author_sort_for_book()
// - src/calibre/db/cache.py: set_field() and set_metadata()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "field_writers.h"

#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>

#include "calibre_db.h"
#include "calibre_text.h"
#include "opf_reader.h"
#include "sqlite_db.h"

namespace {

using FieldWriters::Context;
using FieldWriters::Result;

// single_text(): stripped, its runs of spaces made one; nothing when empty.
std::optional<QString> singleText(const QString &text)
{
    const QString collapsed = CalibreText::collapseSpaces(text);
    return collapsed.isEmpty() ? std::nullopt : std::optional<QString>(collapsed);
}

// multiple_text() given a list: a value containing the separator the user
// types between values gets another one instead.
QStringList multipleText(const QStringList &values, QChar uiSeparator)
{
    const QChar replacement = uiSeparator == u';' ? u',' : u';';
    QStringList result;
    for (const QString &value : values) {
        if (CalibreText::strip(value).isEmpty())
            continue;
        const QString cleaned = CalibreText::collapseSpaces(CalibreText::strip(value).replace(uiSeparator, replacement));
        if (!cleaned.isEmpty())
            result.append(cleaned);
    }
    return result;
}

// icu_lower(): the key Calibre matches items by.
QString lowerKey(const QString &text)
{
    return text.toLower();
}

// uniq(): the first of the values with the same key, in order.
QStringList unique(const QStringList &values, bool caseInsensitive)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &value : values) {
        const QString key = caseInsensitive ? lowerKey(value) : value;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        result.append(value);
    }
    return result;
}

// A field whose items live in a table of their own: series, publisher and
// rating link a book to one item, authors, tags and languages to a list.
struct ItemTable
{
    QString field;
    QString table;
    QString column;
    QString linkTable;
    QString linkColumn;
    bool caseInsensitive;
    bool authors = false;
};

const ItemTable seriesTable{ QStringLiteral("series"), QStringLiteral("series"), QStringLiteral("name"),
                             QStringLiteral("books_series_link"), QStringLiteral("series"), true };
const ItemTable publisherTable{ QStringLiteral("publisher"), QStringLiteral("publishers"), QStringLiteral("name"),
                                QStringLiteral("books_publishers_link"), QStringLiteral("publisher"), true };
const ItemTable ratingTable{ QStringLiteral("rating"), QStringLiteral("ratings"), QStringLiteral("rating"),
                             QStringLiteral("books_ratings_link"), QStringLiteral("rating"), false };
const ItemTable authorTable{ QStringLiteral("authors"), QStringLiteral("authors"), QStringLiteral("name"),
                             QStringLiteral("books_authors_link"), QStringLiteral("author"), true, true };
const ItemTable tagTable{ QStringLiteral("tags"), QStringLiteral("tags"), QStringLiteral("name"),
                          QStringLiteral("books_tags_link"), QStringLiteral("tag"), true };
const ItemTable languageTable{ QStringLiteral("languages"), QStringLiteral("languages"), QStringLiteral("lang_code"),
                               QStringLiteral("books_languages_link"), QStringLiteral("lang_code"), true };

// An item table read for a write: id to value, and the key Calibre matches
// values by to id.
struct Items
{
    QHash<int, QString> values;
    QHash<QString, int> ids;
};

bool readItems(SqliteDb &db, const ItemTable &table, Items *items, QString *error)
{
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT id, %1 FROM %2").arg(table.column, table.table))) {
        QString value = row.at(1).toString();
        if (table.authors)
            value.replace(u'|', u',');
        const int id = row.at(0).toInt();
        const QString key = table.caseInsensitive ? lowerKey(value) : value;
        // fix_case_duplicates() would merge them; Cousoir leaves that to
        // Calibre, which runs it the next time it writes the field.
        if (items->ids.contains(key)) {
            if (error) {
                *error = QObject::tr("Two %1 differ only by case (“%2”): Calibre merges them the next time it saves "
                                     "one, and Cousoir can change them then.")
                             .arg(table.table, value);
            }
            return false;
        }
        items->values.insert(id, value);
        items->ids.insert(key, id);
    }
    return true;
}

QList<int> booksOf(SqliteDb &db, const ItemTable &table, int itemId)
{
    QList<int> books;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT book FROM %1 WHERE %2=?")
                                               .arg(table.linkTable, table.linkColumn), { itemId }))
        books.append(row.at(0).toInt());
    return books;
}

QList<int> itemsOf(SqliteDb &db, const ItemTable &table, int bookId)
{
    QList<int> items;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT %1 FROM %2 WHERE book=? ORDER BY id")
                                               .arg(table.linkColumn, table.linkTable), { bookId }))
        items.append(row.at(0).toInt());
    return items;
}

// get_db_id(): the item for `value`, inserted when new; with a case change
// noted, the existing item renamed by the caller.
bool itemId(SqliteDb &db, const ItemTable &table, const QVariant &value, const Context &context, Items *items,
            QHash<int, QString> *caseChanges, int *id, QString *error)
{
    const QString text = value.toString();
    const QString key = table.caseInsensitive ? lowerKey(text) : text;
    const auto found = items->ids.constFind(key);
    if (found != items->ids.cend()) {
        *id = *found;
        if (context.allowCaseChange && text != items->values.value(*id))
            caseChanges->insert(*id, text);
        return true;
    }
    bool ok;
    if (table.authors) {
        ok = db.run(QStringLiteral("INSERT INTO authors(name,sort) VALUES (?,?)"),
                    { QString(text).replace(u',', u'|'), CalibreText::authorToAuthorSort(text, context.tweaks) }, error);
    } else {
        ok = db.run(QStringLiteral("INSERT INTO %1(%2) VALUES (?)").arg(table.table, table.column), { value }, error);
    }
    if (!ok)
        return false;
    *id = int(db.lastInsertRowId());
    items->values.insert(*id, text);
    items->ids.insert(key, *id);
    return true;
}

} // namespace

namespace {

// Removes the items no book uses any more, as each writer of an item field
// does in Calibre, whatever it changed. Items with a note stay.
bool removeUnused(SqliteDb &db, const ItemTable &table, const Context &context, QString *error)
{
    const QList<QVariantList> unused = db.rows(QStringLiteral("SELECT id FROM %1 WHERE id NOT IN (SELECT %2 FROM %3)")
                                                   .arg(table.table, table.linkColumn, table.linkTable));
    for (const QVariantList &row : unused) {
        const int id = row.at(0).toInt();
        if (context.notes.contains({ table.field, id }))
            continue;
        if (!db.run(QStringLiteral("DELETE FROM %1 WHERE id=?").arg(table.table), { id }, error))
            return false;
    }
    return true;
}

// one_one_in_books(): a column of the book's row, written when it differs.
bool setBookColumn(SqliteDb &db, int bookId, const QString &column, const QVariant &value, Result *result,
                   QString *error, bool *changed = nullptr)
{
    const QList<QVariantList> current = db.rows(QStringLiteral("SELECT %1 FROM books WHERE id=?").arg(column),
                                                { bookId });
    if (current.isEmpty()) {
        if (error)
            *error = QObject::tr("Book %1 is no longer in the library.").arg(bookId);
        return false;
    }
    const QVariant stored = current.first().first();
    const bool same = value.typeId() == QMetaType::Double
        ? !stored.isNull() && stored.toDouble() == value.toDouble()
        : stored.isNull() == value.isNull() && stored.toString() == value.toString();
    if (changed)
        *changed = !same;
    if (same)
        return true;
    result->dirtied.insert(bookId);
    return db.run(QStringLiteral("UPDATE books SET %1=? WHERE id=?").arg(column), { value, bookId }, error);
}

// author_sort_for_book(): the authors' sorts joined by " & ", in link order,
// as Calibre holds them in memory.
QString authorSortFor(SqliteDb &db, int bookId, const Context &context, const Result &result)
{
    QStringList sorts;
    for (const QVariantList &row : db.rows(QStringLiteral(
             "SELECT authors.id, authors.name, authors.sort FROM books_authors_link link JOIN authors "
             "ON authors.id = link.author WHERE link.book=? ORDER BY link.id"), { bookId })) {
        const auto renamed = result.authorSorts.constFind(row.at(0).toInt());
        const QString sort = row.at(2).toString();
        sorts.append(renamed != result.authorSorts.cend() ? *renamed
                     : sort.isEmpty() ? CalibreText::authorToAuthorSort(row.at(1).toString().replace(u'|', u','),
                                                                        context.tweaks)
                                      : sort);
    }
    return sorts.join(QStringLiteral(" & "));
}

// change_case(): items renamed to the case the user typed; every book of
// them changed.
bool changeCase(SqliteDb &db, const ItemTable &table, const QHash<int, QString> &caseChanges, const Context &context,
                Result *result, QString *error)
{
    for (auto it = caseChanges.constBegin(); it != caseChanges.constEnd(); ++it) {
        const QString value = table.authors ? QString(it.value()).replace(u',', u'|') : it.value();
        if (!db.run(QStringLiteral("UPDATE %1 SET %2=? WHERE id=?").arg(table.table, table.column),
                    { value, it.key() }, error))
            return false;
        const QList<int> books = booksOf(db, table, it.key());
        for (const int book : books)
            result->dirtied.insert(book);
        if (!table.authors)
            continue;
        result->authorSorts.insert(it.key(), CalibreText::authorToAuthorSort(it.value(), context.tweaks));
        // A book whose author sort only differed by case follows the new one.
        for (const int book : books) {
            const QString current = db.rows(QStringLiteral("SELECT author_sort FROM books WHERE id=?"), { book })
                                        .value(0).value(0).toString();
            const QString renewed = authorSortFor(db, book, context, *result);
            if (current.compare(renewed, Qt::CaseInsensitive) == 0
                && !setBookColumn(db, book, QStringLiteral("author_sort"), renewed, result, error))
                return false;
        }
    }
    return true;
}

// many_one(): series, publisher, rating. A null value unlinks the book.
bool setManyOne(SqliteDb &db, const ItemTable &table, int bookId, const QVariant &value, const Context &context,
                Result *result, QString *error)
{
    Items items;
    if (!readItems(db, table, &items, error))
        return false;
    QHash<int, QString> caseChanges;
    int item = 0;
    if (!value.isNull() && !itemId(db, table, value, context, &items, &caseChanges, &item, error))
        return false;
    if (!caseChanges.isEmpty() && !changeCase(db, table, caseChanges, context, result, error))
        return false;
    const QList<int> current = itemsOf(db, table, bookId);
    if (current.value(0, 0) != item) {
        result->dirtied.insert(bookId);
        if (!db.run(QStringLiteral("DELETE FROM %1 WHERE book=?").arg(table.linkTable), { bookId }, error))
            return false;
        if (item > 0 && !db.run(QStringLiteral("INSERT INTO %1(book,%2) VALUES(?, ?)").arg(table.linkTable,
                                                                                         table.linkColumn),
                                { bookId, item }, error))
            return false;
    }
    return removeUnused(db, table, context, error);
}

// many_many(): authors, tags, languages, in the order given.
bool setManyMany(SqliteDb &db, const ItemTable &table, int bookId, const QStringList &values, const Context &context,
                 Result *result, QString *error)
{
    Items items;
    if (!readItems(db, table, &items, error))
        return false;
    QHash<int, QString> caseChanges;
    QList<int> ids;
    for (const QString &value : unique(values, table.caseInsensitive)) {
        int id = 0;
        if (!itemId(db, table, value, context, &items, &caseChanges, &id, error))
            return false;
        ids.append(id);
    }
    if (!caseChanges.isEmpty() && !changeCase(db, table, caseChanges, context, result, error))
        return false;
    if (itemsOf(db, table, bookId) != ids) {
        result->dirtied.insert(bookId);
        if (!db.run(QStringLiteral("DELETE FROM %1 WHERE book=?").arg(table.linkTable), { bookId }, error))
            return false;
        for (const int id : std::as_const(ids)) {
            if (!db.run(QStringLiteral("INSERT INTO %1(book,%2) VALUES(?, ?)").arg(table.linkTable, table.linkColumn),
                        { bookId, id }, error))
                return false;
        }
        if (table.authors && !setBookColumn(db, bookId, QStringLiteral("author_sort"),
                                            authorSortFor(db, bookId, context, *result), result, error))
            return false;
    }
    return removeUnused(db, table, context, error);
}

// get_series_values(): "Name [2.5]" is the series and its index.
QPair<QString, std::optional<double>> seriesValues(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^(.*)\\s+\\[([.0-9]+)\\]$"),
                                            QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = pattern.match(CalibreText::strip(value));
    if (match.hasMatch()) {
        bool ok = false;
        const double index = match.captured(2).toDouble(&ok);
        if (ok)
            return { CalibreText::strip(match.captured(1)), index };
    }
    return { value, std::nullopt };
}

// clean_identifier() and adapt_identifiers(): a type without ":" or ",",
// lower case; a value with "|" for ","; empty ones dropped, the last of a
// type kept in the place of the first.
QList<QPair<QString, QString>> cleanIdentifiers(const QList<QPair<QString, QString>> &identifiers)
{
    QList<QPair<QString, QString>> result;
    for (const auto &identifier : identifiers) {
        const QString type = CalibreText::strip(identifier.first.toLower()).remove(u':').remove(u',');
        const QString value = CalibreText::strip(identifier.second).replace(u',', u'|');
        if (type.isEmpty() || value.isEmpty())
            continue;
        auto existing = std::find_if(result.begin(), result.end(), [&](const auto &entry) { return entry.first == type; });
        if (existing != result.end())
            existing->second = value;
        else
            result.append({ type, value });
    }
    return result;
}

bool setIdentifiers(SqliteDb &db, int bookId, const QList<QPair<QString, QString>> &identifiers, Result *result,
                    QString *error)
{
    const QList<QPair<QString, QString>> cleaned = cleanIdentifiers(identifiers);
    QHash<QString, QString> current;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT type, val FROM identifiers WHERE book=?"), { bookId }))
        current.insert(row.at(0).toString(), row.at(1).toString());
    QHash<QString, QString> wanted;
    for (const auto &identifier : cleaned)
        wanted.insert(identifier.first, identifier.second);
    if (current == wanted)
        return true;
    result->dirtied.insert(bookId);
    if (!db.run(QStringLiteral("DELETE FROM identifiers WHERE book=?"), { bookId }, error))
        return false;
    for (const auto &identifier : cleaned) {
        if (!db.run(QStringLiteral("INSERT OR REPLACE INTO identifiers (book, type, val) VALUES (?, ?, ?)"),
                    { bookId, identifier.first, identifier.second }, error))
            return false;
    }
    return true;
}

// one_one_in_other() for comments.
bool setComments(SqliteDb &db, int bookId, const std::optional<QString> &text, Result *result, QString *error)
{
    const QList<QVariantList> current = db.rows(QStringLiteral("SELECT text FROM comments WHERE book=?"), { bookId });
    const std::optional<QString> stored = current.isEmpty() ? std::nullopt
                                                            : std::optional<QString>(current.first().first().toString());
    if (stored == text)
        return true;
    result->dirtied.insert(bookId);
    if (!text)
        return db.run(QStringLiteral("DELETE FROM comments WHERE book=?"), { bookId }, error);
    return db.run(QStringLiteral("INSERT OR REPLACE INTO comments(book,text) VALUES (?,?)"), { bookId, *text }, error);
}

QVariant nullable(const std::optional<QString> &text)
{
    return text ? QVariant(*text) : QVariant();
}

} // namespace

namespace FieldWriters {

bool apply(SqliteDb &db, const BookEdit &edit, const Context &context, Result *result, QString *error)
{
    const int book = edit.bookId;
    const QString unknown = CalibreText::unknownText(context.interfaceLanguage);
    if (db.rows(QStringLiteral("SELECT id FROM books WHERE id=?"), { book }).isEmpty()) {
        if (error)
            *error = QObject::tr("Book %1 is no longer in the library.").arg(book);
        return false;
    }

    if (edit.title) {
        // set_title(): the title, then its sort with Calibre's articles.
        const QString title = singleText(*edit.title).value_or(unknown);
        bool changed = false;
        if (!setBookColumn(db, book, QStringLiteral("title"), title, result, error, &changed))
            return false;
        if (changed && !setBookColumn(db, book, QStringLiteral("sort"),
                                      CalibreText::titleSort(title, {}, context.tweaks, context.interfaceLanguage),
                                      result, error))
            return false;
    }
    if (edit.authors) {
        QStringList authors;
        for (const QString &author : multipleText(*edit.authors, u'&'))
            authors.append(QString(author).replace(u'|', u','));
        if (authors.isEmpty())
            authors.append(unknown);
        if (!setManyMany(db, authorTable, book, authors, context, result, error))
            return false;
    }
    if (edit.rating) {
        const int rating = std::clamp(*edit.rating, 0, 10);
        if (!setManyOne(db, ratingTable, book, rating == 0 ? QVariant() : QVariant(rating), context, result, error))
            return false;
    }
    if (edit.seriesIndex && !setBookColumn(db, book, QStringLiteral("series_index"), *edit.seriesIndex, result, error))
        return false;
    if (edit.timestamp
        && !setBookColumn(db, book, QStringLiteral("timestamp"), edit.timestamp->isoFormat(u' '), result, error))
        return false;
    if (edit.authorSort
        && !setBookColumn(db, book, QStringLiteral("author_sort"), singleText(*edit.authorSort).value_or(QString()),
                          result, error))
        return false;
    if (edit.publisher && !setManyOne(db, publisherTable, book, nullable(singleText(*edit.publisher)), context, result,
                                      error))
        return false;
    if (edit.series) {
        const auto [name, index] = seriesValues(*edit.series);
        if (!setManyOne(db, seriesTable, book, nullable(singleText(name)), context, result, error))
            return false;
        if (index && !setBookColumn(db, book, QStringLiteral("series_index"), *index, result, error))
            return false;
    }
    if (edit.tags && !setManyMany(db, tagTable, book, multipleText(*edit.tags, u','), context, result, error))
        return false;
    if (edit.comments && !setComments(db, book, singleText(*edit.comments), result, error))
        return false;
    if (edit.languages) {
        // adapt_languages(): Calibre's codes, without duplicates or the codes
        // for no language.
        QStringList codes;
        for (const QString &language : multipleText(*edit.languages, u',')) {
            const QString code = CalibreText::canonicalizeLanguage(language);
            if (code.isEmpty() || codes.contains(code) || code == QLatin1String("und") || code == QLatin1String("zxx")
                || code == QLatin1String("mis") || code == QLatin1String("mul"))
                continue;
            codes.append(code);
        }
        if (!setManyMany(db, languageTable, book, codes, context, result, error))
            return false;
    }
    if (edit.pubdate) {
        // Cleared, it is Calibre's undefined date, as its editor's clear
        // button leaves it.
        CalibreDate date = CalibreDate::fromDateTime(CalibreDb::undefinedDate());
        if (!CalibreText::strip(*edit.pubdate).isEmpty()) {
            std::optional<CalibreDate> parsed = parseExactDate(*edit.pubdate);
            if (!parsed) {
                if (const std::optional<QDateTime> only = parseOnlyDate(*edit.pubdate, context.now))
                    parsed = CalibreDate::fromDateTime(*only);
            }
            if (!parsed) {
                if (error)
                    *error = QObject::tr("“%1” is not a date.").arg(*edit.pubdate);
                return false;
            }
            date = *parsed;
        }
        // Compared with the date as Calibre holds it, without its fraction.
        const CalibreDate stored = CalibreDate::fromDateTime(CalibreDb::parseDate(
            db.rows(QStringLiteral("SELECT pubdate FROM books WHERE id=?"), { book }).value(0).value(0)));
        if (!(stored == date)) {
            result->dirtied.insert(book);
            if (!db.run(QStringLiteral("UPDATE books SET pubdate=? WHERE id=?"), { date.isoFormat(u' '), book }, error))
                return false;
        }
    }
    if (edit.sort) {
        // The sort field takes no empty value.
        if (const std::optional<QString> sort = singleText(*edit.sort))
            if (!setBookColumn(db, book, QStringLiteral("sort"), *sort, result, error))
                return false;
    }
    if (edit.identifiers && !setIdentifiers(db, book, *edit.identifiers, result, error))
        return false;
    if (edit.cover) {
        // The flag is written whatever its value: the file changes anyway.
        result->dirtied.insert(book);
        if (!db.run(QStringLiteral("UPDATE books SET has_cover=? WHERE id=?"), { edit.cover->isEmpty() ? 0 : 1, book },
                    error))
            return false;
    }
    return true;
}

bool markDirtied(SqliteDb &db, const QSet<int> &books, const QDateTime &now, QString *error)
{
    // In id order, as Calibre's set of ids comes out.
    QList<int> sorted(books.cbegin(), books.cend());
    std::sort(sorted.begin(), sorted.end());
    for (const int book : std::as_const(sorted)) {
        if (!db.run(QStringLiteral("UPDATE books SET last_modified=? WHERE id=?"), { sqliteDate(now), book }, error)
            || !db.run(QStringLiteral("INSERT OR IGNORE INTO metadata_dirtied (book) VALUES (?)"), { book }, error))
            return false;
    }
    return true;
}

bool touch(SqliteDb &db, const QSet<int> &books, const QDateTime &now, QString *error)
{
    QList<int> sorted(books.cbegin(), books.cend());
    std::sort(sorted.begin(), sorted.end());
    for (const int book : std::as_const(sorted)) {
        if (!db.run(QStringLiteral("UPDATE books SET last_modified=? WHERE id=?"), { sqliteDate(now), book }, error))
            return false;
    }
    return true;
}

QString sqliteDate(const QDateTime &date)
{
    return CalibreDate::fromDateTime(date).isoFormat(u' ');
}

std::optional<CalibreDate> parseExactDate(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^\\s*\\d{4}-\\d{2}-\\d{2}[T ]\\d{2}:\\d{2}"));
    if (!pattern.match(text).hasMatch())
        return std::nullopt;
    const std::optional<CalibreDate> date = OpfReader::parseDate(text, true);
    if (!date || CalibreDb::isUndefinedDate(date->dateTime))
        return std::nullopt;
    return date;
}

std::optional<QDateTime> parseOnlyDate(const QString &text, const QDateTime &now)
{
    static const QRegularExpression pattern(QStringLiteral("^\\s*(\\d{1,4})(?:[-/.](\\d{1,2})(?:[-/.](\\d{1,2}))?)?\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    // dateutil fills what is missing from the 15th of the current month.
    const QDate today = now.toUTC().date();
    const int year = match.captured(1).toInt();
    const int month = match.captured(2).isEmpty() ? today.month() : match.captured(2).toInt();
    const int day = match.captured(3).isEmpty() ? 15 : match.captured(3).toInt();
    const QDate date(year, month, day);
    if (!date.isValid() || year < 1)
        return std::nullopt;
    return OpfReader::fixOnlyDate(QDateTime(date, QTime(0, 0), QTimeZone::UTC));
}

} // namespace FieldWriters
