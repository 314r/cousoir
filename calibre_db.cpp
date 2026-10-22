// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/backend.py: the SQL functions, aggregates and temporary
//   author triggers of a connection, and format_abspath()
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
#include <map>
#include <memory>

#include <sqlite3.h>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "calibre_date.h"
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

        QFile file(database);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QObject::tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(database), file.errorString());
            return false;
        }
        const QByteArray contents = file.readAll();
        file.close();
        if (identify(database) != before || contents.size() != before.size) {
            reason = QObject::tr("The library kept changing while it was read. Try again in a moment.");
            continue;
        }

        QDir().mkpath(QFileInfo(snapshotPath).absolutePath());
        QSaveFile copy(snapshotPath);
        if (!copy.open(QIODevice::WriteOnly) || copy.write(contents) != contents.size() || !copy.commit()) {
            if (error)
                *error = QObject::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(snapshotPath), copy.errorString());
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

bool readLibrary(SqliteDb &db, LibraryData *data, QString *error)
{
    data->userVersion = db.value(QStringLiteral("PRAGMA user_version")).toInt();
    data->schemaFingerprint = schemaFingerprint(db);
    data->schemaKnown = isKnownSchema(data->userVersion, data->schemaFingerprint);
    data->uuid = db.value(QStringLiteral("SELECT uuid FROM library_id")).toString();
    bool expiryOk = false;
    const double expiry = db.value(QStringLiteral("SELECT val FROM preferences WHERE key='expire_old_trash_after'"))
                              .toString().toDouble(&expiryOk);
    data->trashExpirySeconds = expiryOk ? std::max<qint64>(24 * 3600, qint64(expiry)) : 14 * 24 * 3600;

    for (const char *key : { "saved_searches", "virtual_libraries", "grouped_search_terms", "user_categories",
                             "virtual_lib_on_startup" }) {
        SqliteStatement preference(db, QStringLiteral("SELECT val FROM preferences WHERE key=?"));
        preference.bind(1, QString::fromLatin1(key));
        if (preference.step())
            data->preferences.insert(QString::fromLatin1(key), preference.text(0));
    }
    if (!forEachRow(db, QStringLiteral("SELECT label FROM custom_columns WHERE mark_for_delete=0 ORDER BY id"),
                    [&](SqliteStatement &row) { data->customColumns.append(row.text(0)); }, error)) {
        return false;
    }

    QList<Book> books;
    QHash<int, qsizetype> rows;
    if (!forEachRow(db, QStringLiteral("SELECT id, title, sort, timestamp, pubdate, series_index, author_sort, path, "
                                       "uuid, has_cover, last_modified FROM books ORDER BY id"),
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
    QHash<int, FacetItem> authors;
    QHash<int, FacetItem> series;
    QHash<int, FacetItem> tags;
    QHash<int, FacetItem> publishers;
    QHash<int, FacetItem> languages;
    QHash<int, int> ratings;
    QHash<int, int> authorCounts, seriesCounts, tagCounts, publisherCounts, languageCounts;
    // AuthorsTable computes the sort of an author without one, with this
    // computer's tweaks.
    const bool ok =
        forEachRow(db, QStringLiteral("SELECT id, name, CASE WHEN sort IS NULL OR sort = '' "
                                      "THEN author_to_author_sort(name) ELSE sort END, link FROM authors"),
                   [&](SqliteStatement &row) {
                       authors.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1).replace(u'|', u','),
                                                             row.text(2), 0, row.text(3) });
                   }, error)
        && forEachRow(db, QStringLiteral("SELECT book, author FROM books_authors_link ORDER BY id"),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto author = authors.constFind(int(row.integer(1)));
                          if (!book || author == authors.cend())
                              return;
                          book->authorIds.append(author->id);
                          book->authors.append(author->name);
                          addTo(authorCounts, author->id);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, sort, link FROM series"), [&](SqliteStatement &row) {
            series.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1), row.text(2), 0, row.text(3) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, series FROM books_series_link"), [&](SqliteStatement &row) {
            Book *book = bookWithId(row.integer(0));
            const auto item = series.constFind(int(row.integer(1)));
            if (!book || item == series.cend())
                return;
            book->seriesId = item->id;
            book->series = item->name;
            addTo(seriesCounts, item->id);
        }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, link FROM tags"), [&](SqliteStatement &row) {
            tags.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1), row.text(1), 0, row.text(2) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, tag FROM books_tags_link ORDER BY id"),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto tag = tags.constFind(int(row.integer(1)));
                          if (!book || tag == tags.cend())
                              return;
                          book->tagIds.append(tag->id);
                          book->tags.append(tag->name);
                          addTo(tagCounts, tag->id);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, name, sort, link FROM publishers"), [&](SqliteStatement &row) {
            publishers.insert(int(row.integer(0)), { int(row.integer(0)), row.text(1),
                                                     row.isNull(2) ? row.text(1) : row.text(2), 0, row.text(3) });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, publisher FROM books_publishers_link"),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto publisher = publishers.constFind(int(row.integer(1)));
                          if (!book || publisher == publishers.cend())
                              return;
                          book->publisherId = publisher->id;
                          book->publisher = publisher->name;
                          addTo(publisherCounts, publisher->id);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT id, rating FROM ratings"), [&](SqliteStatement &row) {
            ratings.insert(int(row.integer(0)), int(row.integer(1)));
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, rating FROM books_ratings_link"), [&](SqliteStatement &row) {
            // RatingTable treats a rating of 0 as none.
            if (Book *book = bookWithId(row.integer(0)))
                book->rating = std::clamp(ratings.value(int(row.integer(1))), 0, 10);
        }, error)
        && forEachRow(db, QStringLiteral("SELECT id, lang_code FROM languages"), [&](SqliteStatement &row) {
            // Shown by name; the code stays in `sort`.
            const QString code = row.text(1);
            languages.insert(int(row.integer(0)), { int(row.integer(0)), CalibreText::languageName(code), code, 0, {} });
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, lang_code FROM books_languages_link ORDER BY id"),
                      [&](SqliteStatement &row) {
                          Book *book = bookWithId(row.integer(0));
                          const auto language = languages.constFind(int(row.integer(1)));
                          if (!book || language == languages.cend())
                              return;
                          book->languageIds.append(language->id);
                          book->languages.append(language->sort);
                          addTo(languageCounts, language->id);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, type, val FROM identifiers ORDER BY id"),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->identifiers.append({ row.text(1), row.text(2) });
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, text FROM comments"), [&](SqliteStatement &row) {
            if (Book *book = bookWithId(row.integer(0)))
                book->comments = row.text(1);
        }, error)
        && forEachRow(db, QStringLiteral("SELECT book, format, name, uncompressed_size FROM data"),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->formats.append({ row.text(1).toUpper(), row.text(2), row.integer(3), {} });
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, data FROM conversion_options WHERE format='PIPE'"),
                      [&](SqliteStatement &row) {
                          if (Book *book = bookWithId(row.integer(0)))
                              book->conversionOptions = row.blob(1);
                      }, error)
        && forEachRow(db, QStringLiteral("SELECT book, pages FROM books_pages_link"), [&](SqliteStatement &row) {
            if (Book *book = bookWithId(row.integer(0)))
                book->pages = row.isNull(1) ? -1 : int(row.integer(1));
        }, error);
    if (!ok)
        return false;

    for (Book &book : books) {
        std::sort(book.formats.begin(), book.formats.end(),
                  [](const BookFormat &a, const BookFormat &b) { return a.format < b.format; });
    }

    data->books = std::move(books);
    data->authors = facetItems(authors, authorCounts);
    data->series = facetItems(series, seriesCounts);
    data->tags = facetItems(tags, tagCounts);
    data->publishers = facetItems(publishers, publisherCounts);
    data->languages = facetItems(languages, languageCounts);
    return true;
}

void findFiles(LibraryData *data)
{
    const QDir library(data->libraryPath);
    for (Book &book : data->books) {
        const QString folder = library.filePath(book.path);
        const QFileInfo cover(QDir(folder).filePath(QStringLiteral("cover.jpg")));
        book.coverFile = !book.path.isEmpty() && cover.isFile() ? cover.filePath() : QString();
        book.coverModified = book.coverFile.isEmpty()
            ? 0 : cover.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch();

        QStringList entries;
        bool listed = false;
        for (BookFormat &format : book.formats) {
            const QString extension = u'.' + format.format.toLower();
            const QString expected = QDir(folder).filePath(format.name + extension);
            if (!book.path.isEmpty() && QFileInfo(expected).isFile()) {
                format.file = expected;
                continue;
            }
            // format_abspath() renames a file found this way; Cousoir only
            // reads it.
            format.file.clear();
            if (book.path.isEmpty())
                continue;
            if (!listed) {
                entries = QDir(folder).entryList(QDir::Files | QDir::Hidden, QDir::Name);
                listed = true;
            }
            for (const QString &entry : std::as_const(entries)) {
                if (entry.endsWith(extension) && !entry.startsWith(QLatin1String("._"))) {
                    format.file = QDir(folder).filePath(entry);
                    break;
                }
            }
        }
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
        || !readLibrary(db, &loaded, error)) {
        return false;
    }
    db.close();
    findFiles(&loaded);
    *data = std::move(loaded);
    return true;
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
