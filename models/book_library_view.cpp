// BookLibrary: the section shown, its filter and search, the virtual
// libraries and saved searches, the sort and columns, the current book.

#include "book_library.h"
#include "book_library_support.h"

using namespace LibrarySupport;

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

QString BookLibrary::coverUrl(int bookId) const
{
    const Book *book = findBook(bookId);
    return book && !book->coverFile.isEmpty() ? CoverCache::url(coverKeyOf(m_libraryPath), book->id, book->coverModified)
                                              : QString();
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
