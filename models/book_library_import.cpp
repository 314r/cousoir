// BookLibrary: adding books and formats, the trash, renaming items, and
// the library's check.

#include "book_library.h"
#include "book_library_support.h"

using namespace LibrarySupport;

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
            if (promise.isCanceled())
                return;
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

void BookLibrary::cancelImport()
{
    m_importWatcher.cancel();
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

QString BookLibrary::renameItem(const QString &section, int itemId, const QString &name)
{
    const QList<FacetItem> *items = facetItems(section);
    if (!items)
        return tr("This item is no longer in the library.");
    const auto item = std::find_if(items->cbegin(), items->cend(), [itemId](const FacetItem &i) { return i.id == itemId; });
    if (item == items->cend())
        return tr("This item is no longer in the library.");
    const QString renamed = name.simplified();
    if (renamed == item->name)
        return {};
    if (renamed.isEmpty() && section == QLatin1String("authors"))
        return tr("An author cannot be taken off the books: give another name.");
    // FieldWriters refuses every author while two differ only by case.
    if (section == QLatin1String("authors") && !m_findings.caseDuplicates.isEmpty())
        return tr("Two author names differ only by case: Calibre merges them the next time it saves an author, and "
                  "authors can be renamed then.");
    // The item in its place in each book's list, once.
    const auto replaced = [&renamed](const QList<int> &ids, const QStringList &names, int id) {
        QStringList result;
        for (qsizetype i = 0; i < ids.size(); ++i) {
            const QString value = ids.at(i) == id ? renamed : names.value(i);
            if (!value.isEmpty()
                && std::none_of(result.cbegin(), result.cend(),
                                [&value](const QString &other) { return other.compare(value, Qt::CaseInsensitive) == 0; }))
                result.append(value);
        }
        return result;
    };
    CommitEngine::Request change;
    for (const Book &book : std::as_const(m_data.books)) {
        BookEdit edit;
        edit.bookId = book.id;
        if (section == QLatin1String("authors") && book.authorIds.contains(itemId)) {
            edit.authors = replaced(book.authorIds, book.authors, itemId);
            // An author sort set by hand stays, as the merge of names keeps it.
            if (!LibraryCheck::authorSortFollows(m_data, book))
                edit.authorSort = book.authorSort;
        } else if (section == QLatin1String("tags") && book.tagIds.contains(itemId)) {
            edit.tags = replaced(book.tagIds, book.tags, itemId);
        } else if (section == QLatin1String("series") && book.seriesId == itemId) {
            edit.series = renamed;
            // A book taken out of a series is number 1, as Calibre's bulk
            // edit leaves it.
            if (renamed.isEmpty())
                edit.seriesIndex = 1.0;
        } else if (section == QLatin1String("publishers") && book.publisherId == itemId) {
            edit.publisher = renamed;
        } else {
            continue;
        }
        change.edits.append(edit);
    }
    if (change.edits.isEmpty())
        return tr("No book has this item any more.");
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
                { QStringLiteral("added"), BookDisplay::date(book->timestamp) },
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
