// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/db/utils.py: find_identical_books()
// - src/calibre/ebooks/__init__.py: BOOK_EXTENSIONS
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "book_library.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>
#include <QPromise>
#include <QSet>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <numeric>

#include "book_import.h"
#include "calibre_db.h"
#include "calibre_text.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "cover_cache.h"
#include "field_writers.h"
#include "cousoir_paths.h"
#include "description.h"
#include "palette.h"

namespace {

const QStringList &sections()
{
    static const QStringList names = { QStringLiteral("books"), QStringLiteral("authors"), QStringLiteral("series"),
                                       QStringLiteral("tags"), QStringLiteral("languages"),
                                       QStringLiteral("publishers"), QStringLiteral("trash"),
                                       QStringLiteral("check") };
    return names;
}

bool isSortOption(const QString &option)
{
    static const QStringList keys = { QStringLiteral("title"), QStringLiteral("authors"), QStringLiteral("series"),
                                      QStringLiteral("added"), QStringLiteral("rating") };
    for (const QString &key : keys) {
        if (option == key || option == key + QLatin1String("Desc"))
            return true;
    }
    return false;
}

bool isFacetSortOption(const QString &option)
{
    return option == QLatin1String("name") || option == QLatin1String("nameDesc")
        || option == QLatin1String("count") || option == QLatin1String("countDesc");
}

// How the books and items sort: as the system's language sorts them, case
// aside.
QCollator listCollator()
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

QString localDateTime(const QDateTime &date)
{
    return CalibreDb::isUndefinedDate(date) ? QString()
                                            : date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// Calibre shows a publication date as its month: "Apr 2015".
QString monthYear(const QDateTime &date)
{
    return CalibreDb::isUndefinedDate(date) ? QString()
                                            : QLocale::c().toString(date.toLocalTime().date(), QStringLiteral("MMM yyyy"));
}

QString sizeText(qint64 size)
{
    return QLocale::c().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

// A book identifier's label and, where there is one, its page.
QVariantMap identifierMap(const QString &type, const QString &value)
{
    QString label = type;
    QString url;
    const QString key = type.toLower();
    if (key == QLatin1String("isbn")) {
        label = QStringLiteral("ISBN");
    } else if (key == QLatin1String("amazon") || key == QLatin1String("mobi-asin") || key == QLatin1String("asin")) {
        label = QStringLiteral("Amazon");
        url = QStringLiteral("https://www.amazon.com/dp/%1").arg(value);
    } else if (key == QLatin1String("google")) {
        label = QStringLiteral("Google Books");
        url = QStringLiteral("https://books.google.com/books?id=%1").arg(value);
    } else if (key == QLatin1String("goodreads")) {
        label = QStringLiteral("Goodreads");
        url = QStringLiteral("https://www.goodreads.com/book/show/%1").arg(value);
    } else if (key == QLatin1String("doi")) {
        label = QStringLiteral("DOI");
        url = QStringLiteral("https://doi.org/%1").arg(value);
    } else if (key == QLatin1String("uri") || key == QLatin1String("url")) {
        label = QStringLiteral("URL");
        url = value;
    }
    return {
        { QStringLiteral("type"), type },
        { QStringLiteral("label"), label },
        { QStringLiteral("value"), value },
        { QStringLiteral("url"), url },
    };
}

const QStringList &readerFormats()
{
    static const QStringList formats = { QStringLiteral("EPUB"), QStringLiteral("KEPUB"), QStringLiteral("AZW3"),
                                         QStringLiteral("MOBI"), QStringLiteral("PDF") };
    return formats;
}

// The words of a search, each to be found somewhere in a book.
QStringList searchWords(const QString &text)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    return CalibreText::foldForSearch(text).split(spaces, Qt::SkipEmptyParts);
}

bool matchesAll(const QString &haystack, const QStringList &words)
{
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return haystack.contains(word); });
}

// BOOK_EXTENSIONS of ebooks/__init__.py: the files of a folder Calibre adds.
const QSet<QString> &bookExtensions()
{
    static const QSet<QString> extensions = [] {
        QSet<QString> result;
        for (const char *extension :
             { "lrf", "rar", "zip", "rtf", "lit", "txt", "txtz", "text", "htm", "xhtm", "html", "htmlz", "xhtml", "pdf",
               "pdb", "updb", "pdr", "prc", "mobi", "azw", "doc", "epub", "fb2", "fbz", "djv", "djvu", "lrx", "cbr",
               "cb7", "cbz", "cbc", "oebzip", "rb", "imp", "odt", "chm", "tpz", "azw1", "pml", "pmlz", "mbp", "tan",
               "snb", "xps", "oxps", "azw4", "book", "zbf", "pobi", "docx", "docm", "md", "textile", "markdown",
               "ibook", "ibooks", "iba", "azw3", "ps", "kepub", "kfx", "kpf" })
            result.insert(QString::fromLatin1(extension));
        return result;
    }();
    return extensions;
}

QString localPath(const QVariant &url)
{
    const QUrl parsed = url.toUrl();
    if (parsed.isLocalFile())
        return parsed.toLocalFile();
    const QString text = url.toString();
    return text.startsWith(QLatin1String("file:")) ? QUrl(text).toLocalFile() : text;
}

// The files to add: those given, and the book files of the folders given.
QStringList bookFiles(const QVariantList &urls)
{
    QStringList files;
    for (const QVariant &url : urls) {
        const QString path = localPath(url);
        const QFileInfo info(path);
        if (info.isFile()) {
            files.append(info.absoluteFilePath());
        } else if (info.isDir()) {
            QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            QStringList found;
            while (it.hasNext()) {
                const QFileInfo file(it.next());
                if (!file.fileName().startsWith(u'.') && bookExtensions().contains(file.suffix().toLower()))
                    found.append(file.absoluteFilePath());
            }
            found.sort();
            files += found;
        }
    }
    files.removeDuplicates();
    return files;
}

// The key of the library's covers in CoverCache: its cache folder's name.
QString coverKeyOf(const QString &libraryPath)
{
    return QFileInfo(CousoirPaths::cacheFolder(libraryPath)).fileName();
}

} // namespace

BookLibrary::BookLibrary(QObject *parent)
    : BookLibrary(QString(), CalibreConfig::load(), parent)
{
}

BookLibrary::BookLibrary(const QString &libraryPath, const CalibreConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    m_defaultPath = libraryPath.isEmpty();
    m_libraryPath = m_defaultPath ? CalibreConfig::defaultLibraryPath()
                                  : QDir::cleanPath(QFileInfo(libraryPath).absoluteFilePath());
    m_collator = listCollator();
    m_changeTimer.setSingleShot(true);
    m_changeTimer.setInterval(1500);
    connect(&m_changeTimer, &QTimer::timeout, this, &BookLibrary::reloadIfChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &BookLibrary::finishLoad);
    connect(&m_commitWatcher, &QFutureWatcher<CommitResult>::finished, this, &BookLibrary::finishCommit);
    connect(&m_importWatcher, &QFutureWatcher<ImportResult>::finished, this, &BookLibrary::finishImport);
    connect(&m_importWatcher, &QFutureWatcher<ImportResult>::progressValueChanged, this, &BookLibrary::importChanged);
    connect(&m_checkWatcher, &QFutureWatcher<LibraryCheck::Findings>::finished, this, &BookLibrary::finishCheck);
    connect(this, &BookLibrary::libraryChanged, this, &BookLibrary::countsChanged);
    connect(this, &BookLibrary::trashChanged, this, &BookLibrary::countsChanged);
    // QML may still set another library; only the last one loads.
    QMetaObject::invokeMethod(this, &BookLibrary::startLoad, Qt::QueuedConnection);
}

BookLibrary::~BookLibrary()
{
    m_checkWatcher.cancel();
    m_checkWatcher.waitForFinished();
    m_loadWatcher.waitForFinished();
    m_commitWatcher.waitForFinished();
    m_importWatcher.waitForFinished();
}

QString BookLibrary::cacheFolder(const QString &libraryPath)
{
    return CousoirPaths::cacheFolder(libraryPath);
}

QString BookLibrary::libraryPath() const
{
    return m_libraryPath;
}

void BookLibrary::setLibraryPath(const QString &path)
{
    const bool useDefault = path.trimmed().isEmpty();
    const QString cleaned = useDefault ? CalibreConfig::defaultLibraryPath()
                                       : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_defaultPath = useDefault;
    if (m_libraryPath == cleaned)
        return;
    m_libraryPath = cleaned;
    emit libraryPathChanged();
    m_data = {};
    m_index = {};
    m_sorted.clear();
    m_sortedBy.clear();
    m_restriction.clear();
    m_searchedText.reset();
    m_sectionRows.clear();
    m_sectionError.clear();
    m_visibleItems.clear();
    m_loaded = false;
    m_filterKind.clear();
    m_filterId = 0;
    m_currentBookId = 0;
    m_checkWatcher.cancel();
    m_findings = {};
    m_checkedAt = {};
    // The changes waiting are the other library's, as was its backup.
    m_pending.clear();
    m_importedBooks.clear();
    m_importedDuplicates.clear();
    m_backedUp = false;
    m_bulkUndo.clear();
    emit bulkUndoChanged();
    rebuildCheck();
    emit libraryChanged();
    emit filterChanged();
    emit savingChanged();
    rebuildRows();
    rebuildFacetRows();
    rebuildCurrentBook();
    startLoad();
}

bool BookLibrary::loaded() const
{
    return m_loaded;
}

bool BookLibrary::loading() const
{
    return m_loadWatcher.isRunning();
}

QString BookLibrary::lastError() const
{
    return m_lastError;
}

QString BookLibrary::libraryUuid() const
{
    return m_data.uuid;
}

int BookLibrary::schemaVersion() const
{
    return m_data.userVersion;
}

bool BookLibrary::schemaKnown() const
{
    return m_loaded && m_data.schemaKnown;
}

QVariantMap BookLibrary::counts() const
{
    const bool restricted = !m_virtualLibrary.isEmpty() && m_virtualLibraryError.isEmpty();
    const auto items = [&](const char *kind) {
        return restricted ? visibleItems(QLatin1String(kind)).size() : facetItems(QLatin1String(kind))->size();
    };
    return {
        { QStringLiteral("books"), restricted ? m_restriction.count(true) : m_data.books.size() },
        { QStringLiteral("authors"), items("authors") },
        { QStringLiteral("series"), items("series") },
        { QStringLiteral("tags"), items("tags") },
        { QStringLiteral("languages"), items("languages") },
        { QStringLiteral("publishers"), items("publishers") },
        // Nothing for an empty trash.
        { QStringLiteral("trash"), m_trashEntries.isEmpty() ? QVariant(QString()) : QVariant(m_trashEntries.size()) },
    };
}

QString BookLibrary::section() const
{
    return m_section;
}

void BookLibrary::setSection(const QString &section)
{
    const bool savedSearch = section.startsWith(QLatin1String("search:"))
        && m_index.search.preferences().savedSearches.contains(section.mid(7));
    if (m_section == section || !(sections().contains(section) || savedSearch))
        return;
    m_section = section;
    const CalibreSearch::Result found = savedSearch ? this->savedSearch(section.mid(7)) : CalibreSearch::Result();
    m_sectionRows = found.rows;
    m_sectionError = found.error;
    const bool hadFilter = !m_filterKind.isEmpty();
    m_filterKind.clear();
    m_filterId = 0;
    if (!m_searchText.isEmpty()) {
        m_searchText.clear();
        emit searchTextChanged();
    }
    emit sectionChanged();
    if (hadFilter)
        emit filterChanged();
    rebuildRows();
    rebuildFacetRows();
    // Files may have come and gone without the database changing.
    if (m_section == QLatin1String("check"))
        scheduleCheck();
}

QVariantMap BookLibrary::filter() const
{
    if (m_filterKind.isEmpty())
        return {};
    const QList<FacetItem> *items = facetItems(m_filterKind);
    QString name;
    for (const FacetItem &item : *items) {
        if (item.id == m_filterId)
            name = item.name;
    }
    return {
        { QStringLiteral("kind"), m_filterKind },
        { QStringLiteral("id"), m_filterId },
        { QStringLiteral("name"), name },
    };
}

int BookLibrary::scopeCount() const
{
    return m_scopeCount;
}

BookListModel *BookLibrary::books()
{
    return &m_bookList;
}

QVariantList BookLibrary::facetRows() const
{
    return m_facetRows;
}

int BookLibrary::facetCount() const
{
    return int(visibleItems(m_section).size());
}

QString BookLibrary::searchText() const
{
    return m_searchText;
}

void BookLibrary::setSearchText(const QString &searchText)
{
    if (m_searchText == searchText)
        return;
    m_searchText = searchText;
    emit searchTextChanged();
    rebuildRows();
    rebuildFacetRows();
}

QString BookLibrary::searchError() const
{
    return m_searchError;
}

QStringList BookLibrary::virtualLibraries() const
{
    QStringList names = m_index.search.preferences().virtualLibraries.keys();
    std::sort(names.begin(), names.end(), [this](const QString &a, const QString &b) {
        return m_collator.compare(a, b) < 0;
    });
    return names;
}

QString BookLibrary::virtualLibrary() const
{
    return m_virtualLibrary;
}

void BookLibrary::setVirtualLibrary(const QString &name)
{
    const QString chosen = m_index.search.preferences().virtualLibraries.contains(name) ? name : QString();
    if (!m_data.uuid.isEmpty())
        QSettings().setValue(QStringLiteral("Library/virtualLibrary/%1").arg(m_data.uuid), chosen);
    if (chosen == m_virtualLibrary)
        return;
    m_virtualLibrary = chosen;
    emit virtualLibraryChanged();
    applyVirtualLibrary();
    rebuildRows();
    rebuildFacetRows();
}

QVariantList BookLibrary::savedSearches() const
{
    return m_savedSearches;
}

void BookLibrary::applyVirtualLibrary()
{
    m_visibleItems.clear();
    m_restriction = QBitArray(m_data.books.size(), true);
    QString error;
    const QString query = m_index.search.preferences().virtualLibraries.value(m_virtualLibrary);
    if (!m_virtualLibrary.isEmpty()) {
        const CalibreSearch::Result result = CalibreSearch::search(m_index.search, QString(), query);
        // One Cousoir cannot search, a custom column's for instance, leaves
        // the books whole rather than hide them, and says so.
        if (result.ok())
            m_restriction = result.rows;
        else
            error = result.error;
    }
    if (error != m_virtualLibraryError) {
        m_virtualLibraryError = error;
        emit virtualLibraryChanged();
    }
    m_searchedText.reset();
    if (m_section.startsWith(QLatin1String("search:"))) {
        const CalibreSearch::Result found = savedSearch(m_section.mid(7));
        m_sectionRows = found.rows;
        m_sectionError = found.error;
    }
    rebuildSavedSearches();
    emit countsChanged();
}

QString BookLibrary::virtualLibraryError() const
{
    return m_virtualLibraryError;
}

QString BookLibrary::sectionError() const
{
    return m_sectionError;
}

void BookLibrary::rebuildSavedSearches()
{
    const QMap<QString, QString> &saved = m_index.search.preferences().savedSearches;
    QStringList names = saved.keys();
    std::sort(names.begin(), names.end(), [this](const QString &a, const QString &b) {
        return m_collator.compare(a, b) < 0;
    });
    QVariantList rows;
    for (const QString &name : std::as_const(names)) {
        const CalibreSearch::Result found = savedSearch(name);
        rows.append(QVariantMap{
            { QStringLiteral("name"), name },
            { QStringLiteral("query"), saved.value(name) },
            { QStringLiteral("count"), found.ok() ? QVariant(found.rows.count(true)) : QVariant(QString()) },
            { QStringLiteral("error"), found.error },
        });
    }
    if (rows == m_savedSearches)
        return;
    m_savedSearches = rows;
    emit savedSearchesChanged();
}

const QBitArray &BookLibrary::searchRows()
{
    if (m_searchedText == m_searchText && m_searchRows.size() == m_data.books.size())
        return m_searchRows;
    const CalibreSearch::Result result = CalibreSearch::search(m_index.search, m_searchText, m_restriction);
    m_searchRows = result.rows;
    m_searchedText = m_searchText;
    if (m_searchError != result.error) {
        m_searchError = result.error;
        emit searchErrorChanged();
    }
    return m_searchRows;
}

CalibreSearch::Result BookLibrary::savedSearch(const QString &name)
{
    QString quoted = name;
    quoted.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    return CalibreSearch::search(m_index.search, QStringLiteral("search:\"=%1\"").arg(quoted), m_restriction);
}

QList<FacetItem> BookLibrary::visibleItems(const QString &kind) const
{
    const QList<FacetItem> *all = facetItems(kind);
    if (!all)
        return {};
    if (m_virtualLibrary.isEmpty() || !m_virtualLibraryError.isEmpty())
        return *all;
    if (const auto cached = m_visibleItems.constFind(kind); cached != m_visibleItems.cend())
        return *cached;
    QHash<int, int> counts;
    for (qsizetype row = 0; row < m_data.books.size(); ++row) {
        if (row >= m_restriction.size() || !m_restriction.testBit(row))
            continue;
        const Book &book = m_data.books.at(row);
        if (kind == QLatin1String("authors")) {
            for (const int id : book.authorIds)
                ++counts[id];
        } else if (kind == QLatin1String("series")) {
            if (book.seriesId > 0)
                ++counts[book.seriesId];
        } else if (kind == QLatin1String("tags")) {
            for (const int id : book.tagIds)
                ++counts[id];
        } else if (kind == QLatin1String("languages")) {
            for (const int id : book.languageIds)
                ++counts[id];
        } else if (kind == QLatin1String("publishers")) {
            if (book.publisherId > 0)
                ++counts[book.publisherId];
        }
    }
    QList<FacetItem> items;
    for (FacetItem item : *all) {
        item.count = counts.value(item.id);
        if (item.count > 0)
            items.append(item);
    }
    m_visibleItems.insert(kind, items);
    return items;
}

QString BookLibrary::sortOption() const
{
    return m_sortOption;
}

void BookLibrary::setSortOption(const QString &sortOption)
{
    if (m_sortOption == sortOption || !isSortOption(sortOption))
        return;
    m_sortOption = sortOption;
    emit sortOptionChanged();
    rebuildRows();
}

QString BookLibrary::facetSortOption() const
{
    return m_facetSortOption;
}

void BookLibrary::setFacetSortOption(const QString &sortOption)
{
    if (m_facetSortOption == sortOption || !isFacetSortOption(sortOption))
        return;
    m_facetSortOption = sortOption;
    emit facetSortOptionChanged();
    rebuildFacetRows();
}

int BookLibrary::currentBookId() const
{
    return m_currentBookId;
}

void BookLibrary::setCurrentBookId(int bookId)
{
    const int id = findBook(bookId) ? bookId : 0;
    if (m_currentBookId == id)
        return;
    m_currentBookId = id;
    if (m_originalBookId != 0) {
        m_originalBookId = 0;
        m_original.clear();
        m_originalCover.clear();
        emit canRevertChanged();
    }
    rebuildCurrentBook();
}

QVariantMap BookLibrary::currentBook() const
{
    return m_currentBook;
}

const LibraryData &BookLibrary::data() const
{
    return m_data;
}

void BookLibrary::openFacetItem(int id)
{
    const QList<FacetItem> *items = facetItems(m_section);
    if (!items || std::none_of(items->cbegin(), items->cend(), [id](const FacetItem &item) { return item.id == id; }))
        return;
    m_filterKind = m_section;
    m_filterId = id;
    if (!m_searchText.isEmpty()) {
        m_searchText.clear();
        emit searchTextChanged();
        rebuildFacetRows();
    }
    emit filterChanged();
    rebuildRows();
}

void BookLibrary::clearFilter()
{
    if (m_filterKind.isEmpty())
        return;
    m_filterKind.clear();
    m_filterId = 0;
    if (!m_searchText.isEmpty()) {
        m_searchText.clear();
        emit searchTextChanged();
        rebuildFacetRows();
    }
    emit filterChanged();
    rebuildRows();
}

QString BookLibrary::openFormat(int bookId, const QString &format)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    const BookFormat *chosen = nullptr;
    if (!format.isEmpty()) {
        for (const BookFormat &candidate : book->formats) {
            if (candidate.format.compare(format, Qt::CaseInsensitive) == 0)
                chosen = &candidate;
        }
    } else {
        for (const QString &preferred : readerFormats()) {
            for (const BookFormat &candidate : book->formats) {
                if (!chosen && candidate.format == preferred && !candidate.file.isEmpty())
                    chosen = &candidate;
            }
        }
        for (const BookFormat &candidate : book->formats) {
            if (!chosen && !candidate.file.isEmpty())
                chosen = &candidate;
        }
    }
    if (!chosen)
        return book->formats.isEmpty() ? tr("This book has no file to open.") : tr("The book's files are missing.");
    if (chosen->file.isEmpty())
        return tr("The %1 file is missing from the library.").arg(chosen->format);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(chosen->file)))
        return tr("No application opens %1 files.").arg(chosen->format);
    return {};
}

QString BookLibrary::openFolder(int bookId)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    const QString folder = QDir(m_data.libraryPath).filePath(book->path);
    if (!QFileInfo(folder).isDir())
        return tr("The book's folder is missing from the library.");
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder)))
        return tr("No application shows folders.");
    return {};
}

void BookLibrary::reload()
{
    // Calibre may have made its first library since, or opened another.
    if (m_defaultPath && CalibreConfig::defaultLibraryPath() != m_libraryPath)
        setLibraryPath(QString());
    else
        startLoad();
}

void BookLibrary::startLoad()
{
    // A save replaces the snapshot a load writes: one at a time.
    if (m_loadWatcher.isRunning() || m_commitWatcher.isRunning()) {
        m_reloadPending = true;
        return;
    }
    if (m_libraryPath.isEmpty()) {
        m_loaded = false;
        m_lastError = tr("Calibre has no library on this computer yet.");
        emit libraryChanged();
        return;
    }
    const QString path = m_libraryPath;
    const QString cache = cacheFolder(path);
    const CalibreConfig config = effectiveConfig();
    m_loadWatcher.setFuture(QtConcurrent::run([path, cache, config] {
        LoadResult result;
        result.path = path;
        // A change a crash interrupted is put right before anything is read.
        QString error;
        if (!CommitEngine::recover(path, cache, &result.recovered, &error)) {
            result.error = QObject::tr("An interrupted change could not be undone: %1").arg(error);
            return result;
        }
        result.ok = CalibreDb::loadLibrary(path, cache + QStringLiteral("/snapshot.db"), config, &result.data,
                                           &result.error);
        if (result.ok)
            result.index = indexBooks(result.data, config);
        return result;
    }));
    emit loadingChanged();
}

void BookLibrary::finishLoad()
{
    // Taken, not copied: the watcher would keep a whole library alive.
    LoadResult result = m_loadWatcher.future().takeResult();
    emit loadingChanged();
    if (!result.recovered.isEmpty())
        emit recovered(result.recovered);
    // Another library was picked meanwhile: this one is not shown.
    if (result.path != m_libraryPath) {
        m_reloadPending = false;
        startLoad();
        return;
    }
    // The database changed again during the load: show this one and read it
    // again.
    if (std::exchange(m_reloadPending, false))
        QMetaObject::invokeMethod(this, &BookLibrary::startLoad, Qt::QueuedConnection);

    if (result.ok) {
        m_loaded = true;
        m_lastError.clear();
        applyData(std::move(result.data), std::move(result.index));
    } else if (m_loaded) {
        // A failed reload, Calibre writing for instance, keeps the books
        // shown; the next change to the database reloads them.
        m_lastError = result.error;
        emit loadFailed(result.error);
        emit libraryChanged();
    } else {
        m_lastError = result.error;
        applyData({}, {});
    }
    // Edits made while it loaded.
    startCommit();
}

BookLibrary::BookIndex BookLibrary::indexBooks(const LibraryData &data, const CalibreConfig &config)
{
    const QCollator collator = listCollator();
    QHash<int, QString> seriesSorts;
    for (const FacetItem &item : data.series)
        seriesSorts.insert(item.id, item.sort.isEmpty() ? item.name : item.sort);
    BookIndex index;
    index.search = CalibreSearch::Index(data, CalibreSearch::Preferences::fromRows(data.preferences), config.tweaks,
                                        config.interfaceLanguage);
    const qsizetype count = data.books.size();
    index.titleKeys.reserve(count);
    index.authorKeys.reserve(count);
    index.seriesKeys.reserve(count);
    index.rowOf.reserve(count);
    for (qsizetype row = 0; row < count; ++row) {
        const Book &book = data.books.at(row);
        index.titleKeys.push_back(collator.sortKey(book.sort));
        index.authorKeys.push_back(collator.sortKey(book.authorSort));
        index.seriesKeys.push_back(collator.sortKey(seriesSorts.value(book.seriesId)));
        index.rowOf.insert(book.id, row);
    }
    return index;
}

void BookLibrary::applyData(LibraryData data, BookIndex index)
{
    m_data = std::move(data);
    m_index = std::move(index);
    m_sorted.clear();
    m_sortedBy.clear();
    QHash<int, CoverCache::Cover> covers;
    for (const Book &book : std::as_const(m_data.books)) {
        if (!book.coverFile.isEmpty())
            covers.insert(book.id, { book.coverFile, book.coverModified });
    }
    const QString key = coverKeyOf(m_libraryPath);
    CoverCache::setCovers(key, cacheFolder(m_libraryPath) + QStringLiteral("/covers"), covers);

    if (!m_filterKind.isEmpty()) {
        const QList<FacetItem> *items = facetItems(m_filterKind);
        if (std::none_of(items->cbegin(), items->cend(), [this](const FacetItem &item) { return item.id == m_filterId; })) {
            m_filterKind.clear();
            m_filterId = 0;
        }
    }
    if (!findBook(m_currentBookId))
        m_currentBookId = 0;

    // The choice made for this library, or Calibre's at its start.
    const CalibreSearch::Preferences &preferences = m_index.search.preferences();
    if (m_virtualLibraryOf != m_data.uuid) {
        m_virtualLibraryOf = m_data.uuid;
        const QVariant chosen = QSettings().value(QStringLiteral("Library/virtualLibrary/%1").arg(m_data.uuid));
        const QString name = chosen.isValid() ? chosen.toString() : preferences.virtualLibraryOnStartup;
        if (name != m_virtualLibrary) {
            m_virtualLibrary = name;
            emit virtualLibraryChanged();
        }
    }
    if (!m_virtualLibrary.isEmpty() && !preferences.virtualLibraries.contains(m_virtualLibrary)) {
        m_virtualLibrary.clear();
        emit virtualLibraryChanged();
    }
    if (m_section.startsWith(QLatin1String("search:")) && !preferences.savedSearches.contains(m_section.mid(7))) {
        m_section = QStringLiteral("books");
        emit sectionChanged();
    }
    applyVirtualLibrary();

    rebuildTrash();
    emit libraryChanged();
    emit filterChanged();
    rebuildRows();
    rebuildFacetRows();
    rebuildCurrentBook();
    watchLibrary();
    scheduleCheck();
}

void BookLibrary::watchLibrary()
{
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (m_libraryPath.isEmpty() || !QFileInfo(m_libraryPath).isDir())
        return;
    // The folder too: Calibre and Dropbox replace metadata.db by renaming
    // another file over it, which ends a watch on the file itself.
    m_watcher.addPath(m_libraryPath);
    const QString database = QDir(m_libraryPath).filePath(QStringLiteral("metadata.db"));
    if (QFileInfo::exists(database))
        m_watcher.addPath(database);
}

void BookLibrary::reloadIfChanged()
{
    const FileIdentity now = CalibreDb::identify(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db")));
    if (!m_loaded || now != m_data.source)
        reload();
    else
        watchLibrary();
}

bool BookLibrary::inFilter(const Book &book) const
{
    if (m_filterKind == QLatin1String("authors"))
        return book.authorIds.contains(m_filterId);
    if (m_filterKind == QLatin1String("series"))
        return book.seriesId == m_filterId;
    if (m_filterKind == QLatin1String("tags"))
        return book.tagIds.contains(m_filterId);
    if (m_filterKind == QLatin1String("languages"))
        return book.languageIds.contains(m_filterId);
    if (m_filterKind == QLatin1String("publishers"))
        return book.publisherId == m_filterId;
    return true;
}

const QList<qsizetype> &BookLibrary::sortedBooks()
{
    const QString key = m_sortOption.endsWith(QLatin1String("Desc")) ? m_sortOption.chopped(4) : m_sortOption;
    if (m_sortedBy == key && m_sorted.size() == m_data.books.size())
        return m_sorted;
    const QList<Book> &books = m_data.books;
    const BookIndex &index = m_index;
    m_sorted.resize(books.size());
    std::iota(m_sorted.begin(), m_sorted.end(), qsizetype(0));
    const auto byTitle = [&](qsizetype a, qsizetype b) {
        const int order = index.titleKeys[a].compare(index.titleKeys[b]);
        return order != 0 ? order < 0 : books.at(a).id < books.at(b).id;
    };
    std::stable_sort(m_sorted.begin(), m_sorted.end(), [&](qsizetype a, qsizetype b) {
        const Book &first = books.at(a);
        const Book &second = books.at(b);
        if (key == QLatin1String("authors")) {
            const int order = index.authorKeys[a].compare(index.authorKeys[b]);
            if (order != 0)
                return order < 0;
        } else if (key == QLatin1String("series")) {
            // Books in no series come after the others.
            if ((first.seriesId > 0) != (second.seriesId > 0))
                return first.seriesId > 0;
            const int order = index.seriesKeys[a].compare(index.seriesKeys[b]);
            if (order != 0)
                return order < 0;
            if (first.seriesIndex != second.seriesIndex)
                return first.seriesIndex < second.seriesIndex;
        } else if (key == QLatin1String("added")) {
            if (first.timestamp != second.timestamp)
                return first.timestamp < second.timestamp;
        } else if (key == QLatin1String("rating")) {
            if (first.rating != second.rating)
                return first.rating < second.rating;
        }
        return byTitle(a, b);
    });
    m_sortedBy = key;
    return m_sorted;
}

void BookLibrary::rebuildRows()
{
    // A browse section lists its items until one is opened.
    const bool savedSearch = m_section.startsWith(QLatin1String("search:"));
    const bool listed = m_section == QLatin1String("books") || !m_filterKind.isEmpty() || savedSearch;
    static const QList<qsizetype> none;
    const QList<qsizetype> &sorted = listed ? sortedBooks() : none;
    static const QBitArray nothing;
    const QBitArray &found = listed ? searchRows() : nothing;
    const bool searching = !m_searchText.trimmed().isEmpty();
    QList<qsizetype> order;
    int scopeCount = 0;
    const auto add = [&](qsizetype row) {
        if (row >= m_restriction.size() || !m_restriction.testBit(row) || !inFilter(m_data.books.at(row))
            || (savedSearch && !m_sectionRows.testBit(row)))
            return;
        ++scopeCount;
        if (!searching || found.testBit(row))
            order.append(row);
    };
    // The reverse order is the order reversed, ties included.
    if (m_sortOption.endsWith(QLatin1String("Desc")))
        std::for_each(sorted.crbegin(), sorted.crend(), add);
    else
        std::for_each(sorted.cbegin(), sorted.cend(), add);

    const bool changed = m_bookList.setRows(m_data.books, std::move(order), coverKeyOf(m_libraryPath));
    if (m_scopeCount == scopeCount && !changed)
        return;
    m_scopeCount = scopeCount;
    emit rowsChanged();
}

void BookLibrary::rebuildFacetRows()
{
    QList<FacetItem> items = visibleItems(m_section);

    const QStringList words = searchWords(m_searchText);
    if (!words.isEmpty() && m_filterKind.isEmpty()) {
        items.removeIf([&words](const FacetItem &item) {
            return !matchesAll(CalibreText::foldForSearch(item.name), words);
        });
    }

    // Languages show their names, the other items sort as Calibre sorts them.
    const bool byName = m_section == QLatin1String("languages") || m_section == QLatin1String("tags");
    const bool byCount = m_facetSortOption.startsWith(QLatin1String("count"));
    std::stable_sort(items.begin(), items.end(), [&](const FacetItem &a, const FacetItem &b) {
        if (byCount && a.count != b.count)
            return a.count < b.count;
        const int order = m_collator.compare(byName ? a.name : a.sort, byName ? b.name : b.sort);
        return order != 0 ? order < 0 : a.id < b.id;
    });
    if (m_facetSortOption.endsWith(QLatin1String("Desc")))
        std::reverse(items.begin(), items.end());

    QVariantList rows;
    for (const FacetItem &item : std::as_const(items)) {
        rows.append(QVariantMap{
            { QStringLiteral("id"), item.id },
            { QStringLiteral("name"), item.name },
            { QStringLiteral("count"), item.count },
        });
    }
    if (m_facetRows == rows)
        return;
    m_facetRows = rows;
    emit facetRowsChanged();
}

void BookLibrary::rebuildCurrentBook()
{
    const Book *book = findBook(m_currentBookId);
    if (!book) {
        if (m_currentBook.isEmpty())
            return;
        m_currentBook.clear();
        emit currentBookChanged();
        return;
    }

    QVariantList formats;
    int missing = 0;
    for (const BookFormat &format : book->formats) {
        missing += format.file.isEmpty() ? 1 : 0;
        formats.append(QVariantMap{
            { QStringLiteral("format"), format.format },
            { QStringLiteral("size"), sizeText(format.size) },
            { QStringLiteral("file"), format.file },
            { QStringLiteral("missing"), format.file.isEmpty() },
        });
    }
    QVariantList identifiers;
    QList<QPair<QString, QString>> sortedIdentifiers = book->identifiers;
    std::sort(sortedIdentifiers.begin(), sortedIdentifiers.end());
    for (const auto &identifier : std::as_const(sortedIdentifiers))
        identifiers.append(identifierMap(identifier.first, identifier.second));
    QStringList identifierTexts;
    for (const auto &identifier : book->identifiers)
        identifierTexts.append(identifier.first + u':' + identifier.second);
    QStringList languages;
    for (const QString &code : book->languages)
        languages.append(CalibreText::languageName(code));

    const QString coverKey = coverKeyOf(m_libraryPath);
    const QVariantMap current{
        { QStringLiteral("id"), book->id },
        { QStringLiteral("title"), book->title },
        { QStringLiteral("sort"), book->sort },
        { QStringLiteral("authors"), book->authors },
        { QStringLiteral("authorSort"), book->authorSort },
        { QStringLiteral("series"), book->series },
        { QStringLiteral("seriesText"), book->seriesId > 0
              ? tr("Book %1 of %2").arg(BookDisplay::seriesIndex(book->seriesIndex), book->series) : QString() },
        { QStringLiteral("tags"), book->tags },
        { QStringLiteral("publisher"), book->publisher },
        { QStringLiteral("rating"), BookDisplay::rating(book->rating) },
        { QStringLiteral("languages"), languages },
        { QStringLiteral("identifiers"), identifiers },
        { QStringLiteral("published"), monthYear(book->pubdate) },
        { QStringLiteral("added"), localDateTime(book->timestamp) },
        { QStringLiteral("modified"), localDateTime(book->lastModified) },
        { QStringLiteral("path"), book->path },
        { QStringLiteral("description"), Description::cleaned(book->comments) },
        // The description as the editor shows it, and whether saving it
        // from there leaves something out.
        { QStringLiteral("descriptionText"), Description::toText(book->comments) },
        { QStringLiteral("descriptionLosesFormatting"), Description::losesFormatting(book->comments) },
        { QStringLiteral("cover"), book->coverFile.isEmpty()
              ? QString() : CoverCache::url(coverKey, book->id, book->coverModified) },
        { QStringLiteral("formats"), formats },
        { QStringLiteral("missingFiles"), missing },
        { QStringLiteral("hasCover"), !book->coverFile.isEmpty() },
        // The values as the fields edit them.
        { QStringLiteral("authorsText"), book->authors.join(QStringLiteral(" & ")) },
        { QStringLiteral("seriesIndexText"), BookDisplay::seriesIndex(book->seriesIndex) },
        { QStringLiteral("tagsText"), book->tags.join(QStringLiteral(", ")) },
        { QStringLiteral("ratingValue"), book->rating },
        { QStringLiteral("languagesText"), languages.join(QStringLiteral(", ")) },
        { QStringLiteral("identifiersText"), identifierTexts.join(QStringLiteral(", ")) },
        { QStringLiteral("pubdateText"), CalibreDb::isUndefinedDate(book->pubdate)
              ? QString() : book->pubdate.toUTC().date().toString(Qt::ISODate) },
        { QStringLiteral("custom"), customFields(*book) },
    };
    if (m_currentBook == current)
        return;
    m_currentBook = current;
    emit currentBookChanged();
}

bool BookLibrary::writable() const
{
    return m_writable;
}

void BookLibrary::setWritable(bool writable)
{
    if (m_writable == writable)
        return;
    m_writable = writable;
    emit writableChanged();
}

bool BookLibrary::saving() const
{
    return m_commitWatcher.isRunning() || !m_pending.isEmpty();
}

bool BookLibrary::canRevert() const
{
    return m_originalBookId != 0 && m_originalBookId == m_currentBookId;
}

QString BookLibrary::publishedKey() const
{
    return m_publishedKey;
}

QString BookLibrary::calibreLanguage() const
{
    return m_calibreLanguage;
}

void BookLibrary::setCalibreLanguage(const QString &language)
{
    if (m_calibreLanguage == language.trimmed())
        return;
    m_calibreLanguage = language.trimmed();
    emit calibreLanguageChanged();
}

QString BookLibrary::interfaceLanguage() const
{
    return effectiveConfig().interfaceLanguage;
}

QString BookLibrary::backupFolder() const
{
    return CousoirPaths::backupFolder(m_libraryPath);
}

CalibreConfig BookLibrary::effectiveConfig() const
{
    CalibreConfig config = m_config;
    if (!m_calibreLanguage.isEmpty())
        config.interfaceLanguage = m_calibreLanguage;
    return config;
}

QVariantList BookLibrary::customFields(const Book &book) const
{
    QVariantList fields;
    for (qsizetype index = 0; index < m_data.customColumns.size(); ++index) {
        const CustomColumn &column = m_data.customColumns.at(index);
        const QJsonObject display = QJsonDocument::fromJson(column.display).object();
        const bool names = display.value(QLatin1String("is_names")).toBool();
        const QString interpretAs = display.value(QLatin1String("interpret_as")).toString();
        const bool html = column.datatype == QLatin1String("comments") && interpretAs != QLatin1String("short-text")
            && interpretAs != QLatin1String("long-text") && interpretAs != QLatin1String("markdown");
        const CustomValue value = book.custom.value(index);
        const QString &type = column.datatype;
        // What the page shows, and what the field edits.
        QString text;
        QString editText;
        if (type == QLatin1String("composite")) {
            // As its template made it.
            text = value.value.toString();
        } else if (column.multiple) {
            text = value.value.toStringList().join(names ? QStringLiteral(" & ") : QStringLiteral(", "));
            editText = text;
        } else if (value.value.isNull()) {
        } else if (type == QLatin1String("rating")) {
            text = BookDisplay::rating(value.value.toInt());
            editText = value.value.toString();
        } else if (type == QLatin1String("bool")) {
            text = value.value.toBool() ? tr("Yes") : tr("No");
            editText = value.value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        } else if (type == QLatin1String("datetime")) {
            text = CalibreDb::isUndefinedDate(value.value.toDateTime()) ? QString()
                                                                       : BookDisplay::date(value.value.toDateTime());
            editText = text;
        } else if (type == QLatin1String("float")) {
            text = PythonJson::floatRepr(value.value.toDouble());
            editText = text;
        } else if (type == QLatin1String("series")) {
            text = tr("%1 [%2]").arg(value.value.toString(), BookDisplay::seriesIndex(value.seriesIndex));
            editText = value.value.toString();
        } else if (type == QLatin1String("comments")) {
            // HTML unless the column says it holds plain text or Markdown.
            text = html ? Description::toText(value.value.toString()) : value.value.toString();
            editText = text;
        } else {
            text = value.value.toString();
            editText = text;
        }
        QStringList choices;
        for (const QJsonValue &choice : display.value(QLatin1String("enum_values")).toArray())
            choices.append(choice.toString());
        fields.append(QVariantMap{
            { QStringLiteral("key"), u'#' + column.label },
            { QStringLiteral("name"), column.name },
            { QStringLiteral("datatype"), type },
            { QStringLiteral("multiple"), column.multiple },
            { QStringLiteral("names"), names },
            { QStringLiteral("editable"), column.editable && type != QLatin1String("composite") },
            { QStringLiteral("halfStars"), display.value(QLatin1String("allow_half_stars")).toBool() },
            { QStringLiteral("html"), html },
            // A long text holding more than its text shows, a table or an
            // image, which saving from its text would leave out.
            { QStringLiteral("losesFormatting"), type == QLatin1String("comments") && html
                                                     && Description::losesFormatting(value.value.toString()) },
            { QStringLiteral("choices"), choices },
            { QStringLiteral("text"), text },
            { QStringLiteral("editText"), editText },
            { QStringLiteral("seriesIndexText"), value.value.isNull() ? QString()
                                                                      : BookDisplay::seriesIndex(value.seriesIndex) },
        });
    }
    return fields;
}

QVariantMap BookLibrary::editableValues(const Book &book) const
{
    QStringList identifiers;
    for (const auto &identifier : book.identifiers)
        identifiers.append(identifier.first + u':' + identifier.second);
    QVariantMap values{
        { QStringLiteral("title"), book.title },
        { QStringLiteral("sort"), book.sort },
        { QStringLiteral("authors"), book.authors },
        { QStringLiteral("authorSort"), book.authorSort },
        { QStringLiteral("series"), book.series },
        { QStringLiteral("seriesIndex"), book.seriesIndex },
        { QStringLiteral("tags"), book.tags },
        { QStringLiteral("rating"), book.rating },
        { QStringLiteral("publisher"), book.publisher },
        { QStringLiteral("languages"), book.languages },
        { QStringLiteral("identifiers"), identifiers.join(QStringLiteral(", ")) },
        // The exact date, which a revert puts back as it was.
        { QStringLiteral("pubdate"), CalibreDb::isUndefinedDate(book.pubdate) ? QString()
                                                                              : CalibreDb::isoFormat(book.pubdate) },
        { QStringLiteral("comments"), book.comments },
    };
    // The custom columns, as updateBook() takes them back.
    for (qsizetype index = 0; index < m_data.customColumns.size(); ++index) {
        const CustomColumn &column = m_data.customColumns.at(index);
        if (column.datatype == QLatin1String("composite"))
            continue;
        const CustomValue value = book.custom.value(index);
        const QString key = u'#' + column.label;
        // A date as its exact text, the undefined date as nothing, which
        // Calibre's adapter makes undefined again.
        if (column.datatype == QLatin1String("datetime") && !value.value.isNull())
            values.insert(key, CalibreDb::isUndefinedDate(value.value.toDateTime())
                                   ? QString() : CalibreDb::isoFormat(value.value.toDateTime()));
        else
            values.insert(key, value.value);
        if (column.datatype == QLatin1String("series") && !value.value.isNull())
            values.insert(key + QStringLiteral("_index"), value.seriesIndex);
    }
    return values;
}

QString BookLibrary::textToHtml(const QString &text) const
{
    return Description::toHtml(text);
}

QVariantList BookLibrary::paletteSearch(const QString &text, const QVariantList &commands, int limit) const
{
    // A command's key rides in the hit's title's place while it is matched.
    QList<Palette::Hit> given;
    QStringList keys;
    for (const QVariant &command : commands) {
        const QVariantMap map = command.toMap();
        given.append({ map.value(QStringLiteral("kind")).toString(), int(keys.size()),
                       map.value(QStringLiteral("title")).toString(), map.value(QStringLiteral("detail")).toString(), 0 });
        keys.append(map.value(QStringLiteral("key")).toString());
    }
    QVariantList hits;
    for (const Palette::Hit &hit : Palette::search(m_index.search, text, limit, given)) {
        const bool command = hit.kind == QLatin1String("action") || hit.kind == QLatin1String("section");
        hits.append(QVariantMap{
            { QStringLiteral("kind"), hit.kind },
            { QStringLiteral("id"), command ? 0 : hit.id },
            { QStringLiteral("key"), command ? keys.value(hit.id) : QString() },
            { QStringLiteral("title"), hit.title },
            { QStringLiteral("detail"), hit.detail },
        });
    }
    return hits;
}

bool BookLibrary::isDate(const QString &text) const
{
    return text.trimmed().isEmpty() || FieldWriters::parseOnlyDate(text, QDateTime::currentDateTimeUtc())
        || FieldWriters::parseExactDate(text);
}

bool BookLibrary::isCustomDate(const QString &text) const
{
    return text.trimmed().isEmpty() || FieldWriters::parseCustomDate(text);
}

void BookLibrary::openBackupFolder()
{
    QDir().mkpath(backupFolder());
    QDesktopServices::openUrl(QUrl::fromLocalFile(backupFolder()));
}

QString BookLibrary::updateBook(int bookId, const QVariantMap &changes)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    if (changes.contains(QStringLiteral("pubdate")) && !isDate(changes.value(QStringLiteral("pubdate")).toString()))
        return tr("“%1” is not a date.").arg(changes.value(QStringLiteral("pubdate")).toString());

    QVariantMap adjusted = changes;
    const CalibreConfig config = effectiveConfig();
    // Calibre's editor: a sort that followed its title or authors keeps
    // following them, any other stays as it is.
    if (adjusted.contains(QStringLiteral("title")) && !adjusted.contains(QStringLiteral("sort"))) {
        const QString language = book->languages.value(0);
        const QString automatic = CalibreText::titleSort(book->title, language, config.tweaks, config.interfaceLanguage);
        adjusted.insert(QStringLiteral("sort"), book->sort == automatic
            ? CalibreText::titleSort(adjusted.value(QStringLiteral("title")).toString(), language, config.tweaks,
                                     config.interfaceLanguage)
            : book->sort);
    }
    if (adjusted.contains(QStringLiteral("authors")) && !adjusted.contains(QStringLiteral("authorSort"))
        && !LibraryCheck::authorSortFollows(m_data, *book))
        adjusted.insert(QStringLiteral("authorSort"), book->authorSort);

    BookEdit edit;
    QString error;
    if (!BookEdit::fromVariantMap(bookId, adjusted, &edit, &error))
        return error;
    keepOriginal(*book);
    return queueEdit(edit);
}

void BookLibrary::keepOriginal(const Book &book)
{
    if (book.id != m_currentBookId || m_originalBookId == book.id)
        return;
    m_originalBookId = book.id;
    m_original = editableValues(book);
    QFile cover(book.coverFile);
    m_originalCover = !book.coverFile.isEmpty() && cover.open(QIODevice::ReadOnly) ? cover.readAll() : QByteArray();
    emit canRevertChanged();
}

QString BookLibrary::saveDescription(int bookId, const QString &text)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    // Unchanged, the description keeps its HTML as it is.
    if (text.trimmed() == Description::toText(book->comments).trimmed())
        return {};
    return updateBook(bookId, { { QStringLiteral("comments"), Description::toHtml(text) } });
}

QString BookLibrary::setCover(int bookId, const QString &fileUrl)
{
    const QString path = fileUrl.startsWith(QLatin1String("file:")) ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (QImageReader(path).format().isEmpty())
        return tr("%1 is not an image Cousoir can read.").arg(QFileInfo(path).fileName());
    return updateBook(bookId, { { QStringLiteral("coverFile"), path } });
}

QString BookLibrary::pasteCover(int bookId)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull())
        return tr("The clipboard holds no image.");
    BookEdit edit;
    edit.bookId = bookId;
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    edit.cover = data;
    keepOriginal(*book);
    return queueEdit(edit);
}

QString BookLibrary::removeCover(int bookId)
{
    return updateBook(bookId, { { QStringLiteral("removeCover"), true } });
}

void BookLibrary::revertCurrentBook()
{
    const Book *book = findBook(m_currentBookId);
    if (!canRevert() || !book)
        return;
    const QVariantMap current = editableValues(*book);
    QVariantMap changes;
    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it) {
        if (current.value(it.key()) != it.value())
            changes.insert(it.key(), it.value());
    }
    BookEdit edit;
    QString error;
    BookEdit::fromVariantMap(m_currentBookId, changes, &edit, &error);
    QFile cover(book->coverFile);
    const QByteArray coverNow = !book->coverFile.isEmpty() && cover.open(QIODevice::ReadOnly) ? cover.readAll()
                                                                                             : QByteArray();
    if (coverNow != m_originalCover)
        edit.cover = m_originalCover;
    m_originalBookId = 0;
    m_original.clear();
    m_originalCover.clear();
    emit canRevertChanged();
    if (!edit.isEmpty())
        queueEdit(edit);
}

int BookLibrary::bulkUndoCount() const
{
    return int(m_bulkUndo.size());
}

QString BookLibrary::updateBooks(const QVariantList &bookIds, const QVariantMap &changes)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    if (changes.contains(QStringLiteral("pubdate")) && !isDate(changes.value(QStringLiteral("pubdate")).toString()))
        return tr("“%1” is not a date.").arg(changes.value(QStringLiteral("pubdate")).toString());
    QVariantMap withBooks = changes;
    withBooks.insert(QStringLiteral("books"), bookIds);
    BulkEdit bulk;
    QString error;
    if (!BulkEdit::fromVariantMap(withBooks, &bulk, &error))
        return error;
    bulk.bookIds.removeIf([this](int id) { return !findBook(id); });
    if (bulk.isEmpty())
        return {};
    // The undo of an earlier one gives way; this one's comes once it is
    // saved.
    if (!m_bulkUndo.isEmpty()) {
        m_bulkUndo.clear();
        emit bulkUndoChanged();
    }
    CommitEngine::Request request;
    request.bulkEdits.append(bulk);
    return queueChange(request);
}

void BookLibrary::undoBulkEdit()
{
    if (m_bulkUndo.isEmpty() || !m_writable)
        return;
    CommitEngine::Request request;
    for (auto it = m_bulkUndo.constBegin(); it != m_bulkUndo.constEnd(); ++it) {
        const Book *book = findBook(it.key());
        if (!book)
            continue;
        // A field changed since, by Cousoir or by Calibre, stays as it is.
        const QVariantMap current = editableValues(*book);
        QVariantMap changes;
        for (auto field = it->before.constBegin(); field != it->before.constEnd(); ++field) {
            if (current.value(field.key()) == it->after.value(field.key()) && field.value() != it->after.value(field.key()))
                changes.insert(field.key(), field.value());
        }
        BookEdit edit;
        QString error;
        if (BookEdit::fromVariantMap(it.key(), changes, &edit, &error) && !edit.isEmpty())
            request.edits.append(edit);
    }
    m_bulkUndo.clear();
    emit bulkUndoChanged();
    if (!request.edits.isEmpty())
        queueChange(request);
}

void BookLibrary::keepBulkUndo(const QList<BulkEdit> &bulkEdits, const QList<Book> &before)
{
    // The fields the edits set.
    QStringList fields;
    for (const BulkEdit &bulk : bulkEdits) {
        if (bulk.authors)
            fields << QStringLiteral("authors") << QStringLiteral("authorSort");
        if (bulk.series)
            fields << QStringLiteral("series") << QStringLiteral("seriesIndex");
        if (bulk.removeAllTags || !bulk.addTags.isEmpty() || !bulk.removeTags.isEmpty())
            fields << QStringLiteral("tags");
        if (bulk.rating)
            fields << QStringLiteral("rating");
        if (bulk.publisher)
            fields << QStringLiteral("publisher");
        if (bulk.languages)
            fields << QStringLiteral("languages");
        if (bulk.pubdate)
            fields << QStringLiteral("pubdate");
    }
    m_bulkUndo.clear();
    for (const Book &old : before) {
        const Book *now = findBook(old.id);
        if (!now)
            continue;
        const QVariantMap was = editableValues(old);
        const QVariantMap is = editableValues(*now);
        BulkUndo undo;
        for (const QString &field : std::as_const(fields)) {
            undo.before.insert(field, was.value(field));
            undo.after.insert(field, is.value(field));
        }
        if (undo.before != undo.after)
            m_bulkUndo.insert(old.id, undo);
    }
    emit bulkUndoChanged();
}

QString BookLibrary::queueEdit(const BookEdit &edit)
{
    if (edit.isEmpty())
        return {};
    // With the edits just before it, if the last change waiting is edits.
    if (!m_pending.isEmpty() && !m_pending.constLast().hasOperations() && m_pending.constLast().bulkEdits.isEmpty()) {
        m_pending.last().edits.append(edit);
    } else {
        CommitEngine::Request request;
        request.edits.append(edit);
        m_pending.append(request);
    }
    emit savingChanged();
    startCommit();
    return {};
}

QString BookLibrary::queueChange(const CommitEngine::Request &change)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    m_pending.append(change);
    emit savingChanged();
    startCommit();
    return {};
}

void BookLibrary::startCommit()
{
    // A load writes the snapshot a save replaces: one at a time.
    if (m_commitWatcher.isRunning() || m_loadWatcher.isRunning() || m_pending.isEmpty())
        return;
    if (!m_writable) {
        m_pending.clear();
        emit savingChanged();
        emit saveFailed(tr("The library cannot be changed right now."));
        return;
    }
    CommitEngine::Request request = m_pending.takeFirst();
    request.libraryPath = m_libraryPath;
    request.cacheFolder = cacheFolder(m_libraryPath);
    // One copy of the database before the session's first change.
    request.backupFolder = m_backedUp ? QString() : backupFolder();
    request.config = effectiveConfig();
    const QString version = QCoreApplication::applicationVersion();
    request.producer = version.isEmpty() ? QStringLiteral("Cousoir") : QStringLiteral("Cousoir (%1)").arg(version);
    request.producerId = QStringLiteral("cousoir");
    m_commitWatcher.setFuture(QtConcurrent::run([request] {
        CommitResult committed{ CommitEngine::commit(request), {}, request.bulkEdits };
        if (committed.result.ok && committed.result.written)
            committed.index = indexBooks(committed.result.data, request.config);
        return committed;
    }));
    // The files move meanwhile: the check waits for the library saved.
    if (m_checkWatcher.isRunning()) {
        m_checkWatcher.cancel();
        m_checkPending = true;
    }
    emit savingChanged();
}

void BookLibrary::finishCommit()
{
    CommitResult committed = m_commitWatcher.future().takeResult();
    const CommitEngine::Result &result = committed.result;
    if (!result.ok) {
        // What follows was made on what failed; the library shows what it
        // holds.
        m_pending.clear();
        m_bulkUndo.clear();
        emit bulkUndoChanged();
        emit saveFailed(result.error);
        m_reloadPending = true;
    } else if (result.data.libraryPath == m_libraryPath) {
        // Not when another library was picked meanwhile: its load, waiting
        // for this save, shows it.
        m_backedUp = m_backedUp || !result.backup.isEmpty();
        if (result.written) {
            m_loaded = true;
            m_lastError.clear();
            applyData(result.data, std::move(committed.index));
            if (!committed.bulkEdits.isEmpty())
                keepBulkUndo(committed.bulkEdits, result.bulkBefore);
            m_publishedKey = ConcurrencyGuard::changeKey(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db")));
            emit publishedKeyChanged();
        }
        emit saved();
        if (!result.added.isEmpty()) {
            QVariantList added;
            for (const int id : result.added)
                added.append(id);
            emit booksAdded(added);
        }
    }
    emit savingChanged();
    if (std::exchange(m_reloadPending, false))
        reloadIfChanged();
    startCommit();
    if (m_checkPending && !m_commitWatcher.isRunning())
        scheduleCheck();
}

QVariantList BookLibrary::trashEntries() const
{
    return m_trashEntries;
}

int BookLibrary::trashExpiryDays() const
{
    return int(m_data.trashExpirySeconds / (24 * 3600));
}

bool BookLibrary::importing() const
{
    return m_importWatcher.isRunning();
}

int BookLibrary::importDone() const
{
    return m_importWatcher.isRunning() ? m_importWatcher.progressValue() : 0;
}

int BookLibrary::importTotal() const
{
    return m_importWatcher.isRunning() ? m_importWatcher.progressMaximum() : 0;
}

QString BookLibrary::addBooks(const QVariantList &urls)
{
    if (!m_writable)
        return tr("The library cannot be changed right now.");
    if (m_importWatcher.isRunning() || !m_importedBooks.isEmpty())
        return tr("Books are already being added.");
    const QStringList files = bookFiles(urls);
    if (files.isEmpty())
        return tr("There is no book file to add.");
    const CalibreConfig config = effectiveConfig();
    m_importWatcher.setFuture(QtConcurrent::run([files, config](QPromise<ImportResult> &promise) {
        ImportResult result;
        promise.setProgressRange(0, int(files.size()));
        for (qsizetype index = 0; index < files.size(); ++index) {
            NewBook book;
            book.files = { files.at(index) };
            QString warning;
            book.metadata = BookImport::metadataForAdding(files.at(index), config, &warning);
            if (!warning.isEmpty())
                result.warnings.append(warning);
            result.books.append(book);
            promise.setProgressValue(int(index) + 1);
        }
        promise.addResult(result);
    }));
    emit importChanged();
    return {};
}

void BookLibrary::finishImport()
{
    const ImportResult result = m_importWatcher.future().resultCount() ? m_importWatcher.result() : ImportResult();
    emit importChanged();
    if (!result.warnings.isEmpty())
        emit importWarning(result.warnings.join(u'\n'));
    if (result.books.isEmpty())
        return;
    QStringList titles;
    m_importedDuplicates.clear();
    for (const NewBook &book : result.books) {
        const bool duplicate = !identicalBooks(book.metadata).isEmpty();
        m_importedDuplicates.append(duplicate);
        if (duplicate)
            titles.append(book.metadata.title);
    }
    m_importedBooks = result.books;
    if (titles.isEmpty())
        addDuplicates(true);
    else
        emit duplicatesFound(titles, int(result.books.size()));
}

void BookLibrary::addDuplicates(bool add)
{
    CommitEngine::Request change;
    for (qsizetype index = 0; index < m_importedBooks.size(); ++index) {
        if (add || !m_importedDuplicates.value(index))
            change.newBooks.append(m_importedBooks.at(index));
    }
    m_importedBooks.clear();
    m_importedDuplicates.clear();
    if (change.newBooks.isEmpty())
        return;
    const QString error = queueChange(change);
    if (!error.isEmpty())
        emit saveFailed(error);
}

QList<int> BookLibrary::identicalBooks(const ImportedMetadata &metadata) const
{
    const CalibreConfig config = effectiveConfig();
    const QString unknown = CalibreText::unknownText(config.interfaceLanguage);
    const QStringList authors = metadata.authors.isEmpty() ? QStringList{ unknown } : metadata.authors;
    // Books with all the authors, by name without regard to case.
    std::optional<QSet<int>> found;
    for (const QString &author : authors) {
        QSet<int> byAuthor;
        bool known = false;
        for (const FacetItem &item : m_data.authors) {
            if (item.name.toLower() != author.toLower())
                continue;
            known = true;
            for (const Book &book : m_data.books) {
                if (book.authorIds.contains(item.id))
                    byAuthor.insert(book.id);
            }
        }
        if (!known)
            return {};
        found = found ? found->intersect(byAuthor) : byAuthor;
        if (found->isEmpty())
            return {};
    }
    const QString title = CalibreText::fuzzyTitle(metadata.title.isEmpty() ? unknown : metadata.title, config.tweaks,
                                                  config.interfaceLanguage);
    QStringList languages;
    for (const QString &language : metadata.languages) {
        const QString code = CalibreText::canonicalizeLanguage(language);
        if (!code.isEmpty() && code != QLatin1String("und"))
            languages.append(code);
    }
    QList<int> identical;
    for (const Book &book : m_data.books) {
        if (!found->contains(book.id)
            || CalibreText::fuzzyTitle(book.title, config.tweaks, config.interfaceLanguage) != title)
            continue;
        if (languages.isEmpty() || book.languages.isEmpty() || book.languages == languages)
            identical.append(book.id);
    }
    return identical;
}

QString BookLibrary::addFormat(int bookId, const QString &fileUrl)
{
    const QString file = localPath(fileUrl);
    if (!findBook(bookId))
        return tr("This book is no longer in the library.");
    if (!QFileInfo(file).isFile())
        return tr("%1 cannot be read.").arg(QFileInfo(file).fileName());
    if (QFileInfo(file).suffix().isEmpty())
        return tr("%1 has no extension to tell its format.").arg(QFileInfo(file).fileName());
    CommitEngine::Request change;
    change.newFormats.append({ bookId, file });
    return queueChange(change);
}

QString BookLibrary::addConversion(int bookId, const QString &format, const Conversion::Outcome &outcome)
{
    const Book *book = findBook(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    CommitEngine::Request change;
    Conversion::addTo(&change, *book, format, outcome);
    return queueChange(change);
}

QString BookLibrary::removeFormat(int bookId, const QString &format)
{
    if (!findBook(bookId))
        return tr("This book is no longer in the library.");
    CommitEngine::Request change;
    change.formatRemovals.append({ bookId, { format } });
    return queueChange(change);
}

QString BookLibrary::moveToTrash(const QVariantList &bookIds)
{
    CommitEngine::Request change;
    for (const QVariant &id : bookIds) {
        if (findBook(id.toInt()) && !change.trash.contains(id.toInt()))
            change.trash.append(id.toInt());
    }
    if (change.trash.isEmpty())
        return tr("These books are no longer in the library.");
    return queueChange(change);
}

QString BookLibrary::restoreFromTrash(int bookId, const QStringList &formats)
{
    if (formats.isEmpty() && findBook(bookId))
        return tr("The library already has a book with this one's number: restore it in Calibre.");
    if (!formats.isEmpty() && !findBook(bookId))
        return tr("The book of these files is no longer in the library.");
    CommitEngine::Request change;
    if (formats.isEmpty())
        change.restorations.append(Restoration{ bookId, QString() });
    for (const QString &format : formats)
        change.restorations.append(Restoration{ bookId, format });
    return queueChange(change);
}

QString BookLibrary::keepDuplicate(int keepId, const QVariantList &otherIds)
{
    QList<int> others;
    for (const QVariant &id : otherIds)
        others.append(id.toInt());
    CommitEngine::Request change;
    QString error;
    if (!LibraryCheck::keepOne(m_data, keepId, others, &change, &error))
        return error;
    return queueChange(change);
}

QString BookLibrary::mergeAuthors(const QVariantList &authorIds, int keepAuthorId)
{
    // FieldWriters refuses every author while two differ only by case.
    if (!m_findings.caseDuplicates.isEmpty())
        return tr("Two author names differ only by case: Calibre merges them the next time it saves an author, and "
                  "these names can be merged then.");
    QList<int> ids;
    for (const QVariant &id : authorIds)
        ids.append(id.toInt());
    CommitEngine::Request change;
    QString error;
    change.edits = LibraryCheck::mergeAuthors(m_data, ids, keepAuthorId, &error);
    if (change.edits.isEmpty())
        return error;
    return queueChange(change);
}

QString BookLibrary::removeMissingFormats(int bookId)
{
    if (!findBook(bookId))
        return tr("This book is no longer in the library.");
    // As the check found them: a file kept online only is not missing.
    QStringList formats;
    for (const LibraryCheck::MissingFiles &missing : std::as_const(m_findings.missing)) {
        if (missing.book == bookId)
            formats = missing.formats;
    }
    if (formats.isEmpty())
        return tr("None of the book's files is missing.");
    CommitEngine::Request change;
    change.formatRemovals.append({ bookId, formats });
    return queueChange(change);
}

QString BookLibrary::showInFolder(const QString &path)
{
    const QString library = QDir::cleanPath(m_libraryPath);
    const QFileInfo info(QDir::cleanPath(QDir(library).filePath(path)));
    if (library.isEmpty() || !info.absoluteFilePath().startsWith(library + u'/'))
        return tr("%1 is not in the library.").arg(path);
    if (!info.exists())
        return tr("%1 is no longer there.").arg(path);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath())))
        return tr("No application shows folders.");
    return {};
}

void BookLibrary::ignoreFinding(const QString &key)
{
    LibraryCheck::ignore(m_data.uuid, key);
    rebuildCheck();
}

void BookLibrary::showIgnored()
{
    LibraryCheck::clearIgnored(m_data.uuid);
    rebuildCheck();
}

void BookLibrary::recheck()
{
    scheduleCheck();
}

QVariantMap BookLibrary::check() const
{
    return m_check;
}

bool BookLibrary::checking() const
{
    return m_checkWatcher.isRunning() || m_checkPending;
}

void BookLibrary::scheduleCheck()
{
    if (m_checkWatcher.isRunning())
        m_checkWatcher.cancel();
    if (!m_loaded || m_libraryPath.isEmpty()) {
        m_checkPending = false;
        emit checkingChanged();
        return;
    }
    // A save moves files about: after it.
    if (m_commitWatcher.isRunning() || !m_pending.isEmpty()) {
        m_checkPending = true;
        emit checkingChanged();
        return;
    }
    m_checkPending = false;
    const LibraryData data = m_data;
    const CalibreConfig config = effectiveConfig();
    LibraryCheck::Options options;
    options.tweaks = config.tweaks;
    options.interfaceLanguage = config.interfaceLanguage;
    m_checkWatcher.setFuture(QtConcurrent::run([data, options](QPromise<LibraryCheck::Findings> &promise) {
        LibraryCheck::Options checks = options;
        checks.canceled = [&promise] { return promise.isCanceled(); };
        promise.addResult(LibraryCheck::check(data, checks));
    }));
    emit checkingChanged();
}

void BookLibrary::finishCheck()
{
    if (!m_checkWatcher.isCanceled() && m_checkWatcher.future().resultCount() > 0) {
        m_findings = m_checkWatcher.result();
        m_checkedAt = QDateTime::currentDateTime();
        rebuildCheck();
    }
    emit checkingChanged();
}

void BookLibrary::rebuildCheck()
{
    const QStringList ignored = m_loaded ? LibraryCheck::ignoredKeys(m_data.uuid) : QStringList();
    const QString coverKey = coverKeyOf(m_libraryPath);
    const auto present = [](const Book &book) {
        QStringList formats;
        for (const BookFormat &format : book.formats) {
            if (!format.file.isEmpty())
                formats.append(format.format);
        }
        return formats;
    };
    QVariantList rows;
    int ignoredCount = 0;
    const auto add = [&rows, &ignored, &ignoredCount](QVariantMap row) {
        if (ignored.contains(row.value(QStringLiteral("key")).toString())) {
            ++ignoredCount;
            return;
        }
        rows.append(row);
    };

    for (const LibraryCheck::DuplicateGroup &group : std::as_const(m_findings.duplicates)) {
        QVariantList books;
        for (const int id : group.books) {
            const Book *book = findBook(id);
            if (!book)
                continue;
            // What keeping this copy brings it from the others.
            QStringList gains;
            for (const int other : group.books) {
                const Book *copy = findBook(other);
                if (!copy || other == id)
                    continue;
                for (const QString &format : present(*copy)) {
                    if (!present(*book).contains(format) && !gains.contains(format))
                        gains.append(format);
                }
            }
            QStringList missing;
            for (const BookFormat &format : book->formats) {
                if (format.file.isEmpty())
                    missing.append(format.format);
            }
            books.append(QVariantMap{
                { QStringLiteral("id"), id },
                { QStringLiteral("title"), book->title },
                { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
                { QStringLiteral("formats"), present(*book) },
                { QStringLiteral("missing"), missing },
                { QStringLiteral("added"), QLocale().toString(book->timestamp.toLocalTime().date(), QLocale::ShortFormat) },
                { QStringLiteral("cover"), book->coverFile.isEmpty()
                      ? QString() : CoverCache::url(coverKey, book->id, book->coverModified) },
                { QStringLiteral("gains"), gains },
            });
        }
        add({ { QStringLiteral("kind"), QStringLiteral("duplicates") },
              { QStringLiteral("key"), LibraryCheck::key(group) },
              { QStringLiteral("sameTitle"), group.sameTitle },
              { QStringLiteral("isbns"), group.isbns },
              { QStringLiteral("books"), books } });
    }

    for (const LibraryCheck::AuthorGroup &group : std::as_const(m_findings.authorVariants)) {
        QVariantList authors;
        QVariantList ids;
        for (const int id : group.authors) {
            for (const FacetItem &author : std::as_const(m_data.authors)) {
                if (author.id != id)
                    continue;
                authors.append(QVariantMap{ { QStringLiteral("id"), id },
                                            { QStringLiteral("name"), author.name },
                                            { QStringLiteral("count"), author.count } });
                ids.append(id);
            }
        }
        add({ { QStringLiteral("kind"), QStringLiteral("authors") },
              { QStringLiteral("key"), LibraryCheck::key(group) },
              { QStringLiteral("initials"), group.initials },
              { QStringLiteral("ids"), ids },
              { QStringLiteral("authors"), authors } });
    }

    QList<std::pair<const Book *, const LibraryCheck::MissingFiles *>> missing;
    for (const LibraryCheck::MissingFiles &entry : std::as_const(m_findings.missing)) {
        if (const Book *book = findBook(entry.book))
            missing.append({ book, &entry });
    }
    std::stable_sort(missing.begin(), missing.end(), [this](const auto &a, const auto &b) {
        return m_collator.compare(a.first->sort, b.first->sort) < 0;
    });
    for (const auto &[book, entry] : std::as_const(missing)) {
        add({ { QStringLiteral("kind"), QStringLiteral("missing") },
              { QStringLiteral("key"), LibraryCheck::key(*entry) },
              { QStringLiteral("bookId"), entry->book },
              { QStringLiteral("title"), book->title },
              { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
              { QStringLiteral("formats"), entry->formats },
              { QStringLiteral("noFormats"), entry->noFormats },
              { QStringLiteral("folderMissing"), entry->folderMissing } });
    }

    for (const LibraryCheck::OutsideFile &file : std::as_const(m_findings.outside)) {
        add({ { QStringLiteral("kind"), QStringLiteral("outside") },
              { QStringLiteral("key"), LibraryCheck::key(file) },
              { QStringLiteral("path"), file.path },
              { QStringLiteral("folder"), file.folder },
              { QStringLiteral("size"), file.folder ? QString() : sizeText(file.size) },
              { QStringLiteral("bookId"), file.book } });
    }

    const QVariantMap check{
        { QStringLiteral("count"), rows.size() },
        { QStringLiteral("checked"), m_checkedAt.isValid() ? QLocale().toString(m_checkedAt.time(), QLocale::ShortFormat)
                                                           : QString() },
        { QStringLiteral("caseDuplicates"), !m_findings.caseDuplicates.isEmpty() },
        { QStringLiteral("ignored"), ignoredCount },
        { QStringLiteral("rows"), rows },
    };
    if (check == m_check)
        return;
    m_check = check;
    emit checkChanged();
}

void BookLibrary::rebuildTrash()
{
    // A removed book's own cover; for removed files, their book's.
    const QString coverKey = coverKeyOf(m_libraryPath);
    const auto trashCover = [&](const TrashEntry &entry) {
        if (!entry.formats.isEmpty()) {
            const Book *book = findBook(entry.bookId);
            return book && !book->coverFile.isEmpty() ? CoverCache::url(coverKey, book->id, book->coverModified)
                                                      : QString();
        }
        return QFileInfo(entry.coverFile).isFile() ? QUrl::fromLocalFile(entry.coverFile).toString() : QString();
    };
    QList<TrashEntry> entries = m_libraryPath.isEmpty()
        ? QList<TrashEntry>() : BookOperations::trashEntries(m_libraryPath, effectiveConfig().interfaceLanguage);
    std::stable_sort(entries.begin(), entries.end(),
                     [](const TrashEntry &a, const TrashEntry &b) { return a.removed > b.removed; });
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVariantList rows;
    for (const TrashEntry &entry : std::as_const(entries)) {
        const bool book = entry.formats.isEmpty();
        const qint64 left = m_data.trashExpirySeconds - entry.removed.secsTo(now);
        rows.append(QVariantMap{
            { QStringLiteral("key"), QStringLiteral("%1-%2").arg(book ? u'b' : u'f').arg(entry.bookId) },
            { QStringLiteral("bookId"), entry.bookId },
            { QStringLiteral("kind"), book ? QStringLiteral("book") : QStringLiteral("formats") },
            { QStringLiteral("title"), entry.title },
            { QStringLiteral("author"), entry.author },
            { QStringLiteral("cover"), trashCover(entry) },
            { QStringLiteral("formats"), entry.formats },
            { QStringLiteral("removed"), QLocale().toString(entry.removed.toLocalTime(), QLocale::ShortFormat) },
            { QStringLiteral("daysLeft"), int(std::max<qint64>(0, (left + 24 * 3600 - 1) / (24 * 3600))) },
            // A book comes back under its own number, a file to its book.
            { QStringLiteral("restorable"), book ? !findBook(entry.bookId) : findBook(entry.bookId) != nullptr },
        });
    }
    if (rows == m_trashEntries)
        return;
    m_trashEntries = rows;
    emit trashChanged();
}

const Book *BookLibrary::findBook(int bookId) const
{
    const qsizetype row = m_index.rowOf.value(bookId, -1);
    return row >= 0 ? &m_data.books.at(row) : nullptr;
}

const QList<FacetItem> *BookLibrary::facetItems(const QString &kind) const
{
    if (kind == QLatin1String("authors"))
        return &m_data.authors;
    if (kind == QLatin1String("series"))
        return &m_data.series;
    if (kind == QLatin1String("tags"))
        return &m_data.tags;
    if (kind == QLatin1String("languages"))
        return &m_data.languages;
    if (kind == QLatin1String("publishers"))
        return &m_data.publishers;
    return nullptr;
}
