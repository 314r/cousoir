// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/utils/formatter_functions.py: the builtin template functions
// - src/calibre/utils/titlecase.py, MIT licence, John Gruber, Stuart
//   Colville and Kovid Goyal: titlecase()
// - src/calibre/utils/icu.py: capitalize()
// - src/calibre/ebooks/metadata/__init__.py: rating_to_stars()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include <QCollator>
#include <QLocale>
#include <QTimeZone>
#include <QFileInfo>
#include <QDir>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "calibre_template_p.h"
#include "calibre_date.h"
#include "calibre_db.h"
#include "identifier_links.h"
#include "calibre_text.h"
#include "python_json.h"

namespace CalibreTemplate::Internal {

namespace {

[[noreturn]] void fail(const QString &message)
{
    throw TemplateError{ message };
}

// float(x if x and x != 'None' else 0)
double number(const QString &value)
{
    return Py::toFloat(value.isEmpty() || value == QLatin1String("None") ? QStringLiteral("0") : value);
}

qint64 integer(const QString &value)
{
    return Py::toInt(value.isEmpty() || value == QLatin1String("None") ? QStringLiteral("0") : value);
}

// A text of characters, as Python counts them.
QString characters(const QList<uint> &text)
{
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(text.constData()), text.size());
}

// [v.strip() for v in text.split(separator) if v.strip()]
QStringList items(const QString &text, const QString &separator)
{
    QStringList result;
    if (separator.isEmpty())
        fail(QStringLiteral("empty separator"));
    for (const QString &item : text.split(separator)) {
        const QString stripped = item.trimmed();
        if (!stripped.isEmpty())
            result.append(stripped);
    }
    return result;
}

QString joinFor(const QString &separator, const QStringList &values)
{
    return values.join(separator == QLatin1String(",") ? QStringLiteral(", ") : separator);
}

bool search(const QString &pattern, const QString &text)
{
    return Py::regex(pattern, true).match(text).hasMatch();
}

QString capitalize(const QString &text)
{
    if (text.isEmpty())
        return text;
    return text.left(1).toUpper() + text.mid(1).toLower();
}

// titlecase(): every word capitalized but the small ones, as Calibre does.
QString titlecase(const QString &text)
{
    static const QString small = QStringLiteral("a|an|and|as|at|but|by|en|for|if|in|of|on|or|the|to|v\\.?|via|vs\\.?");
    static const QString punct = QStringLiteral("!\"#$%&'‘’()*+,\\-‒–—―./:;?@[\\\\\\]_`{|}~");
    const auto re = [](const QString &pattern, bool ignoreCase = false) {
        QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
        if (ignoreCase)
            options |= QRegularExpression::CaseInsensitiveOption;
        return QRegularExpression(pattern, options);
    };
    static const QRegularExpression smallWords = re(QStringLiteral("^(%1)$").arg(small), true);
    static const QRegularExpression inlinePeriod = re(QStringLiteral("[a-z][.][a-z]"), true);
    static const QRegularExpression ucElsewhere = re(QStringLiteral("^[%1]*?[a-zA-Z]+[A-Z]+?").arg(punct));
    static const QRegularExpression capFirst = re(QStringLiteral("^[%1]*?(\\w)").arg(punct));
    static const QRegularExpression smallFirst = re(QStringLiteral("^([%1]*)(%2)\\b").arg(punct, small), true);
    static const QRegularExpression smallLast = re(QStringLiteral("\\b(%1)[%2]?$").arg(small, punct), true);
    static const QRegularExpression smallAfterNumber = re(QStringLiteral("(\\d+\\s+)(a|an|the)\\b"), true);
    static const QRegularExpression subphrase = re(QStringLiteral("([:.;?!][ ])(%1)").arg(small));
    static const QRegularExpression aposSecond = re(QStringLiteral("^[dol]{1}['‘]{1}[a-z]+$"), true);
    static const QRegularExpression ucInitials = re(QStringLiteral("^(?:[A-Z]{1}\\.{1}|[A-Z]{1}\\.{1}[A-Z]{1})+$"));
    static const QRegularExpression spaces = re(QStringLiteral("(\\s+)"));

    const bool allCaps = text.toUpper() == text;
    QStringList line;
    // pat.split(text) with the spaces kept.
    QStringList words;
    qsizetype last = 0;
    QRegularExpressionMatchIterator split = spaces.globalMatch(text);
    while (split.hasNext()) {
        const QRegularExpressionMatch match = split.next();
        words.append(text.mid(last, match.capturedStart() - last));
        words.append(match.captured());
        last = match.capturedEnd();
    }
    words.append(text.mid(last));
    for (QString word : std::as_const(words)) {
        if (word.isEmpty())
            continue;
        if (spaces.match(word).capturedStart() == 0) {
            line.append(word);
            continue;
        }
        if (allCaps) {
            if (ucInitials.match(word).hasMatch()) {
                line.append(word);
                continue;
            }
            word = word.toLower();
        }
        if (aposSecond.match(word).hasMatch()) {
            const qsizetype first = word.indexOf(word.at(0));
            word = word.left(first) + word.at(0).toUpper() + word.mid(first + 1);
            word = word.left(2) + word.mid(2, 1).toUpper() + word.mid(3);
            line.append(word);
            continue;
        }
        if (inlinePeriod.match(word).hasMatch() || ucElsewhere.match(word).hasMatch()) {
            line.append(word);
            continue;
        }
        if (smallWords.match(word).hasMatch()) {
            line.append(word.toLower());
            continue;
        }
        QStringList hyphenated;
        for (const QString &item : word.split(u'-')) {
            const QRegularExpressionMatch match = capFirst.match(item);
            hyphenated.append(match.hasMatch() ? match.captured().toUpper() + item.mid(match.capturedEnd()) : item);
        }
        line.append(hyphenated.join(u'-'));
    }
    QString result = line.join(QString());
    const auto substitute = [](const QString &input, const QRegularExpression &pattern,
                               const std::function<QString(const QRegularExpressionMatch &)> &replace) {
        QString output;
        qsizetype at = 0;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(input);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            output += input.mid(at, match.capturedStart() - at) + replace(match);
            at = match.capturedEnd();
        }
        return output + input.mid(at);
    };
    result = substitute(result, smallFirst, [](const auto &m) { return m.captured(1) + capitalize(m.captured(2)); });
    result = substitute(result, smallAfterNumber, [](const auto &m) { return m.captured(1) + capitalize(m.captured(2)); });
    result = substitute(result, smallLast, [](const auto &m) { return capitalize(m.captured(0)); });
    result = substitute(result, subphrase, [](const auto &m) { return m.captured(1) + capitalize(m.captured(2)); });
    return result;
}

// rating_to_stars() of ebooks/metadata/__init__.py.
QString ratingToStars(double value, bool halfStars)
{
    const int rating = std::clamp(int(value), 0, 10);
    QString stars = QString(rating / 2, QChar(0x2605));
    if (halfStars && rating % 2)
        stars += QChar(0x2BE8);
    return stars;
}

// re.sub() whose replacement is computed for each match.
QString substitute(const QRegularExpression &pattern, const QString &text,
                   const std::function<QString(const QRegularExpressionMatch &)> &replace)
{
    QString output;
    qsizetype at = 0;
    QRegularExpressionMatchIterator matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        output += text.mid(at, match.capturedStart() - at) + replace(match);
        at = match.capturedEnd();
    }
    return output + text.mid(at);
}

// The group each capturing group of a Python pattern is in, 0 for none, by
// group number from 1.
QList<int> groupParents(const QString &pattern)
{
    QList<int> parents{ 0 };
    QList<int> open;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == u'\\') {
            ++i;
        } else if (c == u'[') {
            // A class: a ']' first, or escaped, is in it.
            ++i;
            if (i < pattern.size() && pattern.at(i) == u'^')
                ++i;
            if (i < pattern.size() && pattern.at(i) == u']')
                ++i;
            while (i < pattern.size() && pattern.at(i) != u']') {
                if (pattern.at(i) == u'\\')
                    ++i;
                ++i;
            }
        } else if (c == u'(') {
            const QStringView rest = QStringView(pattern).mid(i + 1);
            const bool capturing = !rest.startsWith(u'?') || rest.startsWith(u"?P<")
                || (rest.startsWith(u"?<") && !rest.startsWith(u"?<=") && !rest.startsWith(u"?<!"));
            if (capturing) {
                int parent = 0;
                for (qsizetype k = open.size() - 1; k >= 0 && parent == 0; --k)
                    parent = open.at(k);
                parents.append(parent);
                open.append(int(parents.size() - 1));
            } else {
                open.append(0);
            }
        } else if (c == u')' && !open.isEmpty()) {
            open.removeLast();
        }
    }
    return parents;
}

// mo.lastindex: the group that closed last, the one ending last, of those
// ending there the outer one, else the later one.
int lastIndex(const QRegularExpressionMatch &match, const QList<int> &parents)
{
    const auto contains = [&parents](int outer, int inner) {
        for (int group = parents.value(inner); group > 0; group = parents.value(group)) {
            if (group == outer)
                return true;
        }
        return false;
    };
    int last = 0;
    for (int group = 1; group <= match.lastCapturedIndex(); ++group) {
        if (match.capturedStart(group) < 0)
            continue;
        // A later group ending there closes after, unless it is inside.
        if (last == 0 || match.capturedEnd(group) > match.capturedEnd(last)
            || (match.capturedEnd(group) == match.capturedEnd(last) && !contains(last, group)))
            last = group;
    }
    return last;
}

// The re_group() replacement: each group through its template, run by an
// EvalFormatter with $ the group, as it is.
QString groupTemplates(Formatter &formatter, const QRegularExpressionMatch &match, const QList<int> &parents,
                       const QStringList &templates)
{
    QString result;
    const int last = lastIndex(match, parents);
    for (int group = 1; group <= last; ++group) {
        if (match.capturedStart(group) < 0)
            continue;
        const QString value = match.captured(group);
        if (templates.size() > group - 1) {
            QString text = templates.at(group - 1);
            text.replace(QStringLiteral("[["), QStringLiteral("{")).replace(QStringLiteral("]]"), QStringLiteral("}"));
            result += formatter.evalFormat(text, { { QStringLiteral("$"), value } }, false);
        } else {
            result += value;
        }
    }
    return result;
}

// datetime.fromtimestamp(float(text)): the UTC time and its microseconds,
// rounded as Python rounds them.
std::pair<QDateTime, int> fromTimestamp(double seconds)
{
    if (!std::isfinite(seconds) || std::abs(seconds) > 253402300800.0)
        fail(QStringLiteral("year is out of range"));
    double whole = std::floor(seconds);
    int microsecond = int(std::nearbyint((seconds - whole) * 1e6));
    if (microsecond >= 1000000) {
        whole += 1;
        microsecond -= 1000000;
    }
    return { QDateTime::fromSecsSinceEpoch(qint64(whole), QTimeZone::UTC), microsecond };
}

std::pair<QDateTime, int> fromTimestamp(const QString &text)
{
    return fromTimestamp(Py::toFloat(text));
}

// qquote() of search_internet.py: quote_plus(), or quote() keeping '/'.
QString qquote(const QString &value, bool plus)
{
    if (plus)
        return QString::fromLatin1(QUrl::toPercentEncoding(value, " ")).replace(u' ', u'+');
    return QString::fromLatin1(QUrl::toPercentEncoding(value, "/"));
}

// prepare_string_for_xml(), but for the entities it decodes first.
QString xmlEscaped(const QString &text, bool attribute)
{
    QString result = text;
    result.replace(u'&', QStringLiteral("&amp;")).replace(u'<', QStringLiteral("&lt;")).replace(u'>', QStringLiteral("&gt;"));
    if (attribute)
        result.replace(u'"', QStringLiteral("&quot;")).replace(u'\'', QStringLiteral("&apos;"));
    return result;
}

// divmod() of ints.
std::pair<qint64, qint64> divmod(qint64 a, qint64 b)
{
    qint64 quotient = a / b;
    qint64 remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        --quotient;
        remainder += b;
    }
    return { quotient, remainder };
}

// format_duration(): the [w], [d], [h], [m] and [s] of `templateText`
// with the weeks, days, hours, minutes and seconds of `value`.
QString formatDuration(const QString &value, const QString &templateText, const QString &largestUnit)
{
    QString largest = largestUnit;
    if (!QStringLiteral("wdhms").contains(largest))
        fail(QStringLiteral("the largest_unit parameter must be one of wdhms"));
    static const QRegularExpression pattern(QStringLiteral("\\[(.)(:(.*?))?\\]"));
    if (largest.isEmpty()) {
        int highest = 0;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(templateText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const qsizetype index = QStringLiteral("smhdw").indexOf(match.captured(1).toLower());
            if (index < 0 || match.captured(1).toLower().size() != 1)
                fail(QStringLiteral("The %1 format specifier is not valid").arg(match.captured()));
            highest = std::max(highest, int(index));
        }
        largest = QStringLiteral("smhdw").at(highest);
    }
    const qint64 total = value.isEmpty() ? 0 : Py::toInt64(std::nearbyint(Py::toFloat(value)));
    qint64 remainder = total;
    const auto unit = [&remainder, &largest](const char *units, qint64 size) -> qint64 {
        if (!QString::fromLatin1(units).contains(largest))
            return -1;
        const auto [quotient, rest] = divmod(remainder, size);
        remainder = rest;
        return quotient;
    };
    const qint64 weeks = unit("w", 60 * 60 * 24 * 7);
    const qint64 days = unit("wd", 60 * 60 * 24);
    const qint64 hours = unit("wdh", 60 * 60);
    const qint64 minutes = unit("wdhm", 60);
    const qint64 seconds = remainder;
    return substitute(pattern, templateText, [&](const QRegularExpressionMatch &match) {
        const QString code = match.captured(1);
        QString zero;
        QString one;
        QString more;
        if (match.capturedStart(3) < 0) {
            zero = one = more = code.toLower() + u' ';
        } else {
            const QStringList suffixes = match.captured(3).split(u'|');
            if (suffixes.size() == 1) {
                zero = one = more = suffixes.at(0);
            } else if (suffixes.size() == 2) {
                zero = more = suffixes.at(0);
                one = suffixes.at(1);
            } else if (suffixes.size() == 3) {
                zero = suffixes.at(0);
                one = suffixes.at(1);
                more = suffixes.at(2);
            } else {
                fail(QStringLiteral("The group %1 has too many suffixes").arg(code));
            }
        }
        const bool lower = code.toLower() == code && code.toUpper() != code;
        const auto withSuffix = [&](qint64 amount, qint64 unitSize) {
            if (amount == -1)
                return QString(u"");
            if (amount == 0 && lower && total < unitSize)
                return QString(u"");
            return QString::number(amount) + (amount == 0 ? zero : amount == 1 ? one : more);
        };
        const QString kind = code.toLower();
        if (kind == QLatin1String("w"))
            return withSuffix(weeks, 60 * 60 * 24 * 7);
        if (kind == QLatin1String("d"))
            return withSuffix(days, 60 * 60 * 24);
        if (kind == QLatin1String("h"))
            return withSuffix(hours, 60 * 60);
        if (kind == QLatin1String("m"))
            return withSuffix(minutes, 60);
        if (kind == QLatin1String("s"))
            return withSuffix(seconds, -1);
        fail(QStringLiteral("The %1 format specifier is not valid").arg(code));
    });
}

// The query of make_url() and make_url_extended(): name=value pairs.
QString queryPairs(const QStringList &args, const char *name, int least)
{
    if (args.size() % 2 != 0)
        fail(QStringLiteral("%1 requires an odd number of arguments").arg(QLatin1String(name)));
    if (args.size() < 2)
        fail(QStringLiteral("%1 requires at least %2 arguments").arg(QLatin1String(name)).arg(least));
    QStringList pairs;
    for (qsizetype i = 0; i < args.size(); i += 2)
        pairs.append(args.at(i) + u'=' + qquote(args.at(i + 1).trimmed(), true));
    return pairs.join(u'&');
}

[[noreturn]] QString onlyInGui(const QString &name)
{
    fail(QStringLiteral("The function %1 can be used only in the GUI").arg(name));
}

QHash<QString, Function> makeFunctions()
{
    QHash<QString, Function> f;
    // A function of any number of arguments reads the first `minArgs` as
    // Calibre's reads args[0]…: fewer is Python's IndexError.
    const auto add = [&f](const char *name, int argCount, std::function<QString(FunctionContext &, const QStringList &)> fn,
                          qsizetype minArgs = 0) {
        if (minArgs > 0) {
            fn = [minArgs, fn = std::move(fn)](FunctionContext &context, const QStringList &args) {
                if (args.size() < minArgs)
                    fail(QStringLiteral("tuple index out of range"));
                return fn(context, args);
            };
        }
        f.insert(QString::fromLatin1(name), Function{ argCount, std::move(fn) });
    };
    using Args = const QStringList &;
    using Ctx = FunctionContext &;

    add("strcmp", 5, [](Ctx c, Args a) {
        const int order = c.formatter.compare(a[0], a[1]);
        return order < 0 ? a[2] : order == 0 ? a[3] : a[4];
    });
    add("strcmpcase", 5, [](Ctx c, Args a) {
        const int order = c.book.library.compareCase(a[0], a[1]);
        return order < 0 ? a[2] : order == 0 ? a[3] : a[4];
    });
    add("cmp", 5, [](Ctx, Args a) {
        const double x = number(a[0]);
        const double y = number(a[1]);
        return x < y ? a[2] : x == y ? a[3] : a[4];
    });
    add("first_matching_cmp", -1, [](Ctx, Args a) {
        if (a.size() % 2 != 0)
            fail(QStringLiteral("first_matching_cmp requires an even number of arguments"));
        const double value = number(a[0]);
        for (qsizetype i = 1; i + 1 < a.size(); i += 2) {
            if (value < number(a[i]))
                return a[i + 1];
        }
        return a.last();
    }, 1);
    add("strcat", -1, [](Ctx, Args a) { return a.join(QString()); });
    add("strlen", 1, [](Ctx, Args a) { return QString::number(a[0].toUcs4().size()); });
    add("add", -1, [](Ctx, Args a) {
        double result = 0;
        for (const QString &v : a)
            result += number(v);
        return PythonJson::floatRepr(result);
    });
    add("subtract", 2, [](Ctx, Args a) { return PythonJson::floatRepr(number(a[0]) - number(a[1])); });
    add("multiply", -1, [](Ctx, Args a) {
        double result = 1;
        for (const QString &v : a)
            result *= number(v);
        return PythonJson::floatRepr(result);
    });
    add("divide", 2, [](Ctx, Args a) {
        const double y = number(a[1]);
        if (y == 0)
            fail(QStringLiteral("float division by zero"));
        return PythonJson::floatRepr(number(a[0]) / y);
    });
    add("ceiling", 1, [](Ctx, Args a) { return Py::intText(std::ceil(number(a[0]))); });
    add("floor", 1, [](Ctx, Args a) { return Py::intText(std::floor(number(a[0]))); });
    add("round", 1, [](Ctx, Args a) {
        // round(): a tie to the even integer.
        const double value = number(a[0]);
        return Py::intText(std::nearbyint(value));
    });
    add("mod", 2, [](Ctx, Args a) {
        const double x = number(a[0]);
        const double y = number(a[1]);
        if (y == 0)
            fail(QStringLiteral("float modulo"));
        double r = std::fmod(x, y);
        if (r != 0 && ((r < 0) != (y < 0)))
            r += y;
        return Py::intText(r);
    });
    add("fractional_part", 1, [](Ctx, Args a) {
        double whole = 0;
        return PythonJson::floatRepr(std::modf(number(a[0]), &whole));
    });
    add("template", 1, [](Ctx c, Args a) {
        QString text = a[0];
        text.replace(QStringLiteral("[["), QStringLiteral("{")).replace(QStringLiteral("]]"), QStringLiteral("}"));
        Formatter inner(c.formatter.library);
        QString error;
        const QString value = inner.safeFormat(text, c.book, &error);
        return error.isNull() ? value : QStringLiteral("TEMPLATE ") + error;
    });
    add("eval", 1, [](Ctx c, Args a) {
        QString text = a[0];
        text.replace(QStringLiteral("[["), QStringLiteral("{")).replace(QStringLiteral("]]"), QStringLiteral("}"));
        return c.formatter.evalFormat(text, c.locals, true);
    });
    add("assign", 2, [](Ctx c, Args a) {
        c.locals.insert(a[0], a[1]);
        return a[1];
    });
    add("list_split", 3, [](Ctx c, Args a) {
        if (a[1].isEmpty())
            fail(QStringLiteral("empty separator"));
        QString result = QString(u"");
        const QStringList parts = a[0].split(a[1]);
        for (qsizetype i = 0; i < parts.size(); ++i) {
            result = parts.at(i).trimmed();
            c.locals.insert(a[2] + u'_' + QString::number(i), result);
        }
        return result;
    });
    add("field", 1, [](Ctx c, Args a) { return c.book.value(a[0]); });
    add("raw_field", -1, [](Ctx c, Args a) {
        const PyValue value = c.book.attribute(a[0]).value_or(PyValue());
        if (value.isNone() && a.size() > 1)
            return a[1];
        if (value.kind == PyValue::Kind::List) {
            const QString separator = c.book.library.listToUi(a[0]);
            return value.list.join(separator.isEmpty() ? QStringLiteral(", ") : separator);
        }
        return value.str();
    }, 1);
    add("raw_list", 2, [](Ctx c, Args a) {
        const PyValue value = c.book.attribute(a[0]).value_or(PyValue());
        if (value.kind != PyValue::Kind::List)
            return QStringLiteral("%1 is not a list").arg(a[0]);
        return value.list.join(a[1]);
    });
    add("substr", 3, [](Ctx, Args a) {
        // By characters, as Python counts them.
        const QList<uint> text = a[0].toUcs4();
        const qsizetype size = text.size();
        const auto clamp = [size](qint64 index) { return index < 0 ? std::max<qint64>(0, size + index) : std::min<qint64>(size, index); };
        const qint64 start = clamp(Py::toInt(a[1]));
        const qint64 end = Py::toInt(a[2]) == 0 ? size : clamp(Py::toInt(a[2]));
        return end > start ? characters(text.mid(start, end - start)) : QString(u"");
    });
    add("lookup", -1, [](Ctx c, Args all) {
        const QString &value = all[0];
        const QStringList args = all.mid(1);
        const auto field = [&c](const QString &name) { return c.formatter.vformat(u'{' + name.trimmed() + u'}'); };
        if (args.size() == 2)
            return field(value.isEmpty() ? args[1] : args[0]);
        if (args.size() % 2 != 1)
            fail(QStringLiteral("lookup requires either 2 or an odd number of arguments"));
        for (qsizetype i = 0; i < args.size(); i += 2) {
            if (i + 1 >= args.size())
                return field(args[i]);
            if (search(args[i], value))
                return field(args[i + 1]);
        }
        return QString(u"");
    }, 1);
    add("test", 3, [](Ctx, Args a) { return a[0].isEmpty() ? a[2] : a[1]; });
    add("contains", 4, [](Ctx, Args a) { return search(a[1], a[0]) ? a[2] : a[3]; });
    add("switch", -1, [](Ctx, Args all) {
        const QStringList args = all.mid(1);
        if (args.size() % 2 != 1)
            fail(QStringLiteral("switch requires an even number of arguments"));
        for (qsizetype i = 0; i < args.size(); i += 2) {
            if (i + 1 >= args.size())
                return args[i];
            if (search(args[i], all[0]))
                return args[i + 1];
        }
        return QString(u"");
    }, 1);
    add("switch_if", -1, [](Ctx, Args a) {
        if (a.size() % 2 != 1)
            fail(QStringLiteral("switch_if requires an odd number of arguments"));
        for (qsizetype i = 0; i < a.size(); i += 2) {
            if (i + 1 >= a.size())
                return a[i];
            if (!a[i].isEmpty())
                return a[i + 1];
        }
        return QString(u"");
    });
    add("strcat_max", -1, [](Ctx, Args a) {
        if (a.size() < 2)
            fail(QStringLiteral("strcat_max requires 2 or more arguments"));
        if (a.size() % 2 != 0)
            fail(QStringLiteral("strcat_max requires an even number of arguments"));
        qint64 max = 0;
        try {
            max = Py::toInt(a[0]);
        } catch (const TemplateError &) {
            fail(QStringLiteral("first argument to strcat_max must be an integer"));
        }
        QString result = a[1];
        for (qsizetype i = 2; i + 1 < a.size(); i += 2) {
            if (result.toUcs4().size() + a[i].toUcs4().size() + a[i + 1].toUcs4().size() > max)
                break;
            result += a[i] + a[i + 1];
        }
        return result.trimmed();
    });
    const auto inList = [](Ctx, Args all) {
        const QStringList list = items(all[0], all[1]);
        const QStringList args = all.mid(2);
        if (args.size() % 2 != 1)
            fail(QStringLiteral("in_list requires an odd number of arguments"));
        for (qsizetype i = 0; i < args.size(); i += 2) {
            if (i + 1 >= args.size())
                return args[i];
            for (const QString &v : list) {
                if (search(args[i], v))
                    return args[i + 1];
            }
        }
        return QString(u"");
    };
    add("list_contains", -1, inList, 2);
    add("in_list", -1, inList, 2);
    add("str_in_list", -1, [](Ctx c, Args all) {
        const QStringList list = items(all[0], all[1]);
        const QStringList args = all.mid(2);
        if (args.size() % 2 != 1)
            fail(QStringLiteral("str_in_list requires an odd number of arguments"));
        for (qsizetype i = 0; i < args.size(); i += 2) {
            if (i + 1 >= args.size())
                return args[i];
            for (const QString &v : list) {
                for (const QString &t : items(args[i], all[1])) {
                    if (c.formatter.compare(t, v) == 0)
                        return args[i + 1];
                }
            }
        }
        return QString(u"");
    }, 2);
    add("identifier_in_list", -1, [](Ctx, Args all) {
        const QStringList args = all.mid(2);
        bool returnId = false;
        QString found;
        QString notFound;
        if (args.isEmpty()) {
            returnId = true;
            notFound = QString(u"");
        } else if (args.size() == 2) {
            found = args[0];
            notFound = args[1];
        } else {
            fail(QStringLiteral("identifier_in_list requires 2 or 4 arguments"));
        }
        const QString id = all[1].section(u':', 0, 0);
        const QString pattern = all[1].contains(u':') ? all[1].section(u':', 1) : QString();
        if (id.isEmpty())
            return notFound;
        for (const QString &candidate : items(all[0], QStringLiteral(","))) {
            const QString type = candidate.section(u':', 0, 0);
            const QString value = candidate.contains(u':') ? candidate.section(u':', 1) : QString();
            if (!value.isEmpty() && type == id && (pattern.isEmpty() || search(pattern, value)))
                return returnId ? candidate : found;
        }
        return notFound;
    }, 2);
    add("re", 3, [](Ctx, Args a) { return Py::sub(Py::regex(a[1], true), a[2], a[0]); });
    add("re_group", -1, [](Ctx c, Args all) {
        const QStringList templates = all.mid(2);
        const QList<int> parents = groupParents(all[1]);
        return substitute(Py::regex(all[1], true), all[0], [&](const QRegularExpressionMatch &m) {
            return groupTemplates(c.formatter, m, parents, templates);
        });
    }, 2);
    add("swap_around_comma", 1, [](Ctx, Args a) {
        static const QRegularExpression pattern(QStringLiteral("^(.*?),\\s*(.*$)"),
                                                QRegularExpression::CaseInsensitiveOption
                                                    | QRegularExpression::UseUnicodePropertiesOption);
        return Py::sub(pattern, QStringLiteral("\\2 \\1"), a[0]).trimmed();
    });
    add("ifempty", 2, [](Ctx, Args a) { return a[0].isEmpty() ? a[1] : a[0]; });
    add("shorten", 4, [](Ctx, Args a) {
        const qint64 leading = std::max<qint64>(0, Py::toInt(a[1]));
        const qint64 trailing = std::max<qint64>(0, Py::toInt(a[3]));
        const QList<uint> text = a[0].toUcs4();
        if (text.size() > leading + a[2].toUcs4().size() + trailing)
            return characters(text.mid(0, leading)) + a[2]
                + (trailing == 0 ? QString() : characters(text.mid(std::max<qint64>(0, text.size() - trailing))));
        return a[0];
    });
    const auto listCount = [](Ctx, Args a) {
        if (a[1].isEmpty())
            fail(QStringLiteral("empty separator"));
        qsizetype count = 0;
        for (const QString &v : a[0].split(a[1])) {
            if (!v.isEmpty())
                ++count;
        }
        return QString::number(count);
    };
    add("list_count", 2, listCount);
    add("count", 2, listCount);
    const auto countMatching = [](Ctx, Args a) {
        qsizetype count = 0;
        for (const QString &v : items(a[0], a[2])) {
            if (search(a[1], v))
                ++count;
        }
        return QString::number(count);
    };
    add("list_count_matching", 3, countMatching);
    add("count_matching", 3, countMatching);
    add("list_item", 3, [](Ctx, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        const qint64 index = Py::toInt(a[1]);
        if (a[2].isEmpty())
            fail(QStringLiteral("empty separator"));
        const QStringList values = a[0].split(a[2]);
        const qint64 at = index < 0 ? values.size() + index : index;
        return at >= 0 && at < values.size() ? values.at(at).trimmed() : QString(u"");
    });
    add("select", 2, [](Ctx, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        const QString key = a[1] + u':';
        for (const QString &v : a[0].split(u',')) {
            const QString value = v.trimmed();
            if (value.startsWith(key))
                return value.mid(key.size());
        }
        return QString(u"");
    });
    add("approximate_formats", 0, [](Ctx c, Args) {
        QStringList formats = c.book.get(QStringLiteral("db_approx_formats")).list;
        if (formats.isEmpty())
            return QString(u"");
        for (QString &format : formats)
            format = format.toUpper();
        std::sort(formats.begin(), formats.end());
        return formats.join(u',');
    });
    add("formats_sizes", 0, [](Ctx c, Args) {
        QStringList sizes;
        for (const BookFormat &format : c.book.book.formats) {
            if (!format.file.isEmpty())
                sizes.append(format.format.toUpper() + u':' + QString::number(format.size));
        }
        return sizes.join(u',');
    });
    add("formats_paths", -1, [](Ctx c, Args a) {
        const QString separator = a.isEmpty() ? QStringLiteral(",") : a[0];
        QStringList paths;
        for (const BookFormat &format : c.book.book.formats) {
            if (!format.file.isEmpty())
                paths.append(format.format.toUpper() + u':' + format.file);
        }
        return paths.join(separator);
    });
    add("formats_modtimes", 1, [](Ctx c, Args a) {
        // Each format's file's time, the latest first.
        // A file missing is Calibre's KeyError, caught: nothing.
        QList<std::pair<CalibreDate, QString>> times;
        for (const BookFormat &format : c.book.book.formats) {
            if (format.file.isEmpty())
                return QString(u"");
            std::error_code error;
            const auto time = std::filesystem::last_write_time(format.file.toStdString(), error);
            if (error)
                return QString(u"");
            // st_mtime: the seconds plus the nanoseconds, in floats.
            const qint64 since = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::file_clock::to_sys(time).time_since_epoch())
                                     .count();
            const qint64 seconds = since / 1000000000 - (since % 1000000000 < 0);
            const qint64 nanoseconds = since - seconds * 1000000000;
            const auto [date, microsecond] = fromTimestamp(double(seconds) + double(nanoseconds) * 1e-9);
            times.append({ CalibreDate{ date, microsecond }, format.format.toUpper() });
        }
        std::stable_sort(times.begin(), times.end(), [](const auto &x, const auto &y) { return y.first < x.first; });
        QStringList result;
        for (const auto &[date, format] : std::as_const(times))
            result.append(format + u':' + formatDate(c.book.library, date.dateTime, date.microsecond, a[0]));
        return result.join(u',');
    });
    add("formats_path_segments", 5, [](Ctx c, Args a) {
        for (const BookFormat &format : c.book.book.formats) {
            if (format.file.isEmpty())
                continue;
            const QFileInfo file(format.file);
            const QString name = a[3] == QLatin1String("0") || a[3].isEmpty() ? file.completeBaseName() : file.fileName();
            const QDir titleFolder = file.dir();
            const QString title = titleFolder.dirName();
            const QString author = QFileInfo(titleFolder.absolutePath()).dir().dirName();
            QStringList parts;
            if (a[0] == QLatin1String("1"))
                parts.append(author);
            if (a[1] == QLatin1String("1"))
                parts.append(title);
            if (a[2] == QLatin1String("1"))
                parts.append(name);
            return parts.join(a[4]);
        }
        return c.book.library.word(QStringLiteral("No book formats found so the path can't be generated"));
    });
    add("human_readable", 1, [](Ctx, Args a) {
        try {
            return Py::humanReadable(double(std::nearbyint(Py::toFloat(a[0]))));
        } catch (const TemplateError &) {
            return QString(u"");
        }
    });
    add("format_number", 2, [](Ctx, Args a) {
        if (a[0].isEmpty() || a[0] == QLatin1String("None"))
            return QString(u"");
        QString templateText = a[1];
        if (!templateText.contains(u'{'))
            templateText = QStringLiteral("{0:") + templateText + u'}';
        double value = 0;
        try {
            value = Py::toFloat(a[0]);
        } catch (const TemplateError &) {
            return QString(u"");
        }
        // str.format() of one field, "{0:spec}" or "{:spec}", with text
        // around it.
        static const QRegularExpression field(QStringLiteral("^(.*?)\\{0?(?::([^{}]*))?\\}(.*)$"),
                                              QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = field.match(templateText);
        if (!match.hasMatch())
            return QString(u"");
        try {
            return match.captured(1) + Py::format(PyValue::from(value), match.captured(2)) + match.captured(3);
        } catch (const TemplateError &) {
        }
        try {
            if (std::trunc(value) == value && std::abs(value) < 9.2e18)
                return match.captured(1) + Py::format(PyValue::from(qint64(value)), match.captured(2)) + match.captured(3);
        } catch (const TemplateError &) {
        }
        return QString(u"");
    });
    add("sublist", 4, [](Ctx, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        const qint64 start = Py::toInt(a[1]);
        const qint64 end = Py::toInt(a[2]);
        if (a[3].isEmpty())
            fail(QStringLiteral("empty separator"));
        QStringList values;
        for (const QString &v : a[0].split(a[3]))
            values.append(v.trimmed());
        const qint64 size = values.size();
        const auto clamp = [size](qint64 index) { return index < 0 ? std::max<qint64>(0, size + index) : std::min(size, index); };
        const qint64 from = clamp(start);
        const qint64 to = end == 0 ? size : clamp(end);
        return joinFor(a[3], to > from ? values.mid(from, to - from) : QStringList());
    });
    add("subitems", 3, [](Ctx c, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        const qint64 start = Py::toInt(a[1]);
        const qint64 end = Py::toInt(a[2]);
        const bool periods = a[0].contains(u'.');
        static const QRegularExpression period(QStringLiteral("(?<=[^\\.\\s])\\.(?=[^\\.\\s])"),
                                               QRegularExpression::UseUnicodePropertiesOption);
        QStringList found;
        for (const QString &item : items(a[0], QStringLiteral(","))) {
            const QStringList components = periods && item.contains(u'.') ? item.split(period) : QStringList{ item };
            const qint64 size = components.size();
            const auto clamp = [size](qint64 index) { return index < 0 ? std::max<qint64>(0, size + index) : std::min(size, index); };
            const qint64 from = clamp(start);
            const qint64 to = end == 0 ? size : clamp(end);
            const QString value = (to > from ? components.mid(from, to - from) : QStringList()).join(u'.').trimmed();
            if (!value.isEmpty() && !found.contains(value))
                found.append(value);
        }
        std::sort(found.begin(), found.end(), [&c](const QString &x, const QString &y) { return c.formatter.compare(x, y) < 0; });
        return found.join(QStringLiteral(", "));
    });
    add("format_date", 2, [](Ctx c, Args a) {
        if (a[0].isEmpty() || a[0] == QLatin1String("None"))
            return QString(u"");
        if (a[1].startsWith(QLatin1String("from_number"))) {
            // A local time from a number of seconds.
            try {
                const auto [date, microsecond] = fromTimestamp(a[0]);
                const QString format = a[1].mid(12);
                return formatDate(c.book.library, date, microsecond, format.isEmpty() ? QStringLiteral("iso") : format);
            } catch (const TemplateError &) {
                return QStringLiteral("BAD DATE");
            }
        }
        const std::optional<QDateTime> date = parseDate(a[0]);
        if (!date)
            return QStringLiteral("BAD DATE");
        if (a[1] == QLatin1String("to_number"))
            return PythonJson::floatRepr(double(date->toMSecsSinceEpoch()) / 1000);
        return formatDate(c.book.library, *date, 0, a[1]);
    });
    add("format_date_field", 2, [](Ctx c, Args a) {
        const QString field = c.book.library.fieldKey(a[0]);
        if (!c.book.hasKey(field))
            fail(QStringLiteral("Function format_date_field: Unknown field '%1'").arg(field));
        const PyValue value = c.book.get(field);
        const CustomColumn *column = c.book.library.customColumn(field);
        const bool date = column ? column->datatype == QLatin1String("datetime")
                                 : (field == QLatin1String("timestamp") || field == QLatin1String("pubdate")
                                    || field == QLatin1String("last_modified"));
        if (!date)
            fail(QStringLiteral("Function format_date_field: field '%1' is not a date").arg(field));
        if (value.isNone())
            return QString(u"");
        if (a[1] == QLatin1String("to_number"))
            return PythonJson::floatRepr(double(value.date.toMSecsSinceEpoch()) / 1000);
        // float() of the datetime.
        if (a[1].startsWith(QLatin1String("from_number")))
            fail(QStringLiteral("float() argument must be a string or a real number, not 'datetime.datetime'"));
        return formatDate(c.book.library, value.date, value.microsecond, a[1]);
    });
    add("uppercase", 1, [](Ctx, Args a) { return a[0].toUpper(); });
    add("lowercase", 1, [](Ctx, Args a) { return a[0].toLower(); });
    add("titlecase", 1, [](Ctx, Args a) { return titlecase(a[0]); });
    add("capitalize", 1, [](Ctx, Args a) { return capitalize(a[0]); });
    add("booksize", 0, [](Ctx c, Args) {
        const PyValue size = c.book.get(QStringLiteral("book_size"));
        return size.isNone() ? QString(u"") : size.str();
    });
    add("ondevice", 0, [](Ctx, Args) { return QString(u""); });
    add("annotation_count", 0, [](Ctx, Args) { return onlyInGui(QStringLiteral("annotation_count")); });
    add("is_marked", 0, [](Ctx, Args) { return onlyInGui(QStringLiteral("is_marked")); });
    add("series_sort", 0, [](Ctx c, Args) {
        const PyValue series = c.book.get(QStringLiteral("series"));
        if (!series.truthy())
            return QString(u"");
        return CalibreText::titleSort(series.string, c.book.book.languages.value(0), CalibreText::Tweaks::defaults(),
                                      c.book.library.language());
    });
    add("has_cover", 0, [](Ctx c, Args) {
        return c.book.book.hasCover ? c.book.library.word(QStringLiteral("Yes")) : QString(u"");
    });
    add("first_non_empty", -1, [](Ctx, Args a) {
        for (const QString &v : a) {
            if (!v.isEmpty())
                return v;
        }
        return QString(u"");
    });
    add("and", -1, [](Ctx, Args a) {
        for (const QString &v : a) {
            if (v.isEmpty())
                return QString(u"");
        }
        return QStringLiteral("1");
    });
    add("or", -1, [](Ctx, Args a) {
        for (const QString &v : a) {
            if (!v.isEmpty())
                return QStringLiteral("1");
        }
        return QString(u"");
    });
    add("not", 1, [](Ctx, Args a) { return a[0].isEmpty() ? QStringLiteral("1") : QString(u""); });
    add("list_join", -1, [](Ctx, Args all) {
        const QStringList args = all.mid(1);
        if (args.size() % 2 != 0)
            fail(QStringLiteral("Invalid 'List, separator' pairs. Every list must have one associated separator"));
        QStringList keys;
        QHash<QString, QString> values;
        for (qsizetype i = 0; i < args.size(); i += 2) {
            for (const QString &item : items(args[i], args[i + 1])) {
                const QString key = item.toLower();
                if (!values.contains(key))
                    keys.append(key);
                values.insert(key, item);
            }
        }
        QStringList result;
        for (const QString &key : std::as_const(keys))
            result.append(values.value(key));
        return result.join(all[0]);
    }, 1);
    // A dict by lower case key: the first place, the last value.
    const auto merge = [](QStringList *keys, QHash<QString, QString> *values, const QStringList &list) {
        for (const QString &item : list) {
            const QString key = item.toLower();
            if (!values->contains(key))
                keys->append(key);
            values->insert(key, item);
        }
    };
    const auto listUnion = [merge](Ctx, Args a) {
        QStringList keys;
        QHash<QString, QString> values;
        merge(&keys, &values, items(a[1], a[2]));
        merge(&keys, &values, items(a[0], a[2]));
        QStringList result;
        for (const QString &key : std::as_const(keys))
            result.append(values.value(key));
        return joinFor(a[2], result);
    };
    add("list_union", 3, listUnion);
    add("merge_lists", 3, listUnion);
    add("range", -1, [](Ctx, Args a) {
        qint64 start = 0;
        qint64 stop = 0;
        qint64 step = 1;
        qint64 limit = 1000;
        if (a.size() == 1) {
            stop = integer(a[0]);
        } else if (a.size() >= 2) {
            start = integer(a[0]);
            stop = integer(a[1]);
            if (a.size() >= 3)
                step = integer(a[2]);
            if (a.size() > 3)
                limit = integer(a[3]);
        }
        if (step == 0)
            fail(QStringLiteral("range() arg 3 must not be zero"));
        const quint64 length = Py::rangeLength(start, stop, step);
        if (limit < 0 || length > quint64(limit))
            fail(QStringLiteral("range: length (%1) longer than limit (%2)").arg(length).arg(limit));
        QStringList values;
        for (quint64 i = 0; i < length; ++i)
            values.append(QString::number(qint64(quint64(start) + i * quint64(step))));
        return values.join(QStringLiteral(", "));
    }, 1);
    add("list_remove_duplicates", 2, [merge](Ctx, Args a) {
        QStringList keys;
        QHash<QString, QString> values;
        merge(&keys, &values, items(a[0], a[1]));
        QStringList result;
        for (const QString &key : std::as_const(keys))
            result.append(values.value(key));
        return joinFor(a[1], result);
    });
    add("list_difference", 3, [](Ctx, Args a) {
        QSet<QString> other;
        for (const QString &item : items(a[1], a[2]))
            other.insert(item.toLower());
        QStringList result;
        for (const QString &item : items(a[0], a[2])) {
            if (!other.contains(item.toLower()) && !result.contains(item))
                result.append(item);
        }
        return joinFor(a[2], result);
    });
    add("list_intersection", 3, [](Ctx, Args a) {
        QSet<QString> other;
        for (const QString &item : items(a[1], a[2]))
            other.insert(item.toLower());
        QStringList result;
        for (const QString &item : items(a[0], a[2])) {
            if (other.contains(item.toLower()) && !result.contains(item))
                result.append(item);
        }
        return joinFor(a[2], result);
    });
    add("list_sort", 3, [](Ctx c, Args a) {
        QStringList values = items(a[0], a[2]);
        const bool reverse = a[1] != QLatin1String("0");
        std::stable_sort(values.begin(), values.end(), [&](const QString &x, const QString &y) {
            return reverse ? c.formatter.compare(y, x) < 0 : c.formatter.compare(x, y) < 0;
        });
        return joinFor(a[2], values);
    });
    add("list_equals", 6, [](Ctx, Args a) {
        QSet<QString> first;
        QSet<QString> second;
        for (const QString &item : items(a[0], a[1]))
            first.insert(item.toLower());
        for (const QString &item : items(a[2], a[3]))
            second.insert(item.toLower());
        return first == second ? a[4] : a[5];
    });
    add("list_re", 4, [](Ctx, Args a) {
        QStringList result;
        const QRegularExpression include = Py::regex(a[2], true);
        for (QString item : items(a[0], a[1])) {
            if (!include.match(item).hasMatch())
                continue;
            if (!a[3].isEmpty())
                item = Py::sub(Py::regex(a[2], false), a[3], item);
            for (const QString &i : items(item, a[1])) {
                if (!result.contains(i))
                    result.append(i);
            }
        }
        return joinFor(a[1], result);
    });
    add("list_re_group", -1, [](Ctx c, Args all) {
        const QStringList templates = all.mid(4);
        QStringList result;
        const QRegularExpression include = Py::regex(all[2], true);
        const QList<int> parents = groupParents(all[3]);
        for (QString item : items(all[0], all[1])) {
            if (!include.match(item).hasMatch())
                continue;
            item = substitute(Py::regex(all[3], true), item,
                              [&](const QRegularExpressionMatch &m) {
                                  return groupTemplates(c.formatter, m, parents, templates);
                              });
            for (const QString &i : items(item, all[1])) {
                if (!result.contains(i))
                    result.append(i);
            }
        }
        return joinFor(all[1], result);
    }, 4);
    add("today", 0, [](Ctx c, Args) {
        return formatDate(c.book.library, QDateTime::currentDateTimeUtc(), 0, QStringLiteral("iso"));
    });
    add("days_between", 2, [](Ctx, Args a) {
        const std::optional<QDateTime> first = parseDate(a[0]);
        const std::optional<QDateTime> second = parseDate(a[1]);
        if (!first || !second || CalibreDb::isUndefinedDate(*first) || CalibreDb::isUndefinedDate(*second))
            return QString(u"");
        const double days = double(second->msecsTo(*first)) / 86400000.0;
        return Py::format(PyValue::from(days), QStringLiteral(".1f"));
    });
    add("date_arithmetic", -1, [](Ctx c, Args a) {
        if (a.size() < 2)
            fail(QStringLiteral("date_arithmetic requires 2 or 3 arguments"));
        // parse_date('') is the undefined date.
        if (a[0].isEmpty())
            return QString(u"");
        std::optional<QDateTime> date = parseDate(a[0]);
        if (!date)
            fail(QStringLiteral("date_arithmetic: error: invalid date"));
        if (CalibreDb::isUndefinedDate(*date))
            return QString(u"");
        QString spec = a[1];
        static const QRegularExpression part(QStringLiteral("^([-+\\d]+)([smhdwy])"));
        while (!spec.isEmpty()) {
            const QRegularExpressionMatch match = part.match(spec);
            if (!match.hasMatch())
                fail(QStringLiteral("date_arithmetic: invalid calculation specifier '%1'").arg(spec));
            const qint64 amount = Py::toInt(match.captured(1));
            const QChar unit = match.captured(2).at(0);
            if (unit == u's')
                date = date->addSecs(amount);
            else if (unit == u'm')
                date = date->addSecs(amount * 60);
            else if (unit == u'h')
                date = date->addSecs(amount * 3600);
            else if (unit == u'd')
                date = date->addDays(amount);
            else if (unit == u'w')
                date = date->addDays(amount * 7);
            else
                date = date->addDays(amount * 365);
            spec = spec.mid(match.capturedLength());
        }
        return formatDate(c.book.library, *date, 0, a.size() > 2 && !a[2].isEmpty() ? a[2] : QStringLiteral("iso"));
    });
    add("language_strings", 2, [](Ctx, Args a) {
        QStringList names;
        for (const QString &code : items(a[0], QStringLiteral(","))) {
            const QString name = CalibreText::languageName(code);
            if (!name.isEmpty())
                names.append(name);
        }
        return names.join(QStringLiteral(", "));
    });
    add("language_codes", 1, [](Ctx, Args a) {
        QStringList codes;
        for (const QString &name : items(a[0], QStringLiteral(","))) {
            const QString code = CalibreText::canonicalizeLanguage(name);
            if (!code.isEmpty())
                codes.append(code);
        }
        return codes.join(QStringLiteral(", "));
    });
    add("current_library_name", 0, [](Ctx c, Args) {
        QString path = c.book.library.data().libraryPath;
        while (path.endsWith(u'/'))
            path.chop(1);
        return path.section(u'/', -1);
    });
    add("current_library_path", 0, [](Ctx c, Args) { return c.book.library.data().libraryPath; });
    add("finish_formatting", 4, [](Ctx c, Args a) {
        if (a[0].isEmpty())
            return a[0];
        return a[2] + c.formatter.doFormat(a[0], a[1]) + a[3];
    });
    add("virtual_libraries", 0, [](Ctx, Args) { return onlyInGui(QStringLiteral("virtual_libraries")); });
    add("current_virtual_library_name", 0, [](Ctx, Args) { return onlyInGui(QStringLiteral("current_virtual_library_name")); });
    add("user_categories", 0, [](Ctx, Args) { return QString(u""); });
    add("transliterate", 1, [](Ctx, Args a) { return CalibreText::asciiText(a[0]); });
    add("get_link", 2, [](Ctx, Args) { return QString(u""); });
    add("author_links", 2, [](Ctx, Args) { return QString(u""); });
    add("author_sorts", 1, [](Ctx c, Args a) {
        const PyValue map = c.book.get(QStringLiteral("author_sort_map"));
        if (map.dict.isEmpty())
            return QString(u"");
        QStringList sorts;
        for (const QString &author : c.book.book.authors) {
            if (author.trimmed().isEmpty())
                continue;
            for (const auto &[name, sort] : map.dict) {
                if (name == author)
                    sorts.append(sort);
            }
        }
        return sorts.join(a[0]);
    });
    add("connected_device_name", 1, [](Ctx, Args) { return onlyInGui(QStringLiteral("connected_device_name")); });
    add("connected_device_uuid", 1, [](Ctx, Args) { return onlyInGui(QStringLiteral("connected_device_uuid")); });
    add("check_yes_no", 4, [](Ctx c, Args a) {
        const PyValue value = c.book.attribute(a[0]).value_or(PyValue());
        if (value.isNone())
            return a[1] == QLatin1String("1") ? QStringLiteral("Yes") : QString(u"");
        if (value.kind != PyValue::Kind::Bool)
            fail(QStringLiteral("check_yes_no requires the field be a Yes/No custom column"));
        if ((a[2] == QLatin1String("1") && !value.boolean) || (a[3] == QLatin1String("1") && value.boolean))
            return QStringLiteral("Yes");
        return QString(u"");
    });
    add("rating_to_stars", 2, [](Ctx, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        double value = 0;
        try {
            value = Py::toFloat(a[0]) * 2;
        } catch (const TemplateError &) {
            fail(QStringLiteral("The rating must be a number between 0 and 5"));
        }
        if (value < 0 || value > 10)
            fail(QStringLiteral("The rating must be a number between 0 and 5"));
        return ratingToStars(value, a[1] == QLatin1String("1"));
    });
    add("swap_around_articles", 2, [](Ctx c, Args a) {
        if (a[0].isEmpty())
            return QString(u"");
        const auto sort = [&c](const QString &text) {
            return CalibreText::titleSort(text, QString(), CalibreText::Tweaks::defaults(), c.book.library.language())
                .replace(u',', u';');
        };
        if (a[1].isEmpty())
            return sort(a[0]);
        QStringList result;
        for (const QString &v : a[0].split(a[1]))
            result.append(sort(v.trimmed()));
        std::stable_sort(result.begin(), result.end(),
                         [&c](const QString &x, const QString &y) { return c.formatter.compare(x, y) < 0; });
        return result.join(a[1]);
    });
    add("field_exists", 1, [](Ctx c, Args a) {
        return c.book.hasKey(a[0].toLower()) ? QStringLiteral("1") : QString(u"");
    });
    add("to_hex", 1, [](Ctx, Args a) { return QString::fromLatin1(a[0].toUtf8().toHex()); });
    add("book_count", 2, [](Ctx, Args) -> QString {
        fail(QStringLiteral("The book_count() function cannot be used in a composite column"));
    });
    add("book_values", 4, [](Ctx, Args) -> QString {
        fail(QStringLiteral("The book_values() function cannot be used in a composite column"));
    });
    add("has_extra_files", -1, [](Ctx, Args) { return onlyInGui(QStringLiteral("has_extra_files")); });
    add("extra_file_names", -1, [](Ctx, Args) { return onlyInGui(QStringLiteral("extra_file_names")); });
    add("extra_file_size", 1, [](Ctx, Args) { return onlyInGui(QStringLiteral("extra_file_size")); });
    add("extra_file_modtime", 2, [](Ctx, Args) { return onlyInGui(QStringLiteral("extra_file_modtime")); });
    add("get_note", 3, [](Ctx, Args) { return onlyInGui(QStringLiteral("get_note")); });
    add("has_note", 2, [](Ctx, Args) { return onlyInGui(QStringLiteral("has_note")); });
    add("reading_progress", -1, [](Ctx, Args) { return onlyInGui(QStringLiteral("reading_progress")); });
    add("is_dark_mode", 0, [](Ctx, Args) { return onlyInGui(QStringLiteral("is_dark_mode")); });
    add("selected_books", 0, [](Ctx, Args) { return QString(u""); });
    add("sort_book_ids", -1, [](Ctx, Args) { return QString(u""); });
    add("selected_column", 0, [](Ctx, Args) { return QString(u""); });
    add("show_dialog", 1, [](Ctx, Args) { return onlyInGui(QStringLiteral("show_dialog")); });
    add("format_duration", -1, [](Ctx, Args a) { return formatDuration(a[0], a[1], a.value(2)); }, 2);
    add("make_url", -1, [](Ctx, Args a) { return a[0] + u'?' + queryPairs(a.mid(1), "make_url", 3); }, 1);
    add("make_url_extended", -1, [](Ctx, Args a) {
        const QStringList args = a.mid(3);
        QString query = args.size() == 1 ? args.at(0) : queryPairs(args, "make_url_extended", 5);
        if (!query.isEmpty())
            query.prepend(u'?');
        QString path = a[2];
        if (path.startsWith(u'/'))
            path.remove(0, 1);
        return a[0] + QStringLiteral("://") + a[1] + (a[1].isEmpty() ? QString() : QStringLiteral("/")) + path + query;
    }, 3);
    add("query_string", -1, [](Ctx, Args a) {
        if (a.size() % 3 != 0 || a.size() < 3)
            fail(QStringLiteral("query_string requires at least one group of 3 arguments"));
        QStringList pairs;
        for (qsizetype i = 0; i < a.size(); i += 3) {
            const QString &kind = a[i + 2];
            if (kind != QLatin1String("0") && kind != QLatin1String("1") && kind != QLatin1String("2"))
                fail(QStringLiteral("In query_string the third argument of a group must be 0, 1, or 2, not %1").arg(kind));
            const QString value = a[i + 1].trimmed();
            pairs.append(a[i] + u'=' + (kind == QLatin1String("2") ? value : qquote(value, kind == QLatin1String("0"))));
        }
        return pairs.join(u'&');
    });
    add("urls_from_identifiers", 2, [](Ctx c, Args a) {
        QList<QPair<QString, QString>> identifiers;
        for (const QString &part : a[0].split(u',')) {
            if (part.isEmpty() || !part.contains(u':'))
                continue;
            const QString type = part.section(u':', 0, 0).trimmed();
            const QString value = part.section(u':', 1).trimmed();
            if (!type.isEmpty() && !value.isEmpty())
                identifiers.append({ type, value });
        }
        Formatter &formatter = c.formatter;
        QList<IdentifierLinks::Link> links = IdentifierLinks::links(
            identifiers, c.book.library.linkRules(),
            [&formatter](const QString &text, const QHash<QString, QString> &values) -> std::optional<QString> {
                const QString url = formatter.evalFormat(text, values, true);
                if (url.startsWith(QLatin1String("EVAL ")))
                    return std::nullopt;
                return url;
            });
        if (a[1] != QLatin1String("0")) {
            // primary_sort_key(): case and accents aside.
            const auto key = [](const QString &name) {
                QString folded;
                for (const QChar ch : name.normalized(QString::NormalizationForm_D)) {
                    if (ch.category() != QChar::Mark_NonSpacing)
                        folded.append(ch);
                }
                return folded;
            };
            std::stable_sort(links.begin(), links.end(), [&](const auto &x, const auto &y) {
                return c.book.library.compare(key(x.name), key(y.name)) < 0;
            });
        }
        QStringList result;
        for (const IdentifierLinks::Link &link : std::as_const(links))
            result.append(QStringLiteral("<a href=\"%1\" title=\"%2:%3\">%4</a>")
                              .arg(xmlEscaped(link.url, true), xmlEscaped(link.type, true), xmlEscaped(link.value, true),
                                   xmlEscaped(link.name, false)));
        return result.join(QStringLiteral(", "));
    });
    add("width_from_pages", -1, [](Ctx, Args a) {
        qint64 pagesForMax = 1500;
        double factor = 2;
        QString defaultWidth = QStringLiteral("0.3");
        if (a.size() == 2 && !a[1].isEmpty()) {
            pagesForMax = Py::toInt(a[1]);
        } else if (a.size() == 3) {
            pagesForMax = Py::toInt(a[1]);
            factor = Py::toFloat(a[2]);
        } else if (a.size() == 4) {
            pagesForMax = Py::toInt(a[1]);
            factor = Py::toFloat(a[2]);
            defaultWidth = a[3];
        }
        qint64 pages = 0;
        try {
            pages = Py::toInt(a[0]);
        } catch (const TemplateError &) {
            return defaultWidth;
        }
        if (pages < 0)
            return defaultWidth;
        if (pagesForMax == 0)
            fail(QStringLiteral("division by zero"));
        // base_log() of bookshelf_view.py.
        const double share = std::max(0.0, std::min(double(pages) / double(pagesForMax), 1.0));
        if (factor + 1 <= 0 || factor + 1 == 1)
            fail(QStringLiteral("math domain error"));
        return PythonJson::floatRepr(std::log(1 + share * factor) / std::log(factor + 1));
    }, 1);
    add("encode_for_url", 2, [](Ctx, Args a) {
        if (a[1] != QLatin1String("0") && a[1] != QLatin1String("1"))
            fail(QStringLiteral("In quote_for_url the second argument must be 0, or 1, not %1").arg(a[1]));
        QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(a[0]));
        if (a[1] == QLatin1String("0"))
            encoded.replace(QStringLiteral("%20"), QStringLiteral("+"));
        return encoded;
    });
    // The functions the parser makes nodes of; called otherwise, as some
    // are when their arguments do not fit, they fail as Calibre's do.
    for (const char *name : { "arguments", "globals", "set_globals", "character", "list_count_field", "f_string",
                              "print" }) {
        add(name, -1, [](Ctx, Args) -> QString { fail(QStringLiteral("This function cannot be called directly")); });
    }
    return f;
}

} // namespace

const QHash<QString, Function> &functions()
{
    static const QHash<QString, Function> all = makeFunctions();
    return all;
}

} // namespace CalibreTemplate::Internal
