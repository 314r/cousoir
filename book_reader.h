#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "book.h"

class BookLibrary;
class BookSearch;
class BookSpeech;
class EpubBook;

// A library book read in Cousoir: its EPUB served to Qt WebEngine as
// cousoir-book://book-<id>/<name in the archive>, its spine and table of
// contents, and where reading stopped. Positions are CFIs as Calibre's viewer
// writes them, "epubcfi(/6/2/4/2/1:120)": the spine item's step, then the
// path in its document from the document node (see assets/reader/reader.js).
class BookReader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString format READ format NOTIFY bookChanged)
    // { url, name, size } by spine item.
    Q_PROPERTY(QVariantList spine READ spine NOTIFY bookChanged)
    // The size of all its spine items, by which how far into the book a
    // place is counts.
    Q_PROPERTY(qint64 totalSize READ totalSize NOTIFY bookChanged)
    // { title, spineIndex, fragment, depth }.
    Q_PROPERTY(QVariantList toc READ toc NOTIFY bookChanged)
    // Where to open: { spineIndex, cfi, device }, the newest position kept,
    // Cousoir's own or one in the library's, of Calibre's viewer for
    // instance.
    Q_PROPERTY(QVariantMap startPosition READ startPosition NOTIFY bookChanged)
    // A PDF's file, which Qt Pdf shows.
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY bookChanged)
    // The script that pages a document and finds positions in it.
    Q_PROPERTY(QString script READ script CONSTANT)
    // The highlights of Calibre's viewer and Cousoir's in the book, and the
    // Kobos': { uuid, spineIndex, start, end, text, notes, chapter, which,
    // timestamp, source, sourceName }, source "kobo:SERIAL" for a Kobo's, in
    // the book's order.
    Q_PROPERTY(QVariantList highlights READ highlights NOTIFY highlightsChanged)
    // The bookmarks of Calibre's viewer and Cousoir's in the book: { title,
    // cfi, spineIndex, timestamp }, in the book's order.
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    // The places the last search found, in the book's order: { spineIndex,
    // start, end, text, before, after, chapter, newChapter }, start and end
    // CFIs within the spine item, newChapter for the first of a chapter; at
    // most 1000, searchMore telling there were more.
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(bool searchMore READ searchMore NOTIFY searchChanged)
    // The book read aloud (BookSpeech); null when Cousoir is built without
    // Qt TextToSpeech.
    Q_PROPERTY(QObject *speech READ speech CONSTANT)

public:
    explicit BookReader(QObject *parent = nullptr);
    ~BookReader() override;

    // Before the application is made: the cousoir-book scheme and Qt
    // WebEngine; then ready() tells it was done.
    static void initialize();
    static bool ready();

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    int bookId() const;
    QString title() const;
    QString format() const;
    QVariantList spine() const;
    qint64 totalSize() const;
    QVariantList toc() const;
    QVariantMap startPosition() const;
    QString script() const;
    QVariantList highlights() const;
    QVariantList bookmarks() const;
    QUrl fileUrl() const;
    QVariantList searchResults() const;
    QString searchQuery() const;
    bool searching() const;
    bool searchMore() const;
    QObject *speech() const;

    // Opens the book's EPUB. Returns why it cannot, empty once open.
    Q_INVOKABLE QString open(int bookId);
    // Opens the book's PDF; startPosition then has its { page }.
    Q_INVOKABLE QString openPdf(int bookId);
    // Keeps the page of a PDF being read.
    Q_INVOKABLE void savePage(int page, int pages);
    Q_INVOKABLE void close();
    // The spine item of an address of the book, -1 for another.
    Q_INVOKABLE int spineIndexOf(const QUrl &url) const;
    // How far into the book a position in a spine item is, from 0 to 1.
    Q_INVOKABLE double bookFraction(int spineIndex, double fileFraction) const;
    // Where a place so far into the book is: { spineIndex, fileFraction },
    // spineIndex -1 for a book with nothing to read.
    Q_INVOKABLE QVariantMap locate(double bookFraction) const;
    // Keeps where reading is: a CFI in the spine item, and how far into it.
    Q_INVOKABLE void savePosition(int spineIndex, const QString &cfi, double fileFraction);
    // A highlight of the text from `start` to `end`, CFIs within the spine
    // item, in Calibre's colour `which` ("yellow", "green", "blue", "red",
    // "purple"), kept where Calibre's viewer keeps its own. Returns its id.
    Q_INVOKABLE QString addHighlight(int spineIndex, const QString &start, const QString &end, const QString &text,
                                     const QString &which = QStringLiteral("yellow"));
    // As Calibre's viewer removes one: kept, marked removed, for the
    // programs that have it to forget it too.
    Q_INVOKABLE void removeHighlight(const QString &uuid);
    // A highlight's colour, one of Calibre's five, as the viewer's
    // set_style_for_highlights() changes it.
    Q_INVOKABLE void setHighlightStyle(const QString &uuid, const QString &which);
    // A highlight's note, none when empty, as the viewer's
    // set_notes_for_highlight() sets it; its time too, so that Calibre's
    // viewer, merging, takes it.
    Q_INVOKABLE void setHighlightNotes(const QString &uuid, const QString &notes);
    // A bookmark at a CFI of the book, as the viewer's add_bookmark() adds
    // one: one of the same title is replaced.
    Q_INVOKABLE void addBookmark(const QString &title, const QString &cfi);
    // As the viewer's remove_bookmark(): kept, marked removed.
    Q_INVOKABLE void removeBookmark(const QString &title);
    // The viewer's edit_bookmark(); false when the new title is empty or
    // another bookmark's. The library keeps bookmarks by their titles: the
    // old one is written as removed.
    Q_INVOKABLE bool renameBookmark(const QString &title, const QString &newTitle);
    // Puts text on the clipboard.
    Q_INVOKABLE void copyText(const QString &text) const;
    // The note an address of the book points to, for a box over the page:
    // { html, text }, html for a Text's rich text, the element it names or
    // the block it is in, its italics, bold and raised text kept, a link
    // back left out. Empty when it names nothing, or more than a note.
    Q_INVOKABLE QVariantMap note(const QUrl &url) const;
    // The address of a picture of the book for QML's Image, through the
    // provider "book" the reader gives its engine.
    Q_INVOKABLE QUrl imageUrl(const QUrl &url) const;
    // Searches the book's text, case and accents ignored, on a thread of its
    // own; searchResults has what it finds. An empty query clears them.
    Q_INVOKABLE void search(const QString &query);

signals:
    void libraryChanged();
    void bookChanged();
    void highlightsChanged();
    void bookmarksChanged();
    void searchChanged();

private:
    QString positionsFile() const;
    QJsonObject readPositions() const;
    void writePositions(const QJsonObject &positions) const;
    void queueAnnotation(const Annotation &annotation);
    // A highlight changed, kept as it is now, as whose it is.
    void queueHighlight(const QString &uuid);
    void queueBookmark(const QJsonObject &bookmark);
    void keepPending() const;
    // The highlights of the book `uuid` still to be in the library.
    void keepPending(const QString &uuid) const;
    // A change on its way went in, or did not: what it wrote waits no more,
    // or waits still.
    void finishSave(int id, bool saved);
    // Writes the pending highlights to the library, with the position when
    // `withPosition`.
    void flush(bool withPosition);

    QPointer<BookLibrary> m_library;
    std::shared_ptr<EpubBook> m_book;
    int m_bookId = 0;
    QString m_uuid;
    QString m_format;
    QString m_token;
    QString m_pdfPath;
    QString m_pdfTitle;
    QVariantMap m_start;
    QString m_lastCfi;
    double m_lastFraction = 0;
    // By uuid, as Calibre's viewer keeps them, and whose: the viewer's,
    // "local" and "viewer", or a Kobo's, "kobo" and its serial.
    QMap<QString, QJsonObject> m_highlights;
    QHash<QString, QPair<QString, QString>> m_owners;
    // By title, as the viewer keeps them.
    QMap<QString, QJsonObject> m_bookmarks;
    // The Kobos' names, by serial.
    QHash<QString, QString> m_koboNames;
    // Those made or removed and not yet in the library, written together a
    // while after the last, or when the book closes; kept with the
    // positions meanwhile, should the library not be writable, until the
    // library says they are in it.
    QList<Annotation> m_pending;
    // By change on its way to the library: the book and what it writes.
    QHash<int, QPair<QString, QList<Annotation>>> m_saving;
    QTimer m_flushTimer;
    std::shared_ptr<BookSearch> m_search;
    BookSpeech *m_speech = nullptr;
    QVariantList m_searchResults;
    QString m_searchQuery;
    bool m_searchMore = false;
    // The search running, if any: a newer one, or another book, makes its
    // answer stale.
    int m_searchRun = 0;
    bool m_searching = false;
};
