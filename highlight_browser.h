#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "annotation_search.h"
#include "highlight_formats.h"
#include "highlight_sync.h"
#include "kobo_sync.h"

class BookLibrary;

// The library's highlights and bookmarks for the Highlights section, as
// Calibre's "Browse annotations" shows them (AnnotationSearch): grouped by
// book, each book's in their order, those a search of Calibre's full text
// index finds with the words found marked; exported as Calibre exports
// them. Read from the copy of the database the library was read from, again
// each time the library is. Each says whose it is: Calibre's viewer and
// Cousoir's reader, or a Kobo's. After them, what the Kobos have that the
// library cannot hold: highlights not found in the library's copy, books
// the library has not. All copied as kbextract2 writes them.
class HighlightBrowser : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(HighlightSync *sync READ sync WRITE setSync NOTIFY syncChanged)
    // FTS5's query language, as Calibre's search box takes it.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(bool stemming READ stemming WRITE setStemming NOTIFY queryChanged)
    // "highlight", "bookmark", or empty for both.
    Q_PROPERTY(QString type READ type WRITE setType NOTIFY queryChanged)
    // Whose: "kobo:N418000000000", "local:viewer", or empty for all.
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY queryChanged)
    // { key, name } of each source there is, all first.
    Q_PROPERTY(QVariantList sources READ sources NOTIFY rowsChanged)
    // { kind: "book", key, bookId, title, authors, count, counts } before
    // each book's { kind: "annotation", index, key, bookId, format, type,
    // html, notes, chapter, newChapter, date, color, spineIndex, cfi,
    // source, sourceName, placed, reason }, html the text with the words
    // found in bold, newChapter the chapter when it is not the one before's;
    // a Kobo's books the library has not after { kind: "heading", title }.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int count READ count NOTIFY rowsChanged)
    // Why the search found nothing: a query FTS5 cannot read.
    Q_PROPERTY(QString error READ error NOTIFY rowsChanged)

public:
    explicit HighlightBrowser(QObject *parent = nullptr);

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    HighlightSync *sync() const;
    void setSync(HighlightSync *sync);
    QString query() const;
    void setQuery(const QString &query);
    bool stemming() const;
    void setStemming(bool stemming);
    QString type() const;
    void setType(const QString &type);
    QString source() const;
    void setSource(const QString &source);
    QVariantList sources() const;
    QVariantList rows() const;
    int count() const;
    QString error() const;

    // The annotations shown, or those of one book, as plain text or
    // Markdown, as Calibre's export writes them.
    Q_INVOKABLE QString exportText(bool markdown, int bookId = 0) const;
    // The highlights shown, or those of the book of `key`, as kbextract2
    // writes them: "markdown", "obsidian" or "plain".
    Q_INVOKABLE QString formatted(const QString &format, const QString &key = {}) const;
    Q_INVOKABLE void copy(const QString &text) const;
    // Saves text to a file; an error message, empty on success.
    Q_INVOKABLE QString save(const QString &text, const QUrl &file) const;

signals:
    void libraryChanged();
    void syncChanged();
    void queryChanged();
    void rowsChanged();

private:
    // A book shown: the library's, its highlights found and those of a Kobo
    // not found in it, or a Kobo's the library has not.
    struct Left
    {
        QString device;
        KoboHighlights::Highlight highlight;
        QString reason;
    };
    struct Group
    {
        QString key;
        int bookId = 0;
        QString title;
        QStringList authors;
        QList<AnnotationSearch::Hit> hits;
        QList<Left> left;
    };

    void refresh();
    void search();
    // Whether a highlight left out matches the query and the source.
    bool shows(const QString &device, const KoboHighlights::Highlight &highlight) const;
    QString sourceName(const QString &userType, const QString &user) const;
    void appendGroup(const Group &group, int *index);

    QPointer<BookLibrary> m_library;
    QPointer<HighlightSync> m_sync;
    QString m_query;
    bool m_stemming = true;
    QString m_type;
    QString m_source;
    QList<Group> m_groups;
    QVariantList m_sources;
    QVariantList m_rows;
    int m_count = 0;
    QString m_error;
    QTimer m_pause;
};
