#include "calibre_list.h"

#include <QLocale>
#include <QVariant>

#include <algorithm>
#include <cmath>

#include "calibre_db.h"

namespace {

// A JSON value as Python builds it: dicts write their keys sorted, floats
// keep their ".0".
struct PyValue
{
    enum class Kind { String, Integer, Float, List, Dict };

    Kind kind = Kind::String;
    QString string;
    qint64 integer = 0;
    double real = 0;
    QList<PyValue> items;
    QList<QPair<QString, PyValue>> fields;

    static PyValue text(const QString &value)
    {
        PyValue result;
        result.string = value;
        return result;
    }
    static PyValue number(qint64 value)
    {
        PyValue result;
        result.kind = Kind::Integer;
        result.integer = value;
        return result;
    }
    static PyValue floating(double value)
    {
        PyValue result;
        result.kind = Kind::Float;
        result.real = value;
        return result;
    }
    static PyValue list(const QStringList &values)
    {
        PyValue result;
        result.kind = Kind::List;
        for (const QString &value : values)
            result.items.append(text(value));
        return result;
    }
    static PyValue dict() { PyValue result; result.kind = Kind::Dict; return result; }

    void insert(const QString &key, const PyValue &value)
    {
        for (auto &field : fields) {
            if (field.first == key) {
                field.second = value;
                return;
            }
        }
        fields.append({ key, value });
    }
};

// json.dumps() with ensure_ascii: everything outside printable ASCII is
// escaped, surrogate pairs and all.
void writeString(QByteArray &out, const QString &value)
{
    out += '"';
    for (const QChar character : value) {
        const char16_t code = character.unicode();
        switch (code) {
        case u'"': out += "\\\""; break;
        case u'\\': out += "\\\\"; break;
        case u'\n': out += "\\n"; break;
        case u'\r': out += "\\r"; break;
        case u'\t': out += "\\t"; break;
        case u'\b': out += "\\b"; break;
        case u'\f': out += "\\f"; break;
        default:
            if (code < 0x20 || code > 0x7e)
                out += "\\u" + QByteArray::number(code, 16).rightJustified(4, '0');
            else
                out += char(code);
        }
    }
    out += '"';
}

// Python's float repr: the shortest text that reads back, with ".0" on whole
// numbers.
QByteArray floatRepr(double value)
{
    if (std::isnan(value))
        return "NaN";
    if (std::isinf(value))
        return value > 0 ? "Infinity" : "-Infinity";
    QByteArray text = QByteArray::number(value, 'g', QLocale::FloatingPointShortest);
    if (!text.contains('.') && !text.contains('e') && !text.contains('n'))
        text += ".0";
    return text;
}

void write(QByteArray &out, const PyValue &value, int depth)
{
    const QByteArray indent(2 * (depth + 1), ' ');
    const QByteArray closing(2 * depth, ' ');
    switch (value.kind) {
    case PyValue::Kind::String:
        writeString(out, value.string);
        return;
    case PyValue::Kind::Integer:
        out += QByteArray::number(value.integer);
        return;
    case PyValue::Kind::Float:
        out += floatRepr(value.real);
        return;
    case PyValue::Kind::List:
        if (value.items.isEmpty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (qsizetype index = 0; index < value.items.size(); ++index) {
            out += indent;
            write(out, value.items.at(index), depth + 1);
            out += index + 1 < value.items.size() ? ",\n" : "\n";
        }
        out += closing + ']';
        return;
    case PyValue::Kind::Dict: {
        if (value.fields.isEmpty()) {
            out += "{}";
            return;
        }
        QList<QPair<QString, PyValue>> fields = value.fields;
        std::sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        out += "{\n";
        for (qsizetype index = 0; index < fields.size(); ++index) {
            out += indent;
            writeString(out, fields.at(index).first);
            out += ": ";
            write(out, fields.at(index).second, depth + 1);
            out += index + 1 < fields.size() ? ",\n" : "\n";
        }
        out += closing + '}';
        return;
    }
    }
}

// authors_to_string(): " & " between names, a "&" inside one doubled.
QString authorsToString(const QStringList &authors)
{
    QStringList escaped;
    for (const QString &author : authors) {
        if (!author.isEmpty())
            escaped.append(QString(author).replace(u'&', QStringLiteral("&&")));
    }
    return escaped.join(QStringLiteral(" & "));
}

} // namespace

namespace CalibreList {

QByteArray machineList(const LibraryData &data)
{
    QList<const Book *> books;
    for (const Book &book : data.books)
        books.append(&book);
    std::sort(books.begin(), books.end(), [](const Book *a, const Book *b) { return a->id < b->id; });

    PyValue list;
    list.kind = PyValue::Kind::List;
    for (const Book *book : std::as_const(books)) {
        PyValue entry = PyValue::dict();
        entry.insert(QStringLiteral("id"), PyValue::number(book->id));
        if (!book->authorSort.isNull())
            entry.insert(QStringLiteral("author_sort"), PyValue::text(book->authorSort));
        entry.insert(QStringLiteral("authors"), PyValue::text(authorsToString(book->authors)));
        if (!book->comments.isNull())
            entry.insert(QStringLiteral("comments"), PyValue::text(book->comments));
        if (!book->coverFile.isEmpty())
            entry.insert(QStringLiteral("cover"), PyValue::text(book->coverFile));

        QStringList formats;
        qint64 size = -1;
        for (const BookFormat &format : book->formats) {
            if (!format.file.isEmpty())
                formats.append(format.file);
            size = std::max(size, format.size);
        }
        entry.insert(QStringLiteral("formats"), PyValue::list(formats));

        PyValue identifiers = PyValue::dict();
        QString isbn;
        for (const auto &identifier : book->identifiers) {
            identifiers.insert(identifier.first, PyValue::text(identifier.second));
            if (identifier.first == QLatin1String("isbn"))
                isbn = identifier.second;
        }
        entry.insert(QStringLiteral("identifiers"), identifiers);
        entry.insert(QStringLiteral("isbn"), PyValue::text(isbn));
        entry.insert(QStringLiteral("languages"), PyValue::list(book->languages));
        entry.insert(QStringLiteral("last_modified"), PyValue::text(CalibreDb::isoFormat(book->lastModified)));
        entry.insert(QStringLiteral("pubdate"), PyValue::text(CalibreDb::isoFormat(book->pubdate)));
        if (book->publisherId > 0)
            entry.insert(QStringLiteral("publisher"), PyValue::text(book->publisher));
        if (book->rating > 0)
            entry.insert(QStringLiteral("rating"), PyValue::number(book->rating));
        if (book->seriesId > 0)
            entry.insert(QStringLiteral("series"), PyValue::text(book->series));
        entry.insert(QStringLiteral("series_index"), PyValue::floating(book->seriesIndex));
        if (size >= 0)
            entry.insert(QStringLiteral("size"), PyValue::number(size));
        entry.insert(QStringLiteral("tags"), PyValue::list(book->tags));
        entry.insert(QStringLiteral("timestamp"), PyValue::text(CalibreDb::isoFormat(book->timestamp)));
        entry.insert(QStringLiteral("title"), PyValue::text(book->title));
        if (!book->uuid.isNull())
            entry.insert(QStringLiteral("uuid"), PyValue::text(book->uuid));
        list.items.append(entry);
    }

    QByteArray out;
    write(out, list, 0);
    return out;
}

QByteArray pythonJsonString(const QString &value)
{
    QByteArray out;
    writeString(out, value);
    return out;
}

} // namespace CalibreList
