#pragma once

// The inside of CalibreTemplate, shared by its files: the book as templates
// read it, the formatter, and the template functions.

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "calibre_template.h"

namespace CalibreTemplate::Internal {

// Python's ValueError, which a template's evaluation turns into its error.
struct TemplateError
{
    QString message;
};

// The book as a template reads it, Calibre's ProxyMetadata: its attributes,
// raw, and its fields as format_field_extended() shows them.
class BookProxy
{
public:
    BookProxy(const Library &library, const Book &book);

    const Library &library;
    const Book &book;

    // getattr(mi, name): none when Metadata has no such attribute.
    std::optional<PyValue> attribute(const QString &name) const;
    // mi.get(name): the attribute, None when there is none.
    PyValue get(const QString &name) const;
    // format_field_extended(key, series_with_index)[1]: the field as Calibre
    // shows it; none for an empty value or no such field.
    std::optional<QString> formatField(const QString &key, bool seriesWithIndex) const;
    // SafeFormat.get_value(): what {field} gives. Throws TemplateError for a
    // field the book does not have.
    QString value(const QString &key) const;
    // The key of every field a template may name, built in and custom.
    bool hasKey(const QString &key) const;
};

class Formatter;

// What a function sees: the formatter, the book and the template's
// variables.
struct FunctionContext
{
    Formatter &formatter;
    const BookProxy &book;
    QHash<QString, QString> &locals;
};

// A formatter function of formatter_functions.py: its argument count, -1
// for any, and its evaluation, which throws TemplateError.
struct Function
{
    int argCount = 0;
    std::function<QString(FunctionContext &, const QStringList &)> evaluate;
};

const QHash<QString, Function> &functions();

// TemplateFormatter with SafeFormat's get_value(): evaluates templates, single
// function mode and program mode, for one book at a time.
class Formatter
{
public:
    explicit Formatter(const Library &library);
    ~Formatter();

    const Library &library;
    // The book being formatted.
    const BookProxy *book = nullptr;
    QHash<QString, QString> globals;
    // EvalFormatter's: its fields are variables, for program mode's field()
    // and $name as for its templates.
    std::function<QString(const QString &)> variables;
    // strip_results: the results of functions in single function mode, and
    // the template's, stripped of spaces.
    bool stripResults = true;

    // safe_format(): the template's text, or empty with `error`.
    QString safeFormat(const QString &templateText, const BookProxy &book, QString *error);
    // EvalFormatter.safe_format(): a template whose fields are `variables`,
    // "No such variable" for others; an error is "EVAL" and its message.
    QString evalFormat(const QString &templateText, const QHash<QString, QString> &variables, bool strip);
    // SafeFormat's vformat() of a single function mode template, for the
    // book being formatted.
    QString vformat(const QString &templateText);
    // TemplateFormatter._do_format(): the value as the spec formats it.
    QString doFormat(const QString &value, const QString &spec);
    // _eval_program(): a program with `$` set to `value`.
    QString evalProgram(const std::optional<QString> &value, const QString &program);
    // The interface language's collator, as Calibre's sort collator compares.
    int compare(const QString &a, const QString &b) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
    friend struct Interpreter;
};

// format_date(): a date, here in local time, with Qt's codes as Calibre
// reads them, in the interface language: empty for the undefined date.
QString formatDate(const Library &library, const QDateTime &utc, int microsecond, const QString &format);
// parse_date() of a text a template gives, local unless it says otherwise;
// none for text that is no date.
std::optional<QDateTime> parseDate(const QString &text);

// Python's helpers the formatter and functions share.
namespace Py {

// float(text): throws TemplateError for text that is no number.
double toFloat(const QString &text);
// int(text).
qint64 toInt(const QString &text);
// str(int(value)) of a float: every digit of a large one; Python's
// OverflowError or ValueError for infinity and NaN.
QString intText(double value);
// int(value) of a float that has to fit 64 bits: an error otherwise.
qint64 toInt64(double value);
// len(range(start, stop, step)), step not zero, without overflow; its
// values are start + i * step in quint64 arithmetic, cast back.
quint64 rangeLength(qint64 start, qint64 stop, qint64 step);
// str(float) and str(int(x)) when x is whole, as the interpreter writes
// numbers.
QString number(double value);
// format(value, spec), with value text, an int or a float.
QString format(const PyValue &value, const QString &spec);
// re.compile(pattern, flags): throws TemplateError for a bad pattern.
QRegularExpression regex(const QString &pattern, bool ignoreCase);
// re.sub(pattern, replacement, text), the replacement's \1 and \g<name>
// expanded.
QString sub(const QRegularExpression &regex, const QString &replacement, const QString &text);
// repr(text).
QString repr(const QString &text);
// str.strip(), lower(), upper() as Python's.
QString strip(const QString &text);
// human_readable() of calibre/__init__.py: "1.5 MB".
QString humanReadable(double size, const QString &separator = QStringLiteral(" "));

} // namespace Py

} // namespace CalibreTemplate::Internal
