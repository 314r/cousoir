// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/cache.py: multisort()
// - src/calibre/db/fields.py: each field's sort_keys_for_books(), its
//   default and sort keys, SeriesField's title sort in the book's language
// - src/calibre/db/view.py: add_to_sort_history()
// - src/calibre/library/field_metadata.py: the sortable fields
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_sort.h"

#include <QCollator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>

#include <algorithm>
#include <cmath>
#include <optional>

#include "book.h"
#include "calibre_config.h"
#include "calibre_db.h"
#include "calibre_template.h"
#include "calibre_text.h"
#include "python_json.h"

namespace BookSort {

namespace {

// A sort key as Python compares it: a number, a date as a count of time, a
// text's rank among the field's texts in the collator's order, or a tuple
// of ranks or numbers, compared item by item. Keys of different kinds,
// which Python would refuse to compare, order by kind.
struct Key
{
    enum class Kind : quint8 { Number, Date, Tuple };
    Kind kind = Kind::Number;
    double number = 0;
    qint64 date = 0;
    std::vector<double> tuple;

    static Key ofNumber(double value)
    {
        Key key;
        key.number = value;
        return key;
    }
    static Key ofDate(qint64 time)
    {
        Key key;
        key.kind = Kind::Date;
        key.date = time;
        return key;
    }
    static Key ofTuple(std::vector<double> values)
    {
        Key key;
        key.kind = Kind::Tuple;
        key.tuple = std::move(values);
        return key;
    }
};

int compare(const Key &a, const Key &b)
{
    if (a.kind != b.kind)
        return a.kind < b.kind ? -1 : 1;
    switch (a.kind) {
    case Key::Kind::Number:
        return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
    case Key::Kind::Date:
        return a.date < b.date ? -1 : a.date > b.date ? 1 : 0;
    case Key::Kind::Tuple: {
        const auto [first, second] = std::mismatch(a.tuple.cbegin(), a.tuple.cend(), b.tuple.cbegin(), b.tuple.cend());
        if (first == a.tuple.cend())
            return second == b.tuple.cend() ? 0 : -1;
        if (second == b.tuple.cend())
            return 1;
        return *first < *second ? -1 : 1;
    }
    }
    return 0;
}

using Keys = std::vector<Key>;

// A date to the second, as Calibre reads them from the database.
qint64 seconds(const QDateTime &date)
{
    return date.toSecsSinceEpoch();
}

QString text(const QVariant &value)
{
    return value.isNull() ? QString() : value.toString();
}

} // namespace

struct Sorter::Private
{
    Private(const LibraryData &library, const CalibreConfig &config) : data(library), config(config)
    {
        // sort_key(): Calibre's interface language, case aside.
        collator = QCollator(QLocale(config.interfaceLanguage));
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(config.numericCollation);
        const QString tristate = data.preferences.value(QStringLiteral("bools_are_tristate"));
        boolsAreTristate = tristate.isEmpty() || tristate.trimmed() == QLatin1String("true");
    }

    const LibraryData data;
    const CalibreConfig config;
    QCollator collator;
    bool boolsAreTristate = true;
    mutable QMutex lock;
    mutable QHash<QString, std::shared_ptr<const Keys>> keys;

    // The rank of each text in the collator's order, texts that collate
    // alike sharing theirs; by code point for `raw` ones, which Calibre
    // compares as Python strings.
    QHash<QString, double> ranks(QStringList texts, bool raw) const
    {
        texts.removeDuplicates();
        QHash<QString, double> result;
        result.reserve(texts.size());
        if (raw) {
            std::sort(texts.begin(), texts.end());
            for (qsizetype i = 0; i < texts.size(); ++i)
                result.insert(texts.at(i), double(i));
            return result;
        }
        std::vector<std::pair<QCollatorSortKey, QString>> sorted;
        sorted.reserve(texts.size());
        for (const QString &text : std::as_const(texts))
            sorted.emplace_back(collator.sortKey(text), text);
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.compare(b.first) < 0; });
        double rank = 0;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0 && sorted[i - 1].first.compare(sorted[i].first) != 0)
                ++rank;
            result.insert(sorted[i].second, rank);
        }
        return result;
    }

    // A text's sort_key(); none, b'', comes first.
    Keys textKeys(const std::function<QString(const Book &)> &value, bool raw = false) const
    {
        QStringList texts{ QString() };
        texts.reserve(data.books.size() + 1);
        for (const Book &book : data.books)
            texts.append(value(book));
        const QHash<QString, double> rank = ranks(texts, raw);
        Keys result;
        result.reserve(data.books.size());
        for (qsizetype i = 0; i < data.books.size(); ++i)
            result.push_back(Key::ofNumber(rank.value(texts.at(i + 1))));
        return result;
    }

    // A field of several values: the tuple of their keys, sorted unless
    // `inOrder`; (b'',) for none.
    Keys tupleKeys(const std::function<QStringList(const Book &)> &values, bool inOrder = false, bool raw = false) const
    {
        QList<QStringList> lists;
        lists.reserve(data.books.size());
        QStringList all{ QString() };
        for (const Book &book : data.books) {
            lists.append(values(book));
            all.append(lists.last());
        }
        const QHash<QString, double> rank = ranks(all, raw);
        Keys result;
        result.reserve(data.books.size());
        for (const QStringList &list : std::as_const(lists)) {
            std::vector<double> tuple;
            for (const QString &value : list)
                tuple.push_back(rank.value(value));
            if (tuple.empty())
                tuple.push_back(rank.value(QString()));
            else if (!inOrder)
                std::sort(tuple.begin(), tuple.end());
            result.push_back(Key::ofTuple(std::move(tuple)));
        }
        return result;
    }

    // numeric_sort_key(): an undefined number takes the tweak's value.
    Keys numberKeys(const std::function<std::optional<double>(const Book &)> &value) const
    {
        Keys result;
        result.reserve(data.books.size());
        for (const Book &book : data.books)
            result.push_back(Key::ofNumber(value(book).value_or(config.tweaks.valueForUndefinedNumbers)));
        return result;
    }

    Keys dateKeys(const std::function<qint64(const Book &)> &value) const
    {
        Keys result;
        result.reserve(data.books.size());
        for (const Book &book : data.books)
            result.push_back(Key::ofDate(value(book)));
        return result;
    }

    // SeriesField: the series' name sorted as a title in the book's
    // language, then its number, as multisort() pairs them; b'' for none.
    Keys seriesKeys(const std::function<QString(const Book &)> &series,
                    const std::function<double(const Book &)> &index) const
    {
        const Keys names = textKeys([&](const Book &book) {
            const QString name = series(book);
            return name.isEmpty() ? QString()
                                  : CalibreText::titleSort(name, book.languages.value(0), config.tweaks,
                                                           config.interfaceLanguage);
        });
        Keys result;
        result.reserve(data.books.size());
        for (qsizetype i = 0; i < data.books.size(); ++i)
            result.push_back(Key::ofTuple({ names.at(i).number, index(data.books.at(i)) }));
        return result;
    }

    // bool_sort_key(): yes, no, then none, or none with no.
    double boolKey(const QVariant &value) const
    {
        if (value.isNull())
            return boolsAreTristate ? 3 : 2;
        return value.toBool() ? 1 : 2;
    }

    std::optional<Keys> customKeys(const QString &field) const;
    std::optional<Keys> make(const QString &field) const;

    // The field's keys, made the first time; none for no field.
    std::shared_ptr<const Keys> keysOf(const QString &field) const
    {
        {
            const QMutexLocker locker(&lock);
            if (const auto found = keys.constFind(field); found != keys.cend())
                return *found;
        }
        std::optional<Keys> made = make(field);
        std::shared_ptr<const Keys> result = made ? std::make_shared<const Keys>(std::move(*made)) : nullptr;
        const QMutexLocker locker(&lock);
        keys.insert(field, result);
        return result;
    }
};

std::optional<Keys> Sorter::Private::customKeys(const QString &field) const
{
    const bool seriesIndex = field.endsWith(QLatin1String("_index"));
    const QString label = (seriesIndex ? field.chopped(6) : field).mid(1);
    const auto found = std::find_if(data.customColumns.cbegin(), data.customColumns.cend(),
                                    [&label](const CustomColumn &column) { return column.label == label; });
    if (found == data.customColumns.cend())
        return std::nullopt;
    const qsizetype index = found - data.customColumns.cbegin();
    const CustomColumn &column = *found;
    const QString &type = column.datatype;
    const auto value = [index](const Book &book) { return book.custom.value(index).value; };
    if (seriesIndex) {
        if (type != QLatin1String("series"))
            return std::nullopt;
        return numberKeys([index](const Book &book) -> std::optional<double> {
            const CustomValue value = book.custom.value(index);
            return value.value.isNull() ? std::nullopt : std::optional(value.seriesIndex);
        });
    }
    if (type == QLatin1String("series")) {
        return seriesKeys([value](const Book &book) { return text(value(book)); },
                          [index](const Book &book) { return book.custom.value(index).seriesIndex; });
    }
    if (type == QLatin1String("int") || type == QLatin1String("float") || type == QLatin1String("rating")) {
        return numberKeys([value](const Book &book) -> std::optional<double> {
            const QVariant number = value(book);
            return number.isNull() ? std::nullopt : std::optional(number.toDouble());
        });
    }
    if (type == QLatin1String("bool")) {
        Keys result;
        for (const Book &book : data.books)
            result.push_back(Key::ofNumber(boolKey(value(book))));
        return result;
    }
    if (type == QLatin1String("datetime")) {
        return dateKeys([value](const Book &book) {
            const QVariant date = value(book);
            return seconds(date.isNull() ? CalibreDb::undefinedDate() : date.toDateTime());
        });
    }
    if (type == QLatin1String("comments"))
        return textKeys([value](const Book &book) { return text(value(book)); }, true);
    const PythonJson::Value display = PythonJson::parse(column.display);
    if (type == QLatin1String("composite")) {
        const QString sort = display.text(QStringLiteral("composite_sort"));
        if (sort == QLatin1String("number")) {
            // number_sort_key(): a size's suffix counts.
            static const QString suffixes = QStringLiteral(" KMGTPE");
            return numberKeys([&](const Book &book) -> std::optional<double> {
                QString text = value(book).toString();
                double scale = 1;
                if (text.endsWith(u'B')) {
                    const qsizetype power = text.size() > 1 ? suffixes.indexOf(text.at(text.size() - 2)) : -1;
                    if (power > 0) {
                        scale = std::ldexp(1.0, 10 * int(power));
                        text.chop(2);
                    } else {
                        text.chop(1);
                    }
                }
                bool ok = false;
                const double number = text.trimmed().toDouble(&ok);
                if (!ok)
                    return config.tweaks.valueForUndefinedNumbers;
                return number * scale;
            });
        }
        if (sort == QLatin1String("date")) {
            return dateKeys([value](const Book &book) {
                const std::optional<QDateTime> date = CalibreTemplate::parseDate(value(book).toString());
                // parse_date() keeps the microseconds of a text that has
                // some.
                return date ? date->toMSecsSinceEpoch() * 1000 : seconds(CalibreDb::undefinedDate()) * 1000000;
            });
        }
        if (sort == QLatin1String("bool")) {
            Keys result;
            for (const Book &book : data.books) {
                // force_to_bool().
                const QString text = value(book).toString().toLower();
                QVariant flag;
                if (text == QLatin1String("yes") || text == QLatin1String("true") || text == QLatin1String("checked"))
                    flag = true;
                else if (text == QLatin1String("no") || text == QLatin1String("false") || text == QLatin1String("unchecked"))
                    flag = false;
                else if (bool ok = false; !text.isEmpty() && (text.toInt(&ok), ok))
                    flag = text.toInt() != 0;
                result.push_back(Key::ofNumber(boolKey(flag)));
            }
            return result;
        }
        if (column.multiple) {
            const QString splitter = CalibreDb::multipleSeparators(column).text(QStringLiteral("cache_to_list"));
            return tupleKeys([value, splitter](const Book &book) {
                QStringList items;
                for (const QString &item : value(book).toString().split(splitter))
                    items.append(item.trimmed());
                return items;
            });
        }
        return textKeys([value](const Book &book) { return value(book).toString(); });
    }
    if (column.multiple) {
        // A column of names sorts each as an author's sort, in order.
        const PythonJson::Value *names = display.find(QStringLiteral("is_names"));
        if (names && PythonJson::truthy(*names)) {
            return tupleKeys([value, this](const Book &book) {
                QStringList sorts;
                for (const QString &name : value(book).toStringList())
                    sorts.append(CalibreText::authorToAuthorSort(name, config.tweaks));
                return sorts;
            }, true);
        }
        return tupleKeys([value](const Book &book) { return value(book).toStringList(); });
    }
    return textKeys([value](const Book &book) { return text(value(book)); });
}

std::optional<Keys> Sorter::Private::make(const QString &field) const
{
    if (field.startsWith(u'#'))
        return customKeys(field);
    // multisort() sorts the title by its sort, the authors by theirs.
    if (field == QLatin1String("title") || field == QLatin1String("sort"))
        return textKeys([](const Book &book) { return book.sort; });
    if (field == QLatin1String("authors") || field == QLatin1String("author_sort"))
        return textKeys([](const Book &book) { return book.authorSort; });
    if (field == QLatin1String("series"))
        return seriesKeys([](const Book &book) { return book.series; }, [](const Book &book) { return book.seriesIndex; });
    if (field == QLatin1String("series_index"))
        return numberKeys([](const Book &book) { return std::optional(book.seriesIndex); });
    if (field == QLatin1String("publisher"))
        return textKeys([](const Book &book) { return book.publisher; });
    if (field == QLatin1String("rating")) {
        return numberKeys([](const Book &book) {
            return book.rating > 0 ? std::optional(double(book.rating)) : std::nullopt;
        });
    }
    if (field == QLatin1String("tags"))
        return tupleKeys([](const Book &book) { return book.tags; });
    if (field == QLatin1String("languages")) {
        return tupleKeys([](const Book &book) {
            QStringList names;
            for (const QString &code : book.languages)
                names.append(CalibreText::languageName(code));
            return names;
        });
    }
    if (field == QLatin1String("formats")) {
        // The formats table names no items: each format is b'', and books
        // sort by how many they have.
        return tupleKeys([](const Book &book) { return QStringList(book.formats.size(), QString()); });
    }
    if (field == QLatin1String("identifiers")) {
        // Their types, as Python strings.
        return tupleKeys([](const Book &book) {
            QStringList types;
            for (const auto &identifier : book.identifiers)
                types.append(identifier.first);
            return types;
        }, false, true);
    }
    if (field == QLatin1String("timestamp"))
        return dateKeys([](const Book &book) { return seconds(book.timestamp); });
    if (field == QLatin1String("pubdate"))
        return dateKeys([](const Book &book) { return seconds(book.pubdate); });
    if (field == QLatin1String("last_modified"))
        return dateKeys([](const Book &book) { return seconds(book.lastModified); });
    if (field == QLatin1String("size")) {
        // The largest of its files.
        return numberKeys([](const Book &book) -> std::optional<double> {
            if (book.formats.isEmpty())
                return std::nullopt;
            qint64 largest = 0;
            for (const BookFormat &format : book.formats)
                largest = std::max(largest, format.size);
            return double(largest);
        });
    }
    if (field == QLatin1String("pages"))
        return numberKeys([](const Book &book) { return book.pages >= 0 ? std::optional(double(book.pages)) : std::nullopt; });
    if (field == QLatin1String("id"))
        return numberKeys([](const Book &book) { return std::optional(double(book.id)); });
    if (field == QLatin1String("path"))
        return textKeys([](const Book &book) { return book.path; });
    if (field == QLatin1String("uuid"))
        return textKeys([](const Book &book) { return book.uuid; });
    // A text field, unlike a custom column of comments.
    if (field == QLatin1String("comments"))
        return textKeys([](const Book &book) { return book.comments; });
    return std::nullopt;
}

Sorter::Sorter(const LibraryData &data, const CalibreConfig &config) : d(std::make_unique<Private>(data, config)) {}

Sorter::~Sorter() = default;

void Sorter::prepare(const QStringList &fields) const
{
    for (const QString &field : fields)
        d->keysOf(field);
}

void Sorter::sort(const QList<Level> &levels, QList<qsizetype> *rows) const
{
    // Each field once, the first time it comes.
    QList<std::pair<std::shared_ptr<const Keys>, int>> keys;
    QStringList seen;
    for (const Level &level : levels) {
        if (seen.contains(level.field))
            continue;
        seen.append(level.field);
        if (std::shared_ptr<const Keys> fieldKeys = d->keysOf(level.field))
            keys.append({ fieldKeys, level.ascending ? 1 : -1 });
    }
    if (keys.isEmpty())
        return;
    std::stable_sort(rows->begin(), rows->end(), [&keys](qsizetype a, qsizetype b) {
        for (const auto &[fieldKeys, order] : keys) {
            const int result = compare(fieldKeys->at(a), fieldKeys->at(b));
            if (result != 0)
                return result * order < 0;
        }
        return false;
    });
}

QStringList fields(const LibraryData &data)
{
    QStringList result{ QStringLiteral("title"),      QStringLiteral("authors"),      QStringLiteral("series"),
                        QStringLiteral("series_index"), QStringLiteral("publisher"), QStringLiteral("rating"),
                        QStringLiteral("tags"),       QStringLiteral("languages"),    QStringLiteral("formats"),
                        QStringLiteral("identifiers"), QStringLiteral("timestamp"),  QStringLiteral("pubdate"),
                        QStringLiteral("last_modified"), QStringLiteral("size"),     QStringLiteral("pages"),
                        QStringLiteral("id"),         QStringLiteral("path"),         QStringLiteral("uuid"),
                        QStringLiteral("comments"),   QStringLiteral("sort"),         QStringLiteral("author_sort") };
    for (const CustomColumn &column : data.customColumns) {
        result.append(u'#' + column.label);
        if (column.datatype == QLatin1String("series"))
            result.append(u'#' + column.label + QStringLiteral("_index"));
    }
    return result;
}

bool sortable(const LibraryData &data, const QString &field)
{
    return fields(data).contains(field);
}

QList<Level> addToHistory(const QList<Level> &history, const Level &level, int maximum)
{
    QList<Level> result{ level };
    for (const Level &earlier : history) {
        if (result.size() >= maximum)
            break;
        if (std::none_of(result.cbegin(), result.cend(), [&earlier](const Level &kept) { return kept.field == earlier.field; }))
            result.append(earlier);
    }
    return result;
}

} // namespace BookSort
