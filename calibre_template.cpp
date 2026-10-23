// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/utils/formatter.py: TemplateFormatter._do_format(), and the
//   parts of Python's format() and re it relies on
// - src/calibre/ebooks/metadata/book/formatter.py: SafeFormat
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_template.h"

#include <QCollator>
#include <QLocale>
#include <QMutex>
#include <QSet>

#include <cmath>
#include <optional>

#include "calibre_template_p.h"
#include "calibre_config.h"
#include "calibre_db.h"
#include "python_json.h"

namespace {

#include "calibre_template.inc"

using CalibreTemplate::PyValue;
using CalibreTemplate::Internal::TemplateError;

[[noreturn]] void fail(const QString &message)
{
    throw TemplateError{ message };
}

} // namespace

namespace CalibreTemplate {

// --- PyValue ---------------------------------------------------------------

PyValue PyValue::from(bool value)
{
    PyValue result;
    result.kind = Kind::Bool;
    result.boolean = value;
    return result;
}

PyValue PyValue::from(qint64 value)
{
    PyValue result;
    result.kind = Kind::Int;
    result.integer = value;
    return result;
}

PyValue PyValue::from(double value)
{
    PyValue result;
    result.kind = Kind::Float;
    result.real = value;
    return result;
}

PyValue PyValue::from(const QString &value)
{
    PyValue result;
    result.kind = Kind::Str;
    result.string = value;
    return result;
}

PyValue PyValue::from(const QStringList &values)
{
    PyValue result;
    result.kind = Kind::List;
    result.list = values;
    return result;
}

PyValue PyValue::fromDate(const QDateTime &utc, int microsecond)
{
    PyValue result;
    result.kind = Kind::Date;
    result.date = utc;
    result.microsecond = microsecond;
    return result;
}

bool PyValue::truthy() const
{
    switch (kind) {
    case Kind::None: return false;
    case Kind::Bool: return boolean;
    case Kind::Int: return integer != 0;
    case Kind::Float: return real != 0;
    case Kind::Str: return !string.isEmpty();
    case Kind::List: return !list.isEmpty();
    case Kind::Dict: return !dict.isEmpty();
    case Kind::Date: return true;
    }
    return false;
}

QString PyValue::str() const
{
    switch (kind) {
    case Kind::None: return QStringLiteral("None");
    case Kind::Bool: return boolean ? QStringLiteral("True") : QStringLiteral("False");
    case Kind::Int: return QString::number(integer);
    case Kind::Float: return PythonJson::floatRepr(real);
    case Kind::Str: return string;
    case Kind::List: {
        QStringList items;
        for (const QString &item : list)
            items.append(Internal::Py::repr(item));
        return u'[' + items.join(QStringLiteral(", ")) + u']';
    }
    case Kind::Dict: {
        QStringList items;
        for (const auto &[key, value] : dict)
            items.append(Internal::Py::repr(key) + QStringLiteral(": ") + Internal::Py::repr(value));
        return u'{' + items.join(QStringLiteral(", ")) + u'}';
    }
    case Kind::Date: {
        // str() of an aware datetime: isoformat(' ').
        const QDateTime utc = date.toUTC();
        QString text = utc.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (microsecond > 0)
            text += QStringLiteral(".%1").arg(microsecond, 6, 10, QLatin1Char('0'));
        return text + QStringLiteral("+00:00");
    }
    }
    return {};
}

namespace Internal::Py {

QString strip(const QString &text)
{
    return text.trimmed();
}

QString humanReadable(double size, const QString &separator)
{
    static const char *const suffixes[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
    double divisor = 1;
    QString suffix = QStringLiteral("B");
    for (int i = 0; i < 7; ++i) {
        if (size < std::ldexp(1.0, (i + 1) * 10)) {
            divisor = std::ldexp(1.0, i * 10);
            suffix = QLatin1String(suffixes[i]);
            break;
        }
    }
    QString text = PythonJson::floatRepr(size / divisor);
    if (text.contains(u'.'))
        text = text.left(text.indexOf(u'.') + 2);
    if (text.endsWith(QLatin1String(".0")))
        text.chop(2);
    return text + separator + suffix;
}

double toFloat(const QString &text)
{
    // float(): spaces around, underscores between digits, inf and nan.
    QString cleaned = text.trimmed();
    static const QRegularExpression underscores(QStringLiteral("(?<=\\d)_(?=\\d)"));
    cleaned.remove(underscores);
    const QString lower = cleaned.toLower();
    if (lower == QLatin1String("inf") || lower == QLatin1String("+inf") || lower == QLatin1String("infinity")
        || lower == QLatin1String("+infinity"))
        return INFINITY;
    if (lower == QLatin1String("-inf") || lower == QLatin1String("-infinity"))
        return -INFINITY;
    if (lower == QLatin1String("nan") || lower == QLatin1String("+nan") || lower == QLatin1String("-nan"))
        return NAN;
    static const QRegularExpression number(QStringLiteral("^[+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?$"));
    bool ok = false;
    const double value = number.match(cleaned).hasMatch() ? cleaned.toDouble(&ok) : 0;
    if (!ok)
        fail(QStringLiteral("could not convert string to float: %1").arg(repr(text)));
    return value;
}

qint64 toInt(const QString &text)
{
    QString cleaned = text.trimmed();
    static const QRegularExpression underscores(QStringLiteral("(?<=\\d)_(?=\\d)"));
    cleaned.remove(underscores);
    static const QRegularExpression integer(QStringLiteral("^[+-]?\\d+$"));
    bool ok = false;
    const qint64 value = integer.match(cleaned).hasMatch() ? cleaned.toLongLong(&ok) : 0;
    if (!ok)
        fail(QStringLiteral("invalid literal for int() with base 10: %1").arg(repr(text)));
    return value;
}

QString intText(double value)
{
    if (std::isnan(value))
        fail(QStringLiteral("cannot convert float NaN to integer"));
    if (std::isinf(value))
        fail(QStringLiteral("cannot convert float infinity to integer"));
    const double whole = std::trunc(value);
    if (std::abs(whole) < 9.2e18)
        return QString::number(qint64(whole));
    // Mantissa times a power of two, in decimal digits from the last.
    int exponent = 0;
    const double mantissa = std::frexp(std::abs(whole), &exponent);
    quint64 bits = quint64(std::ldexp(mantissa, 53));
    exponent -= 53;
    QList<int> digits;
    while (bits > 0) {
        digits.append(int(bits % 10));
        bits /= 10;
    }
    for (int i = 0; i < exponent; ++i) {
        int carry = 0;
        for (int &digit : digits) {
            const int doubled = digit * 2 + carry;
            digit = doubled % 10;
            carry = doubled / 10;
        }
        if (carry)
            digits.append(carry);
    }
    QString text = whole < 0 ? QStringLiteral("-") : QString();
    for (qsizetype i = digits.size() - 1; i >= 0; --i)
        text.append(QChar(u'0' + digits.at(i)));
    return text;
}

qint64 toInt64(double value)
{
    const QString text = intText(value);
    bool ok = false;
    const qint64 result = text.toLongLong(&ok);
    if (!ok)
        fail(QStringLiteral("Python int too large to convert to C long"));
    return result;
}

quint64 rangeLength(qint64 start, qint64 stop, qint64 step)
{
    if (step > 0) {
        if (stop <= start)
            return 0;
        const quint64 span = quint64(stop) - quint64(start);
        return span / quint64(step) + (span % quint64(step) != 0);
    }
    if (start <= stop)
        return 0;
    const quint64 span = quint64(start) - quint64(stop);
    const quint64 size = quint64(-(step + 1)) + 1;
    return span / size + (span % size != 0);
}

QString number(double value)
{
    double whole = 0;
    if (std::isfinite(value) && std::modf(value, &whole) == 0 && std::abs(whole) < 9.2e18)
        return QString::number(qint64(whole));
    return PythonJson::floatRepr(value);
}

QString repr(const QString &text)
{
    const QChar quote = text.contains(u'\'') && !text.contains(u'"') ? u'"' : u'\'';
    QString result(quote);
    for (const QChar c : text) {
        if (c == quote || c == u'\\')
            result += u'\\' + QString(c);
        else if (c == u'\n')
            result += QStringLiteral("\\n");
        else if (c == u'\r')
            result += QStringLiteral("\\r");
        else if (c == u'\t')
            result += QStringLiteral("\\t");
        else if (!c.isPrint() && !c.isSurrogate())
            result += c.unicode() < 0x100 ? QStringLiteral("\\x%1").arg(int(c.unicode()), 2, 16, QLatin1Char('0'))
                                          : QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
        else
            result += c;
    }
    return result + quote;
}

QRegularExpression regex(const QString &pattern, bool ignoreCase)
{
    // Python's syntax where PCRE's differs: named groups and references.
    QString converted = pattern;
    converted.replace(QStringLiteral("(?P<"), QStringLiteral("(?<"));
    static const QRegularExpression backReference(QStringLiteral("\\(\\?P=(\\w+)\\)"));
    converted.replace(backReference, QStringLiteral("\\k<\\1>"));
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (ignoreCase)
        options |= QRegularExpression::CaseInsensitiveOption;
    QRegularExpression result(converted, options);
    if (!result.isValid())
        fail(result.errorString());
    return result;
}

QString sub(const QRegularExpression &regex, const QString &replacement, const QString &text)
{
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = regex.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += text.mid(last, match.capturedStart() - last);
        // The replacement's escapes: \1, \g<1>, \g<name>, \n and the like.
        for (qsizetype i = 0; i < replacement.size(); ++i) {
            const QChar c = replacement.at(i);
            if (c != u'\\' || i + 1 >= replacement.size()) {
                result += c;
                continue;
            }
            const QChar next = replacement.at(++i);
            if (next.isDigit()) {
                int group = next.digitValue();
                if (i + 1 < replacement.size() && replacement.at(i + 1).isDigit() && group * 10 + replacement.at(i + 1).digitValue() <= regex.captureCount())
                    group = group * 10 + replacement.at(++i).digitValue();
                if (group > regex.captureCount())
                    fail(QStringLiteral("invalid group reference %1").arg(group));
                result += match.captured(group);
            } else if (next == u'g' && i + 1 < replacement.size() && replacement.at(i + 1) == u'<') {
                const qsizetype end = replacement.indexOf(u'>', i + 2);
                if (end < 0)
                    fail(QStringLiteral("missing >, unterminated name"));
                const QString name = replacement.mid(i + 2, end - i - 2);
                bool numeric = false;
                const int group = name.toInt(&numeric);
                result += numeric ? match.captured(group) : match.captured(name);
                i = end;
            } else if (next == u'n') {
                result += u'\n';
            } else if (next == u't') {
                result += u'\t';
            } else if (next == u'r') {
                result += u'\r';
            } else if (next == u'\\') {
                result += u'\\';
            } else if (next.isLetter()) {
                fail(QStringLiteral("bad escape \\%1").arg(next));
            } else {
                result += u'\\' + QString(next);
            }
        }
        last = match.capturedEnd();
        // An empty match moves on by one, as Python's does.
    }
    result += text.mid(last);
    return result;
}

namespace {

struct Spec
{
    QChar fill = u' ';
    QChar align;
    QChar sign;
    bool alternate = false;
    bool zero = false;
    int width = -1;
    QChar grouping;
    int precision = -1;
    QChar type;
};

Spec parseSpec(const QString &spec, const QString &typeName)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^(?:(.)?([<>=^]))?([-+ ])?(z)?(#)?(0)?(\\d+)?([,_])?(?:\\.(\\d+))?(.)?$"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = pattern.match(spec);
    if (!match.hasMatch())
        fail(QStringLiteral("Invalid format specifier '%1' for object of type '%2'").arg(spec, typeName));
    Spec result;
    if (!match.captured(2).isEmpty()) {
        result.align = match.captured(2).at(0);
        if (!match.captured(1).isEmpty())
            result.fill = match.captured(1).at(0);
    }
    if (!match.captured(3).isEmpty())
        result.sign = match.captured(3).at(0);
    result.alternate = !match.captured(5).isEmpty();
    if (!match.captured(6).isEmpty()) {
        result.zero = true;
        if (match.captured(2).isEmpty())
            result.fill = u'0';
    }
    if (!match.captured(7).isEmpty())
        result.width = match.captured(7).toInt();
    if (!match.captured(8).isEmpty())
        result.grouping = match.captured(8).at(0);
    if (!match.captured(9).isEmpty())
        result.precision = match.captured(9).toInt();
    if (!match.captured(10).isEmpty())
        result.type = match.captured(10).at(0);
    return result;
}

QString group(const QString &digits, QChar separator, int every = 3)
{
    QString result;
    for (qsizetype i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % every == 0)
            result += separator;
        result += digits.at(i);
    }
    return result;
}

// Pads `body`, after `prefix` (sign and base) for '=' alignment.
QString pad(const QString &prefix, const QString &body, const Spec &spec, QChar defaultAlign)
{
    const QChar align = spec.align.isNull() ? (spec.zero && defaultAlign == u'>' ? u'=' : defaultAlign) : spec.align;
    const qsizetype length = prefix.size() + body.size();
    if (spec.width <= length)
        return prefix + body;
    const qsizetype missing = spec.width - length;
    const QString fill(missing, spec.fill);
    if (align == u'<')
        return prefix + body + fill;
    if (align == u'^')
        return QString(missing / 2, spec.fill) + prefix + body + QString(missing - missing / 2, spec.fill);
    if (align == u'=')
        return prefix + fill + body;
    return fill + prefix + body;
}

QString signOf(bool negative, const Spec &spec)
{
    if (negative)
        return QStringLiteral("-");
    if (spec.sign == u'+')
        return QStringLiteral("+");
    if (spec.sign == u' ')
        return QStringLiteral(" ");
    return {};
}

QString formatFloat(double value, const Spec &spec)
{
    const QChar type = spec.type;
    const bool negative = std::signbit(value);
    const double magnitude = std::abs(value);
    QString body;
    if (std::isnan(value)) {
        body = QStringLiteral("nan");
    } else if (std::isinf(value)) {
        body = QStringLiteral("inf");
    } else if (type == u'f' || type == u'F') {
        body = QString::number(magnitude, 'f', spec.precision < 0 ? 6 : spec.precision);
    } else if (type == u'e' || type == u'E') {
        body = QString::number(magnitude, 'e', spec.precision < 0 ? 6 : spec.precision);
    } else if (type == u'%') {
        body = QString::number(magnitude * 100, 'f', spec.precision < 0 ? 6 : spec.precision) + u'%';
    } else if (type == u'g' || type == u'G' || type == u'n') {
        const int precision = spec.precision < 0 ? 6 : std::max(1, spec.precision);
        body = QString::number(magnitude, 'g', precision);
        if (spec.alternate && !body.contains(u'.') && !body.contains(u'e'))
            body += u'.';
    } else if (spec.precision < 0) {
        body = PythonJson::floatRepr(magnitude);
    } else {
        // No type with a precision: 'g', but a digit after the point.
        body = QString::number(magnitude, 'g', std::max(1, spec.precision));
        if (!body.contains(u'.') && !body.contains(u'e'))
            body += QStringLiteral(".0");
    }
    if (!spec.grouping.isNull() && !body.contains(u'n')) {
        const qsizetype end = std::min(body.contains(u'.') ? body.indexOf(u'.') : body.size(),
                                       body.contains(u'e') ? body.indexOf(u'e') : body.size());
        body = group(body.left(end), spec.grouping) + body.mid(end);
    }
    if (type.isUpper())
        body = body.toUpper();
    return pad(signOf(negative && !std::isnan(value), spec), body, spec, u'>');
}

} // namespace

QString format(const PyValue &value, const QString &spec)
{
    if (value.kind == PyValue::Kind::Str) {
        const Spec parsed = parseSpec(spec, QStringLiteral("str"));
        if (!parsed.type.isNull() && parsed.type != u's')
            fail(QStringLiteral("Unknown format code '%1' for object of type 'str'").arg(parsed.type));
        if (!parsed.sign.isNull())
            fail(QStringLiteral("Sign not allowed in string format specifier"));
        if (parsed.alternate)
            fail(QStringLiteral("Alternate form (#) not allowed in string format specifier"));
        if (!parsed.grouping.isNull())
            fail(QStringLiteral("Cannot specify '%1' with 's'.").arg(parsed.grouping));
        if (parsed.align == u'=')
            fail(QStringLiteral("'=' alignment not allowed in string format specifier"));
        const QString body = parsed.precision >= 0 ? value.string.left(parsed.precision) : value.string;
        return pad({}, body, parsed, u'<');
    }
    if (value.kind == PyValue::Kind::Int) {
        const Spec parsed = parseSpec(spec, QStringLiteral("int"));
        const QChar type = parsed.type;
        if (QStringView(u"eEfFgG%").contains(type))
            return formatFloat(double(value.integer), parsed);
        if (!type.isNull() && !QStringView(u"bcdoxXn").contains(type))
            fail(QStringLiteral("Unknown format code '%1' for object of type 'int'").arg(type));
        if (parsed.precision >= 0)
            fail(QStringLiteral("Precision not allowed in integer format specifier"));
        const bool negative = value.integer < 0;
        const quint64 magnitude = negative ? quint64(0) - quint64(value.integer) : quint64(value.integer);
        QString body;
        QString prefix;
        if (type == u'c') {
            body = QString(QChar(char16_t(magnitude)));
        } else if (type == u'x' || type == u'X') {
            body = QString::number(magnitude, 16);
            prefix = parsed.alternate ? QStringLiteral("0x") : QString();
        } else if (type == u'o') {
            body = QString::number(magnitude, 8);
            prefix = parsed.alternate ? QStringLiteral("0o") : QString();
        } else if (type == u'b') {
            body = QString::number(magnitude, 2);
            prefix = parsed.alternate ? QStringLiteral("0b") : QString();
        } else {
            body = QString::number(magnitude);
        }
        if (!parsed.grouping.isNull()) {
            if (parsed.grouping == u',' && !type.isNull() && type != u'd')
                fail(QStringLiteral("Cannot specify ',' with '%1'.").arg(type));
            body = group(body, parsed.grouping, type == u'd' || type.isNull() ? 3 : 4);
        }
        if (type == u'X') {
            body = body.toUpper();
            prefix = prefix.toUpper();
        }
        return pad(signOf(negative, parsed) + prefix, body, parsed, u'>');
    }
    if (value.kind == PyValue::Kind::Float) {
        const Spec parsed = parseSpec(spec, QStringLiteral("float"));
        if (!parsed.type.isNull() && !QStringView(u"eEfFgGn%").contains(parsed.type))
            fail(QStringLiteral("Unknown format code '%1' for object of type 'float'").arg(parsed.type));
        return formatFloat(value.real, parsed);
    }
    return format(PyValue::from(value.str()), spec);
}

} // namespace Internal::Py

// --- Library ---------------------------------------------------------------

struct Library::Private
{
    Private(const LibraryData &data, const QString &language) : data(data), language(language) {}

    const LibraryData &data;
    QString language;
    int languageRow = 0;
    QHash<QString, QString> searchTerms;
    QHash<QString, int> customs;
    QHash<int, const Book *> books;
    QHash<int, QString> authorSorts;
    QCollator collator;
    QCollator caseCollator;
    mutable std::optional<IdentifierLinks::Rules> linkRules;
    mutable QMutex lock;
    mutable QHash<QPair<int, QString>, QString> composites;
    // The composites being computed, a template that reads itself told so.
    mutable QSet<QPair<int, QString>> computing;
};

Library::Library(const LibraryData &data, const QString &interfaceLanguage)
    : d(std::make_unique<Private>(data, interfaceLanguage))
{
    // The language's names, else its base language's, else English.
    const QString base = interfaceLanguage.section(u'_', 0, 0);
    int english = 0;
    for (int row = 0; row < int(std::size(templateLanguages)); ++row) {
        const QString code = QString::fromUtf8(templateLanguages[row].language);
        if (code == QLatin1String("en"))
            english = row;
        if (code == interfaceLanguage)
            d->languageRow = row + 1000;
        else if (code == base && d->languageRow < 1000)
            d->languageRow = row + 1;
    }
    d->languageRow = d->languageRow >= 1000 ? d->languageRow - 1000 : d->languageRow > 0 ? d->languageRow - 1 : english;
    for (const auto &term : templateSearchTerms)
        d->searchTerms.insert(QString::fromUtf8(term.term), QString::fromUtf8(term.key));
    for (int index = 0; index < data.customColumns.size(); ++index)
        d->customs.insert(u'#' + data.customColumns.at(index).label, index);
    for (const Book &book : data.books)
        d->books.insert(book.id, &book);
    for (const FacetItem &author : data.authors)
        d->authorSorts.insert(author.id, author.sort);
    d->collator = QCollator(QLocale(interfaceLanguage));
    d->collator.setCaseSensitivity(Qt::CaseInsensitive);
    d->caseCollator = QCollator(QLocale(interfaceLanguage));
    d->caseCollator.setCaseSensitivity(Qt::CaseSensitive);
}

const Book *Library::book(int id) const
{
    return d->books.value(id);
}

int Library::compare(const QString &a, const QString &b) const
{
    return d->collator.compare(a, b);
}

int Library::compareCase(const QString &a, const QString &b) const
{
    // Upper case first: ICU's tertiary order, which puts lower case first,
    // turned round where only case tells the texts apart.
    const int order = d->collator.compare(a, b);
    return order != 0 ? order : -d->caseCollator.compare(a, b);
}

QString Library::authorSort(int authorId) const
{
    return d->authorSorts.value(authorId);
}

const IdentifierLinks::Rules &Library::linkRules() const
{
    const QMutexLocker locker(&d->lock);
    if (!d->linkRules)
        d->linkRules = IdentifierLinks::readRules(CalibreConfig::configDirectory());
    return *d->linkRules;
}

Library::~Library() = default;

const LibraryData &Library::data() const
{
    return d->data;
}

QString Library::language() const
{
    return d->language;
}

QString Library::fieldKey(const QString &term) const
{
    return d->searchTerms.value(term, term);
}

bool Library::hasField(const QString &key) const
{
    if (d->customs.contains(key))
        return true;
    for (const auto &field : templateFields) {
        if (key == QLatin1String(field.key))
            return true;
    }
    return false;
}

QString Library::listToUi(const QString &key) const
{
    if (const CustomColumn *column = customColumn(key)) {
        if (!column->multiple)
            return {};
        return CalibreDb::multipleSeparators(*column).text(QStringLiteral("list_to_ui"));
    }
    for (const auto &field : templateFields) {
        if (key == QLatin1String(field.key))
            return QString::fromUtf8(field.listToUi);
    }
    return {};
}

const CustomColumn *Library::customColumn(const QString &key) const
{
    const int index = d->customs.value(key, -1);
    return index >= 0 ? &d->data.customColumns.at(index) : nullptr;
}

int Library::customIndex(const QString &key) const
{
    return d->customs.value(key, -1);
}

QString Library::word(const QString &english) const
{
    static const QStringList words{ QStringLiteral("Yes"), QStringLiteral("No"), QStringLiteral("None"),
                                    QStringLiteral("Error"), QStringLiteral("DRM"), QStringLiteral("TEMPLATE ERROR"),
                                    QStringLiteral("%s: unknown function"), QStringLiteral("Unknown"),
                                    QStringLiteral("No book formats found so the path can't be generated"),
                                    QStringLiteral("No such variable {0}") };
    const qsizetype index = words.indexOf(english);
    return index < 0 ? english : QString::fromUtf8(templateLanguages[d->languageRow].words[index]);
}

QString Library::dayName(int day, bool abbreviated) const
{
    const auto &names = abbreviated ? templateLanguages[d->languageRow].abbreviatedDays
                                    : templateLanguages[d->languageRow].days;
    return QString::fromUtf8(names[((day % 7) + 7) % 7]);
}

QString Library::monthName(int month, bool abbreviated) const
{
    const auto &names = abbreviated ? templateLanguages[d->languageRow].abbreviatedMonths
                                    : templateLanguages[d->languageRow].months;
    return QString::fromUtf8(names[std::clamp(month, 1, 12) - 1]);
}

QString Library::composite(int bookId, const QString &key) const
{
    {
        QMutexLocker locker(&d->lock);
        const auto found = d->composites.constFind({ bookId, key });
        if (found != d->composites.cend())
            return *found;
        // composite_getter(): a template that reads its own column.
        if (d->computing.contains({ bookId, key }))
            return QStringLiteral("RECURSIVE_COMPOSITE FIELD (Metadata) ") + key;
        d->computing.insert({ bookId, key });
    }
    const CustomColumn *column = customColumn(key);
    const Book *book = this->book(bookId);
    if (!column || !book) {
        QMutexLocker locker(&d->lock);
        d->computing.remove({ bookId, key });
        return {};
    }
    QString error;
    const QString templateText = PythonJson::parse(column->display).text(QStringLiteral("composite_template"));
    QString value = evaluate(templateText, *this, *book, &error);
    // _render_composite_with_cache(): the error after "TEMPLATE ERROR".
    if (!error.isNull())
        value = word(QStringLiteral("TEMPLATE ERROR")) + u' ' + error;
    value = value.trimmed();
    QMutexLocker locker(&d->lock);
    d->computing.remove({ bookId, key });
    d->composites.insert({ bookId, key }, value);
    return value;
}

QString evaluate(const QString &templateText, const Library &library, const Book &book, QString *error)
{
    Internal::Formatter formatter(library);
    const Internal::BookProxy proxy(library, book);
    return formatter.safeFormat(templateText, proxy, error);
}

std::optional<QDateTime> parseDate(const QString &text)
{
    return Internal::parseDate(text);
}

void computeComposites(LibraryData *data, const QString &interfaceLanguage, const QSet<int> *only)
{
    QList<int> columns;
    for (int index = 0; index < data->customColumns.size(); ++index) {
        if (data->customColumns.at(index).datatype == QLatin1String("composite"))
            columns.append(index);
    }
    if (columns.isEmpty())
        return;
    QList<QList<QVariant>> values(data->books.size());
    {
        const Library library(*data, interfaceLanguage);
        for (qsizetype row = 0; row < data->books.size(); ++row) {
            if (only && !only->contains(data->books.at(row).id))
                continue;
            for (const int index : std::as_const(columns)) {
                const CustomColumn &column = data->customColumns.at(index);
                values[row].append(library.composite(data->books.at(row).id, u'#' + column.label));
            }
        }
    }
    for (qsizetype row = 0; row < data->books.size(); ++row) {
        if (values.at(row).isEmpty())
            continue;
        Book &book = data->books[row];
        if (book.custom.size() < data->customColumns.size())
            book.custom.resize(data->customColumns.size());
        for (qsizetype i = 0; i < columns.size(); ++i)
            book.custom[columns.at(i)].value = values.at(row).at(i);
    }
}

} // namespace CalibreTemplate
