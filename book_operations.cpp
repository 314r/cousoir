// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/cache.py: create_book_entry(), set_metadata(),
//   _do_add_format(), remove_formats(), remove_books(),
//   move_book_from_trash(), move_format_from_trash(),
//   author_sort_from_authors(), get_next_series_num_for(),
//   _add_newbook_tag() and _add_default_custom_column_values()
// - src/calibre/db/backend.py: update_path(), add_format(), remove_formats(),
//   remove_books(), move_book_to_trash(), list_trash_entries() and
//   get_metadata_for_trash_book()
// - src/calibre/db/tables.py: update_fmt()
// - src/calibre/db/write.py: the adapters' handling of a value's kind
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_operations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

#include "calibre_db.h"
#include "calibre_list.h"
#include "calibre_paths.h"
#include "calibre_text.h"
#include "commit_engine.h"
#include "file_transaction.h"
#include "highlight_json.h"
#include "sqlite_db.h"

namespace {

using BookOperations::Context;
using BookOperations::Result;

const QString trashFolder = QStringLiteral(".caltrash");

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool bookExists(SqliteDb &db, int bookId)
{
    return !db.rows(QStringLiteral("SELECT id FROM books WHERE id=?"), { bookId }).isEmpty();
}

QString titleOf(SqliteDb &db, int bookId, const QString &unknown)
{
    const QString title = db.rows(QStringLiteral("SELECT title FROM books WHERE id=?"), { bookId })
                              .value(0).value(0).toString();
    return title.isEmpty() ? unknown : title;
}

// _field_for('authors'): the names in link order, with their commas.
QStringList authorsOf(SqliteDb &db, int bookId)
{
    QStringList authors;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT authors.name FROM books_authors_link link JOIN "
                                                          "authors ON authors.id = link.author WHERE link.book=? "
                                                          "ORDER BY link.id"), { bookId }))
        authors.append(row.at(0).toString().replace(u'|', u','));
    return authors;
}

QString pathOf(SqliteDb &db, int bookId)
{
    return db.rows(QStringLiteral("SELECT path FROM books WHERE id=?"), { bookId }).value(0).value(0).toString();
}

QString trashPath(const Context &context, const QString &category, int bookId)
{
    return QDir(context.libraryPath).filePath(QStringLiteral("%1/%2/%3").arg(trashFolder, category).arg(bookId));
}

// ensure_trash_dir(): .caltrash/b for books, .caltrash/f for formats.
bool ensureTrash(FileTransaction &files, const Context &context, QString *error)
{
    const QDir library(context.libraryPath);
    return files.makeFolder(library.filePath(trashFolder + QStringLiteral("/b")), error)
        && files.makeFolder(library.filePath(trashFolder + QStringLiteral("/f")), error);
}

// Moves `from` to `to`, replacing what is there, as os.replace() does.
bool replaceWith(FileTransaction &files, const QString &from, const QString &to, QString *error)
{
    if (QFileInfo::exists(to) && !files.remove(to, error))
        return false;
    return files.move(from, to, error);
}

// A folder's entries, but for the files the change keeps on their way in or
// out, which stay where its journal expects them.
QStringList folderEntries(const QString &folder)
{
    QStringList entries = QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                                 QDir::Name);
    entries.removeIf([](const QString &entry) { return entry.startsWith(QLatin1String(".~cousoir-")); });
    return entries;
}

// What set_metadata() hands a custom column's writer, as FieldWriters takes
// it: none where Calibre's adapter fails on the kind of value, which a new
// book's set_metadata() ignores.
std::optional<QVariant> customArgument(const CustomColumn &column, const PythonJson::Value &value)
{
    using Kind = PythonJson::Value::Kind;
    const QString &type = column.datatype;
    if (column.multiple) {
        // multiple_text(): nothing for a false value, a string split, else
        // the strings of a list.
        if (!PythonJson::truthy(value))
            return QVariant(QStringList());
        if (value.kind == Kind::String)
            return QVariant(value.string);
        if (value.kind != Kind::List)
            return std::nullopt;
        QStringList items;
        for (const PythonJson::Value &item : value.items) {
            if (item.kind != Kind::String)
                return std::nullopt;
            items.append(item.string);
        }
        return QVariant(items);
    }
    const bool number = type == QLatin1String("int") || type == QLatin1String("float")
        || type == QLatin1String("rating");
    switch (value.kind) {
    case Kind::String:
        return QVariant(value.string);
    case Kind::Boolean:
    case Kind::Integer:
        if (number)
            return QVariant(value.kind == Kind::Boolean ? qint64(value.boolean) : value.integer);
        break;
    case Kind::Float:
        if (number)
            return QVariant(value.real);
        break;
    case Kind::Dict:
        // from_json(): a date as Calibre writes one.
        if (type == QLatin1String("datetime") && value.text(QStringLiteral("__class__")) == QLatin1String("datetime.datetime")) {
            const std::optional<CalibreDate> date = OpfReader::parseDate(value.text(QStringLiteral("__value__")), true);
            if (!date)
                return std::nullopt;
            return QVariant(date->dateTime.addMSecs(date->microsecond / 1000));
        }
        break;
    default:
        break;
    }
    // adapt_bool(): bool() of anything.
    if (type == QLatin1String("bool"))
        return QVariant(PythonJson::truthy(value));
    return std::nullopt;
}

// create_book_entry()'s custom fields. A book being added first takes the
// values its custom columns give new books, where it has none
// (_add_default_custom_column_values()); set_metadata() then writes each
// field of the book that a column of the library takes: same type and, for
// text, same separators.
bool customFields(SqliteDb &db, const ImportedMetadata &mi, bool defaults, const QDateTime &now, BookEdit *edit,
                  QString *error)
{
    using PythonJson::Value;
    QList<CustomColumn> columns;
    if (!CalibreDb::readCustomColumns(db, &columns, error))
        return false;
    ImportedMetadata book;
    book.userMetadata = mi.userMetadata;
    for (const CustomColumn &column : std::as_const(columns)) {
        const Value display = PythonJson::parse(column.display);
        const Value *found = display.find(QStringLiteral("default_value"));
        if (!defaults || column.datatype == QLatin1String("composite") || !found || found->kind == Value::Kind::Null)
            continue;
        const QString key = u'#' + column.label;
        const bool had = book.userField(key);
        if (!had) {
            Value field = Value::dict();
            field.insert(QStringLiteral("datatype"), Value::from(column.datatype));
            field.insert(QStringLiteral("is_multiple"), CalibreDb::multipleSeparators(column));
            field.insert(QStringLiteral("#value#"), Value::null());
            field.insert(QStringLiteral("#extra#"), Value::null());
            book.setUserMetadata(key, field);
        }
        // A date's default is "now", the time of adding, or a date as typed;
        // icu_lower() fails on anything else.
        Value value = *found;
        if (column.datatype == QLatin1String("datetime")) {
            if (value.kind != Value::Kind::String)
                continue;
            if (value.string.toLower() == QLatin1String("now")) {
                value = Value::dict();
                value.insert(QStringLiteral("__class__"), Value::from(QStringLiteral("datetime.datetime")));
                value.insert(QStringLiteral("__value__"), Value::from(CalibreDate::fromDateTime(now).isoFormat()));
            }
        }
        Value field = *book.userField(key);
        const Value *current = field.find(QStringLiteral("#value#"));
        if (had && current && current->kind != Value::Kind::Null)
            continue;
        field.insert(QStringLiteral("#value#"), value);
        field.insert(QStringLiteral("#extra#"), Value::null());
        book.setUserMetadata(key, field);
    }

    for (const auto &[key, field] : std::as_const(book.userMetadata)) {
        const auto column = std::find_if(columns.cbegin(), columns.cend(),
                                         [&key](const CustomColumn &c) { return u'#' + c.label == key; });
        if (column == columns.cend() || column->datatype == QLatin1String("composite")
            || field.text(QStringLiteral("datatype")) != column->datatype)
            continue;
        const Value *separators = field.find(QStringLiteral("is_multiple"));
        if (column->datatype == QLatin1String("text")
            && (!separators || !PythonJson::equals(*separators, CalibreDb::multipleSeparators(*column))))
            continue;
        const Value *value = field.find(QStringLiteral("#value#"));
        if (!value || value->kind == Value::Kind::Null)
            continue;
        if (const std::optional<QVariant> argument = customArgument(*column, *value))
            edit->custom.insert(column->label, *argument);
        // adapt_series_index() of the number: a number or text.
        const Value *extra = field.find(QStringLiteral("#extra#"));
        if (column->datatype != QLatin1String("series") || !extra)
            continue;
        if (extra->kind == Value::Kind::String)
            edit->custom.insert(column->label + QStringLiteral("_index"), extra->string);
        else if (extra->kind == Value::Kind::Integer || extra->kind == Value::Kind::Boolean)
            edit->custom.insert(column->label + QStringLiteral("_index"),
                                double(extra->kind == Value::Kind::Boolean ? extra->boolean : extra->integer));
        else if (extra->kind == Value::Kind::Float)
            edit->custom.insert(column->label + QStringLiteral("_index"), extra->real);
    }
    return true;
}

// author_sort_from_authors(): each author's sort in the library, else the
// one Calibre computes, joined by " & ".
QString authorSortFromAuthors(SqliteDb &db, const QStringList &authors, const CalibreText::Tweaks &tweaks)
{
    QHash<QString, QString> sorts;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT name, sort FROM authors")))
        sorts.insert(row.at(0).toString().replace(u'|', u',').toLower(), row.at(1).toString());
    QStringList result;
    for (const QString &author : authors) {
        const auto found = sorts.constFind(author.toLower());
        const QString sort = found != sorts.cend() ? *found : CalibreText::authorToAuthorSort(author, tweaks);
        if (!sort.isEmpty())
            result.append(sort);
    }
    return result.join(QStringLiteral(" & "));
}

// get_next_series_num_for(): after the indices of the series' books.
double nextSeriesIndexFor(SqliteDb &db, const QString &series, const CalibreText::Tweaks &tweaks)
{
    QList<double> indices;
    if (!series.isEmpty()) {
        for (const QVariantList &row : db.rows(QStringLiteral("SELECT id, name FROM series"))) {
            if (row.at(1).toString().toLower() != series.toLower())
                continue;
            for (const QVariantList &book : db.rows(QStringLiteral("SELECT books.series_index FROM books_series_link "
                                                                   "link JOIN books ON books.id = link.book "
                                                                   "WHERE link.series=?"), { row.at(0) }))
                indices.append(book.at(0).isNull() ? 1.0 : book.at(0).toDouble());
            break;
        }
    }
    return CalibreText::nextSeriesIndex(indices, tweaks);
}

bool queuePagesScan(SqliteDb &db, int bookId, QString *error)
{
    return db.run(QStringLiteral("UPDATE books_pages_link SET needs_scan=1 WHERE book=?"), { bookId }, error);
}

// create_book_entry() and the set_metadata() it calls: the book's row, its
// fields as a new book's, and its cover.
bool createBookEntry(SqliteDb &db, FileTransaction &files, const ImportedMetadata &mi, int forceId,
                     bool applyImportTags, bool preserveUuid, const Context &context, FieldWriters::Result *fields,
                     Result *result, int *bookId, QString *error)
{
    const CalibreText::Tweaks &tweaks = context.fields.tweaks;
    const QString unknown = CalibreText::unknownText(context.fields.interfaceLanguage);
    QStringList tags = mi.tags;
    if (applyImportTags) {
        // _add_newbook_tag()
        for (const QString &raw : context.newBookTags) {
            const QString tag = CalibreText::strip(raw);
            if (!tag.isEmpty() && !tags.contains(tag))
                tags.append(tag);
        }
    }
    const double seriesIndex = mi.seriesIndex ? *mi.seriesIndex : nextSeriesIndexFor(db, mi.series, tweaks);
    const QStringList authors = mi.authors.isEmpty() ? QStringList{ unknown } : mi.authors;
    const bool authorSortNull = mi.authorSort.isEmpty() || mi.authorSort == unknown;
    const QString authorSort = authorSortNull ? authorSortFromAuthors(db, authors, tweaks) : mi.authorSort;
    const QString title = mi.title.isEmpty() ? unknown : mi.title;
    const QString titleSort = !mi.titleSort.isEmpty()
        ? mi.titleSort
        : CalibreText::titleSort(title, mi.languages.value(0), tweaks, context.fields.interfaceLanguage);

    const bool inserted = forceId > 0
        ? db.run(QStringLiteral("INSERT INTO books(id, title, series_index, author_sort) VALUES (?, ?, ?, ?)"),
                 { forceId, title, seriesIndex, authorSort }, error)
        : db.run(QStringLiteral("INSERT INTO books(title, series_index, author_sort) VALUES (?, ?, ?)"),
                 { title, seriesIndex, authorSort }, error);
    if (!inserted)
        return false;
    *bookId = forceId > 0 ? forceId : int(db.lastInsertRowId());

    // A cover Calibre cannot read is left out, as set_metadata() ignores the
    // error.
    QString coverError;
    const QByteArray cover = mi.cover.isEmpty() ? QByteArray() : CommitEngine::coverData(mi.cover, &coverError);

    BookEdit edit;
    edit.bookId = *bookId;
    edit.title = title;
    QStringList split;
    for (const QString &author : authors)
        split += CalibreText::stringToAuthors(author);
    edit.authors = split;
    if (!cover.isEmpty())
        edit.cover = cover;
    if (mi.rating)
        edit.rating = std::clamp(int(*mi.rating), 0, 10);
    if (mi.seriesIndex)
        edit.seriesIndex = *mi.seriesIndex;
    edit.timestamp = mi.timestamp && !CalibreDb::isUndefinedDate(mi.timestamp->dateTime)
        ? *mi.timestamp : CalibreDate::fromDateTime(context.fields.now);
    edit.authorSort = authorSort;
    if (!mi.publisher.isEmpty())
        edit.publisher = mi.publisher;
    if (!mi.series.isEmpty())
        edit.series = mi.series;
    if (!tags.isEmpty())
        edit.tags = tags;
    if (!mi.comments.isEmpty())
        edit.comments = mi.comments;
    if (!mi.languages.isEmpty())
        edit.languages = mi.languages;
    // A book without a date has Calibre's undefined one.
    edit.pubdate = mi.pubdate && !CalibreDb::isUndefinedDate(mi.pubdate->dateTime) ? mi.pubdate->isoFormat() : QString();
    edit.sort = titleSort;
    QList<QPair<QString, QString>> identifiers;
    for (const auto &identifier : mi.identifiers) {
        if (!CalibreText::strip(identifier.second).isEmpty())
            identifiers.append({ identifier.first.toLower(), identifier.second });
    }
    if (!identifiers.isEmpty())
        edit.identifiers = identifiers;
    if (!customFields(db, mi, applyImportTags, context.fields.now, &edit, error))
        return false;

    FieldWriters::Context fieldContext = context.fields;
    fieldContext.allowCaseChange = false;
    fieldContext.ignoreCustomErrors = true;
    if (!FieldWriters::apply(db, edit, fieldContext, fields, error)
        || !BookOperations::updatePath(db, files, context.libraryPath, *bookId, unknown, error))
        return false;
    if (preserveUuid && !mi.uuid.isEmpty()
        && !db.run(QStringLiteral("UPDATE books SET uuid=? WHERE id=?"), { mi.uuid, *bookId }, error))
        return false;
    result->dirtied.insert(*bookId);
    fields->dirtied.insert(*bookId);
    if (!cover.isEmpty()) {
        const QString folder = QDir(context.libraryPath).filePath(pathOf(db, *bookId));
        if (!files.write(QDir(folder).filePath(QStringLiteral("cover.jpg")), cover, error))
            return false;
    }
    return true;
}

// add_format() of backend.py and the formats table: the file under the
// book's name for this format, the old one of that format moved there
// first, and its row in data. The file is copied in, or moved from the
// trash.
bool addFormatFile(SqliteDb &db, FileTransaction &files, int bookId, const QString &format, const QString &source,
                   bool move, const Context &context, QString *error)
{
    const QString unknown = CalibreText::unknownText(context.fields.interfaceLanguage);
    const QString extension = u'.' + format.toLower();
    const QString title = titleOf(db, bookId, unknown);
    const QString author = authorsOf(db, bookId).value(0, unknown);
    const QString name = CalibrePaths::constructFileName(bookId, title, author, int(extension.size()), unknown);
    const QList<QVariantList> current = db.rows(QStringLiteral("SELECT name FROM data WHERE book=? AND format=?"),
                                                { bookId, format });
    QString path = pathOf(db, bookId);
    if (path.isEmpty()) {
        if (!BookOperations::updatePath(db, files, context.libraryPath, bookId, unknown, error))
            return false;
        path = pathOf(db, bookId);
    }
    const QDir folder(QDir(context.libraryPath).filePath(path));
    const QString destination = folder.filePath(name + extension);
    if (!current.isEmpty()) {
        // The file of the old name is not left behind.
        const QString old = folder.filePath(current.first().first().toString() + extension);
        if (old != destination && QFileInfo::exists(old) && !replaceWith(files, old, destination, error))
            return false;
    }
    const qint64 size = QFileInfo(source).size();
    if (move ? !replaceWith(files, source, destination, error) : !files.copy(source, destination, error))
        return false;
    return db.run(QStringLiteral("INSERT OR REPLACE INTO data (book,format,uncompressed_size,name) VALUES (?,?,?,?)"),
                  { bookId, format, size, name }, error)
        && queuePagesScan(db, bookId, error);
}

// The item tables remove_books() cleans: the built-in ones and the custom
// columns' own.
struct ItemTable
{
    QString field;
    QString table;
    QString linkTable;
    QString linkColumn;
};

QList<ItemTable> itemTables(SqliteDb &db)
{
    QList<ItemTable> tables{
        { QStringLiteral("authors"), QStringLiteral("authors"), QStringLiteral("books_authors_link"), QStringLiteral("author") },
        { QStringLiteral("tags"), QStringLiteral("tags"), QStringLiteral("books_tags_link"), QStringLiteral("tag") },
        { QStringLiteral("languages"), QStringLiteral("languages"), QStringLiteral("books_languages_link"),
          QStringLiteral("lang_code") },
        { QStringLiteral("series"), QStringLiteral("series"), QStringLiteral("books_series_link"), QStringLiteral("series") },
        { QStringLiteral("publisher"), QStringLiteral("publishers"), QStringLiteral("books_publishers_link"),
          QStringLiteral("publisher") },
        { QStringLiteral("rating"), QStringLiteral("ratings"), QStringLiteral("books_ratings_link"), QStringLiteral("rating") },
    };
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT id, label FROM custom_columns WHERE normalized"))) {
        const QString id = row.at(0).toString();
        tables.append({ u'#' + row.at(1).toString(), QStringLiteral("custom_column_") + id,
                        QStringLiteral("books_custom_column_%1_link").arg(id), QStringLiteral("value") });
    }
    return tables;
}

QString pythonJson(const QString &title, const QStringList &authors)
{
    // json.dumps({'title': …, 'authors': […]})
    QStringList names;
    for (const QString &author : authors)
        names.append(QString::fromLatin1(CalibreList::pythonJsonString(author)));
    return QStringLiteral("{\"title\": %1, \"authors\": [%2]}")
        .arg(QString::fromLatin1(CalibreList::pythonJsonString(title)), names.join(QStringLiteral(", ")));
}

} // namespace

namespace BookOperations {

bool updatePath(SqliteDb &db, FileTransaction &files, const QString &libraryPath, int bookId, const QString &unknown,
                QString *error)
{
    SqliteStatement book(db, QStringLiteral("SELECT title, path FROM books WHERE id=?"));
    book.bind(1, qint64(bookId));
    if (!book.step())
        return true;
    const QString title = book.text(0);
    const QString current = book.text(1);
    SqliteStatement author(db, QStringLiteral("SELECT authors.name FROM books_authors_link link JOIN authors "
                                              "ON authors.id = link.author WHERE link.book=? ORDER BY link.id LIMIT 1"));
    author.bind(1, qint64(bookId));
    const QString firstAuthor = author.step() ? author.text(0).replace(u'|', u',') : unknown;

    QList<QPair<QString, QString>> formats;
    SqliteStatement data(db, QStringLiteral("SELECT format, name FROM data WHERE book=?"));
    data.bind(1, qint64(bookId));
    int extensionLength = 0;
    while (data.step()) {
        formats.append({ data.text(0).toUpper(), data.text(1) });
        extensionLength = std::max(extensionLength, int(data.text(0).size()) + 1);
    }
    if (formats.isEmpty())
        extensionLength = 10;

    const QString path = CalibrePaths::constructPathName(bookId, title, firstAuthor, unknown);
    const QString name = CalibrePaths::constructFileName(bookId, title, firstAuthor, extensionLength, unknown);
    const auto updateDatabase = [&] {
        for (const auto &format : std::as_const(formats)) {
            if (!db.run(QStringLiteral("UPDATE data SET name=? WHERE book=? AND format=?"), { name, bookId, format.first },
                        error))
                return false;
        }
        return db.run(QStringLiteral("UPDATE books SET path=? WHERE id=?"), { path, bookId }, error);
    };

    if (current.isEmpty())
        return updateDatabase();

    const QDir library(libraryPath);
    const QString source = library.filePath(current);
    const QString target = library.filePath(path);
    if (path == current) {
        // Only the files' names may have changed.
        bool changed = false;
        for (const auto &format : std::as_const(formats))
            changed = changed || (!format.second.isEmpty() && format.second != name);
        if (!changed)
            return true;
        for (const auto &format : std::as_const(formats)) {
            const QString file = CalibreDb::formatFile(source, format.second, format.first);
            const QString renamed = QDir(source).filePath(name + u'.' + format.first.toLower());
            if (!file.isEmpty() && file != renamed && !files.move(file, renamed, error))
                return false;
        }
        return updateDatabase();
    }

    // Calibre deletes whatever is already at the new place; Cousoir only
    // clears an empty folder and refuses otherwise.
    if (QFileInfo::exists(target)) {
        if (!QFileInfo(target).isDir()
            || !QDir(target).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            return fail(error, QObject::tr("The library already has a folder %1, where Calibre would move this book.")
                                   .arg(path));
        if (!files.remove(target, error))
            return false;
    }

    if (QFileInfo(source).isDir()) {
        // Every entry moves; the format files take the new name.
        QHash<QString, QString> renamedFormats;
        for (const auto &format : std::as_const(formats)) {
            const QString file = CalibreDb::formatFile(source, format.second, format.first);
            if (!file.isEmpty())
                renamedFormats.insert(QFileInfo(file).fileName(), name + u'.' + format.first.toLower());
        }
        for (const QString &entry : folderEntries(source)) {
            const QString destination = QDir(target).filePath(renamedFormats.value(entry, entry));
            if (!files.move(QDir(source).filePath(entry), destination, error))
                return false;
        }
        files.removeFolderIfEmpty(source);
        files.removeFolderIfEmpty(QFileInfo(source).absolutePath());
    } else if (!files.makeFolder(target, error)) {
        // Nothing to move: Calibre makes the folder all the same.
        return false;
    }
    return updateDatabase();
}

bool addBook(SqliteDb &db, FileTransaction &files, const NewBook &book, const Context &context,
             FieldWriters::Result *fields, Result *result, QString *error)
{
    // format_map: one file per format, the last one given.
    QStringList formats;
    QHash<QString, QString> fileOf;
    for (const QString &file : book.files) {
        const QString format = QFileInfo(file).suffix().toUpper();
        if (format.isEmpty())
            return fail(error, QObject::tr("%1 has no extension to tell its format.").arg(QFileInfo(file).fileName()));
        if (!QFileInfo(file).isFile())
            return fail(error, QObject::tr("%1 cannot be read.").arg(file));
        if (!formats.contains(format))
            formats.append(format);
        fileOf.insert(format, file);
    }
    int bookId = 0;
    if (!createBookEntry(db, files, book.metadata, 0, true, false, context, fields, result, &bookId, error))
        return false;
    for (const QString &format : std::as_const(formats)) {
        if (!addFormatFile(db, files, bookId, format, fileOf.value(format), false, context, error))
            return false;
    }
    result->added.append(bookId);
    return true;
}

bool addFormat(SqliteDb &db, FileTransaction &files, const NewFormat &format, const Context &context, Result *result,
               QString *error)
{
    if (!bookExists(db, format.bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(format.bookId));
    const QString extension = format.format.isEmpty() ? QFileInfo(format.file).suffix().toUpper() : format.format;
    if (extension.isEmpty())
        return fail(error, QObject::tr("%1 has no extension to tell its format.").arg(QFileInfo(format.file).fileName()));
    if (!QFileInfo(format.file).isFile())
        return fail(error, QObject::tr("%1 cannot be read.").arg(format.file));
    if (!addFormatFile(db, files, format.bookId, extension, format.file, false, context, error))
        return false;
    result->touched.insert(format.bookId);
    return true;
}

bool setConversionOptions(SqliteDb &db, int bookId, const QByteArray &data, QString *error)
{
    if (!bookExists(db, bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
    return db.run(QStringLiteral("INSERT OR REPLACE INTO conversion_options(book,format,data) VALUES (?,'PIPE',?)"),
                  { bookId, data }, error);
}

bool setReadPosition(SqliteDb &db, int bookId, const ReadPosition &position, QString *error)
{
    if (!bookExists(db, bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
    return db.run(QStringLiteral("INSERT OR REPLACE INTO last_read_positions(book,format,user,device,cfi,epoch,pos_frac) "
                                 "VALUES (?,?,?,?,?,?,?)"),
                  { bookId, position.format.toUpper(), position.user, position.device, position.cfi, position.epoch,
                    position.fraction },
                  error);
}

bool setAnnotation(SqliteDb &db, int bookId, const Annotation &annotation, QString *error)
{
    if (!bookExists(db, bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
    // Deleted, then inserted, as Calibre does: a REPLACE would delete the old
    // row without its trigger, leaving its words in the search index.
    return db.run(QStringLiteral("INSERT OR IGNORE INTO annotations_dirtied (book) VALUES (?)"), { bookId }, error)
        && db.run(QStringLiteral("DELETE FROM annotations WHERE book=? AND format=? AND user_type=? AND user=? "
                                 "AND annot_type=? AND annot_id=?"),
                  { bookId, annotation.format.toUpper(), annotation.userType, annotation.user, annotation.type, annotation.id },
                  error)
        && db.run(QStringLiteral("INSERT INTO annotations(book,format,user_type,user,timestamp,annot_id,annot_type,"
                                 "annot_data,searchable_text) VALUES (?,?,?,?,?,?,?,?,?)"),
                  { bookId, annotation.format.toUpper(), annotation.userType, annotation.user, annotation.timestamp,
                    annotation.id, annotation.type, QString::fromUtf8(annotation.data), annotation.searchable },
                  error);
}

QList<Annotation> mergeSteps(const QList<Annotation> &rows, const DeviceAnnotations &set, double now)
{
    const auto parsed = [](const Annotation &annotation) {
        return QJsonDocument::fromJson(annotation.data).object();
    };
    // What it says, its time aside.
    const auto content = [&parsed](const Annotation &annotation) {
        QJsonObject object = parsed(annotation);
        object.remove(QStringLiteral("timestamp"));
        return object;
    };
    const auto removedLike = [&set](const Annotation &row, double at) {
        Annotation removed = row;
        removed.format = set.format.toUpper();
        removed.timestamp = at;
        removed.data = QJsonDocument(HighlightJson::removed(row.id, HighlightJson::isoTimestamp(
                                                                QDateTime::fromMSecsSinceEpoch(qint64(at * 1000), QTimeZone::UTC))))
                           .toJson(QJsonDocument::Compact);
        removed.searchable.clear();
        return removed;
    };
    QHash<QString, const Annotation *> library;
    for (const Annotation &row : rows)
        library.insert(row.id, &row);
    QList<Annotation> steps;
    QSet<QString> seen;
    for (const Annotation &annotation : set.annotations) {
        seen.insert(annotation.id);
        const Annotation *row = library.value(annotation.id);
        const bool removedThere = parsed(annotation).value(QLatin1String("removed")).toBool();
        if (!row) {
            if (!removedThere)
                steps.append(annotation);
            continue;
        }
        // Removed in the library: it stays so.
        if (parsed(*row).value(QLatin1String("removed")).toBool())
            continue;
        if (removedThere) {
            steps.append(removedLike(*row, annotation.timestamp));
            continue;
        }
        if (annotation.timestamp >= row->timestamp
            && (content(annotation) != content(*row) || annotation.format.toUpper() != row->format.toUpper()))
            steps.append(annotation);
    }
    if (set.present) {
        const QSet<QString> present(set.present->cbegin(), set.present->cend());
        for (const Annotation &row : rows) {
            if (!seen.contains(row.id) && !present.contains(row.id) && !parsed(row).value(QLatin1String("removed")).toBool())
                steps.append(removedLike(row, now));
        }
    }
    for (Annotation &step : steps) {
        step.userType = set.userType;
        step.user = set.user;
    }
    return steps;
}

bool mergeDeviceAnnotations(SqliteDb &db, const DeviceAnnotations &set, double now, int *changed, QString *error)
{
    if (!bookExists(db, set.bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(set.bookId));
    QList<Annotation> rows;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT format, annot_type, annot_id, timestamp, annot_data, "
                                                          "searchable_text FROM annotations WHERE book=? AND user_type=? "
                                                          "AND user=? AND annot_type='highlight'"),
                                           { set.bookId, set.userType, set.user })) {
        rows.append({ row.value(0).toString(), row.value(1).toString(), row.value(2).toString(), row.value(3).toDouble(),
                      row.value(4).toString().toUtf8(), row.value(5).toString(), set.userType, set.user });
    }
    for (const Annotation &step : mergeSteps(rows, set, now)) {
        // Whatever format it was kept with: the book's to read now.
        if (!db.run(QStringLiteral("DELETE FROM annotations WHERE book=? AND user_type=? AND user=? AND annot_type=? "
                                   "AND annot_id=?"),
                    { set.bookId, set.userType, set.user, step.type, step.id }, error)
            || !setAnnotation(db, set.bookId, step, error))
            return false;
        ++*changed;
    }
    return true;
}

bool removeAnnotationUser(SqliteDb &db, const QString &userType, const QString &user, int *changed, QString *error)
{
    *changed += int(db.rows(QStringLiteral("SELECT count(*) FROM annotations WHERE user_type=? AND user=?"), { userType, user })
                        .value(0)
                        .value(0)
                        .toLongLong());
    return db.run(QStringLiteral("INSERT OR IGNORE INTO annotations_dirtied (book) "
                                 "SELECT DISTINCT book FROM annotations WHERE user_type=? AND user=?"),
                  { userType, user }, error)
        && db.run(QStringLiteral("DELETE FROM annotations WHERE user_type=? AND user=?"), { userType, user }, error);
}

bool copyFormats(SqliteDb &db, FileTransaction &files, const FormatCopy &copy, const Context &context, Result *result,
                 QString *error)
{
    for (const int bookId : { copy.from, copy.to }) {
        if (!bookExists(db, bookId))
            return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
    }
    if (copy.from == copy.to)
        return true;
    // has_format(): a format whose file is there.
    const auto present = [&db, &context](int bookId) {
        QMap<QString, QString> found;
        const QString path = pathOf(db, bookId);
        if (path.isEmpty())
            return found;
        const QString folder = QDir(context.libraryPath).filePath(path);
        for (const QVariantList &row : db.rows(QStringLiteral("SELECT format, name FROM data WHERE book=?"), { bookId })) {
            const QString file = CalibreDb::formatFile(folder, row.at(1).toString(), row.at(0).toString());
            if (!file.isEmpty())
                found.insert(row.at(0).toString(), file);
        }
        return found;
    };
    const QMap<QString, QString> kept = present(copy.to);
    const QMap<QString, QString> other = present(copy.from);
    for (auto it = other.cbegin(); it != other.cend(); ++it) {
        if (kept.contains(it.key()))
            continue;
        if (!addFormatFile(db, files, copy.to, it.key(), it.value(), false, context, error))
            return false;
        result->touched.insert(copy.to);
    }
    return true;
}

bool removeFormats(SqliteDb &db, FileTransaction &files, const FormatRemoval &removal, const Context &context,
                   Result *result, QString *error)
{
    const int bookId = removal.bookId;
    if (!bookExists(db, bookId))
        return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
    QStringList formats;
    for (const QString &format : removal.formats) {
        if (!formats.contains(format.toUpper()))
            formats.append(format.toUpper());
    }
    const QString path = pathOf(db, bookId);
    const QString folder = QDir(context.libraryPath).filePath(path);
    QStringList removed;
    for (const QString &format : std::as_const(formats)) {
        const QList<QVariantList> name = db.rows(QStringLiteral("SELECT name FROM data WHERE book=? AND format=?"),
                                                 { bookId, format });
        if (name.isEmpty() || name.first().first().toString().isEmpty() || path.isEmpty())
            continue;
        const QString file = CalibreDb::formatFile(folder, name.first().first().toString(), format);
        if (!file.isEmpty())
            removed.append(file);
    }
    if (!removed.isEmpty()) {
        // move_book_files_to_trash(): f/<id>/<extension>, beside the title
        // and authors the book had.
        const QString unknown = CalibreText::unknownText(context.fields.interfaceLanguage);
        const QString destination = trashPath(context, QStringLiteral("f"), bookId);
        if (!ensureTrash(files, context, error) || !files.makeFolder(destination, error)
            || !files.write(QDir(destination).filePath(QStringLiteral("metadata.json")),
                            pythonJson(titleOf(db, bookId, unknown), authorsOf(db, bookId)).toUtf8(), error))
            return false;
        for (const QString &file : std::as_const(removed)) {
            const QString extension = file.section(u'.', -1).toLower();
            if (!replaceWith(files, file, QDir(destination).filePath(extension), error))
                return false;
        }
    }
    for (const QString &format : std::as_const(formats)) {
        if (!db.run(QStringLiteral("DELETE FROM data WHERE book=? AND format=?"), { bookId, format }, error))
            return false;
    }
    result->touched.insert(bookId);
    return queuePagesScan(db, bookId, error);
}

bool removeBooks(SqliteDb &db, FileTransaction &files, const QList<int> &bookIds, const Context &context,
                 QString *error)
{
    const QList<ItemTable> tables = itemTables(db);
    // The items of the books, which go when no other book has them.
    QList<QSet<int>> items(tables.size());
    QList<QPair<int, QString>> folders;
    for (const int bookId : bookIds) {
        if (!bookExists(db, bookId))
            return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
        for (qsizetype index = 0; index < tables.size(); ++index) {
            for (const QVariantList &row : db.rows(QStringLiteral("SELECT %1 FROM %2 WHERE book=?")
                                                       .arg(tables.at(index).linkColumn, tables.at(index).linkTable),
                                                   { bookId }))
                items[index].insert(row.at(0).toInt());
        }
        const QString path = pathOf(db, bookId);
        if (path.isEmpty())
            continue;
        // A metadata.opf up to date, so that the book can be restored.
        const QString folder = QDir(context.libraryPath).filePath(path);
        if (const Book *book = context.before ? context.before->book(bookId) : nullptr) {
            const QString opf = QDir(folder).filePath(QStringLiteral("metadata.opf"));
            if (!files.write(opf, Opf::metadataToOpf(*context.before, *book, context.opf, readFile(opf)), error))
                return false;
        }
        folders.append({ bookId, folder });
    }
    for (const int bookId : bookIds) {
        if (!db.run(QStringLiteral("DELETE FROM books WHERE id=?"), { bookId }, error))
            return false;
    }
    if (!folders.isEmpty() && !ensureTrash(files, context, error))
        return false;
    for (const auto &[bookId, folder] : std::as_const(folders)) {
        if (!QFileInfo(folder).isDir())
            continue;
        // move_book_to_trash(): a new b/<id>, an older one replaced, the
        // book's entries moved into it.
        const QString destination = trashPath(context, QStringLiteral("b"), bookId);
        if (QFileInfo::exists(destination) && !files.remove(destination, error))
            return false;
        if (!files.makeFolder(destination, error))
            return false;
        for (const QString &entry : folderEntries(folder)) {
            if (!files.move(QDir(folder).filePath(entry), QDir(destination).filePath(entry), error))
                return false;
        }
        files.removeFolderIfEmpty(folder);
        files.removeFolderIfEmpty(QFileInfo(folder).absolutePath());
    }
    for (qsizetype index = 0; index < tables.size(); ++index) {
        const ItemTable &table = tables.at(index);
        for (const int item : std::as_const(items.at(index))) {
            if (context.fields.notes.contains({ table.field, item }))
                continue;
            const bool used = !db.rows(QStringLiteral("SELECT 1 FROM %1 WHERE %2=? LIMIT 1")
                                           .arg(table.linkTable, table.linkColumn), { item }).isEmpty();
            if (!used && !db.run(QStringLiteral("DELETE FROM %1 WHERE id=?").arg(table.table), { item }, error))
                return false;
        }
    }
    return true;
}

bool restore(SqliteDb &db, FileTransaction &files, const Restoration &restoration, const Context &context,
             FieldWriters::Result *fields, Result *result, QString *error)
{
    const int bookId = restoration.bookId;
    if (!restoration.format.isEmpty()) {
        // move_format_from_trash()
        if (!bookExists(db, bookId))
            return fail(error, QObject::tr("Book %1 is no longer in the library.").arg(bookId));
        const QString format = restoration.format.toUpper();
        const QString folder = trashPath(context, QStringLiteral("f"), bookId);
        const QString file = QDir(folder).filePath(format.toLower());
        if (!QFileInfo(file).isFile())
            return fail(error, QObject::tr("The trash has no %1 file for book %2.").arg(format).arg(bookId));
        if (!addFormatFile(db, files, bookId, format, file, true, context, error))
            return false;
        // The folder goes with its last format; metadata.json does not count.
        if (folderEntries(folder).size() <= 1 && !files.remove(folder, error))
            return false;
        return true;
    }

    // move_book_from_trash()
    const QString folder = trashPath(context, QStringLiteral("b"), bookId);
    if (!QFileInfo(folder).isDir())
        return fail(error, QObject::tr("Book %1 is not in the trash.").arg(bookId));
    if (bookExists(db, bookId))
        return fail(error, QObject::tr("The library already has a book %1.").arg(bookId));
    const QByteArray raw = readFile(QDir(folder).filePath(QStringLiteral("metadata.opf")));
    if (raw.contains("calibre:annotation"))
        return fail(error, QObject::tr("The book has annotations, which Cousoir cannot restore yet: restore it in "
                                       "Calibre."));
    OpfReader::Package package;
    if (!OpfReader::read(raw, &package, error))
        return false;
    ImportedMetadata mi = package.metadata;
    mi.cover.clear();
    // get_metadata_for_trash_book(): the files with an extension, but for
    // the cover and metadata.opf.
    struct Format
    {
        QString format;
        qint64 size;
        QString name;
    };
    QList<Format> formats;
    for (const QFileInfo &entry : QDir(folder).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                                                             QDir::Name)) {
        const QString name = entry.fileName();
        if (name == QLatin1String("cover.jpg") || name == QLatin1String("metadata.opf") || !name.contains(u'.'))
            continue;
        // os.path.splitext(): a leading dot is no extension's.
        const qsizetype dot = name.lastIndexOf(u'.');
        if (dot <= 0 || name.left(dot).count(u'.') == dot)
            continue;
        formats.append({ name.mid(dot + 1).toUpper(), entry.size(), name.left(dot) });
    }

    int restored = 0;
    if (!createBookEntry(db, files, mi, bookId, false, true, context, fields, result, &restored, error))
        return false;
    const QString destination = QDir(context.libraryPath).filePath(pathOf(db, bookId));
    if (!files.makeFolder(destination, error))
        return false;
    for (const QString &entry : folderEntries(folder)) {
        if (!replaceWith(files, QDir(folder).filePath(entry), QDir(destination).filePath(entry), error))
            return false;
    }
    files.removeFolderIfEmpty(folder);
    for (const Format &format : std::as_const(formats)) {
        if (!db.run(QStringLiteral("INSERT OR REPLACE INTO data (book,format,uncompressed_size,name) VALUES (?,?,?,?)"),
                    { bookId, format.format, format.size, format.name }, error))
            return false;
    }
    if (QFileInfo(QDir(destination).filePath(QStringLiteral("cover.jpg"))).isFile()
        && !db.run(QStringLiteral("UPDATE books SET has_cover=1 WHERE id=?"), { bookId }, error))
        return false;
    result->restored.append(bookId);
    return queuePagesScan(db, bookId, error);
}

QList<TrashEntry> trashEntries(const QString &libraryPath, const QString &interfaceLanguage)
{
    const QString unknown = CalibreText::unknownText(interfaceLanguage);
    const QDir trash(QDir(libraryPath).filePath(trashFolder));
    QList<TrashEntry> entries;
    const auto numbered = [](const QFileInfo &info, int *id) {
        bool ok = false;
        *id = info.fileName().toInt(&ok);
        return ok && info.isDir() && !info.isSymLink();
    };
    for (const QFileInfo &info : QDir(trash.filePath(QStringLiteral("b"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        TrashEntry entry;
        OpfReader::Package package;
        if (!numbered(info, &entry.bookId)
            || !OpfReader::read(readFile(QDir(info.filePath()).filePath(QStringLiteral("metadata.opf"))), &package, nullptr))
            continue;
        entry.title = package.metadata.title.isEmpty() ? unknown : package.metadata.title;
        entry.author = package.metadata.authors.value(0, unknown);
        entry.coverFile = QDir(info.filePath()).filePath(QStringLiteral("cover.jpg"));
        entry.removed = info.lastModified(QTimeZone::UTC);
        entries.append(entry);
    }
    for (const QFileInfo &info : QDir(trash.filePath(QStringLiteral("f"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        TrashEntry entry;
        if (!numbered(info, &entry.bookId))
            continue;
        entry.title = unknown;
        entry.author = unknown;
        for (const QFileInfo &file : QDir(info.filePath()).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
            if (file.isSymLink())
                continue;
            if (file.fileName() == QLatin1String("metadata.json")) {
                const QJsonObject metadata = QJsonDocument::fromJson(readFile(file.filePath())).object();
                if (!metadata.value(QLatin1String("title")).toString().isEmpty())
                    entry.title = metadata.value(QLatin1String("title")).toString();
                const QJsonArray authors = metadata.value(QLatin1String("authors")).toArray();
                if (!authors.isEmpty() && !authors.first().toString().isEmpty())
                    entry.author = authors.first().toString();
            } else {
                entry.formats.append(file.fileName().toUpper());
            }
        }
        entry.removed = info.lastModified(QTimeZone::UTC);
        if (!entry.formats.isEmpty())
            entries.append(entry);
    }
    return entries;
}

} // namespace BookOperations
