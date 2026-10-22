#include "calibre_list.h"

#include <algorithm>

#include "calibre_db.h"
#include "calibre_text.h"
#include "python_json.h"

namespace CalibreList {

QByteArray machineList(const LibraryData &data)
{
    QList<const Book *> books;
    for (const Book &book : data.books)
        books.append(&book);
    std::sort(books.begin(), books.end(), [](const Book *a, const Book *b) { return a->id < b->id; });

    PythonJson::Value list;
    list.kind = PythonJson::Value::Kind::List;
    for (const Book *book : std::as_const(books)) {
        PythonJson::Value entry = PythonJson::Value::dict();
        entry.insert(QStringLiteral("id"), PythonJson::Value::from(book->id));
        if (!book->authorSort.isNull())
            entry.insert(QStringLiteral("author_sort"), PythonJson::Value::from(book->authorSort));
        entry.insert(QStringLiteral("authors"), PythonJson::Value::from(CalibreText::authorsToString(book->authors)));
        if (!book->comments.isNull())
            entry.insert(QStringLiteral("comments"), PythonJson::Value::from(book->comments));
        if (!book->coverFile.isEmpty())
            entry.insert(QStringLiteral("cover"), PythonJson::Value::from(book->coverFile));

        QStringList formats;
        qint64 size = -1;
        for (const BookFormat &format : book->formats) {
            if (!format.file.isEmpty())
                formats.append(format.file);
            size = std::max(size, format.size);
        }
        entry.insert(QStringLiteral("formats"), PythonJson::Value::from(formats));

        PythonJson::Value identifiers = PythonJson::Value::dict();
        QString isbn;
        for (const auto &identifier : book->identifiers) {
            identifiers.insert(identifier.first, PythonJson::Value::from(identifier.second));
            if (identifier.first == QLatin1String("isbn"))
                isbn = identifier.second;
        }
        entry.insert(QStringLiteral("identifiers"), identifiers);
        entry.insert(QStringLiteral("isbn"), PythonJson::Value::from(isbn));
        entry.insert(QStringLiteral("languages"), PythonJson::Value::from(book->languages));
        entry.insert(QStringLiteral("last_modified"), PythonJson::Value::from(CalibreDb::isoFormat(book->lastModified)));
        entry.insert(QStringLiteral("pubdate"), PythonJson::Value::from(CalibreDb::isoFormat(book->pubdate)));
        if (book->publisherId > 0)
            entry.insert(QStringLiteral("publisher"), PythonJson::Value::from(book->publisher));
        if (book->rating > 0)
            entry.insert(QStringLiteral("rating"), PythonJson::Value::from(book->rating));
        if (book->seriesId > 0)
            entry.insert(QStringLiteral("series"), PythonJson::Value::from(book->series));
        entry.insert(QStringLiteral("series_index"), PythonJson::Value::from(book->seriesIndex));
        if (size >= 0)
            entry.insert(QStringLiteral("size"), PythonJson::Value::from(size));
        entry.insert(QStringLiteral("tags"), PythonJson::Value::from(book->tags));
        entry.insert(QStringLiteral("timestamp"), PythonJson::Value::from(CalibreDb::isoFormat(book->timestamp)));
        entry.insert(QStringLiteral("title"), PythonJson::Value::from(book->title));
        if (!book->uuid.isNull())
            entry.insert(QStringLiteral("uuid"), PythonJson::Value::from(book->uuid));

        // The custom columns as *label, but a composite's, which Calibre
        // computes; a date it has not is "None", any other value missing is
        // left out.
        for (qsizetype index = 0; index < data.customColumns.size(); ++index) {
            const CustomColumn &column = data.customColumns.at(index);
            const QVariant value = book->custom.value(index).value;
            const QString key = u'*' + column.label;
            const QString &type = column.datatype;
            if (type == QLatin1String("composite"))
                continue;
            if (type == QLatin1String("datetime")) {
                entry.insert(key, PythonJson::Value::from(value.isNull() ? QStringLiteral("None")
                                                               : CalibreDb::isoFormat(value.toDateTime())));
                continue;
            }
            if (column.multiple) {
                entry.insert(key, PythonJson::Value::from(value.toStringList()));
                continue;
            }
            if (value.isNull())
                continue;
            if (type == QLatin1String("int") || type == QLatin1String("rating"))
                entry.insert(key, PythonJson::Value::from(value.toLongLong()));
            else if (type == QLatin1String("float"))
                entry.insert(key, PythonJson::Value::from(value.toDouble()));
            else if (type == QLatin1String("bool"))
                entry.insert(key, PythonJson::Value::from(value.toBool()));
            else
                entry.insert(key, PythonJson::Value::from(value.toString()));
        }
        list.items.append(entry);
    }

    // json.dumps(indent=2, sort_keys=True), with ensure_ascii.
    PythonJson::Format format;
    format.indent = 2;
    format.sortKeys = true;
    format.ensureAscii = true;
    return PythonJson::dumps(list, format).toLatin1();
}

QByteArray pythonJsonString(const QString &value)
{
    PythonJson::Format format;
    format.ensureAscii = true;
    return PythonJson::dumps(PythonJson::Value::from(value), format).toLatin1();
}

} // namespace CalibreList
