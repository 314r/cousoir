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
#include <QTextDocument>
#include <QUrl>
#include <QPromise>
#include <QSet>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <numeric>

#include "book_import.h"
#ifdef COUSOIR_READER
#include "book_reader.h"
#endif
#include "calibre_db.h"
#include "calibre_text.h"
#include "concurrency_guard.h"
#include "conversion.h"
#include "cover_cache.h"
#include "field_writers.h"
#include "cousoir_paths.h"
#include "description.h"
#include "palette.h"
#include "sqlite_db.h"

namespace {

const QStringList &sections()
{
    static const QStringList names = { QStringLiteral("books"), QStringLiteral("authors"), QStringLiteral("series"),
                                       QStringLiteral("tags"), QStringLiteral("languages"),
                                       QStringLiteral("publishers"), QStringLiteral("trash"),
                                       QStringLiteral("check"), QStringLiteral("highlights") };
    return names;
}

bool isFacetSortOption(const QString &option)
{
    return option == QLatin1String("name") || option == QLatin1String("nameDesc")
        || option == QLatin1String("count") || option == QLatin1String("countDesc");
}

// The columns Cousoir shows a library Calibre has shown none of.
const QStringList defaultColumns{ QStringLiteral("title"), QStringLiteral("authors"), QStringLiteral("series"),
                                  QStringLiteral("formats"), QStringLiteral("timestamp"), QStringLiteral("rating") };

// The built-in columns of Calibre's list (column_map of
// gui2/library/models.py) and how wide their text is.
struct ColumnInfo
{
    const char *key;
    const char *title;
    const char *kind;
};
const ColumnInfo builtinColumns[] = {
    { "title", QT_TRANSLATE_NOOP("BookLibrary", "Title"), "text" },
    { "authors", QT_TRANSLATE_NOOP("BookLibrary", "Authors"), "text" },
    { "series", QT_TRANSLATE_NOOP("BookLibrary", "Series"), "text" },
    { "rating", QT_TRANSLATE_NOOP("BookLibrary", "Rating"), "rating" },
    { "tags", QT_TRANSLATE_NOOP("BookLibrary", "Tags"), "long" },
    { "publisher", QT_TRANSLATE_NOOP("BookLibrary", "Publisher"), "text" },
    { "pubdate", QT_TRANSLATE_NOOP("BookLibrary", "Published"), "date" },
    { "timestamp", QT_TRANSLATE_NOOP("BookLibrary", "Added"), "date" },
    { "last_modified", QT_TRANSLATE_NOOP("BookLibrary", "Modified"), "date" },
    { "size", QT_TRANSLATE_NOOP("BookLibrary", "Size"), "number" },
    { "languages", QT_TRANSLATE_NOOP("BookLibrary", "Languages"), "text" },
    { "formats", QT_TRANSLATE_NOOP("BookLibrary", "Formats"), "text" },
    { "identifiers", QT_TRANSLATE_NOOP("BookLibrary", "Identifiers"), "long" },
    { "pages", QT_TRANSLATE_NOOP("BookLibrary", "Pages"), "number" },
    { "id", QT_TRANSLATE_NOOP("BookLibrary", "Id"), "number" },
    { "path", QT_TRANSLATE_NOOP("BookLibrary", "Path"), "long" },
};

// The `columns` the list can show, by availableColumns(), each once, the
// title first.
QStringList knownColumns(const QStringList &columns, const QVariantList &available)
{
    QStringList known;
    for (const QVariant &column : available)
        known.append(column.toMap().value(QStringLiteral("key")).toString());
    QStringList kept{ QStringLiteral("title") };
    for (const QString &column : columns) {
        if (known.contains(column) && !kept.contains(column))
            kept.append(column);
    }
    return kept;
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

// The book's cover.jpg as it is, empty when it has none.
QByteArray coverBytes(const Book &book)
{
    QFile file(book.coverFile);
    return !book.coverFile.isEmpty() && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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
        { QStringLiteral("highlights"), m_data.annotationCount ? QVariant(m_data.annotationCount) : QVariant(QString()) },
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
    // Its note: HTML whose images are the library's resources, and the text
    // it is edited as.
    const QString field = CalibreDb::notesField(m_filterKind);
    const ItemNote note = m_data.notes.value(field).value(m_filterId);
    QString html = note.doc;
    static const QRegularExpression resource(QStringLiteral("calres://([a-z0-9]+)/([0-9a-f]+)(\\?[^\"']*)?"));
    const QString resources = QDir(CalibreDb::notesFolder(m_libraryPath)).filePath(QStringLiteral("resources"));
    QHash<QString, QSize> sizes;
    for (auto it = resource.globalMatch(note.doc); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString digest = match.captured(2);
        const QString file = QDir(resources).filePath(digest.left(2) + u'/' + match.captured(1) + u'-' + digest);
        const QString url = QUrl::fromLocalFile(file).toString();
        html.replace(match.captured(0), url);
        sizes.insert(url, QImageReader(file).size());
    }
    html = Description::cleaned(html);
    // Each image's own size, which the pane shrinks to its width.
    for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
        if (it.value().isValid())
            html.replace(QStringLiteral("src=\"%1\"").arg(it.key()),
                         QStringLiteral("src=\"%1\" width=\"%2\" height=\"%3\"")
                             .arg(it.key())
                             .arg(it.value().width())
                             .arg(it.value().height()));
    }
    return {
        { QStringLiteral("kind"), m_filterKind },
        { QStringLiteral("id"), m_filterId },
        { QStringLiteral("name"), name },
        // Notes can be kept on it: Calibre 7 opened the library.
        { QStringLiteral("notes"), !field.isEmpty() && QFileInfo::exists(QDir(CalibreDb::notesFolder(m_libraryPath)).filePath(QStringLiteral("notes.db"))) },
        { QStringLiteral("note"), html },
        { QStringLiteral("noteText"), Description::toText(note.doc) },
        { QStringLiteral("noteHasImages"), !note.resources.isEmpty() },
        // What its text would leave out, for Calibre to change it.
        { QStringLiteral("noteLosesFormatting"), Description::noteLosesFormatting(note.doc) },
    };
}

QString BookLibrary::setFilterNote(const QString &text)
{
    const QString field = CalibreDb::notesField(m_filterKind);
    if (field.isEmpty())
        return tr("These items have no notes.");
    const QVariantMap current = filter();
    if (current.value(QStringLiteral("noteHasImages")).toBool())
        return tr("This note has images: change it in Calibre.");
    if (current.value(QStringLiteral("noteLosesFormatting")).toBool())
        return tr("This note has formatting Cousoir cannot write: change it in Calibre.");
    CalibreNotes::Change change;
    change.field = field;
    change.item = m_filterId;
    // The item as Calibre's id_map holds it: a language by its code.
    for (const FacetItem &item : *facetItems(m_filterKind)) {
        if (item.id == m_filterId)
            change.itemValue = m_filterKind == QLatin1String("languages") ? item.sort : item.name;
    }
    change.html = Description::toHtml(text);
    // The text a search finds: the note without its markup.
    QTextDocument document;
    document.setHtml(change.html);
    change.searchable = document.toPlainText();
    CommitEngine::Request request;
    request.notes.append(change);
    return queueChange(request);
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
    const BookSort::Level &first = m_sortHistory.first();
    return first.ascending ? first.field : first.field + QStringLiteral("Desc");
}

void BookLibrary::setSortOption(const QString &sortOption)
{
    const bool descending = sortOption.endsWith(QLatin1String("Desc"));
    QString field = descending ? sortOption.chopped(4) : sortOption;
    // Cousoir's name before it took Calibre's.
    if (field == QLatin1String("added"))
        field = QStringLiteral("timestamp");
    if (!BookSort::sortable(m_data, field))
        return;
    const BookSort::Level level{ field, !descending };
    if (m_sortHistory.first() == level)
        return;
    m_sortHistory = BookSort::addToHistory(m_sortHistory, level, std::max(1, effectiveConfig().tweaks.maximumResortLevels));
    emit sortOptionChanged();
    saveView();
    rebuildRows();
}

void BookLibrary::sortBy(const QString &field)
{
    const BookSort::Level &first = m_sortHistory.first();
    setSortOption(first.field == field && first.ascending ? field + QStringLiteral("Desc") : field);
}

QStringList BookLibrary::sortFields() const
{
    QStringList fields;
    for (const BookSort::Level &level : m_sortHistory)
        fields.append(level.field);
    return fields;
}

QStringList BookLibrary::columns() const
{
    return m_columns.isEmpty() ? defaultColumns : m_columns;
}

void BookLibrary::setColumns(const QStringList &columns)
{
    const QStringList kept = knownColumns(columns, availableColumns());
    if (kept == this->columns())
        return;
    m_columns = kept;
    m_bookList.setColumns(kept, m_data.customColumns, effectiveConfig().interfaceLanguage);
    saveView();
    emit columnsChanged();
}

QVariantList BookLibrary::availableColumns() const
{
    QVariantList shown;
    QVariantList hidden;
    const QStringList current = columns();
    const auto add = [&](const QString &key, const QString &title, const QString &kind) {
        const QVariantMap column{ { QStringLiteral("key"), key }, { QStringLiteral("title"), title },
                                  { QStringLiteral("kind"), kind }, { QStringLiteral("shown"), current.contains(key) } };
        (current.contains(key) ? shown : hidden).append(column);
    };
    for (const ColumnInfo &column : builtinColumns)
        add(QString::fromLatin1(column.key), tr(column.title), QString::fromLatin1(column.kind));
    for (const CustomColumn &column : m_data.customColumns) {
        const QString &type = column.datatype;
        const QString kind = type == QLatin1String("int") || type == QLatin1String("float") ? QStringLiteral("number")
            : type == QLatin1String("rating")                                             ? QStringLiteral("rating")
            : type == QLatin1String("datetime")                                           ? QStringLiteral("date")
            : type == QLatin1String("comments") || column.multiple                        ? QStringLiteral("long")
                                                                                          : QStringLiteral("text");
        add(u'#' + column.label, column.name, kind);
    }
    // Those shown in their order.
    std::sort(shown.begin(), shown.end(), [&current](const QVariant &a, const QVariant &b) {
        return current.indexOf(a.toMap().value(QStringLiteral("key")).toString())
            < current.indexOf(b.toMap().value(QStringLiteral("key")).toString());
    });
    return shown + hidden;
}

void BookLibrary::saveView()
{
    if (m_data.uuid.isEmpty() || m_viewOf != m_data.uuid)
        return;
    QJsonArray history;
    for (const BookSort::Level &level : std::as_const(m_sortHistory))
        history.append(QJsonArray{ level.field, level.ascending });
    QSettings settings;
    settings.setValue(QStringLiteral("Library/sortHistory/%1").arg(m_data.uuid),
                      QString::fromUtf8(QJsonDocument(history).toJson(QJsonDocument::Compact)));
    if (!m_columns.isEmpty())
        settings.setValue(QStringLiteral("Library/columns/%1").arg(m_data.uuid), m_columns);
}

void BookLibrary::restoreView()
{
    if (m_viewOf == m_data.uuid && !m_viewOf.isNull())
        return;
    m_viewOf = m_data.uuid;
    QSettings settings;
    const QString savedHistory = settings.value(QStringLiteral("Library/sortHistory/%1").arg(m_data.uuid)).toString();
    const QStringList savedColumns = settings.value(QStringLiteral("Library/columns/%1").arg(m_data.uuid)).toStringList();
    // Calibre's list as it left it: get_state() of gui2/library/views.py.
    const QJsonObject calibre
        = QJsonDocument::fromJson(m_data.preferences.value(QStringLiteral("library_view books view state")).toUtf8()).object();
    QList<BookSort::Level> history;
    const QJsonArray historyRows = savedHistory.isEmpty() ? calibre.value(QLatin1String("sort_history")).toArray()
                                                          : QJsonDocument::fromJson(savedHistory.toUtf8()).array();
    // apply_sort_history(): Calibre's three last sorts.
    const qsizetype levels = savedHistory.isEmpty() ? 3 : std::max(1, effectiveConfig().tweaks.maximumResortLevels);
    for (const QJsonValue &row : historyRows) {
        QString field = row.toArray().at(0).toString();
        field = field == QLatin1String("date") ? QStringLiteral("timestamp")
            : field == QLatin1String("sort")   ? QStringLiteral("title")
                                               : field;
        if (!row.toArray().at(1).isBool() || !BookSort::sortable(m_data, field)
            || std::any_of(history.cbegin(), history.cend(), [&field](const auto &level) { return level.field == field; }))
            continue;
        if (history.size() < levels)
            history.append({ field, row.toArray().at(1).toBool() });
    }
    const QList<BookSort::Level> sort = history.isEmpty() ? QList<BookSort::Level>{ { QStringLiteral("title"), true } }
                                                          : history;
    if (sort != m_sortHistory) {
        m_sortHistory = sort;
        emit sortOptionChanged();
    }

    QStringList columns = savedColumns;
    if (columns.isEmpty() && calibre.contains(QLatin1String("column_positions"))) {
        // The columns Calibre shows, in its order.
        const QJsonObject positions = calibre.value(QLatin1String("column_positions")).toObject();
        QStringList hidden;
        for (const QJsonValue &column : calibre.value(QLatin1String("hidden_columns")).toArray())
            hidden.append(column.toString());
        QList<QPair<int, QString>> shown;
        for (auto it = positions.begin(); it != positions.end(); ++it) {
            if (!hidden.contains(it.key()))
                shown.append({ it.value().toInt(), it.key() });
        }
        std::sort(shown.begin(), shown.end());
        for (const auto &[position, key] : std::as_const(shown))
            columns.append(key);
    }
    m_columns.clear();
    if (!columns.isEmpty())
        m_columns = knownColumns(columns, availableColumns());
    m_bookList.setColumns(this->columns(), m_data.customColumns, effectiveConfig().interfaceLanguage);
    emit columnsChanged();
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
    const QStringList sortFields = this->sortFields();
    m_loadWatcher.setFuture(QtConcurrent::run([path, cache, config, sortFields] {
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
            result.index = indexBooks(result.data, config, sortFields);
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

BookLibrary::BookIndex BookLibrary::indexBooks(const LibraryData &data, const CalibreConfig &config,
                                               const QStringList &sortFields)
{
    BookIndex index;
    index.search = CalibreSearch::Index(data, CalibreSearch::Preferences::fromRows(data.preferences), config.tweaks,
                                        config.interfaceLanguage);
    const auto sorter = std::make_shared<BookSort::Sorter>(data, config);
    sorter->prepare(sortFields);
    index.sorter = sorter;
    const qsizetype count = data.books.size();
    index.rowOf.reserve(count);
    for (qsizetype row = 0; row < count; ++row)
        index.rowOf.insert(data.books.at(row).id, row);
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
    restoreView();
    // The columns of this load: a custom column added or gone shows so.
    m_columns.removeIf([this](const QString &key) {
        return key.startsWith(u'#') && !BookSort::sortable(m_data, key);
    });
    m_bookList.setColumns(columns(), m_data.customColumns, effectiveConfig().interfaceLanguage);
    emit columnsChanged();
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
    if (m_sortedBy == m_sortHistory && m_sorted.size() == m_data.books.size())
        return m_sorted;
    // The books by id, as Calibre's list has them, then sorted by the
    // history.
    m_sorted.resize(m_data.books.size());
    std::iota(m_sorted.begin(), m_sorted.end(), qsizetype(0));
    if (m_index.sorter)
        m_index.sorter->sort(m_sortHistory, &m_sorted);
    m_sortedBy = m_sortHistory;
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

    const QHash<int, ItemNote> notes = m_data.notes.value(CalibreDb::notesField(m_section));
    QVariantList rows;
    for (const FacetItem &item : std::as_const(items)) {
        rows.append(QVariantMap{
            { QStringLiteral("id"), item.id },
            { QStringLiteral("name"), item.name },
            { QStringLiteral("count"), item.count },
            { QStringLiteral("hasNote"), notes.contains(item.id) },
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
    for (const BookFormat &format : book->formats) {
        formats.append(QVariantMap{
            { QStringLiteral("format"), format.format },
            { QStringLiteral("size"), BookDisplay::fileSize(format.size) },
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
    const QString interfaceLanguage = effectiveConfig().interfaceLanguage;
    for (const QString &code : book->languages)
        languages.append(CalibreText::languageName(code, interfaceLanguage));

    const QString coverKey = coverKeyOf(m_libraryPath);
    const QVariantMap current{
        { QStringLiteral("id"), book->id },
        { QStringLiteral("title"), book->title },
        { QStringLiteral("sort"), book->sort },
        { QStringLiteral("authors"), book->authors },
        { QStringLiteral("authorSort"), book->authorSort },
        { QStringLiteral("series"), book->series },
        { QStringLiteral("seriesText"), book->seriesId > 0
              ? tr("Book %1 of %2").arg(CalibreText::seriesIndexText(book->seriesIndex), book->series) : QString() },
        { QStringLiteral("tags"), book->tags },
        { QStringLiteral("publisher"), book->publisher },
        { QStringLiteral("rating"), BookDisplay::rating(book->rating) },
        { QStringLiteral("languages"), languages },
        { QStringLiteral("identifiers"), identifiers },
        { QStringLiteral("published"), BookDisplay::monthYear(book->pubdate) },
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
        { QStringLiteral("hasCover"), !book->coverFile.isEmpty() },
        // The values as the fields edit them.
        { QStringLiteral("authorsText"), book->authors.join(QStringLiteral(" & ")) },
        { QStringLiteral("seriesIndexText"), CalibreText::seriesIndexText(book->seriesIndex) },
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

bool BookLibrary::readerAvailable() const
{
#ifdef COUSOIR_READER
    // Set up before the application, as main() does.
    return BookReader::ready();
#else
    return false;
#endif
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
            text = BookDisplay::date(value.value.toDateTime());
            editText = text;
        } else if (type == QLatin1String("float")) {
            text = PythonJson::floatRepr(value.value.toDouble());
            editText = text;
        } else if (type == QLatin1String("series")) {
            text = tr("%1 [%2]").arg(value.value.toString(), CalibreText::seriesIndexText(value.seriesIndex));
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
                                                                      : CalibreText::seriesIndexText(value.seriesIndex) },
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
    // A command's key rides, by its index, in the hit's id while it is
    // matched.
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

QVariantMap BookLibrary::downloadedChanges(int bookId, const MetadataDownload::Found &found, const QStringList &fields,
                                           bool appendComments) const
{
    const Book *book = findBook(bookId);
    return book ? MetadataDownload::changes(m_data, *book, effectiveConfig(), found, fields, appendComments) : QVariantMap();
}

QVariantList BookLibrary::downloadFields(int bookId, const MetadataDownload::Found &found, const QStringList &ignored,
                                         bool appendComments) const
{
    const Book *book = findBook(bookId);
    if (!book)
        return {};
    const CalibreConfig config = effectiveConfig();
    const QVariantMap current = editableValues(*book);
    // Each field as the dialog shows it, from the values updateBook() takes.
    const auto text = [&config](const QString &field, const QVariantMap &values) -> QString {
        const QVariant value = values.value(field);
        if (field == QLatin1String("authors"))
            return value.toStringList().join(QStringLiteral(" & "));
        if (field == QLatin1String("tags"))
            return value.toStringList().join(QStringLiteral(", "));
        if (field == QLatin1String("rating"))
            return BookDisplay::rating(value.toInt());
        if (field == QLatin1String("languages")) {
            QStringList names;
            for (const QString &code : value.toStringList())
                names.append(CalibreText::languageName(code, config.interfaceLanguage));
            return names.join(QStringLiteral(", "));
        }
        if (field == QLatin1String("identifiers")) {
            QStringList pairs;
            if (value.typeId() == QMetaType::QVariantMap) {
                const QVariantMap map = value.toMap();
                for (auto it = map.cbegin(); it != map.cend(); ++it)
                    pairs.append(it.key() + u':' + it.value().toString());
            } else {
                pairs = value.toString().split(QStringLiteral(", "), Qt::SkipEmptyParts);
            }
            pairs.sort();
            return pairs.join(QStringLiteral(", "));
        }
        if (field == QLatin1String("pubdate")) {
            const QDateTime date = QDateTime::fromString(value.toString(), Qt::ISODate);
            return date.isValid() ? date.toLocalTime().date().toString(Qt::ISODate) : QString();
        }
        if (field == QLatin1String("series")) {
            const QString series = value.toString();
            return series.isEmpty() ? QString()
                                    : tr("%1, book %2").arg(series, CalibreText::seriesIndexText(values.value(QStringLiteral("seriesIndex")).toDouble()));
        }
        if (field == QLatin1String("comments"))
            return Description::toText(value.toString()).trimmed();
        return value.toString();
    };
    static const QHash<QString, const char *> labels{
        { QStringLiteral("title"), QT_TR_NOOP("Title") },         { QStringLiteral("authors"), QT_TR_NOOP("Authors") },
        { QStringLiteral("rating"), QT_TR_NOOP("Rating") },       { QStringLiteral("publisher"), QT_TR_NOOP("Publisher") },
        { QStringLiteral("tags"), QT_TR_NOOP("Tags") },           { QStringLiteral("identifiers"), QT_TR_NOOP("Identifiers") },
        { QStringLiteral("pubdate"), QT_TR_NOOP("Published") },   { QStringLiteral("series"), QT_TR_NOOP("Series") },
        { QStringLiteral("languages"), QT_TR_NOOP("Languages") }, { QStringLiteral("comments"), QT_TR_NOOP("Description") },
    };
    QVariantList rows;
    for (const QString &field : MetadataDownload::fields()) {
        const QVariantMap changes = MetadataDownload::changes(m_data, *book, config, found, { field }, appendComments);
        if (changes.isEmpty())
            continue;
        QVariantMap after = current;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            after.insert(it.key(), it.value());
        const QString before = text(field, current);
        const QString now = text(field, after);
        rows.append(QVariantMap{
            { QStringLiteral("key"), field },
            { QStringLiteral("label"), tr(labels.value(field)) },
            { QStringLiteral("current"), before },
            { QStringLiteral("found"), now },
            // Calibre's preferences may leave a field aside.
            { QStringLiteral("chosen"), before != now && !ignored.contains(field) },
        });
    }
    return rows;
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
    m_originalCover = coverBytes(book);
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
    if (coverBytes(*book) != m_originalCover)
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

QString BookLibrary::readableFormat(int bookId) const
{
    const Book *book = findBook(bookId);
    if (!book || !readerAvailable())
        return {};
    for (const char *wanted : { "EPUB", "KEPUB", "PDF" }) {
        for (const BookFormat &format : book->formats) {
            if (format.format == QLatin1String(wanted) && !format.file.isEmpty())
                return format.format;
        }
    }
    return {};
}

bool BookLibrary::saveReading(int bookId, const std::optional<ReadPosition> &position, const QList<Annotation> &annotations)
{
    if (!m_writable || !findBook(bookId))
        return false;
    CommitEngine::Request request;
    if (position && !position->cfi.isEmpty())
        request.readPositions.append({ bookId, *position });
    for (const Annotation &annotation : annotations)
        request.annotations.append({ bookId, annotation });
    if (request.readPositions.isEmpty() && request.annotations.isEmpty())
        return true;
    return queueChange(request).isEmpty();
}

QList<Annotation> BookLibrary::annotations(int bookId, const QString &format) const
{
    // From the copy of the database the library was read from.
    QList<Annotation> found;
    SqliteDb db;
    QString error;
    if (m_libraryPath.isEmpty()
        || !db.open(cacheFolder(m_libraryPath) + QStringLiteral("/snapshot.db"), SqliteDb::Mode::Immutable, &error))
        return found;
    for (const QVariantList &row :
         db.rows(QStringLiteral("SELECT annot_type, annot_id, timestamp, annot_data, searchable_text FROM annotations "
                                "WHERE book=? AND format=? AND user_type='local' AND user='viewer'"),
                 { bookId, format.toUpper() })) {
        found.append({ format.toUpper(), row.value(0).toString(), row.value(1).toString(), row.value(2).toDouble(),
                       row.value(3).toString().toUtf8(), row.value(4).toString() });
    }
    return found;
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
    const QStringList sortFields = this->sortFields();
    m_commitWatcher.setFuture(QtConcurrent::run([request, sortFields] {
        CommitResult committed{ CommitEngine::commit(request), {}, request.bulkEdits };
        if (committed.result.ok && committed.result.written)
            committed.index = indexBooks(committed.result.data, request.config, sortFields);
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
    if (result.notesWritten && (!result.ok || !result.written)) {
        // The notes went in, whatever came after them.
        CalibreDb::readNotes(&m_data);
        emit filterChanged();
        rebuildFacetRows();
    }
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
            books.append(QVariantMap{
                { QStringLiteral("id"), id },
                { QStringLiteral("title"), book->title },
                { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
                { QStringLiteral("formats"), present(*book) },
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
              { QStringLiteral("size"), file.folder ? QString() : BookDisplay::fileSize(file.size) },
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
