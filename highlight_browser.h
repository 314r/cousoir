#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

#include "annotation_search.h"
#include "highlight_formats.h"
#include "highlight_sync.h"
#include "kobo_sync.h"

class BookLibrary;

// The library's highlights and notes for the Highlights section, as
// kbextract2 shows a Kobo's: the books that have some, and the one picked
// as a page of its own, by chapter. Calibre's viewer's, Cousoir's reader's
// and the Kobos' (AnnotationSearch), read from the copy of the database the
// library was read from, again each time the library is. With them, what
// the Kobos have that the library cannot hold: highlights not found in the
// library's copy, with their book, and books the library has not, after the
// library's. Copied as kbextract2 writes them.
class HighlightBrowser : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(HighlightSync *sync READ sync WRITE setSync NOTIFY syncChanged)
    // { key, bookId, title, authors, counts, cover, monogram, tint, section }
    // of each book with highlights or notes: the library's by title, then
    // those of a Kobo the library has not, their section "On <Kobo>, not in
    // the library". Without a cover, kbextract2's tile: the title's first
    // letter or digit on colour `tint`, 0 to 5, the same for the same title.
    Q_PROPERTY(QVariantList books READ books NOTIFY booksChanged)
    // The key of the book shown, empty for none.
    Q_PROPERTY(QString current READ current WRITE setCurrent NOTIFY currentChanged)
    // The book shown, as kbextract2 lays it out: { title, authors, counts,
    // chapters: [{ title, entries: [{ paragraphs, note }] }] }, the chapters
    // and their entries as Copy writes them; empty for none.
    Q_PROPERTY(QVariantMap document READ document NOTIFY documentChanged)
    // The highlights and notes of all the books.
    Q_PROPERTY(int count READ count NOTIFY booksChanged)
    // Why there are none: a copy of the database that cannot be read.
    Q_PROPERTY(QString error READ error NOTIFY booksChanged)

public:
    explicit HighlightBrowser(QObject *parent = nullptr);

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    HighlightSync *sync() const;
    void setSync(HighlightSync *sync);
    QVariantList books() const;
    QString current() const;
    void setCurrent(const QString &current);
    QVariantMap document() const;
    int count() const;
    QString error() const;

    // The highlights of the book of `key`, the current one's when empty, as
    // kbextract2 writes them: "markdown", "obsidian" or "plain".
    Q_INVOKABLE QString formatted(const QString &format, const QString &key = {}) const;
    Q_INVOKABLE void copy(const QString &text) const;

signals:
    void libraryChanged();
    void syncChanged();
    void booksChanged();
    void currentChanged();
    void documentChanged();

private:
    // A book shown: the library's, its highlights found and those of a Kobo
    // not found in it, or a Kobo's the library has not.
    struct Group
    {
        QString key;
        int bookId = 0;
        QString title;
        QStringList authors;
        QList<AnnotationSearch::Hit> hits;
        QList<KoboHighlights::Highlight> left;
        QString section;
    };

    void refresh();
    void buildDocument();
    const Group *groupOf(const QString &key) const;
    QList<HighlightFormats::Entry> entriesOf(const Group &group) const;

    QPointer<BookLibrary> m_library;
    QPointer<HighlightSync> m_sync;
    QList<Group> m_groups;
    QVariantList m_books;
    QString m_current;
    QVariantMap m_document;
    int m_count = 0;
    QString m_error;
};
