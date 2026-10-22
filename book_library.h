#pragma once

#include <QCollator>
#include <QCollatorSortKey>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <optional>

#include "book.h"
#include "book_edit.h"
#include "book_list_model.h"
#include "book_operations.h"
#include "calibre_config.h"
#include "calibre_search.h"
#include "commit_engine.h"
#include "library_check.h"

namespace Conversion {
struct Outcome;
}

// A Calibre library: its books and the sections that browse them by author,
// series, tag, language or publisher. Loading copies metadata.db out of the
// library and reads the copy on a worker thread; a change to the database
// reloads it after a moment. Edits are saved one after the other on a
// worker thread through CommitEngine, while `writable` says the library may
// be written. After each load, LibraryCheck looks for duplicates, author
// name variants, missing files and files outside the database, on a worker
// thread too.
class BookLibrary : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY libraryPathChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY libraryChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryUuid READ libraryUuid NOTIFY libraryChanged)
    Q_PROPERTY(bool schemaKnown READ schemaKnown NOTIFY libraryChanged)
    // { books, authors, series, tags, languages, publishers, trash }, within
    // the virtual library.
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    Q_PROPERTY(QString section READ section WRITE setSection NOTIFY sectionChanged)
    // The section's item the book list is narrowed to: { kind, id, name },
    // or empty.
    Q_PROPERTY(QVariantMap filter READ filter NOTIFY filterChanged)
    // The books of the section, or of the filter, before the search.
    Q_PROPERTY(int scopeCount READ scopeCount NOTIFY rowsChanged)
    // The books listed: those of the section or the filter that the search
    // finds, in the sort order.
    Q_PROPERTY(BookListModel *books READ books CONSTANT)
    // The items of a browse section: { id, name, count }.
    Q_PROPERTY(QVariantList facetRows READ facetRows NOTIFY facetRowsChanged)
    Q_PROPERTY(int facetCount READ facetCount NOTIFY facetRowsChanged)
    // Calibre's search language; searchError says why the search cannot
    // run, and nothing is listed then.
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString searchError READ searchError NOTIFY searchErrorChanged)
    // The virtual libraries Calibre keeps in the library, by name, and the
    // one every section is narrowed to, empty for none. The choice is kept
    // per library; until one is made, Calibre's choice for the library's
    // start.
    Q_PROPERTY(QStringList virtualLibraries READ virtualLibraries NOTIFY libraryChanged)
    Q_PROPERTY(QString virtualLibrary READ virtualLibrary WRITE setVirtualLibrary NOTIFY virtualLibraryChanged)
    // The saved searches of the library, { name, query, count }, by name;
    // the section "search:<name>" lists a saved search's books.
    Q_PROPERTY(QVariantList savedSearches READ savedSearches NOTIFY savedSearchesChanged)
    Q_PROPERTY(QString sortOption READ sortOption WRITE setSortOption NOTIFY sortOptionChanged)
    Q_PROPERTY(QString facetSortOption READ facetSortOption WRITE setFacetSortOption NOTIFY facetSortOptionChanged)
    Q_PROPERTY(int currentBookId READ currentBookId WRITE setCurrentBookId NOTIFY currentBookChanged)
    Q_PROPERTY(QVariantMap currentBook READ currentBook NOTIFY currentBookChanged)
    // Set by the session from the guard: edits are refused otherwise.
    Q_PROPERTY(bool writable READ writable WRITE setWritable NOTIFY writableChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    // The current book has changed since it was opened.
    Q_PROPERTY(bool canRevert READ canRevert NOTIFY canRevertChanged)
    // The last bulk edit can be undone: its books' values before it are kept.
    Q_PROPERTY(int bulkUndoCount READ bulkUndoCount NOTIFY bulkUndoChanged)
    // The metadata.db Cousoir last wrote, for the guard's ownChangeKey.
    Q_PROPERTY(QString publishedKey READ publishedKey NOTIFY publishedKeyChanged)
    // Calibre's interface language as set here, empty for Calibre's own; and
    // the one in effect. It decides title sorts, the "Unknown" of a book
    // without author, and names in other scripts.
    Q_PROPERTY(QString calibreLanguage READ calibreLanguage WRITE setCalibreLanguage NOTIFY calibreLanguageChanged)
    Q_PROPERTY(QString interfaceLanguage READ interfaceLanguage NOTIFY calibreLanguageChanged)
    Q_PROPERTY(QString backupFolder READ backupFolder NOTIFY libraryPathChanged)
    // The library's trash, newest first: { key, bookId, kind ("book" or
    // "formats"), title, author, cover, formats, removed, daysLeft,
    // restorable }.
    Q_PROPERTY(QVariantList trashEntries READ trashEntries NOTIFY trashChanged)
    // The days Calibre keeps an entry in the trash.
    Q_PROPERTY(int trashExpiryDays READ trashExpiryDays NOTIFY trashChanged)
    // Files being read to add them: how many of how many.
    Q_PROPERTY(bool importing READ importing NOTIFY importChanged)
    Q_PROPERTY(int importDone READ importDone NOTIFY importChanged)
    Q_PROPERTY(int importTotal READ importTotal NOTIFY importChanged)
    // What the last check found, but what the user ignored: { count,
    // checked (the time it ran), caseDuplicates, ignored, rows }. A row is
    // { kind, key } and, by kind: "duplicates" { sameTitle, isbns, books:
    // [{ id, title, authors, formats, missing, added, cover, gains }] };
    // "authors" { initials, ids, authors: [{ id, name, count }] };
    // "missing" { bookId, title, authors, formats, noFormats, folderMissing };
    // "outside" { path, folder, size, bookId }.
    Q_PROPERTY(QVariantMap check READ check NOTIFY checkChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

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
    // "books", "authors", "series", "tags", "languages", "publishers",
    // "trash", "check", or "search:<name>" for a saved search's books.
    // Another section clears the filter and the search; the check runs
    // again.
    QString section() const;
    void setSection(const QString &section);
    QVariantMap filter() const;
    int scopeCount() const;
    BookListModel *books();
    QVariantList facetRows() const;
    int facetCount() const;
    QString searchText() const;
    void setSearchText(const QString &searchText);
    QString searchError() const;
    QStringList virtualLibraries() const;
    QString virtualLibrary() const;
    void setVirtualLibrary(const QString &name);
    QVariantList savedSearches() const;
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
    int bulkUndoCount() const;
    QString publishedKey() const;
    QString calibreLanguage() const;
    void setCalibreLanguage(const QString &language);
    QString interfaceLanguage() const;
    QString backupFolder() const;
    QVariantList trashEntries() const;
    int trashExpiryDays() const;
    bool importing() const;
    int importDone() const;
    int importTotal() const;
    QVariantMap check() const;
    bool checking() const;

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
    // Reads the library again now, or the one Calibre opens now when none was
    // picked.
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
    // Edits the books together, BulkEdit's keys, as Calibre's bulk metadata
    // edit does; `bookIds` in the list's order, which numbers a series. Same
    // returns as updateBook().
    Q_INVOKABLE QString updateBooks(const QVariantList &bookIds, const QVariantMap &changes);
    // Puts the books of the last bulk edit back as they were before it, in
    // the fields it could change.
    Q_INVOKABLE void undoBulkEdit();
    // Whether `text` is a date updateBook() takes for pubdate.
    Q_INVOKABLE bool isDate(const QString &text) const;
    // Shows the folder of backups in the file manager.
    Q_INVOKABLE void openBackupFolder();

    // Adds a book for each file, or each book file in a folder, with the
    // metadata Calibre would read from it. The files are read first, on a
    // worker thread; books already in the library by title and authors are
    // then announced by duplicatesFound(), and added or not by
    // addDuplicates(). Returns an error message, empty once the files are
    // being read.
    Q_INVOKABLE QString addBooks(const QVariantList &urls);
    Q_INVOKABLE void addDuplicates(bool add);
    // The file as the book's format of its extension, replacing the book's
    // file of that format. Same returns as updateBook().
    Q_INVOKABLE QString addFormat(int bookId, const QString &fileUrl);
    // The book's file of `format`, or the books, to the library's trash.
    Q_INVOKABLE QString removeFormat(int bookId, const QString &format);
    Q_INVOKABLE QString moveToTrash(const QVariantList &bookIds);
    // A book, or with `formats` some of its formats, back from the trash.
    Q_INVOKABLE QString restoreFromTrash(int bookId, const QStringList &formats = {});

    // The fixes of the check, with updateBook()'s returns. Keeps one copy of
    // a book and moves the others to the trash, with the formats it lacks
    // copied from them first.
    Q_INVOKABLE QString keepDuplicate(int keepId, const QVariantList &otherIds);
    // Files the books of the authors `authorIds` under `keepAuthorId`'s name.
    Q_INVOKABLE QString mergeAuthors(const QVariantList &authorIds, int keepAuthorId);
    // Takes out of the database the book's formats whose file is gone.
    Q_INVOKABLE QString removeMissingFormats(int bookId);
    // Shows a file or folder of the library, relative to it, in the file
    // manager. Returns an error message, empty on success.
    Q_INVOKABLE QString showInFolder(const QString &path);
    // Leaves a finding of the check out, by its row's key, until it changes;
    // showIgnored() brings them all back.
    Q_INVOKABLE void ignoreFinding(const QString &key);
    Q_INVOKABLE void showIgnored();
    // Checks the library again now.
    Q_INVOKABLE void recheck();

    // A conversion's file as the book's `format`, as Calibre's convert
    // action adds it. Same returns as updateBook().
    QString addConversion(int bookId, const QString &format, const Conversion::Outcome &outcome);

signals:
    void libraryPathChanged();
    void libraryChanged();
    void loadingChanged();
    void sectionChanged();
    void filterChanged();
    void rowsChanged();
    void facetRowsChanged();
    void countsChanged();
    void searchTextChanged();
    void searchErrorChanged();
    void virtualLibraryChanged();
    void savedSearchesChanged();
    void sortOptionChanged();
    void facetSortOptionChanged();
    void currentBookChanged();
    // A reload failed while the books read before stay shown.
    void loadFailed(const QString &message);
    void writableChanged();
    void savingChanged();
    void canRevertChanged();
    void bulkUndoChanged();
    void publishedKeyChanged();
    void calibreLanguageChanged();
    // An edit went into the library.
    void saved();
    // An edit could not be saved; the books show what the library holds.
    void saveFailed(const QString &message);
    // Something left by an interrupted change was put right.
    void recovered(const QString &message);
    void trashChanged();
    void importChanged();
    void checkChanged();
    void checkingChanged();
    // Books read to be added are already in the library: their titles,
    // among `total` books read.
    void duplicatesFound(const QStringList &titles, int total);
    // Books went into the library, by id in the order they were added.
    void booksAdded(const QVariantList &bookIds);
    // Files whose own metadata could not be read: they go in with what their
    // names say.
    void importWarning(const QString &message);

private:
    // What the book list needs of each book, by its place in the library's
    // books: what a search compares, and its sort keys. Made with each load,
    // on the worker thread.
    struct BookIndex
    {
        CalibreSearch::Index search;
        std::vector<QCollatorSortKey> titleKeys;
        std::vector<QCollatorSortKey> authorKeys;
        // The sort of the book's series, empty for none.
        std::vector<QCollatorSortKey> seriesKeys;
        QHash<int, qsizetype> rowOf;
    };

    struct LoadResult
    {
        QString path;
        LibraryData data;
        BookIndex index;
        QString error;
        QString recovered;
        bool ok = false;
    };

    // A save, and the index of the books it leaves.
    struct CommitResult
    {
        CommitEngine::Result result;
        BookIndex index;
    };

    struct ImportResult
    {
        QList<NewBook> books;
        QStringList warnings;
    };

    void startLoad();
    void finishLoad();
    QString queueEdit(const BookEdit &edit);
    // A change other than an edit, saved after the ones before it.
    QString queueChange(const CommitEngine::Request &change);
    void finishImport();
    // find_identical_books(): the books with the authors and the title, as
    // Calibre fuzzily compares it, and the languages if any.
    QList<int> identicalBooks(const ImportedMetadata &metadata) const;
    void rebuildTrash();
    // Checks the library once nothing is being saved.
    void scheduleCheck();
    void finishCheck();
    void rebuildCheck();
    void startCommit();
    void finishCommit();
    void applyData(LibraryData data, BookIndex index);
    static BookIndex indexBooks(const LibraryData &data, const CalibreConfig &config);
    // The rows of the virtual library, or all, and the saved searches'
    // counts in it, after a load or another choice.
    void applyVirtualLibrary();
    void rebuildSavedSearches();
    // The rows the search text finds within the virtual library, searched
    // again only when the text or the books change.
    const QBitArray &searchRows();
    // The rows of a saved search, within the virtual library.
    QBitArray savedSearchRows(const QString &name);
    // The items of a browse section, counted within the virtual library:
    // those without a book there are left out.
    QList<FacetItem> visibleItems(const QString &kind) const;
    // The library's books in the sort order, without its direction.
    const QList<qsizetype> &sortedBooks();
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
    BookIndex m_index;
    QString m_virtualLibrary;
    // The library the choice of virtual library was read for.
    QString m_virtualLibraryOf;
    QBitArray m_restriction;
    // searchRows(), the text it is for, and why it could not run.
    QBitArray m_searchRows;
    std::optional<QString> m_searchedText;
    QString m_searchError;
    // The rows of the saved search the section lists.
    QBitArray m_sectionRows;
    QVariantList m_savedSearches;
    // sortedBooks(), and the sort it is for; empty until asked.
    QList<qsizetype> m_sorted;
    QString m_sortedBy;

    QString m_section = QStringLiteral("books");
    QString m_filterKind;
    int m_filterId = 0;
    BookListModel m_bookList;
    int m_scopeCount = 0;
    QVariantList m_facetRows;
    QString m_searchText;
    QString m_sortOption = QStringLiteral("title");
    QString m_facetSortOption = QStringLiteral("name");
    int m_currentBookId = 0;
    QVariantMap m_currentBook;

    bool m_writable = false;
    QFutureWatcher<CommitResult> m_commitWatcher;
    // The changes waiting, in order; edits in a row go together.
    QList<CommitEngine::Request> m_pending;
    QFutureWatcher<ImportResult> m_importWatcher;
    // Books read while some are duplicates, until addDuplicates().
    QList<NewBook> m_importedBooks;
    QList<bool> m_importedDuplicates;
    QVariantList m_trashEntries;
    QFutureWatcher<LibraryCheck::Findings> m_checkWatcher;
    // A check to run once the changes waiting are saved.
    bool m_checkPending = false;
    LibraryCheck::Findings m_findings;
    QDateTime m_checkedAt;
    QVariantMap m_check;
    bool m_backedUp = false;
    QString m_publishedKey;
    QString m_calibreLanguage;
    // The books of the last bulk edit as they were before it.
    QHash<int, QVariantMap> m_bulkOriginals;
    // The current book as it was opened, kept at its first change.
    int m_originalBookId = 0;
    QVariantMap m_original;
    QByteArray m_originalCover;
};
