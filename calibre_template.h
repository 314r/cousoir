#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include "identifier_links.h"
#include <QStringList>

#include <memory>
#include <optional>

#include "book.h"

// Calibre's template language, ported from Calibre 9.16's
// src/calibre/utils/formatter.py and formatter_functions.py: the single
// function mode, "{title} by {authors}", "{series_index:0>2s}",
// "{tags:sublist(0,1,\,)}", and the template program mode, "program: ...",
// with its functions. Calibre computes composite columns with it, and its
// "template:" searches. Python templates ("python:") are not run.
namespace CalibreTemplate {

// A value as Python holds it, for the fields a template reads raw: their
// text is Python's str() of them.
struct PyValue
{
    enum class Kind { None, Bool, Int, Float, Str, List, Dict, Date };
    Kind kind = Kind::None;
    bool boolean = false;
    qint64 integer = 0;
    double real = 0;
    QString string;
    QStringList list;
    QList<QPair<QString, QString>> dict;
    // A date in UTC, and its microseconds.
    QDateTime date;
    int microsecond = 0;

    static PyValue none() { return {}; }
    static PyValue from(bool value);
    static PyValue from(qint64 value);
    static PyValue from(int value) { return from(qint64(value)); }
    static PyValue from(double value);
    static PyValue from(const QString &value);
    static PyValue from(const QStringList &values);
    static PyValue fromDate(const QDateTime &utc, int microsecond = 0);

    bool isNone() const { return kind == Kind::None; }
    // bool() of the value.
    bool truthy() const;
    // str() of the value.
    QString str() const;
};

// What templates read of a library: its books, its fields and custom
// columns, and Calibre's interface language, for the words they print and
// the names of months.
class Library
{
public:
    Library(const LibraryData &data, const QString &interfaceLanguage);
    ~Library();

    const LibraryData &data() const;
    QString language() const;
    // The field a search term names, "tag" for "tags", "#genre" as itself.
    QString fieldKey(const QString &term) const;
    // Whether the field exists: built in or a custom column.
    bool hasField(const QString &key) const;
    // " & " for authors, ", " for tags: how the values of a field of several
    // are joined for showing; empty for a field of one.
    QString listToUi(const QString &key) const;
    const CustomColumn *customColumn(const QString &key) const;
    int customIndex(const QString &key) const;
    // A word Calibre's interface language translates: "Yes", "No", "None",
    // "Error", "DRM", "Unknown".
    QString word(const QString &english) const;
    // format_date()'s names: days from Sunday, months from January.
    QString dayName(int day, bool abbreviated) const;
    QString monthName(int month, bool abbreviated) const;

    // The value of a composite column for a book, computed at most once by
    // one evaluation of its template, its errors included.
    QString composite(int bookId, const QString &key) const;
    // The book of that id, or none.
    const Book *book(int id) const;
    // Calibre's sort collator, the interface language's, case aside.
    int compare(const QString &a, const QString &b) const;
    // Its case sensitive collator, upper case first.
    int compareCase(const QString &a, const QString &b) const;
    // The sort of the author of that id.
    QString authorSort(int authorId) const;
    // Calibre's links for identifiers the user added, id_link_rules, read
    // when first asked for.
    const IdentifierLinks::Rules &linkRules() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// What a template makes of a book, as SafeFormat.safe_format() makes it for
// a composite column. A template that fails gives the empty text and its
// error in `error`, the text Calibre writes after its "TEMPLATE ERROR".
QString evaluate(const QString &templateText, const Library &library, const Book &book, QString *error = nullptr);

// Calibre's parse_date() of a text: ISO forms, or dateutil's day, month and
// year in some order with English names; local unless it says otherwise.
// None for text that is no date.
std::optional<QDateTime> parseDate(const QString &text);

// Puts each composite column's value in the books' custom values, as
// Calibre computes it when it reads it: text, even for a column of several
// values, which only a search splits at its commas. Calibre's interface
// language gives the words templates print.
// With `only`, the books of those ids alone.
void computeComposites(LibraryData *data, const QString &interfaceLanguage, const QSet<int> *only = nullptr);

} // namespace CalibreTemplate
