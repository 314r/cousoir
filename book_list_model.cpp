// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/__init__.py: fmt_sidx() and rating_to_stars()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_list_model.h"

#include <QLocale>

#include <cmath>

#include "calibre_db.h"
#include "calibre_text.h"
#include "cover_cache.h"
#include "description.h"
#include "python_json.h"

namespace BookDisplay {

QString seriesIndex(double index)
{
    if (index == std::floor(index))
        return QString::number(qint64(index));
    QString text = QString::number(index, 'f', 2);
    while (text.endsWith(u'0'))
        text.chop(1);
    return text;
}

QString series(const Book &book)
{
    return book.seriesId > 0 ? BookListModel::tr("%1 [%2]").arg(book.series, seriesIndex(book.seriesIndex))
                             : QString();
}

QString rating(int rating)
{
    return QString(rating / 2, u'★') + (rating % 2 ? QStringLiteral("½") : QString());
}

QString date(const QDateTime &date)
{
    return CalibreDb::isUndefinedDate(date) ? QString() : date.toLocalTime().date().toString(Qt::ISODate);
}

namespace {

// A description, or a custom column of comments, as one line of text.
QString oneLine(const QString &html)
{
    return Description::toText(html).simplified();
}

QString customField(const Book &book, const CustomColumn &column, qsizetype index)
{
    const CustomValue value = book.custom.value(index);
    if (value.value.isNull())
        return {};
    const QString &type = column.datatype;
    if (column.multiple) {
        const PythonJson::Value display = PythonJson::parse(column.display);
        const PythonJson::Value *names = display.find(QStringLiteral("is_names"));
        const bool areNames = names && PythonJson::truthy(*names);
        if (type == QLatin1String("composite"))
            return value.value.toString();
        return value.value.toStringList().join(areNames ? QStringLiteral(" & ") : QStringLiteral(", "));
    }
    if (type == QLatin1String("series"))
        return BookListModel::tr("%1 [%2]").arg(value.value.toString(), seriesIndex(value.seriesIndex));
    if (type == QLatin1String("rating"))
        return rating(value.value.toInt());
    if (type == QLatin1String("bool"))
        return value.value.toBool() ? BookListModel::tr("Yes") : BookListModel::tr("No");
    if (type == QLatin1String("datetime"))
        return date(value.value.toDateTime());
    if (type == QLatin1String("float"))
        return PythonJson::floatRepr(value.value.toDouble());
    if (type == QLatin1String("comments"))
        return oneLine(value.value.toString());
    return value.value.toString();
}

} // namespace

QString field(const Book &book, const QString &key, const QList<CustomColumn> &columns)
{
    if (key.startsWith(u'#')) {
        for (qsizetype index = 0; index < columns.size(); ++index) {
            if (u'#' + columns.at(index).label == key)
                return customField(book, columns.at(index), index);
        }
        return {};
    }
    if (key == QLatin1String("title"))
        return book.title;
    if (key == QLatin1String("authors"))
        return book.authors.join(QStringLiteral(" & "));
    if (key == QLatin1String("series"))
        return series(book);
    if (key == QLatin1String("series_index"))
        return seriesIndex(book.seriesIndex);
    if (key == QLatin1String("rating"))
        return rating(book.rating);
    if (key == QLatin1String("publisher"))
        return book.publisher;
    if (key == QLatin1String("tags"))
        return book.tags.join(QStringLiteral(", "));
    if (key == QLatin1String("languages")) {
        QStringList names;
        for (const QString &code : book.languages)
            names.append(CalibreText::languageName(code));
        return names.join(QStringLiteral(", "));
    }
    if (key == QLatin1String("formats")) {
        QStringList formats;
        for (const BookFormat &format : book.formats)
            formats.append(format.format);
        return formats.join(QStringLiteral(", "));
    }
    if (key == QLatin1String("identifiers")) {
        QStringList identifiers;
        for (const auto &[type, value] : book.identifiers)
            identifiers.append(type + u':' + value);
        return identifiers.join(QStringLiteral(", "));
    }
    if (key == QLatin1String("timestamp") || key == QLatin1String("added"))
        return date(book.timestamp);
    if (key == QLatin1String("last_modified"))
        return date(book.lastModified);
    if (key == QLatin1String("pubdate"))
        return CalibreDb::isUndefinedDate(book.pubdate)
            ? QString() : QLocale::c().toString(book.pubdate.toLocalTime().date(), QStringLiteral("MMM yyyy"));
    if (key == QLatin1String("size")) {
        qint64 largest = 0;
        for (const BookFormat &format : book.formats)
            largest = std::max(largest, format.size);
        return book.formats.isEmpty() ? QString()
                                      : QLocale::c().formattedDataSize(largest, 1, QLocale::DataSizeTraditionalFormat);
    }
    if (key == QLatin1String("pages"))
        return book.pages > 0 ? QString::number(book.pages) : QString();
    if (key == QLatin1String("id"))
        return QString::number(book.id);
    if (key == QLatin1String("path"))
        return book.path;
    if (key == QLatin1String("uuid"))
        return book.uuid;
    if (key == QLatin1String("comments"))
        return oneLine(book.comments);
    return {};
}

} // namespace BookDisplay

BookListModel::BookListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

bool BookListModel::setRows(const QList<Book> &books, QList<qsizetype> order, const QString &coverKey)
{
    const bool sameRows = order.size() == m_order.size() && coverKey == m_coverKey
        && std::equal(order.cbegin(), order.cend(), m_order.cbegin(), [&](qsizetype a, qsizetype b) {
               return books.at(a).id == m_books.at(b).id;
           });
    if (sameRows) {
        const bool sameBooks = books.constData() == m_books.constData() && books.size() == m_books.size();
        m_books = books;
        m_order = std::move(order);
        if (!sameBooks && !m_order.isEmpty())
            emit dataChanged(index(0), index(int(m_order.size()) - 1));
        return false;
    }
    const int before = count();
    const qsizetype selected = m_selected.size();
    beginResetModel();
    m_books = books;
    m_order = std::move(order);
    m_coverKey = coverKey;
    m_rowOf.clear();
    // The books the list no longer shows are no longer selected.
    QSet<int> shown;
    for (const qsizetype index : std::as_const(m_order))
        shown.insert(m_books.at(index).id);
    m_selected.intersect(shown);
    endResetModel();
    if (count() != before)
        emit countChanged();
    // Their order may have changed too.
    if (!m_selected.isEmpty() || selected > 0)
        emit selectionChanged();
    return true;
}

int BookListModel::count() const
{
    return int(m_order.size());
}

QList<int> BookListModel::ids() const
{
    QList<int> result;
    result.reserve(m_order.size());
    for (const qsizetype index : m_order)
        result.append(m_books.at(index).id);
    return result;
}

QVariantMap BookListModel::get(int row) const
{
    if (row < 0 || row >= count())
        return {};
    const Book &book = m_books.at(m_order.at(row));
    QStringList formats;
    for (const BookFormat &format : book.formats)
        formats.append(format.format);
    QVariantMap values{
        { QStringLiteral("id"), book.id },
        { QStringLiteral("title"), book.title },
        { QStringLiteral("authors"), book.authors.join(QStringLiteral(" & ")) },
        { QStringLiteral("series"), BookDisplay::series(book) },
        { QStringLiteral("formats"), formats.join(QStringLiteral(", ")) },
        { QStringLiteral("added"), BookDisplay::date(book.timestamp) },
        { QStringLiteral("rating"), BookDisplay::rating(book.rating) },
        { QStringLiteral("cover"), book.coverFile.isEmpty()
              ? QString() : CoverCache::url(m_coverKey, book.id, book.coverModified) },
        { QStringLiteral("selected"), m_selected.contains(book.id) },
    };
    for (const QString &key : m_columns) {
        if (!values.contains(key))
            values.insert(key, BookDisplay::field(book, key, m_customColumns));
    }
    return values;
}

void BookListModel::setColumns(const QStringList &keys, const QList<CustomColumn> &customColumns)
{
    if (keys == m_columns && customColumns.size() == m_customColumns.size())
        return;
    m_columns = keys;
    m_customColumns = customColumns;
    if (!m_order.isEmpty())
        emit dataChanged(index(0), index(int(m_order.size()) - 1));
}

int BookListModel::idAt(int row) const
{
    return row >= 0 && row < count() ? m_books.at(m_order.at(row)).id : 0;
}

int BookListModel::indexOf(int bookId) const
{
    if (m_rowOf.isEmpty() && !m_order.isEmpty()) {
        m_rowOf.reserve(m_order.size());
        for (qsizetype row = 0; row < m_order.size(); ++row)
            m_rowOf.insert(m_books.at(m_order.at(row)).id, int(row));
    }
    return m_rowOf.value(bookId, -1);
}

int BookListModel::selectedCount() const
{
    return int(m_selected.size());
}

void BookListModel::select(int row, const QString &mode)
{
    const int id = idAt(row);
    if (!id)
        return;
    const QSet<int> before = m_selected;
    if (mode == QLatin1String("toggle")) {
        if (!m_selected.remove(id))
            m_selected.insert(id);
        m_anchor = id;
    } else if (mode == QLatin1String("range") && indexOf(m_anchor) >= 0) {
        const int from = indexOf(m_anchor);
        m_selected.clear();
        for (int i = std::min(from, row); i <= std::max(from, row); ++i)
            m_selected.insert(idAt(i));
    } else {
        m_selected = { id };
        m_anchor = id;
    }
    selectionUpdated(before);
}

void BookListModel::selectAll()
{
    const QSet<int> before = m_selected;
    for (int row = 0; row < count(); ++row)
        m_selected.insert(idAt(row));
    selectionUpdated(before);
}

bool BookListModel::isSelected(int row) const
{
    return m_selected.contains(idAt(row));
}

QVariantList BookListModel::selectedIds() const
{
    QVariantList ids;
    for (int row = 0; row < count() && ids.size() < m_selected.size(); ++row) {
        if (m_selected.contains(idAt(row)))
            ids.append(idAt(row));
    }
    return ids;
}

void BookListModel::selectionUpdated(const QSet<int> &before)
{
    if (before == m_selected)
        return;
    // One signal for the span of the rows that changed.
    int first = -1;
    int last = -1;
    for (const int id : (before - m_selected) + (m_selected - before)) {
        const int row = indexOf(id);
        if (row < 0)
            continue;
        first = first < 0 ? row : std::min(first, row);
        last = std::max(last, row);
    }
    if (first >= 0)
        emit dataChanged(index(first), index(last));
    emit selectionChanged();
}

int BookListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant BookListModel::data(const QModelIndex &index, int role) const
{
    if (role != Qt::UserRole || !index.isValid() || index.row() >= count())
        return {};
    return get(index.row());
}

QHash<int, QByteArray> BookListModel::roleNames() const
{
    return { { Qt::UserRole, "book" } };
}
