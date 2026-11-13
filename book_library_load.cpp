// BookLibrary: loading the library, and the rows, items and book shown.

#include "book_library.h"
#include "book_library_support.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

using namespace LibrarySupport;

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
        if (!CommitEngine::recover(path, cache, &result.recovered, &error, &result.recoveryWaits)) {
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
    if (result.recoveryWaits)
        emit recoveryWaiting();
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
    startTextIndexing();
}

BookLibrary::BookIndex BookLibrary::indexBooks(const LibraryData &data, const CalibreConfig &config,
                                               const QStringList &sortFields, const CalibreSearch::Index *previous,
                                               const QSet<int> &changed)
{
    BookIndex index;
    const CalibreSearch::Preferences preferences = CalibreSearch::Preferences::fromRows(data.preferences);
    index.search = previous ? CalibreSearch::Index(*previous, data, changed, preferences, config.tweaks,
                                                   config.interfaceLanguage)
                            : CalibreSearch::Index(data, preferences, config.tweaks, config.interfaceLanguage);
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
    releaseFreedMemory();
}

void BookLibrary::releaseFreedMemory()
{
#ifdef __GLIBC__
    // The library before this one is gone, made on worker threads whose
    // arenas glibc keeps for them: give what they free back to the system,
    // or each save and reload would leave Cousoir larger. A few
    // milliseconds at 50,000 books, about 50 MB less.
    malloc_trim(0);
#endif
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
