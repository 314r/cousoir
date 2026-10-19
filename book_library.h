#pragma once

#include <QCollator>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "book.h"
#include "book_edit.h"
#include "calibre_config.h"
#include "commit_engine.h"

// A Calibre library: its books and the sections that browse them by author,
// series, tag, language or publisher. Loading copies metadata.db out of the
// library and reads the copy on a worker thread; a change to the database
// reloads it after a moment. Edits are saved one after the other on a
// worker thread through CommitEngine, while `writable` says the library may
// be written.
class BookLibrary : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY libraryPathChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY libraryChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryUuid READ libraryUuid NOTIFY libraryChanged)
    Q_PROPERTY(int schemaVersion READ schemaVersion NOTIFY libraryChanged)
    Q_PROPERTY(bool schemaKnown READ schemaKnown NOTIFY libraryChanged)
    // { books, authors, series, tags, languages, publishers }.
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY libraryChanged)
    Q_PROPERTY(QString section READ section WRITE setSection NOTIFY sectionChanged)
    // The section's item the book list is narrowed to: { kind, id, name },
    // or empty.
    Q_PROPERTY(QVariantMap filter READ filter NOTIFY filterChanged)
    // The books of the section, or of the filter, before the search.
    Q_PROPERTY(int scopeCount READ scopeCount NOTIFY rowsChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    // The items of a browse section: { id, name, count }.
    Q_PROPERTY(QVariantList facetRows READ facetRows NOTIFY facetRowsChanged)
    Q_PROPERTY(int facetCount READ facetCount NOTIFY facetRowsChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString sortOption READ sortOption WRITE setSortOption NOTIFY sortOptionChanged)
    Q_PROPERTY(QString facetSortOption READ facetSortOption WRITE setFacetSortOption NOTIFY facetSortOptionChanged)
    Q_PROPERTY(int currentBookId READ currentBookId WRITE setCurrentBookId NOTIFY currentBookChanged)
    Q_PROPERTY(QVariantMap currentBook READ currentBook NOTIFY currentBookChanged)
    // Set by the session from the guard: edits are refused otherwise.
    Q_PROPERTY(bool writable READ writable WRITE setWritable NOTIFY writableChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    // The current book has changed since it was opened.
    Q_PROPERTY(bool canRevert READ canRevert NOTIFY canRevertChanged)
    // The metadata.db Reliure last wrote, for the guard's ownChangeKey.
    Q_PROPERTY(QString publishedKey READ publishedKey NOTIFY publishedKeyChanged)
    // Calibre's interface language as set here, empty for Calibre's own; and
    // the one in effect. It decides title sorts, the "Unknown" of a book
    // without author, and names in other scripts.
    Q_PROPERTY(QString calibreLanguage READ calibreLanguage WRITE setCalibreLanguage NOTIFY calibreLanguageChanged)
    Q_PROPERTY(QString interfaceLanguage READ interfaceLanguage NOTIFY calibreLanguageChanged)
    Q_PROPERTY(QString backupFolder READ backupFolder NOTIFY libraryPathChanged)

public:
    explicit BookLibrary(QObject *parent = nullptr);
    // Reads the library at `libraryPath` with `config`, instead of Calibre's
    // own library and configuration.
    BookLibrary(const QString &libraryPath, const CalibreConfig &config, QObject *parent = nullptr);
    ~BookLibrary() override;

    // Where the library's copy of metadata.db and its cover thumbnails go:
    // a folder per library under the user's cache.
    static QString cacheFolder(const QString &libraryPath);

    QString libraryPath() const;
    // Another library reloads; empty goes back to the default one.
    void setLibraryPath(const QString &path);
    bool loaded() const;
    bool loading() const;
    QString lastError() const;
    QString libraryUuid() const;
    int schemaVersion() const;
    bool schemaKnown() const;
    QVariantMap counts() const;
    // "books", "authors", "series", "tags", "languages" or "publishers".
    // Another section clears the filter and the search.
    QString section() const;
    void setSection(const QString &section);
    QVariantMap filter() const;
    int scopeCount() const;
    QVariantList rows() const;
    QVariantList facetRows() const;
    int facetCount() const;
    QString searchText() const;
    void setSearchText(const QString &searchText);
    // "title", "authors", "series", "added" or "rating", with "Desc" for the
    // reverse order.
    QString sortOption() const;
    void setSortOption(const QString &sortOption);
    // "name" or "count", with "Desc" for the reverse order.
    QString facetSortOption() const;
    void setFacetSortOption(const QString &sortOption);
    int currentBookId() const;
    void setCurrentBookId(int bookId);
    QVariantMap currentBook() const;
    const LibraryData &data() const;
    bool writable() const;
    void setWritable(bool writable);
    bool saving() const;
    bool canRevert() const;
    QString publishedKey() const;
    QString calibreLanguage() const;
    void setCalibreLanguage(const QString &language);
    QString interfaceLanguage() const;
    QString backupFolder() const;

    // Narrows the books to the item with `id` of the current browse section,
    // clearing the search.
    Q_INVOKABLE void openFacetItem(int id);
    // Back to the section's items, with the search cleared.
    Q_INVOKABLE void clearFilter();
    // Opens a book's file with the desktop's application for it: `format`,
    // or the first of EPUB, KEPUB, AZW3, MOBI and PDF it has. Returns an
    // error message, empty on success.
    Q_INVOKABLE QString openFormat(int bookId, const QString &format = {});
    // Shows the book's folder in the file manager. Returns an error message,
    // empty on success.
    Q_INVOKABLE QString openFolder(int bookId);
    // Reads the library again now.
    Q_INVOKABLE void reload();

    // Saves `changes` to the book, BookEdit's keys, after the edits before
    // it. The title sort and author sort follow the title and authors when
    // they followed them before, as in Calibre's editor; the title sort uses
    // the book's language. Returns an error message for a value it refuses,
    // empty once the edit is on its way: saved() or saveFailed() follows.
    Q_INVOKABLE QString updateBook(int bookId, const QVariantMap &changes);
    // The book's cover from an image file, from the image on the clipboard,
    // or none. Same returns as updateBook().
    Q_INVOKABLE QString setCover(int bookId, const QString &fileUrl);
    Q_INVOKABLE QString pasteCover(int bookId);
    Q_INVOKABLE QString removeCover(int bookId);
    // Puts the current book back as it was when it was opened.
    Q_INVOKABLE void revertCurrentBook();
    // Whether `text` is a date updateBook() takes for pubdate.
    Q_INVOKABLE bool isDate(const QString &text) const;
    // Shows the folder of backups in the file manager.
    Q_INVOKABLE void openBackupFolder();

signals:
    void libraryPathChanged();
    void libraryChanged();
    void loadingChanged();
    void sectionChanged();
    void filterChanged();
    void rowsChanged();
    void facetRowsChanged();
    void searchTextChanged();
    void sortOptionChanged();
    void facetSortOptionChanged();
    void currentBookChanged();
    // A reload failed while the books read before stay shown.
    void loadFailed(const QString &message);
    void writableChanged();
    void savingChanged();
    void canRevertChanged();
    void publishedKeyChanged();
    void calibreLanguageChanged();
    // An edit went into the library.
    void saved();
    // An edit could not be saved; the books show what the library holds.
    void saveFailed(const QString &message);
    // Something left by an interrupted change was put right.
    void recovered(const QString &message);

private:
    struct LoadResult
    {
        QString path;
        LibraryData data;
        QString error;
        QString recovered;
        bool ok = false;
    };

    void startLoad();
    void finishLoad();
    QString queueEdit(const BookEdit &edit);
    void startCommit();
    void finishCommit();
    void applyData(LibraryData data);
    // What the current book's fields hold, as updateBook() takes them.
    QVariantMap editableValues(const Book &book) const;
    // Keeps the current book as it is before its first change.
    void keepOriginal(const Book &book);
    CalibreConfig effectiveConfig() const;
    void watchLibrary();
    void reloadIfChanged();
    void rebuildRows();
    void rebuildFacetRows();
    void rebuildCurrentBook();
    const Book *findBook(int bookId) const;
    const QList<FacetItem> *facetItems(const QString &kind) const;
    bool inFilter(const Book &book) const;

    QString m_libraryPath;
    bool m_defaultPath = true;
    CalibreConfig m_config;
    LibraryData m_data;
    bool m_loaded = false;
    QString m_lastError;
    QFutureWatcher<LoadResult> m_loadWatcher;
    bool m_reloadPending = false;
    QFileSystemWatcher m_watcher;
    QTimer m_changeTimer;
    QCollator m_collator;

    QString m_section = QStringLiteral("books");
    QString m_filterKind;
    int m_filterId = 0;
    QVariantList m_rows;
    int m_scopeCount = 0;
    QVariantList m_facetRows;
    QString m_searchText;
    QString m_sortOption = QStringLiteral("title");
    QString m_facetSortOption = QStringLiteral("name");
    int m_currentBookId = 0;
    QVariantMap m_currentBook;

    bool m_writable = false;
    QFutureWatcher<CommitEngine::Result> m_commitWatcher;
    QList<BookEdit> m_pendingEdits;
    bool m_backedUp = false;
    QString m_publishedKey;
    QString m_calibreLanguage;
    // The current book as it was opened, kept at its first change.
    int m_originalBookId = 0;
    QVariantMap m_original;
    QByteArray m_originalCover;
};
