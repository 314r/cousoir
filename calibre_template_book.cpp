// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/lazy.py: ProxyMetadata and its getters
// - src/calibre/ebooks/metadata/book/base.py: format_field_extended() and
//   format_series_index()
// - src/calibre/ebooks/metadata/book/formatter.py: SafeFormat.get_value()
// - src/calibre/ebooks/metadata/book/__init__.py: ALL_METADATA_FIELDS
// - src/calibre/utils/date.py: format_date() and its fd_* helpers
// - src/calibre/ebooks/metadata/__init__.py: fmt_sidx()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include <QCollator>
#include <QFileInfo>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <ctime>

#include <langinfo.h>

#include "calibre_db.h"
#include "calibre_template_p.h"
#include "calibre_text.h"
#include "field_writers.h"
#include "opf_reader.h"
#include "python_json.h"

namespace CalibreTemplate::Internal {

namespace {

[[noreturn]] void fail(const QString &message)
{
    throw TemplateError{ message };
}

// The standard fields of Metadata, ALL_METADATA_FIELDS but user_metadata.
const QSet<QString> &standardFields()
{
    static const QSet<QString> fields{
        QStringLiteral("tags"), QStringLiteral("rating"), QStringLiteral("comments"), QStringLiteral("series"),
        QStringLiteral("series_index"), QStringLiteral("identifiers"), QStringLiteral("title"),
        QStringLiteral("title_sort"), QStringLiteral("authors"), QStringLiteral("author_sort_map"),
        QStringLiteral("author_sort"), QStringLiteral("book_producer"), QStringLiteral("timestamp"),
        QStringLiteral("pubdate"), QStringLiteral("last_modified"), QStringLiteral("rights"),
        QStringLiteral("publication_type"), QStringLiteral("uuid"), QStringLiteral("languages"),
        QStringLiteral("publisher"), QStringLiteral("cover"), QStringLiteral("cover_data"), QStringLiteral("thumbnail"),
        QStringLiteral("toc"), QStringLiteral("spine"), QStringLiteral("guide"), QStringLiteral("manifest"),
        QStringLiteral("device_collections"), QStringLiteral("lpath"), QStringLiteral("size"), QStringLiteral("mime"),
        QStringLiteral("application_id"), QStringLiteral("db_id"), QStringLiteral("formats"),
        QStringLiteral("user_categories"), QStringLiteral("link_maps"), QStringLiteral("pages"),
    };
    return fields;
}

bool allMetadataField(const QString &key)
{
    return key == QLatin1String("user_metadata") || standardFields().contains(key);
}

// The book's formats whose files are there, as format_metadata has them.
QStringList presentFormats(const Book &book)
{
    QStringList formats;
    for (const BookFormat &format : book.formats) {
        if (!format.file.isEmpty())
            formats.append(format.format);
    }
    std::sort(formats.begin(), formats.end());
    return formats;
}

PyValue dateValue(const QDateTime &date)
{
    return PyValue::fromDate(date.toUTC(), 0);
}

// fmt_sidx() of ebooks/metadata/__init__.py: a series number as Calibre
// writes it.
QString seriesIndexText(double value)
{
    if (value == std::floor(value))
        return QString::number(qint64(value));
    QString text = QString::number(value, 'f', 2);
    while (text.endsWith(u'0'))
        text.chop(1);
    return text;
}

} // namespace

BookProxy::BookProxy(const Library &library, const Book &book) : library(library), book(book) {}

std::optional<PyValue> BookProxy::attribute(const QString &name) const
{
    const QString unknown = library.word(QStringLiteral("Unknown"));
    // The getters of db/lazy.py.
    if (name == QLatin1String("title"))
        return PyValue::from(book.title.isEmpty() ? unknown : book.title);
    if (name == QLatin1String("title_sort"))
        return PyValue::from(book.sort.isEmpty() ? unknown : book.sort);
    if (name == QLatin1String("authors"))
        return PyValue::from(book.authors.isEmpty() ? QStringList{ unknown } : book.authors);
    if (name == QLatin1String("author_sort"))
        return PyValue::from(book.authorSort.isEmpty() ? unknown : book.authorSort);
    if (name == QLatin1String("uuid"))
        return PyValue::from(book.uuid.isEmpty() ? QStringLiteral("dummy") : book.uuid);
    if (name == QLatin1String("book_size")) {
        // The largest file, none without one.
        if (book.formats.isEmpty())
            return PyValue();
        qint64 size = 0;
        for (const BookFormat &format : book.formats)
            size = std::max(size, format.size);
        return PyValue::from(size);
    }
    if (name == QLatin1String("pages"))
        return PyValue::from(std::max(0, book.pages));
    if (name == QLatin1String("ondevice_col"))
        return PyValue::from(QString(u""));
    if (name == QLatin1String("languages"))
        return PyValue::from(book.languages);
    if (name == QLatin1String("language"))
        return PyValue::from(book.languages.value(0, QStringLiteral("und")));
    if (name == QLatin1String("db_approx_formats")) {
        QStringList formats;
        for (const BookFormat &format : book.formats)
            formats.append(format.format);
        return PyValue::from(formats);
    }
    if (name == QLatin1String("has_cover"))
        return PyValue::from(book.hasCover ? library.word(QStringLiteral("Yes")) : QString(u""));
    if (name == QLatin1String("tags"))
        return PyValue::from(book.tags);
    if (name == QLatin1String("series_index"))
        return book.series.isEmpty() ? PyValue() : PyValue::from(book.seriesIndex);
    if (name == QLatin1String("application_id") || name == QLatin1String("id"))
        return PyValue::from(book.id);
    if (name == QLatin1String("comments"))
        return book.comments.isEmpty() ? PyValue() : PyValue::from(book.comments);
    if (name == QLatin1String("publisher"))
        return book.publisher.isEmpty() ? PyValue() : PyValue::from(book.publisher);
    if (name == QLatin1String("series"))
        return book.series.isEmpty() ? PyValue() : PyValue::from(book.series);
    if (name == QLatin1String("rating"))
        return book.rating > 0 ? PyValue::from(book.rating) : PyValue();
    if (name == QLatin1String("identifiers")) {
        PyValue value;
        value.kind = PyValue::Kind::Dict;
        value.dict = book.identifiers;
        return value;
    }
    if (name == QLatin1String("author_sort_map")) {
        PyValue value;
        value.kind = PyValue::Kind::Dict;
        for (qsizetype i = 0; i < book.authors.size(); ++i) {
            const int id = book.authorIds.value(i);
            QString sort;
            for (const FacetItem &author : library.data().authors) {
                if (author.id == id) {
                    sort = author.sort;
                    break;
                }
            }
            value.dict.append({ book.authors.at(i), sort });
        }
        return value;
    }
    if (name == QLatin1String("timestamp"))
        return dateValue(book.timestamp);
    if (name == QLatin1String("pubdate"))
        return dateValue(book.pubdate);
    if (name == QLatin1String("last_modified"))
        return dateValue(book.lastModified);
    if (name == QLatin1String("isbn")) {
        for (const auto &[type, value] : book.identifiers) {
            if (type == QLatin1String("isbn"))
                return PyValue::from(value);
        }
        return PyValue();
    }
    if (name == QLatin1String("formats")) {
        const QStringList formats = presentFormats(book);
        return formats.isEmpty() ? PyValue() : PyValue::from(formats);
    }
    if (name == QLatin1String("virtual_libraries"))
        return PyValue::from(QString(u""));
    // The other standard fields a proxy keeps nothing of.
    if (standardFields().contains(name)) {
        if (name == QLatin1String("device_collections"))
            return PyValue::from(QStringList());
        if (name == QLatin1String("user_categories") || name == QLatin1String("link_maps")) {
            PyValue value;
            value.kind = PyValue::Kind::Dict;
            return value;
        }
        return PyValue();
    }
    // A field of the library: its value from the database.
    if (const CustomColumn *column = library.customColumn(name)) {
        const CustomValue &value = book.custom.value(library.customIndex(name));
        const QString &type = column->datatype;
        if (type == QLatin1String("composite"))
            return PyValue::from(library.composite(book.id, name));
        if (value.value.isNull())
            return column->multiple ? PyValue::from(QStringList()) : PyValue();
        if (column->multiple)
            return PyValue::from(value.value.toStringList());
        if (type == QLatin1String("int") || type == QLatin1String("rating"))
            return PyValue::from(value.value.toLongLong());
        if (type == QLatin1String("float"))
            return PyValue::from(value.value.toDouble());
        if (type == QLatin1String("bool"))
            return PyValue::from(value.value.toBool());
        if (type == QLatin1String("datetime"))
            return dateValue(value.value.toDateTime());
        return PyValue::from(value.value.toString());
    }
    if (name.startsWith(u'#') && name.endsWith(QLatin1String("_index"))) {
        const QString series = name.chopped(6);
        if (const CustomColumn *column = library.customColumn(series); column && column->datatype == QLatin1String("series")) {
            const CustomValue &value = book.custom.value(library.customIndex(series));
            return value.value.isNull() ? PyValue() : PyValue::from(value.seriesIndex);
        }
    }
    if (name == QLatin1String("sort"))
        return PyValue::from(book.sort);
    if (name == QLatin1String("path"))
        return PyValue::from(book.path);
    if (name == QLatin1String("ondevice"))
        return PyValue::from(QString(u""));
    if (name == QLatin1String("series_sort"))
        return book.series.isEmpty() ? PyValue()
                                     : PyValue::from(CalibreText::titleSort(book.series, book.languages.value(0),
                                                                            CalibreText::Tweaks::defaults(),
                                                                            library.language()));
    if (name == QLatin1String("cover"))
        return PyValue::from(book.hasCover);
    if (library.hasField(name))
        return PyValue();
    return std::nullopt;
}

PyValue BookProxy::get(const QString &name) const
{
    return attribute(name).value_or(PyValue());
}

bool BookProxy::hasKey(const QString &key) const
{
    return allMetadataField(key) || library.hasField(key);
}

std::optional<QString> BookProxy::formatField(const QString &key, bool seriesWithIndex) const
{
    const auto sorted = [this](QStringList values) {
        values.removeAll(QString());
        std::stable_sort(values.begin(), values.end(),
                         [this](const QString &a, const QString &b) { return library.compare(a, b) < 0; });
        return values;
    };
    const auto rating = [](double value) { return Py::format(PyValue::from(value / 2), QStringLiteral(".2g")); };

    // A custom series' number.
    if (key.startsWith(u'#') && key.endsWith(QLatin1String("_index"))) {
        const QString series = key.chopped(6);
        if (const CustomColumn *column = library.customColumn(series); column && column->datatype == QLatin1String("series")) {
            const CustomValue &value = book.custom.value(library.customIndex(series));
            if (value.value.isNull() || value.value.toString().isEmpty())
                return QString(u"");
            return seriesIndexText(value.seriesIndex);
        }
    }
    if (const CustomColumn *column = library.customColumn(key)) {
        const PyValue value = get(key);
        if (value.isNone() || (value.kind == PyValue::Kind::Str && value.string.isEmpty()))
            return std::nullopt;
        const QString &type = column->datatype;
        const PythonJson::Value display = PythonJson::parse(column->display);
        if (type == QLatin1String("text") && column->multiple)
            return value.list.join(library.listToUi(key));
        if (type == QLatin1String("series") && seriesWithIndex) {
            const CustomValue &custom = book.custom.value(library.customIndex(key));
            return value.string + QStringLiteral(" [%1]").arg(seriesIndexText(custom.seriesIndex));
        }
        if (type == QLatin1String("datetime"))
            return formatDate(library, value.date, value.microsecond, display.text(QStringLiteral("date_format")));
        if (type == QLatin1String("bool"))
            return library.word(value.boolean ? QStringLiteral("Yes") : QStringLiteral("No"));
        if (type == QLatin1String("rating"))
            return rating(double(value.integer));
        if (type == QLatin1String("int") || type == QLatin1String("float")) {
            // The column's number format, Python's str.format(), when it reads.
            const QString format = display.text(QStringLiteral("number_format"));
            static const QRegularExpression single(QStringLiteral("^(.*)\\{0?:([^{}]*)\\}(.*)$"));
            const QRegularExpressionMatch match = single.match(format);
            if (match.hasMatch()) {
                try {
                    return match.captured(1) + Py::format(value, match.captured(2)) + match.captured(3);
                } catch (const TemplateError &) {
                }
            }
        }
        return value.str();
    }

    if (key == QLatin1String("isbn")) {
        const PyValue value = get(key);
        return value.isNone() ? std::nullopt : std::optional<QString>(value.string);
    }

    const QString fieldKey = library.fieldKey(key);
    // A built-in field of kind "field".
    bool builtIn = false;
    for (const char *field : { "authors", "languages", "series", "formats", "publisher", "rating", "tags", "identifiers",
                               "author_sort", "au_map", "comments", "cover", "id", "pages", "last_modified", "ondevice",
                               "path", "pubdate", "marked", "in_tag_browser", "series_index", "series_sort", "sort",
                               "size", "timestamp", "title", "uuid" }) {
        if (fieldKey == QLatin1String(field))
            builtIn = true;
    }
    if (builtIn) {
        const PyValue value = get(key);
        if (value.isNone() || (value.kind == PyValue::Kind::Str && value.string.isEmpty()))
            return std::nullopt;
        if (key == QLatin1String("authors"))
            return CalibreText::authorsToString(value.list);
        if (key == QLatin1String("series_index"))
            return seriesIndexText(value.kind == PyValue::Kind::Float ? value.real : double(value.integer));
        const QString separator = library.listToUi(fieldKey);
        if (!separator.isEmpty()) {
            QStringList values = value.list;
            if (value.kind == PyValue::Kind::Dict) {
                values.clear();
                for (const auto &[type, id] : value.dict)
                    values.append(type + u':' + id);
            }
            return sorted(values).join(separator);
        }
        if (fieldKey == QLatin1String("series") && seriesWithIndex)
            return value.string + QStringLiteral(" [%1]").arg(seriesIndexText(book.seriesIndex));
        if (fieldKey == QLatin1String("timestamp") || fieldKey == QLatin1String("pubdate")
            || fieldKey == QLatin1String("last_modified")) {
            const QString format = fieldKey == QLatin1String("pubdate") ? QStringLiteral("MMM yyyy")
                                                                        : QStringLiteral("dd MMM yyyy");
            return formatDate(library, value.date, value.microsecond, format);
        }
        if (fieldKey == QLatin1String("rating"))
            return rating(double(value.integer));
        if (key == QLatin1String("size"))
            return Py::humanReadable(double(value.kind == PyValue::Kind::Float ? value.real : value.integer));
        if (key == QLatin1String("pages")) {
            if (value.integer == -3)
                return library.word(QStringLiteral("DRM"));
            if (value.integer == -2)
                return library.word(QStringLiteral("Error"));
            if (value.integer == -1)
                return library.word(QStringLiteral("None"));
        }
        return value.str();
    }
    const PyValue value = get(key);
    if (value.truthy())
        return value.str();
    return std::nullopt;
}

QString BookProxy::value(const QString &rawKey) const
{
    // SafeFormat.get_value().
    QString key = rawKey.toLower();
    const QString original = key;
    if (key != QLatin1String("title_sort") && key != QLatin1String("isbn") && !allMetadataField(key)) {
        key = library.fieldKey(key);
        if (!hasKey(key)) {
            if (attribute(original))
                key = original;
            else
                fail(QStringLiteral("Value: unknown field ") + original);
        }
    }
    // A number column without a value is nothing.
    if (const CustomColumn *column = library.customColumn(key)) {
        if ((column->datatype == QLatin1String("int") || column->datatype == QLatin1String("float")) && get(key).isNone())
            return QString(u"");
    }
    return formatField(key, false).value_or(QString(u""));
}

QString formatDate(const Library &library, const QDateTime &utc, int microsecond, const QString &rawFormat)
{
    Q_UNUSED(microsecond);
    const QString format = rawFormat.isEmpty() ? QStringLiteral("dd MMM yyyy") : rawFormat;
    const QDateTime local = utc.toLocalTime();
    if (format == QLatin1String("iso")) {
        const int offset = local.offsetFromUtc();
        return local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
            + QStringLiteral("%1%2:%3").arg(offset < 0 ? u'-' : u'+')
                  .arg(std::abs(offset) / 3600, 2, 10, QLatin1Char('0'))
                  .arg(std::abs(offset) % 3600 / 60, 2, 10, QLatin1Char('0'));
    }
    if (CalibreDb::isUndefinedDate(utc))
        return QString(u"");
    const bool ampm = format.toLower().contains(QLatin1String("ap"));
    static const QRegularExpression codes(
        QStringLiteral("(s{1,2})|(m{1,2})|(h{1,2})|(ap)|(AP)|(aP)|(Ap)|(d{1,4}|M{1,4}|(?:yyyy|yy))"));
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = codes.globalMatch(format);
    const QDate date = local.date();
    const QTime time = local.time();
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += format.mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString code = match.captured();
        const qsizetype length = code.size();
        const auto two = [](int value) { return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0')); };
        switch (code.at(0).unicode()) {
        case 'd':
            result += length == 1 ? QString::number(date.day())
                : length == 2     ? two(date.day())
                                  : library.dayName(date.dayOfWeek() % 7, length == 3);
            break;
        case 'M':
            result += length == 1 ? QString::number(date.month())
                : length == 2     ? two(date.month())
                                  : library.monthName(date.month(), length == 3);
            break;
        case 'y':
            result += length == 2 ? two(date.year() % 100) : QStringLiteral("%1").arg(date.year(), 4, 10, QLatin1Char('0'));
            break;
        case 'h': {
            int hour = time.hour();
            if (ampm)
                hour = hour % 12 == 0 ? 12 : hour % 12;
            result += length == 1 ? QString::number(hour) : two(hour);
            break;
        }
        case 'm':
            result += length == 1 ? QString::number(time.minute()) : two(time.minute());
            break;
        case 's':
            result += length == 1 ? QString::number(time.second()) : two(time.second());
            break;
        default: {
            // strftime('%p'), in this computer's language: none in some.
            struct tm parts = {};
            parts.tm_hour = time.hour();
            char buffer[32] = {};
            std::strftime(buffer, sizeof buffer, "%p", &parts);
            const QString marker = QString::fromLocal8Bit(buffer);
            result += code == QLatin1String("aP") || code == QLatin1String("Ap") ? marker
                : code == QLatin1String("AP")                                    ? marker.toUpper()
                                                                                 : marker.toLower();
        }
        }
    }
    return result + format.mid(last);
}

std::optional<QDateTime> parseDate(const QString &text)
{
    // ISO forms first, as Calibre's iso_pat() sends them to dateutil
    // without dayfirst.
    if (const std::optional<QDateTime> custom = FieldWriters::parseCustomDate(text))
        return custom;
    // dateutil.parser.parse(), for the forms dates take: day, month and year
    // in some order, English month and day names, a time, AM or PM. What it
    // misses is filled from the 15th of this month at midnight.
    static const QStringList months{ QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                     QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                     QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                     QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec") };
    static const QStringList longMonths{ QStringLiteral("january"), QStringLiteral("february"), QStringLiteral("march"),
                                         QStringLiteral("april"), QStringLiteral("may"), QStringLiteral("june"),
                                         QStringLiteral("july"), QStringLiteral("august"), QStringLiteral("september"),
                                         QStringLiteral("october"), QStringLiteral("november"), QStringLiteral("december") };
    static const QStringList days{ QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
                                   QStringLiteral("thu"), QStringLiteral("fri"), QStringLiteral("sat"), QStringLiteral("sun"),
                                   QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"),
                                   QStringLiteral("thursday"), QStringLiteral("friday"), QStringLiteral("saturday"),
                                   QStringLiteral("sunday") };
    static const QRegularExpression tokens(QStringLiteral("\\d+(?::\\d+)+(?:\\.\\d+)?|\\d+|[^\\W\\d_]+|\\S"),
                                           QRegularExpression::UseUnicodePropertiesOption);
    int month = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int ampm = 0;
    bool utc = false;
    QList<QString> numbers;
    QRegularExpressionMatchIterator it = tokens.globalMatch(text);
    while (it.hasNext()) {
        const QString token = it.next().captured();
        if (token.contains(u':')) {
            const QStringList parts = token.split(u':');
            hour = parts.value(0).toInt();
            minute = parts.value(1).toInt();
            second = int(parts.value(2).toDouble());
        } else if (token.at(0).isDigit()) {
            numbers.append(token);
        } else if (token.at(0).isLetter()) {
            const QString word = token.toLower();
            if (months.contains(word) || longMonths.contains(word) || word == QLatin1String("sept"))
                month = word == QLatin1String("sept") ? 9 : int(std::max(months.indexOf(word), longMonths.indexOf(word))) + 1;
            else if (word == QLatin1String("am") || word == QLatin1String("a"))
                ampm = 1;
            else if (word == QLatin1String("pm") || word == QLatin1String("p"))
                ampm = 2;
            else if (word == QLatin1String("utc") || word == QLatin1String("gmt") || word == QLatin1String("z"))
                utc = true;
            else if (!days.contains(word) && word != QLatin1String("of") && word != QLatin1String("at")
                     && word != QLatin1String("on") && word != QLatin1String("t"))
                return std::nullopt;
        } else if (!QStringView(u",/-.").contains(token.at(0))) {
            return std::nullopt;
        }
    }
    if (numbers.isEmpty() && month == 0)
        return std::nullopt;
    // A year has four digits or is past 31; the rest are the day and month,
    // the day first when this computer writes dates so.
    int year = 0;
    QList<int> rest;
    for (const QString &number : std::as_const(numbers)) {
        const int value = number.toInt();
        if (year == 0 && (number.size() >= 3 || value > 31))
            year = value;
        else
            rest.append(value);
    }
    static const bool dayFirst = [] {
        const QString format = QString::fromLocal8Bit(nl_langinfo(D_FMT));
        const auto first = [&format](std::initializer_list<const char *> codes) {
            for (const char *code : codes) {
                const qsizetype at = format.indexOf(QLatin1String(code));
                if (at >= 0)
                    return at;
            }
            return qsizetype(-1);
        };
        return first({ "%d", "%a", "%A" }) < first({ "%m", "%b", "%B" });
    }();
    int day = 0;
    if (month) {
        if (!rest.isEmpty())
            day = rest.takeFirst();
        if (!rest.isEmpty() && year == 0)
            year = rest.takeFirst();
    } else if (rest.size() >= 2) {
        day = dayFirst ? rest.at(0) : rest.at(1);
        month = dayFirst ? rest.at(1) : rest.at(0);
        if (rest.size() >= 3 && year == 0)
            year = rest.at(2);
    } else if (rest.size() == 1) {
        if (year)
            month = rest.at(0);
        else
            day = rest.at(0);
    }
    if (rest.size() > 3)
        return std::nullopt;
    if (year > 0 && year < 100)
        year += year < (QDate::currentDate().year() % 100) + 50 ? 2000 : 1900;
    const QDate today = QDate::currentDate();
    const QDate date(year ? year : today.year(), month ? month : today.month(), day ? day : 15);
    if (!date.isValid())
        return std::nullopt;
    if (ampm == 2 && hour < 12)
        hour += 12;
    else if (ampm == 1 && hour == 12)
        hour = 0;
    const QTime time(hour, minute, second);
    if (!time.isValid())
        return std::nullopt;
    return QDateTime(date, time, utc ? QTimeZone(QTimeZone::UTC) : QTimeZone::systemTimeZone()).toUTC();
}

} // namespace CalibreTemplate::Internal
