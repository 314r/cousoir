// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/backend.py: the SQL functions, aggregates and temporary
//   author triggers of a connection, format_abspath(), and the custom
//   columns initialize_custom_columns() keeps, with their separators
// - src/calibre/db/tables.py: how the custom columns' tables are read
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_db.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <tuple>

#include <sqlite3.h>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "annotation_search.h"
#include "calibre_date.h"
#include "calibre_fts.h"
#include "calibre_template.h"
#include "file_transaction.h"
#include "sqlite_db.h"

namespace {

// The structures Cousoir has been checked against, as schemaFingerprint()
// computes them.
struct KnownSchema
{
    int userVersion;
    const char *fingerprint;
};

const KnownSchema knownSchemas[] = {
    // Calibre 9.16, for a new library and for one created in 2015 and
    // upgraded since.
    { 28, "e7c1b92789e4e2a6" },
};

// Names of the objects Calibre makes for custom columns: custom_column_3,
// books_custom_column_3_link and the views over them.
bool isCustomColumnObject(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("custom_column_\\d+"));
    return pattern.match(name).hasMatch();
}

QString textArgument(sqlite3_value *value)
{
    const auto *text = reinterpret_cast<const char *>(sqlite3_value_text(value));
    return text ? QString::fromUtf8(text, sqlite3_value_bytes(value)) : QString();
}

void resultText(sqlite3_context *context, const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    sqlite3_result_text(context, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
}

// What the SQL functions need of Calibre's settings.
struct TextSettings
{
    CalibreText::Tweaks tweaks;
    QString interfaceLanguage;
};

void destroyTextSettings(void *settings)
{
    delete static_cast<TextSettings *>(settings);
}

// title_sort(title[, order[, lang]]), Calibre's function as is.
void titleSortFunction(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc < 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }
    const auto *settings = static_cast<const TextSettings *>(sqlite3_user_data(context));
    const QString order = argc > 1 && sqlite3_value_type(argv[1]) != SQLITE_NULL
        ? textArgument(argv[1]) : settings->tweaks.titleSeriesSorting;
    const QString language = argc > 2 ? textArgument(argv[2]) : QString();
    resultText(context, CalibreText::titleSort(textArgument(argv[0]), language, settings->tweaks,
                                               settings->interfaceLanguage, order));
}

void authorSortFunction(sqlite3_context *context, int, sqlite3_value **argv)
{
    const auto *settings = static_cast<const TextSettings *>(sqlite3_user_data(context));
    resultText(context, CalibreText::sqlAuthorToAuthorSort(textArgument(argv[0]), settings->tweaks));
}

void uuidFunction(sqlite3_context *context, int, sqlite3_value **)
{
    resultText(context, QUuid::createUuid().toString(QUuid::WithoutBraces));
}

// The dummy filter of Calibre's tag browser views.
void booksListFilterFunction(sqlite3_context *context, int, sqlite3_value **)
{
    sqlite3_result_int(context, 1);
}

// The state of an aggregate, created on its first step and freed by its
// final call.
template<typename State>
State *aggregateState(sqlite3_context *context, bool create)
{
    auto **slot = static_cast<State **>(sqlite3_aggregate_context(context, create ? int(sizeof(State *)) : 0));
    if (!slot)
        return nullptr;
    if (!*slot && create)
        *slot = new State;
    return *slot;
}

template<typename State>
std::unique_ptr<State> takeAggregateState(sqlite3_context *context)
{
    State *state = aggregateState<State>(context, false);
    if (state)
        *static_cast<State **>(sqlite3_aggregate_context(context, 0)) = nullptr;
    return std::unique_ptr<State>(state);
}

// A key of Calibre's sorted aggregates, ordered like SQLite orders values:
// numbers before text.
struct SortKey
{
    bool text = false;
    double number = 0;
    QString string;

    static SortKey of(sqlite3_value *value)
    {
        const int type = sqlite3_value_type(value);
        if (type == SQLITE_INTEGER || type == SQLITE_FLOAT)
            return { false, sqlite3_value_double(value), {} };
        return { true, 0, textArgument(value) };
    }

    bool operator<(const SortKey &other) const
    {
        if (text != other.text)
            return !text;
        return text ? string < other.string : number < other.number;
    }
};

using SortedValues = std::map<SortKey, QString>;

// concat(value): Concatenate() with ",".
void concatStep(sqlite3_context *context, int, sqlite3_value **argv)
{
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        return;
    if (QStringList *values = aggregateState<QStringList>(context, true))
        values->append(textArgument(argv[0]));
}

void concatFinal(sqlite3_context *context)
{
    const std::unique_ptr<QStringList> values = takeAggregateState<QStringList>(context);
    if (!values || values->isEmpty())
        sqlite3_result_null(context);
    else
        resultText(context, values->join(u','));
}

// sortconcat(index, value) and its | and & variants: SortedConcatenate().
void sortConcatStep(sqlite3_context *context, int, sqlite3_value **argv)
{
    if (sqlite3_value_type(argv[1]) == SQLITE_NULL)
        return;
    if (SortedValues *values = aggregateState<SortedValues>(context, true))
        (*values)[SortKey::of(argv[0])] = textArgument(argv[1]);
}

void sortConcatFinal(sqlite3_context *context)
{
    const std::unique_ptr<SortedValues> values = takeAggregateState<SortedValues>(context);
    if (!values || values->empty()) {
        sqlite3_result_null(context);
        return;
    }
    const auto *separator = static_cast<const char *>(sqlite3_user_data(context));
    QStringList joined;
    for (const auto &entry : *values)
        joined.append(entry.second);
    resultText(context, joined.join(QString::fromUtf8(separator)));
}

// identifiers_concat(type, value): IdentifiersConcat().
void identifiersConcatStep(sqlite3_context *context, int, sqlite3_value **argv)
{
    const auto python = [](sqlite3_value *value) {
        return sqlite3_value_type(value) == SQLITE_NULL ? QStringLiteral("None") : textArgument(value);
    };
    if (QStringList *values = aggregateState<QStringList>(context, true))
        values->append(python(argv[0]) + u':' + python(argv[1]));
}

void identifiersConcatFinal(sqlite3_context *context)
{
    const std::unique_ptr<QStringList> values = takeAggregateState<QStringList>(context);
    resultText(context, values ? values->join(u',') : QString());
}

// aum_sortconcat(index, author, sort, link): AumSortedConcatenate().
void authorsConcatStep(sqlite3_context *context, int, sqlite3_value **argv)
{
    if (sqlite3_value_type(argv[1]) == SQLITE_NULL)
        return;
    if (SortedValues *values = aggregateState<SortedValues>(context, true)) {
        (*values)[SortKey::of(argv[0])] = QStringList{ textArgument(argv[1]), textArgument(argv[2]),
                                                       textArgument(argv[3]) }.join(QStringLiteral(":::"));
    }
}

void authorsConcatFinal(sqlite3_context *context)
{
    const std::unique_ptr<SortedValues> values = takeAggregateState<SortedValues>(context);
    if (!values || values->empty()) {
        sqlite3_result_null(context);
        return;
    }
    QStringList joined;
    for (const auto &entry : *values)
        joined.append(entry.second);
    resultText(context, joined.join(QStringLiteral(":#:")));
}

bool registered(sqlite3 *db, int result, const char *name, QString *error)
{
    if (result == SQLITE_OK)
        return true;
    if (error)
        *error = QStringLiteral("Cannot register the SQL function %1: %2")
                     .arg(QLatin1String(name), QString::fromUtf8(sqlite3_errmsg(db)));
    return false;
}

void addTo(QHash<int, int> &counts, int id)
{
    if (id > 0)
        ++counts[id];
}

QList<FacetItem> facetItems(const QHash<int, FacetItem> &items, const QHash<int, int> &counts)
{
    QList<FacetItem> result;
    for (FacetItem item : items) {
        item.count = counts.value(item.id);
        if (item.count > 0)
            result.append(item);
    }
    std::sort(result.begin(), result.end(), [](const FacetItem &a, const FacetItem &b) { return a.id < b.id; });
    return result;
}

// One query whose rows `read` takes; false and `error` when it fails.
template<typename Read>
bool forEachRow(SqliteDb &db, const QString &sql, Read read, QString *error)
{
    SqliteStatement statement(db, sql);
    while (statement.step())
        read(statement);
    if (!statement.error().isEmpty()) {
        if (error)
            *error = statement.error();
        return false;
    }
    return true;
}

} // namespace

namespace CalibreDb {

FileIdentity identify(const QString &path)
{
    FileIdentity identity;
    const QFileInfo info(path);
    if (!info.exists())
        return identity;
    identity.exists = true;
    identity.size = info.size();
    identity.modifiedMs = info.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch();
#ifdef Q_OS_UNIX
    struct stat status {};
    if (::stat(QFile::encodeName(path).constData(), &status) == 0)
        identity.inode = quint64(status.st_ino);
#endif
    return identity;
}

bool hasJournal(const QString &database)
{
    return QFileInfo::exists(database + QStringLiteral("-journal")) || QFileInfo::exists(database + QStringLiteral("-wal"));
}

bool takeSnapshot(const QString &libraryPath, const QString &snapshotPath, FileIdentity *source, QString *error)
{
    const QString database = QDir(libraryPath).filePath(QStringLiteral("metadata.db"));
    QString reason;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (attempt > 0)
            QThread::msleep(150 * attempt);

        const FileIdentity before = identify(database);
        if (!before.exists) {
            if (error)
                *error = QObject::tr("There is no Calibre library in %1.").arg(QDir::toNativeSeparators(libraryPath));
            return false;
        }
        if (hasJournal(database)) {
            reason = QObject::tr("Calibre is writing to the library. Try again in a moment.");
            continue;
        }

        // Copied beside the snapshot, a chunk at a time, then put in its
        // place in one step: the window may be reading the snapshot.
        QDir().mkpath(QFileInfo(snapshotPath).absolutePath());
        const QString incoming = snapshotPath + QStringLiteral(".incoming");
        if (!FileTransaction::copyFile(database, incoming, FileTransaction::Copy::ThroughMemory, false, error))
            return false;
        if (identify(database) != before || QFileInfo(incoming).size() != before.size) {
            QFile::remove(incoming);
            reason = QObject::tr("The library kept changing while it was read. Try again in a moment.");
            continue;
        }
        if (std::rename(QFile::encodeName(incoming).constData(), QFile::encodeName(snapshotPath).constData()) != 0) {
            QFile::remove(incoming);
            if (error)
                *error = QObject::tr("Cannot write %1.").arg(QDir::toNativeSeparators(snapshotPath));
            return false;
        }

        SqliteDb check;
        QString checkError;
        if (!check.open(snapshotPath, SqliteDb::Mode::Immutable, &checkError)
            || check.value(QStringLiteral("PRAGMA quick_check"), &checkError).toString() != QLatin1String("ok")) {
            reason = QObject::tr("The copy of the library is damaged (%1). Try again in a moment.").arg(checkError);
            continue;
        }

        if (source)
            *source = before;
        return true;
    }
    if (error)
        *error = reason;
    return false;
}

bool prepareConnection(SqliteDb &db, const CalibreText::Tweaks &tweaks, const QString &interfaceLanguage,
                       QString *error)
{
    sqlite3 *handle = db.handle();
    sqlite3_busy_timeout(handle, 10000);
    if (!db.exec(QStringLiteral("PRAGMA cache_size=-5000; PRAGMA temp_store=2; PRAGMA foreign_keys=ON;"), error))
        return false;
    // Calibre's full text tokenizer in place of SQLite's, as Calibre's
    // connections have it: the annotations' index is made with it. Without
    // FTS5, a write that needs it fails there.
    QString ftsError;
    CalibreFts::registerTokenizers(handle, interfaceLanguage, &ftsError);

    constexpr int flags = SQLITE_UTF8 | SQLITE_INNOCUOUS;
    constexpr int pure = flags | SQLITE_DETERMINISTIC;
    const auto settings = [&] { return new TextSettings{ tweaks, interfaceLanguage }; };
    if (!registered(handle, sqlite3_create_function_v2(handle, "title_sort", -1, pure, settings(),
                                                       titleSortFunction, nullptr, nullptr, destroyTextSettings),
                    "title_sort", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "author_to_author_sort", 1, pure, settings(),
                                                          authorSortFunction, nullptr, nullptr, destroyTextSettings),
                       "author_to_author_sort", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "uuid4", 0, flags, nullptr, uuidFunction,
                                                          nullptr, nullptr, nullptr),
                       "uuid4", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "books_list_filter", 1, pure, nullptr,
                                                          booksListFilterFunction, nullptr, nullptr, nullptr),
                       "books_list_filter", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "concat", 1, pure, nullptr, nullptr,
                                                          concatStep, concatFinal, nullptr),
                       "concat", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "sortconcat", 2, pure, const_cast<char *>(","),
                                                          nullptr, sortConcatStep, sortConcatFinal, nullptr),
                       "sortconcat", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "sortconcat_bar", 2, pure, const_cast<char *>("|"),
                                                          nullptr, sortConcatStep, sortConcatFinal, nullptr),
                       "sortconcat_bar", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "sortconcat_amper", 2, pure, const_cast<char *>("&"),
                                                          nullptr, sortConcatStep, sortConcatFinal, nullptr),
                       "sortconcat_amper", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "identifiers_concat", 2, pure, nullptr, nullptr,
                                                          identifiersConcatStep, identifiersConcatFinal, nullptr),
                       "identifiers_concat", error)
        || !registered(handle, sqlite3_create_function_v2(handle, "aum_sortconcat", 4, pure, nullptr, nullptr,
                                                          authorsConcatStep, authorsConcatFinal, nullptr),
                       "aum_sortconcat", error)) {
        return false;
    }

    // Calibre keeps these triggers out of the file: each connection makes
    // them, so that author sorts follow its own tweaks.
    if (!db.exec(QStringLiteral(R"SQL(
        CREATE TEMP TRIGGER author_insert_trg
            AFTER INSERT ON authors
            BEGIN
            UPDATE authors SET sort=author_to_author_sort(NEW.name) WHERE id=NEW.id;
        END;
        CREATE TEMP TRIGGER author_update_trg
            BEFORE UPDATE ON authors
            BEGIN
            UPDATE authors SET sort=author_to_author_sort(NEW.name)
            WHERE id=NEW.id AND name <> NEW.name;
        END;
    )SQL"), error)) {
        return false;
    }

    // Deleting a book deletes its custom column values; initialize_custom_columns()
    // skips the columns whose tables are gone.
    QStringList deletes;
    QStringList tables;
    if (!forEachRow(db, QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"),
                    [&](SqliteStatement &row) { tables.append(row.text(0)); }, error)
        || !forEachRow(db, QStringLiteral("SELECT id, normalized FROM custom_columns WHERE mark_for_delete=0"),
                       [&](SqliteStatement &row) {
                           const QString table = QStringLiteral("custom_column_%1").arg(row.integer(0));
                           const QString link = QStringLiteral("books_custom_column_%1_link").arg(row.integer(0));
                           const bool normalized = row.integer(1) != 0;
                           if (!tables.contains(table) || (normalized && !tables.contains(link)))
                               return;
                           deletes.append(QStringLiteral("DELETE FROM %1 WHERE book=OLD.id;")
                                              .arg(normalized ? link : table));
                       }, error)) {
        return false;
    }
    if (!deletes.isEmpty()) {
        return db.exec(QStringLiteral("CREATE TEMP TRIGGER custom_books_delete_trg AFTER DELETE ON books BEGIN %1 END;")
                           .arg(deletes.join(u'\n')), error);
    }
    return true;
}

QString schemaFingerprint(SqliteDb &db)
{
    QStringList entries;
    QStringList tables;
    forEachRow(db, QStringLiteral("SELECT type, name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'"),
               [&](SqliteStatement &row) {
                   const QString type = row.text(0);
                   const QString name = row.text(1);
                   if (isCustomColumnObject(name))
                       return;
                   if (type == QLatin1String("table"))
                       tables.append(name);
                   else
                       entries.append(type + u' ' + name);
               }, nullptr);
    SqliteStatement columnsOf(db, QStringLiteral("SELECT name, upper(type), \"notnull\", pk FROM pragma_table_info(?)"));
    for (const QString &table : std::as_const(tables)) {
        QStringList columns;
        columnsOf.reset();
        columnsOf.bind(1, table);
        while (columnsOf.step()) {
            columns.append(QStringLiteral("%1:%2:%3:%4").arg(columnsOf.text(0), columnsOf.text(1))
                               .arg(columnsOf.integer(2)).arg(columnsOf.integer(3)));
        }
        columns.sort();
        entries.append(QStringLiteral("table %1(%2)").arg(table, columns.join(u',')));
    }
    entries.sort();
    return QString::fromLatin1(QCryptographicHash::hash(entries.join(u'\n').toUtf8(), QCryptographicHash::Sha256)
                                   .toHex().left(16));
}

bool isKnownSchema(int userVersion, const QString &fingerprint)
{
    return std::any_of(std::begin(knownSchemas), std::end(knownSchemas), [&](const KnownSchema &known) {
        return known.userVersion == userVersion && fingerprint == QLatin1String(known.fingerprint);
    });
}

PythonJson::Value multipleSeparators(const CustomColumn &column)
{
    PythonJson::Value separators = PythonJson::Value::dict();
    if (!column.multiple)
        return separators;
    const PythonJson::Value display = PythonJson::parse(column.display);
    const PythonJson::Value *names = display.find(QStringLiteral("is_names"));
    QString cache = QStringLiteral("|");
    QString ui = QStringLiteral(",");
    QString list = QStringLiteral(", ");
    if (names && PythonJson::truthy(*names)) {
        ui = QStringLiteral("&");
        list = QStringLiteral(" & ");
    } else if (column.datatype == QLatin1String("composite")) {
        cache = QStringLiteral(",");
    }
    separators.insert(QStringLiteral("cache_to_list"), PythonJson::Value::from(cache));
    separators.insert(QStringLiteral("ui_to_list"), PythonJson::Value::from(ui));
    separators.insert(QStringLiteral("list_to_ui"), PythonJson::Value::from(list));
    return separators;
}

bool readCustomColumns(SqliteDb &db, QList<CustomColumn> *columns, QString *error)
{
    columns->clear();
    QStringList tables;
    if (!forEachRow(db, QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"),
                    [&](SqliteStatement &row) { tables.append(row.text(0)); }, error)
        || !forEachRow(db, QStringLiteral("SELECT id, label, name, datatype, is_multiple, normalized, editable, display "
                                          "FROM custom_columns WHERE mark_for_delete=0 ORDER BY id"),
                       [&](SqliteStatement &row) {
                           CustomColumn column;
                           column.id = int(row.integer(0));
                           column.label = row.text(1);
                           column.name = row.text(2);
                           column.datatype = row.text(3);
                           column.multiple = row.integer(4) != 0;
                           column.normalized = row.integer(5) != 0;
                           column.editable = row.integer(6) != 0;
                           column.display = row.text(7).toUtf8();
                           columns->append(column);
                       }, error)) {
        return false;
    }
    // initialize_custom_columns() leaves out a column whose tables are gone.
    columns->removeIf([&tables](const CustomColumn &column) {
        const QString table = QStringLiteral("custom_column_%1").arg(column.id);
        const QString link = QStringLiteral("books_custom_column_%1_link").arg(column.id);
        return !tables.contains(table) || (column.normalized && !tables.contains(link));
    });
    return true;
}

// The custom columns and each book's values, as Calibre's tables read them
// in db/tables.py: a normalized column's items by link order, a series'
// number from its link's extra, the others one row per book. A composite's
// values are Calibre's to compute.
template<typename BookWithId>
bool readCustomValues(SqliteDb &db, QList<Book> &books, const BookWithId &bookWithId, QList<CustomColumn> *columns,
                      bool only, QString *error)
{
    if (!readCustomColumns(db, columns, error))
        return false;
    for (Book &book : books)
        book.custom.resize(columns->size());

    for (qsizetype index = 0; index < columns->size(); ++index) {
        const CustomColumn &column = columns->at(index);
        if (column.datatype == QLatin1String("composite"))
            continue;
        const auto convert = [&column](SqliteStatement &row, int at) -> QVariant {
            if (row.isNull(at))
                return {};
            const QString &type = column.datatype;
            if (type == QLatin1String("int") || type == QLatin1String("rating"))
                return row.integer(at);
            if (type == QLatin1String("float"))
                return row.real(at);
            if (type == QLatin1String("bool"))
                return row.integer(at) != 0;
            if (type == QLatin1String("datetime"))
                return parseDate(row.value(at));
            return row.text(at);
        };
        const bool series = column.datatype == QLatin1String("series");
        const QString sql = column.normalized
            ? QStringLiteral("SELECT l.book, v.value%1 FROM books_custom_column_%2_link l "
                             "JOIN custom_column_%2 v ON v.id=l.value%3 ORDER BY l.id")
                  .arg(series ? QStringLiteral(", l.extra") : QString()).arg(column.id)
                  .arg(only ? QStringLiteral(" WHERE l.book IN (SELECT id FROM temp.cousoir_only)") : QString())
            : QStringLiteral("SELECT book, value FROM custom_column_%1%2").arg(column.id)
                  .arg(only ? QStringLiteral(" WHERE book IN (SELECT id FROM temp.cousoir_only)") : QString());
        if (!forEachRow(db, sql, [&](SqliteStatement &row) {
                Book *book = bookWithId(row.integer(0));
                if (!book)
                    return;
                CustomValue &value = book->custom[index];
                const QVariant read = convert(row, 1);
                if (column.multiple) {
                    QStringList values = value.value.toStringList();
                    values.append(read.toString());
                    value.value = values;
                } else {
                    value.value = read;
                }
                if (series)
                    value.seriesIndex = row.isNull(2) ? 1.0 : row.real(2);
            }, error)) {
            return false;
        }
        // A book with none of several values has an empty list.
        if (column.multiple) {
            for (Book &book : books) {
                if (book.custom[index].value.isNull())
                    book.custom[index].value = QStringList();
            }
        }
    }
    return true;
}

namespace {

// The items of the browse sections, by id, as their tables hold them.
struct ItemTables
{
    QHash<int, FacetItem> authors;
    QHash<int, FacetItem> series;
    QHash<int, FacetItem> tags;
    QHash<int, FacetItem> publishers;
    QHash<int, FacetItem> languages;
};

// What readLibrary() reads, the books and their values only for those in
// temp.cousoir_only when `only` is set; the items, the preferences and the
// custom columns always whole. The items' counts are made from the books
// after, by finishLibrary().
bool readRows(SqliteDb &db, LibraryData *data, const QString &interfaceLanguage, bool only, ItemTables *items,
              QString *error)
{
    // " WHERE book IN (…)" for a table of the books' values, when only some
    // books are read; before any ORDER BY.
    const auto onlyBooks = [only](const char *column) {
        return only ? QStringLiteral(" WHERE %1 IN (SELECT id FROM temp.cousoir_only)").arg(QLatin1String(column))
                    : QString();
    };
    data->userVersion = db.value(QStringLiteral("PRAGMA user_version")).toInt();
    data->schemaFingerprint = schemaFingerprint(db);
    data->schemaKnown = isKnownSchema(data->userVersion, data->schemaFingerprint);
    data->uuid = db.value(QStringLiteral("SELECT uuid FROM library_id")).toString();
    bool expiryOk = false;
    const double expiry = db.value(QStringLiteral("SELECT val FROM preferences WHERE key='expire_old_trash_after'"))
                              .toString().toDouble(&expiryOk);
    data->trashExpirySeconds = expiryOk ? std::max<qint64>(24 * 3600, qint64(expiry)) : 14 * 24 * 3600;

    for (const char *key : { "saved_searches", "virtual_libraries", "grouped_search_terms", "user_categories",
                             "virtual_lib_on_startup", "bools_are_tristate", "library_view books view state",
                             "fts_enabled" }) {
        SqliteStatement preference(db, QStringLiteral("SELECT val FROM preferences WHERE key=?"));
        preference.bind(1, QString::fromLatin1(key));
        if (preference.step())
            data->preferences.insert(QString::fromLatin1(key), preference.text(0));
    }

    QList<Book> books;
    QHash<int, qsizetype> rows;
    if (!forEachRow(db, QStringLiteral("SELECT id, title, sort, timestamp, pubdate, series_index, author_sort, path, "
                                       "uuid, has_cover, last_modified FROM books%1 ORDER BY id").arg(onlyBooks("id")),
                    [&](SqliteStatement &row) {
                        Book book;
                        book.id = int(row.integer(0));
                        book.title = row.text(1);
                        book.sort = row.text(2);
                        book.timestamp = parseDate(row.value(3));
                        book.pubdate = parseDate(row.value(4));
                        book.seriesIndex = row.isNull(5) ? 1.0 : row.real(5);
                        book.authorSort = row.text(6);
                        book.path = row.text(7);
                        book.uuid = row.text(8);
                        book.hasCover = row.integer(9) != 0;
                        book.lastModified = parseDate(row.value(10));
                        rows.insert(book.id, books.size());
                        books.append(book);
                    }, error)) {
        return false;
    }
    const auto bookWithId = [&](qint64 id) -> Book * {
        const auto found = rows.constFind(int(id));
        return found == rows.cend() ? nullptr : &books[*found];
    };

    // Items by id, then the links in Calibre's order: by link id for the
    // lists, as ManyToManyTable reads them.
    QHash<int, FacetItem> &authors = items->authors;
    QHash<int, FacetItem> &series = items->series;
    QHash<int, FacetItem> &tags = items->tags;
    QHash<int, FacetItem> &publishers = items->publishers;
    QHash<int, FacetItem> &languages = items->languages;
    QHash<int, int> ratings;
    // AuthorsTable computes the sort of an author without one, with this
    // computer's tweaks.
    const bool ok =
        forEachRow(db, QStringLiteral("SELECT id, name, CASE WHEN sort IS NULL OR sort = '' "
                                      "THEN author_to_author_sort(name) ELSE sort END, link FROM authors"),
                   [&](SqliteStatement &row) {
                       authors.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1).replace(u'|', u','),
                                                             row.text(2), 0, row.text(3) });
                   }, error)
        && forEachRow(db, QStringLiteral("SELECT book, author FROM books_authors_link%1 ORDER BY id").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto author = authors.constFind(int(row.integer(1)));
                          if (!book || author == authors.cend())
                              return;
                          book->authorIds.append(author->id);
                          book->authors.append(author->name);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, sort, link FROM series"), [&](SqliteStatement &row) {
            series.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1), row.text(2), 0, row.text(3) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, series FROM books_series_link%1").arg(onlyBooks("book")), [&](SqliteStatement &row) {
            Book *book = bookWithId(row.integer(0));
            const auto item = series.constFind(int(row.integer(1)));
            if (!book || item == series.cend())
                return;
            book->seriesId = item->id;
            book->series = item->name;
        }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, link FROM tags"), [&](SqliteStatement &row) {
            tags.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1), row.text(1), 0, row.text(2) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, tag FROM books_tags_link%1 ORDER BY id").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto tag = tags.constFind(int(row.integer(1)));
                          if (!book || tag == tags.cend())
                              return;
                          book->tagIds.append(tag->id);
                          book->tags.append(tag->name);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, sort, link FROM publishers"), [&](SqliteStatement &row) {
            publishers.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1),
                                                     row.isNull(2) ? row.text(1) : row.text(2), 0, row.text(3) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, publisher FROM books_publishers_link%1").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto publisher = publishers.constFind(int(row.integer(1)));
                          if (!book || publisher == publishers.cend())
                              return;
                          book->publisherId = publisher->id;
                          book->publisher = publisher->name;
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, rating FROM ratings"), [&](SqliteStatement &row) {
            ratings.insert(int(row.integer(0)), int(row.integer(1)));
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, rating FROM books_ratings_link%1").arg(onlyBooks("book")), [&](SqliteStatement &row) {
            // RatingTable treats a rating of 0 as none.
            if (Book *book = bookWithId(row.integer(0)))
                book->rating = std::clamp(ratings.value(int(row.integer(1))), 0, 10);
        }, error)
        && forEachRow(db, QStringLiteral("SELECT id, lang_code FROM languages"), [&](SqliteStatement &row) {
            // Shown by name; the code stays in `sort`.
            const QString code = row.text(1);
            languages.insert(int(row.integer(0)), { int(row.integer(0)), CalibreText::languageName(code, interfaceLanguage), code, 0, {} });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, lang_code FROM books_languages_link%1 ORDER BY id").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto language = languages.constFind(int(row.integer(1)));
                          if (!book || language == languages.cend())
                              return;
                          book->languageIds.append(language->id);
                          book->languages.append(language->sort);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, type, val FROM identifiers%1 ORDER BY id").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->identifiers.append({ row.text(1), row.text(2) });
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, text FROM comments%1").arg(onlyBooks("book")), [&](SqliteStatement &row) {
            if (Book *book = bookWithId(row.integer(0)))
                book->comments = row.text(1);
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, format, name, uncompressed_size FROM data%1").arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->formats.append({ row.text(1).toUpper(), row.text(2), row.integer(3), {} });
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, data FROM conversion_options WHERE format='PIPE'%1")
                          .arg(only ? QStringLiteral(" AND book IN (SELECT id FROM temp.cousoir_only)") : QString()),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->conversionOptions = row.blob(1);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, pages FROM books_pages_link%1").arg(onlyBooks("book")), [&](SqliteStatement &row) {
            if (Book *book = bookWithId(row.integer(0)))
                book->pages = row.isNull(1) ? -1 : int(row.integer(1));
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, format, user, device, cfi, epoch, pos_frac FROM last_read_positions%1")
                          .arg(onlyBooks("book")),
                      [&](SqliteStatement &row) {
                          data->readPositions[int(row.integer(0))].append(
                              { row.text(1).toUpper(), row.text(2), row.text(3), row.text(4), row.real(5), row.real(6) });
                      }, error);
    if (!ok)
        return false;

    for (Book &book : books) {
        std::sort(book.formats.begin(), book.formats.end(),
                  [](const BookFormat &a, const BookFormat &b) { return a.format < b.format; });
        // The place read last, whoever read it.
        const auto positions = data->readPositions.constFind(book.id);
        if (positions == data->readPositions.cend())
            continue;
        for (const ReadPosition &position : *positions) {
            if (position.epoch > book.readEpoch) {
                book.readEpoch = position.epoch;
                book.readFraction = std::clamp(position.fraction, 0.0, 1.0);
            }
        }
    }

    if (!readCustomValues(db, books, bookWithId, &data->customColumns, only, error))
        return false;

    data->books = std::move(books);
    return true;
}

// The items with the number of books linked to each, from the books: an
// item none links to is left out, as Calibre's browser leaves it.
void finishLibrary(SqliteDb &db, LibraryData *data, const ItemTables &items, bool countAnnotations)
{
    QHash<int, int> authorCounts, seriesCounts, tagCounts, publisherCounts, languageCounts;
    for (const Book &book : std::as_const(data->books)) {
        for (const int id : book.authorIds)
            addTo(authorCounts, id);
        if (book.seriesId)
            addTo(seriesCounts, book.seriesId);
        for (const int id : book.tagIds)
            addTo(tagCounts, id);
        if (book.publisherId)
            addTo(publisherCounts, book.publisherId);
        for (const int id : book.languageIds)
            addTo(languageCounts, id);
    }
    data->authors = facetItems(items.authors, authorCounts);
    data->series = facetItems(items.series, seriesCounts);
    data->tags = facetItems(items.tags, tagCounts);
    data->publishers = facetItems(items.publishers, publisherCounts);
    data->languages = facetItems(items.languages, languageCounts);
    if (countAnnotations)
        data->annotationCount = AnnotationSearch::count(db);
    if (!data->libraryPath.isEmpty())
        readNotes(data);
}

} // namespace

bool readLibrary(SqliteDb &db, LibraryData *data, const QString &interfaceLanguage, QString *error)
{
    ItemTables items;
    data->books.clear();
    data->readPositions.clear();
    if (!readRows(db, data, interfaceLanguage, false, &items, error))
        return false;
    finishLibrary(db, data, items, true);
    return true;
}

bool trackChanges(SqliteDb &db, QString *error)
{
    if (!db.exec(QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS cousoir_changes(book INTEGER, tbl TEXT); "
                                "DELETE FROM temp.cousoir_changes"),
                 error))
        return false;
    // Each table's triggers, made once for the connection.
    if (db.value(QStringLiteral("SELECT count(*) FROM temp.sqlite_master WHERE type='trigger' "
                                "AND name LIKE 'cousoir_track_%'")).toLongLong() > 0)
        return true;
    QStringList triggers;
    const auto track = [&triggers](const QString &table, const QString &column) {
        const QString name = QString(table).replace(u'"', u'_');
        const QString target = QStringLiteral("main.\"%1\"").arg(table);
        const QString note = QStringLiteral("INSERT INTO temp.cousoir_changes VALUES (%2.%1, '%3');")
                                 .arg(column, QStringLiteral("%1"), name);
        triggers << QStringLiteral("CREATE TEMP TRIGGER cousoir_track_%1_i AFTER INSERT ON %2 BEGIN %3 END")
                        .arg(name, target, note.arg(QStringLiteral("NEW")))
                 << QStringLiteral("CREATE TEMP TRIGGER cousoir_track_%1_u AFTER UPDATE ON %2 BEGIN %3 %4 END")
                        .arg(name, target, note.arg(QStringLiteral("OLD")), note.arg(QStringLiteral("NEW")))
                 << QStringLiteral("CREATE TEMP TRIGGER cousoir_track_%1_d AFTER DELETE ON %2 BEGIN %3 END")
                        .arg(name, target, note.arg(QStringLiteral("OLD")));
    };
    // An item renamed or gone changes what its books show.
    const auto trackItems = [&triggers](const QString &table, const QString &link, const QString &column) {
        const QString note = QStringLiteral("INSERT INTO temp.cousoir_changes SELECT book, '%1' FROM main.\"%2\" "
                                            "WHERE \"%3\"=OLD.id;")
                                 .arg(table, link, column);
        triggers << QStringLiteral("CREATE TEMP TRIGGER cousoir_track_%1_iu AFTER UPDATE ON main.\"%1\" BEGIN %2 END")
                        .arg(table, note)
                 << QStringLiteral("CREATE TEMP TRIGGER cousoir_track_%1_id AFTER DELETE ON main.\"%1\" BEGIN %2 END")
                        .arg(table, note);
    };
    QStringList tables;
    if (!forEachRow(db, QStringLiteral("SELECT name FROM main.sqlite_master WHERE type='table' "
                                       "AND sql NOT LIKE 'CREATE VIRTUAL%' AND name NOT LIKE 'sqlite_%'"),
                    [&](SqliteStatement &row) { tables.append(row.text(0)); }, error))
        return false;
    static const QRegularExpression customLink(QStringLiteral("^books_custom_column_(\\d+)_link$"));
    for (const QString &table : std::as_const(tables)) {
        if (table == QLatin1String("books")) {
            track(table, QStringLiteral("id"));
            continue;
        }
        const QList<QVariantList> columns = db.rows(QStringLiteral("SELECT name FROM pragma_table_info(?)"), { table });
        if (std::any_of(columns.cbegin(), columns.cend(),
                        [](const QVariantList &column) { return column.value(0).toString() == QLatin1String("book"); }))
            track(table, QStringLiteral("book"));
        const QRegularExpressionMatch custom = customLink.match(table);
        if (custom.hasMatch())
            trackItems(QStringLiteral("custom_column_%1").arg(custom.captured(1)), table, QStringLiteral("value"));
    }
    for (const auto &[items, link, column] : { std::tuple{ "authors", "books_authors_link", "author" },
                                                std::tuple{ "series", "books_series_link", "series" },
                                                std::tuple{ "tags", "books_tags_link", "tag" },
                                                std::tuple{ "publishers", "books_publishers_link", "publisher" },
                                                std::tuple{ "languages", "books_languages_link", "lang_code" },
                                                std::tuple{ "ratings", "books_ratings_link", "rating" } }) {
        if (tables.contains(QLatin1String(items)) && tables.contains(QLatin1String(link)))
            trackItems(QLatin1String(items), QLatin1String(link), QLatin1String(column));
    }
    for (const QString &trigger : std::as_const(triggers)) {
        if (!db.exec(trigger, error))
            return false;
    }
    return true;
}

QSet<int> changedBooks(SqliteDb &db)
{
    QSet<int> books;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT DISTINCT book FROM temp.cousoir_changes "
                                                          "WHERE book IS NOT NULL")))
        books.insert(row.value(0).toInt());
    return books;
}

QSet<QString> changedTables(SqliteDb &db)
{
    QSet<QString> tables;
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT DISTINCT tbl FROM temp.cousoir_changes")))
        tables.insert(row.value(0).toString());
    return tables;
}

bool refreshLibrary(SqliteDb &db, LibraryData *data, const QSet<int> &books, const QString &interfaceLanguage,
                    bool countAnnotations, bool *whole, QString *error)
{
    *whole = false;
    const auto byId = [](const Book &a, const Book &b) { return a.id < b.id; };
    // The books merged by id; a list made otherwise is read again whole.
    if (!std::is_sorted(data->books.cbegin(), data->books.cend(), byId)) {
        *whole = true;
        return readLibrary(db, data, interfaceLanguage, error);
    }
    if (!db.exec(QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS cousoir_only(id INTEGER PRIMARY KEY); "
                                "DELETE FROM temp.cousoir_only"),
                 error))
        return false;
    {
        SqliteStatement insert(db, QStringLiteral("INSERT OR IGNORE INTO temp.cousoir_only VALUES (?)"));
        for (const int id : books) {
            insert.reset();
            insert.bind(1, qint64(id));
            insert.step();
        }
    }
    LibraryData fresh;
    fresh.libraryPath = data->libraryPath;
    ItemTables items;
    if (!readRows(db, &fresh, interfaceLanguage, true, &items, error))
        return false;
    // Other columns, and the values of the books kept would no longer be
    // theirs.
    if (fresh.customColumns != data->customColumns) {
        *whole = true;
        return readLibrary(db, data, interfaceLanguage, error);
    }

    QList<Book> merged;
    merged.reserve(data->books.size() + fresh.books.size());
    auto kept = data->books.cbegin();
    auto read = fresh.books.cbegin();
    while (kept != data->books.cend() || read != fresh.books.cend()) {
        if (read != fresh.books.cend() && (kept == data->books.cend() || read->id <= kept->id)) {
            if (kept != data->books.cend() && kept->id == read->id)
                ++kept;
            merged.append(*read++);
        } else {
            // A book changed and no longer there was removed.
            if (!books.contains(kept->id))
                merged.append(*kept);
            ++kept;
        }
    }
    for (const int id : books)
        data->readPositions.remove(id);
    for (auto it = fresh.readPositions.cbegin(); it != fresh.readPositions.cend(); ++it)
        data->readPositions.insert(it.key(), it.value());

    data->books = std::move(merged);
    data->userVersion = fresh.userVersion;
    data->schemaFingerprint = fresh.schemaFingerprint;
    data->schemaKnown = fresh.schemaKnown;
    data->uuid = fresh.uuid;
    data->trashExpirySeconds = fresh.trashExpirySeconds;
    data->preferences = fresh.preferences;
    finishLibrary(db, data, items, countAnnotations);
    return true;
}

QString firstDifference(const LibraryData &a, const LibraryData &b)
{
#define COUSOIR_COMPARE(object, field, what)                                                                       \
    if (!(a##object field == b##object field))                                                                     \
        return what QStringLiteral(#field);
    COUSOIR_COMPARE(, .libraryPath, )
    COUSOIR_COMPARE(, .uuid, )
    COUSOIR_COMPARE(, .userVersion, )
    COUSOIR_COMPARE(, .schemaFingerprint, )
    COUSOIR_COMPARE(, .schemaKnown, )
    COUSOIR_COMPARE(, .authors, )
    COUSOIR_COMPARE(, .series, )
    COUSOIR_COMPARE(, .tags, )
    COUSOIR_COMPARE(, .publishers, )
    COUSOIR_COMPARE(, .languages, )
    COUSOIR_COMPARE(, .trashExpirySeconds, )
    COUSOIR_COMPARE(, .customColumns, )
    COUSOIR_COMPARE(, .readPositions, )
    COUSOIR_COMPARE(, .notes, )
    COUSOIR_COMPARE(, .annotationCount, )
    COUSOIR_COMPARE(, .preferences, )
#undef COUSOIR_COMPARE
    if (a.books.size() != b.books.size())
        return QStringLiteral("books: %1 and %2").arg(a.books.size()).arg(b.books.size());
    for (qsizetype row = 0; row < a.books.size(); ++row) {
        const Book &x = a.books.at(row);
        const Book &y = b.books.at(row);
#define COUSOIR_COMPARE(field)                                                                                     \
    if (!(x.field == y.field))                                                                                     \
        return QStringLiteral("book %1 and %2: %3").arg(x.id).arg(y.id).arg(QLatin1String(#field));
        COUSOIR_COMPARE(id)
        COUSOIR_COMPARE(title)
        COUSOIR_COMPARE(sort)
        COUSOIR_COMPARE(authorSort)
        COUSOIR_COMPARE(authorIds)
        COUSOIR_COMPARE(authors)
        COUSOIR_COMPARE(seriesId)
        COUSOIR_COMPARE(series)
        COUSOIR_COMPARE(seriesIndex)
        COUSOIR_COMPARE(tagIds)
        COUSOIR_COMPARE(tags)
        COUSOIR_COMPARE(publisherId)
        COUSOIR_COMPARE(publisher)
        COUSOIR_COMPARE(rating)
        COUSOIR_COMPARE(languageIds)
        COUSOIR_COMPARE(languages)
        COUSOIR_COMPARE(identifiers)
        COUSOIR_COMPARE(timestamp)
        COUSOIR_COMPARE(pubdate)
        COUSOIR_COMPARE(lastModified)
        COUSOIR_COMPARE(path)
        COUSOIR_COMPARE(uuid)
        COUSOIR_COMPARE(hasCover)
        COUSOIR_COMPARE(pages)
        COUSOIR_COMPARE(readEpoch)
        COUSOIR_COMPARE(readFraction)
        COUSOIR_COMPARE(comments)
        COUSOIR_COMPARE(formats)
        COUSOIR_COMPARE(conversionOptions)
        COUSOIR_COMPARE(custom)
        COUSOIR_COMPARE(coverFile)
        COUSOIR_COMPARE(coverModified)
#undef COUSOIR_COMPARE
    }
    return {};
}

QString formatFile(const QString &folder, const QString &name, const QString &format)
{
    const QString extension = u'.' + format.toLower();
    const QString expected = QDir(folder).filePath(name + extension);
    if (QFileInfo(expected).isFile())
        return expected;
    const QStringList entries = QDir(folder).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString &entry : entries) {
        if (entry.endsWith(extension) && !entry.startsWith(QLatin1String("._")))
            return QDir(folder).filePath(entry);
    }
    return {};
}

void findFiles(LibraryData *data, const QSet<int> *only)
{
    const QDir library(data->libraryPath);
    for (Book &book : data->books) {
        if (only && !only->contains(book.id))
            continue;
        const QString folder = library.filePath(book.path);
        const QFileInfo cover(QDir(folder).filePath(QStringLiteral("cover.jpg")));
        book.coverFile = !book.path.isEmpty() && cover.isFile() ? cover.filePath() : QString();
        book.coverModified = book.coverFile.isEmpty()
            ? 0 : cover.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch();
        for (BookFormat &format : book.formats)
            format.file = book.path.isEmpty() ? QString() : formatFile(folder, format.name, format.format);
    }
}

bool loadLibrary(const QString &libraryPath, const QString &snapshotPath, const CalibreConfig &config,
                 LibraryData *data, QString *error)
{
    LibraryData loaded;
    loaded.libraryPath = QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath());
    if (!takeSnapshot(loaded.libraryPath, snapshotPath, &loaded.source, error))
        return false;

    SqliteDb db;
    if (!db.open(snapshotPath, SqliteDb::Mode::Immutable, error)
        || !prepareConnection(db, config.tweaks, config.interfaceLanguage, error)
        || !readLibrary(db, &loaded, config.interfaceLanguage, error)) {
        return false;
    }
    db.close();
    findFiles(&loaded);
    CalibreTemplate::computeComposites(&loaded, config.interfaceLanguage);
    *data = std::move(loaded);
    return true;
}

QString notesFolder(const QString &libraryPath)
{
    return QDir(libraryPath).filePath(QStringLiteral(".calnotes"));
}

void readNotes(LibraryData *data)
{
    data->notes.clear();
    const QString path = QDir(notesFolder(data->libraryPath)).filePath(QStringLiteral("notes.db"));
    if (!QFileInfo::exists(path))
        return;
    // Read without a lock or a file beside it, as metadata.db's copy is.
    SqliteDb db;
    QString error;
    if (!db.open(path, SqliteDb::Mode::Immutable, &error))
        return;
    QHash<int, QPair<QString, int>> byId;
    for (const QVariantList &row :
         db.rows(QStringLiteral("SELECT id, item, colname, doc, searchable_text, ctime, mtime FROM notes"))) {
        const QString field = row.value(2).toString().toLower();
        const int item = row.value(1).toInt();
        ItemNote note;
        note.id = row.value(0).toInt();
        note.doc = row.value(3).toString();
        note.searchable = row.value(4).toString();
        note.created = row.value(5).toDouble();
        note.modified = row.value(6).toDouble();
        data->notes[field].insert(item, note);
        byId.insert(note.id, { field, item });
    }
    for (const QVariantList &row : db.rows(QStringLiteral("SELECT note, resource FROM notes_resources_link"))) {
        const auto found = byId.constFind(row.value(0).toInt());
        if (found != byId.cend())
            data->notes[found->first][found->second].resources.append(row.value(1).toString());
    }
}

QString notesField(const QString &kind)
{
    static const QHash<QString, QString> fields{
        { QStringLiteral("authors"), QStringLiteral("authors") },     { QStringLiteral("series"), QStringLiteral("series") },
        { QStringLiteral("tags"), QStringLiteral("tags") },           { QStringLiteral("publishers"), QStringLiteral("publisher") },
        { QStringLiteral("languages"), QStringLiteral("languages") },
    };
    return fields.value(kind);
}

QDateTime undefinedDate()
{
    return QDateTime(QDate(101, 1, 1), QTime(0, 0), QTimeZone::UTC);
}

bool isUndefinedDate(const QDateTime &date)
{
    // is_date_undefined() in utils/date.py: before 0101, or its 1 January.
    if (!date.isValid())
        return true;
    const QDate day = date.toUTC().date();
    return day.year() < 101 || (day.year() == 101 && day.month() == 1 && day.day() == 1);
}

QDateTime parseDate(const QVariant &value)
{
    // c_parse() in db/tables.py: a year stored as a number is 3 January of
    // that year.
    if (value.typeId() == QMetaType::LongLong || value.typeId() == QMetaType::Double)
        return QDateTime(QDate(int(value.toLongLong()), 1, 3), QTime(0, 0), QTimeZone::UTC);
    if (value.isNull())
        return undefinedDate();

    static const QRegularExpression pattern(QStringLiteral(
        "^\\s*(\\d{4})-(\\d{2})-(\\d{2})(?:[T ](\\d{2}):(\\d{2})(?::(\\d{2})(?:\\.\\d+)?)?)?"
        "\\s*(?:(Z)|([+-])(\\d{2}):?(\\d{2}))?\\s*$"));
    const QRegularExpressionMatch match = pattern.match(value.toString());
    if (!match.hasMatch())
        return undefinedDate();
    const QDate day(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
    const QTime time(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt());
    if (!day.isValid() || !time.isValid())
        return undefinedDate();
    QDateTime date(day, time, QTimeZone::UTC);
    if (!match.captured(8).isEmpty()) {
        const int offset = (match.captured(9).toInt() * 60 + match.captured(10).toInt()) * 60;
        date = date.addSecs(match.captured(8) == QLatin1String("+") ? -offset : offset);
    }
    return date;
}

QString isoFormat(const QDateTime &date)
{
    return CalibreDate{ date }.isoFormat();
}

} // namespace CalibreDb
