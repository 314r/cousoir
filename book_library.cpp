#include "book_library.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include "calibre_db.h"
#include "calibre_text.h"
#include "cover_cache.h"

namespace {

const QStringList &sections()
{
    static const QStringList names = { QStringLiteral("books"), QStringLiteral("authors"), QStringLiteral("series"),
                                       QStringLiteral("tags"), QStringLiteral("languages"),
                                       QStringLiteral("publishers") };
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

// fmt_sidx(): "2" for a whole series index, "2.5" otherwise.
QString seriesIndexText(double index)
{
    if (index == std::floor(index))
        return QString::number(qint64(index));
    QString text = QString::number(index, 'f', 2);
    while (text.endsWith(u'0'))
        text.chop(1);
    return text;
}

QString seriesText(const Book &book)
{
    return book.seriesId > 0
        ? BookLibrary::tr("%1 [%2]").arg(book.series, seriesIndexText(book.seriesIndex)) : QString();
}

// Two ratings make a star; an odd rating ends in a half.
QString ratingText(int rating)
{
    return QString(rating / 2, u'★') + (rating % 2 ? QStringLiteral("½") : QString());
}

QString localDate(const QDateTime &date)
{
    return CalibreDb::isUndefinedDate(date) ? QString() : date.toLocalTime().date().toString(Qt::ISODate);
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

// The HTML Calibre keeps for a description, without the fonts, sizes and
// colours its editors leave behind: the page shows it in the theme's own.
QString cleanDescription(const QString &html)
{
    if (html.trimmed().isEmpty())
        return {};
    if (!html.contains(u'<')) {
        QStringList paragraphs;
        for (const QString &paragraph : html.split(QRegularExpression(QStringLiteral("\\n\\s*\\n"))))
            paragraphs.append(QStringLiteral("<p>%1</p>")
                                  .arg(paragraph.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"))));
        return paragraphs.join(QString());
    }
    static const QRegularExpression attributes(
        QStringLiteral("\\s(style|face|color|size|class|bgcolor|width|height|align|dir|lang)"
                       "\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wrappers(QStringLiteral("</?(font|span)\\b[^>]*>"),
                                             QRegularExpression::CaseInsensitiveOption);
    QString cleaned = html;
    cleaned.remove(attributes);
    cleaned.remove(wrappers);
    return cleaned.trimmed();
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
    return CalibreText::foldForSearch(text).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

bool matchesAll(const QString &haystack, const QStringList &words)
{
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return haystack.contains(word); });
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
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_changeTimer.setSingleShot(true);
    m_changeTimer.setInterval(1500);
    connect(&m_changeTimer, &QTimer::timeout, this, &BookLibrary::reloadIfChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_changeTimer, qOverload<>(&QTimer::start));
    connect(&m_loadWatcher, &QFutureWatcher<LoadResult>::finished, this, &BookLibrary::finishLoad);
    // QML may still set another library; only the last one loads.
    QMetaObject::invokeMethod(this, &BookLibrary::startLoad, Qt::QueuedConnection);
}

BookLibrary::~BookLibrary()
{
    m_loadWatcher.waitForFinished();
}

QString BookLibrary::cacheFolder(const QString &libraryPath)
{
    const QByteArray key = QCryptographicHash::hash(QDir::cleanPath(libraryPath).toUtf8(), QCryptographicHash::Sha1)
                               .toHex().left(16);
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/reliure/")
        + QString::fromLatin1(key);
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
    m_loaded = false;
    m_filterKind.clear();
    m_filterId = 0;
    m_currentBookId = 0;
    emit libraryChanged();
    emit filterChanged();
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
    return {
        { QStringLiteral("books"), m_data.books.size() },
        { QStringLiteral("authors"), m_data.authors.size() },
        { QStringLiteral("series"), m_data.series.size() },
        { QStringLiteral("tags"), m_data.tags.size() },
        { QStringLiteral("languages"), m_data.languages.size() },
        { QStringLiteral("publishers"), m_data.publishers.size() },
    };
}

QString BookLibrary::section() const
{
    return m_section;
}

void BookLibrary::setSection(const QString &section)
{
    if (m_section == section || !sections().contains(section))
        return;
    m_section = section;
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

QVariantList BookLibrary::rows() const
{
    return m_rows;
}

QVariantList BookLibrary::facetRows() const
{
    return m_facetRows;
}

int BookLibrary::facetCount() const
{
    const QList<FacetItem> *items = facetItems(m_section);
    return items ? int(items->size()) : 0;
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
    startLoad();
}

void BookLibrary::startLoad()
{
    if (m_loadWatcher.isRunning()) {
        m_reloadPending = true;
        return;
    }
    if (m_libraryPath.isEmpty()) {
        m_loaded = false;
        m_lastError = tr("Calibre has no library on this computer yet. Choose one in Settings.");
        emit libraryChanged();
        return;
    }
    const QString path = m_libraryPath;
    const QString snapshot = cacheFolder(path) + QStringLiteral("/snapshot.db");
    const CalibreConfig config = m_config;
    m_loadWatcher.setFuture(QtConcurrent::run([path, snapshot, config] {
        LoadResult result;
        result.path = path;
        result.ok = CalibreDb::loadLibrary(path, snapshot, config, &result.data, &result.error);
        return result;
    }));
    emit loadingChanged();
}

void BookLibrary::finishLoad()
{
    LoadResult result = m_loadWatcher.result();
    emit loadingChanged();
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
        m_data = std::move(result.data);
        m_loaded = true;
        m_lastError.clear();
    } else if (m_loaded) {
        // A failed reload, Calibre writing for instance, keeps the books
        // shown; the next change to the database reloads them.
        m_lastError = result.error;
        emit loadFailed(result.error);
    } else {
        m_data = {};
        m_lastError = result.error;
    }

    QHash<int, CoverCache::Cover> covers;
    for (const Book &book : std::as_const(m_data.books)) {
        if (!book.coverFile.isEmpty())
            covers.insert(book.id, { book.coverFile, book.coverModified });
    }
    const QString key = QFileInfo(cacheFolder(m_libraryPath)).fileName();
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

    emit libraryChanged();
    emit filterChanged();
    rebuildRows();
    rebuildFacetRows();
    rebuildCurrentBook();
    watchLibrary();
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

void BookLibrary::rebuildRows()
{
    QList<const Book *> books;
    // A browse section lists its items until one is opened.
    if (m_section == QLatin1String("books") || !m_filterKind.isEmpty()) {
        for (const Book &book : std::as_const(m_data.books)) {
            if (inFilter(book))
                books.append(&book);
        }
    }
    const int scopeCount = int(books.size());

    const QStringList words = searchWords(m_searchText);
    if (!words.isEmpty()) {
        books.removeIf([&words](const Book *book) {
            const QString haystack = CalibreText::foldForSearch(
                QStringList{ book->title, book->authors.join(u' '), book->series, book->tags.join(u' ') }.join(u' '));
            return !matchesAll(haystack, words);
        });
    }

    QHash<int, QString> seriesSorts;
    for (const FacetItem &item : std::as_const(m_data.series))
        seriesSorts.insert(item.id, item.sort.isEmpty() ? item.name : item.sort);
    const QString key = m_sortOption.endsWith(QLatin1String("Desc")) ? m_sortOption.chopped(4) : m_sortOption;
    const auto byTitle = [this](const Book *a, const Book *b) {
        const int order = m_collator.compare(a->sort, b->sort);
        return order != 0 ? order < 0 : a->id < b->id;
    };
    std::stable_sort(books.begin(), books.end(), [&](const Book *a, const Book *b) {
        if (key == QLatin1String("authors")) {
            const int order = m_collator.compare(a->authorSort, b->authorSort);
            if (order != 0)
                return order < 0;
        } else if (key == QLatin1String("series")) {
            // Books in no series come after the others.
            if ((a->seriesId > 0) != (b->seriesId > 0))
                return a->seriesId > 0;
            const int order = m_collator.compare(seriesSorts.value(a->seriesId), seriesSorts.value(b->seriesId));
            if (order != 0)
                return order < 0;
            if (a->seriesIndex != b->seriesIndex)
                return a->seriesIndex < b->seriesIndex;
        } else if (key == QLatin1String("added")) {
            if (a->timestamp != b->timestamp)
                return a->timestamp < b->timestamp;
        } else if (key == QLatin1String("rating")) {
            if (a->rating != b->rating)
                return a->rating < b->rating;
        }
        return byTitle(a, b);
    });
    if (m_sortOption.endsWith(QLatin1String("Desc")))
        std::reverse(books.begin(), books.end());

    const QString coverKey = QFileInfo(cacheFolder(m_libraryPath)).fileName();
    QVariantList rows;
    for (const Book *book : std::as_const(books)) {
        QStringList formats;
        for (const BookFormat &format : book->formats)
            formats.append(format.format);
        rows.append(QVariantMap{
            { QStringLiteral("id"), book->id },
            { QStringLiteral("title"), book->title },
            { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
            { QStringLiteral("series"), seriesText(*book) },
            { QStringLiteral("formats"), formats.join(QStringLiteral(", ")) },
            { QStringLiteral("added"), localDate(book->timestamp) },
            { QStringLiteral("rating"), ratingText(book->rating) },
            { QStringLiteral("cover"), book->coverFile.isEmpty()
                  ? QString() : CoverCache::url(coverKey, book->id, book->coverModified) },
        });
    }

    if (m_rows == rows && m_scopeCount == scopeCount)
        return;
    m_rows = rows;
    m_scopeCount = scopeCount;
    emit rowsChanged();
}

void BookLibrary::rebuildFacetRows()
{
    QList<FacetItem> items;
    if (const QList<FacetItem> *all = facetItems(m_section))
        items = *all;

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
    QStringList languages;
    for (const QString &code : book->languages)
        languages.append(CalibreText::languageName(code));

    const QString coverKey = QFileInfo(cacheFolder(m_libraryPath)).fileName();
    const QVariantMap current{
        { QStringLiteral("id"), book->id },
        { QStringLiteral("title"), book->title },
        { QStringLiteral("sort"), book->sort },
        { QStringLiteral("authors"), book->authors },
        { QStringLiteral("authorSort"), book->authorSort },
        { QStringLiteral("series"), book->series },
        { QStringLiteral("seriesId"), book->seriesId },
        { QStringLiteral("seriesText"), book->seriesId > 0
              ? tr("Book %1 of %2").arg(seriesIndexText(book->seriesIndex), book->series) : QString() },
        { QStringLiteral("tags"), book->tags },
        { QStringLiteral("publisher"), book->publisher },
        { QStringLiteral("rating"), ratingText(book->rating) },
        { QStringLiteral("languages"), languages },
        { QStringLiteral("identifiers"), identifiers },
        { QStringLiteral("published"), monthYear(book->pubdate) },
        { QStringLiteral("added"), localDateTime(book->timestamp) },
        { QStringLiteral("modified"), localDateTime(book->lastModified) },
        { QStringLiteral("path"), book->path },
        { QStringLiteral("folder"), QDir(m_data.libraryPath).filePath(book->path) },
        { QStringLiteral("uuid"), book->uuid },
        { QStringLiteral("description"), cleanDescription(book->comments) },
        { QStringLiteral("cover"), book->coverFile.isEmpty()
              ? QString() : CoverCache::url(coverKey, book->id, book->coverModified) },
        { QStringLiteral("formats"), formats },
        { QStringLiteral("missingFiles"), missing },
    };
    if (m_currentBook == current)
        return;
    m_currentBook = current;
    emit currentBookChanged();
}

const Book *BookLibrary::findBook(int bookId) const
{
    if (bookId <= 0)
        return nullptr;
    for (const Book &book : m_data.books) {
        if (book.id == bookId)
            return &book;
    }
    return nullptr;
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
