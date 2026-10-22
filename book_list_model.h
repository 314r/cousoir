#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QList>
#include <QString>
#include <QVariantMap>

#include "book.h"

// How the book list and the book's page show a book's values.
namespace BookDisplay {

// fmt_sidx(): "2" for a whole series index, "2.5" otherwise.
QString seriesIndex(double index);
// "Name [2]", or empty for a book in no series.
QString series(const Book &book);
// rating_to_stars(): two ratings make a star, an odd rating ends in a half.
QString rating(int rating);
// The local date, "2015-07-07"; empty for Calibre's undefined date.
QString date(const QDateTime &date);

} // namespace BookDisplay

// The books the list shows, in their order: one row per book, whose single
// role, `modelData` in a delegate, is a map of what the list shows of it,
// { id, title, authors, series, formats, added, rating, cover, selected },
// made when a row is shown. The rows point into the library's books, which
// the model shares rather than copies. The books selected are kept by id:
// a sort keeps them, a search drops those it hides.
class BookListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // The ids of the books selected, in the list's order.
    Q_PROPERTY(QVariantList selectedIds READ selectedIds NOTIFY selectionChanged)

public:
    explicit BookListModel(QObject *parent = nullptr);

    // Shows `books` in `order`, indexes into them, with covers under
    // `coverKey` in CoverCache. The same books in the same order only tell
    // the views that their values changed, keeping their place. Returns
    // whether the rows changed.
    bool setRows(const QList<Book> &books, QList<qsizetype> order, const QString &coverKey);

    int count() const;
    // The ids of the rows, in order.
    QList<int> ids() const;

    // The row's map, empty past the end.
    Q_INVOKABLE QVariantMap get(int row) const;
    // The row's book id, 0 past the end.
    Q_INVOKABLE int idAt(int row) const;
    // The row of the book, -1 when it is not shown.
    Q_INVOKABLE int indexOf(int bookId) const;

    int selectedCount() const;
    // Selects the row: "only" it, "toggle" it in the selection, or the
    // "range" from the last row selected alone or toggled to it.
    Q_INVOKABLE void select(int row, const QString &mode = QStringLiteral("only"));
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE bool isSelected(int row) const;
    QVariantList selectedIds() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();
    void selectionChanged();

private:
    QList<Book> m_books;
    QList<qsizetype> m_order;
    QString m_coverKey;
    // Row by book id, made when first asked.
    mutable QHash<int, int> m_rowOf;
    QSet<int> m_selected;
    // The book a range starts from.
    int m_anchor = 0;

    // Tells the views the rows whose selection changed.
    void selectionUpdated(const QSet<int> &before);
};
