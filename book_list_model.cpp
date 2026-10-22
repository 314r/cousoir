// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/__init__.py: fmt_sidx() and rating_to_stars()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_list_model.h"

#include <cmath>

#include "calibre_db.h"
#include "cover_cache.h"

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

} // namespace BookDisplay

BookListModel::BookListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void BookListModel::setRows(const QList<Book> &books, QList<qsizetype> order, const QString &coverKey)
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
        return;
    }
    const int before = count();
    beginResetModel();
    m_books = books;
    m_order = std::move(order);
    m_coverKey = coverKey;
    m_rowOf.clear();
    endResetModel();
    if (count() != before)
        emit countChanged();
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
    return {
        { QStringLiteral("id"), book.id },
        { QStringLiteral("title"), book.title },
        { QStringLiteral("authors"), book.authors.join(QStringLiteral(" & ")) },
        { QStringLiteral("series"), BookDisplay::series(book) },
        { QStringLiteral("formats"), formats.join(QStringLiteral(", ")) },
        { QStringLiteral("added"), BookDisplay::date(book.timestamp) },
        { QStringLiteral("rating"), BookDisplay::rating(book.rating) },
        { QStringLiteral("cover"), book.coverFile.isEmpty()
              ? QString() : CoverCache::url(m_coverKey, book.id, book.coverModified) },
    };
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
