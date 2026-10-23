#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "annotation_search.h"

class BookLibrary;

// The library's highlights and bookmarks for the Highlights section, as
// Calibre's "Browse annotations" shows them (AnnotationSearch): grouped by
// book, each book's in their order, those a search of Calibre's full text
// index finds with the words found marked; exported as Calibre exports
// them. Read from the copy of the database the library was read from, again
// each time the library is.
class HighlightBrowser : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    // FTS5's query language, as Calibre's search box takes it.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(bool stemming READ stemming WRITE setStemming NOTIFY queryChanged)
    // "highlight", "bookmark", or empty for both.
    Q_PROPERTY(QString type READ type WRITE setType NOTIFY queryChanged)
    // { kind: "book", bookId, title, authors, count } before each book's
    // { kind: "annotation", index, bookId, format, type, html, notes,
    // chapter, date, color, spineIndex, cfi }, html the text with the words
    // found in bold.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int count READ count NOTIFY rowsChanged)
    // Why the search found nothing: a query FTS5 cannot read.
    Q_PROPERTY(QString error READ error NOTIFY rowsChanged)

public:
    explicit HighlightBrowser(QObject *parent = nullptr);

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    QString query() const;
    void setQuery(const QString &query);
    bool stemming() const;
    void setStemming(bool stemming);
    QString type() const;
    void setType(const QString &type);
    QVariantList rows() const;
    int count() const;
    QString error() const;

    // The annotations shown, or those of one book, as plain text or
    // Markdown, as Calibre's export writes them.
    Q_INVOKABLE QString exportText(bool markdown, int bookId = 0) const;
    Q_INVOKABLE void copy(const QString &text) const;
    // Saves text to a file; an error message, empty on success.
    Q_INVOKABLE QString save(const QString &text, const QUrl &file) const;

signals:
    void libraryChanged();
    void queryChanged();
    void rowsChanged();

private:
    void refresh();
    void search();

    QPointer<BookLibrary> m_library;
    QString m_query;
    bool m_stemming = true;
    QString m_type;
    QList<QPair<int, QList<AnnotationSearch::Hit>>> m_groups;
    QVariantList m_rows;
    int m_count = 0;
    QString m_error;
    QTimer m_pause;
};
