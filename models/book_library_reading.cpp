// BookLibrary: the books' text and its index, reading, saving to disk,
// dragging out, new libraries.

#include "book_library.h"
#include "book_library_support.h"

using namespace LibrarySupport;

QVariantMap BookLibrary::textIndex() const
{
    const BookTextSearch::Status status = BookTextSearch::status(m_libraryPath);
    return { { QStringLiteral("exists"), status.exists },
             { QStringLiteral("enabled"), m_data.preferences.value(QStringLiteral("fts_enabled")) == QLatin1String("true") },
             { QStringLiteral("indexed"), status.indexed },
             { QStringLiteral("pending"), status.pending } };
}

QString BookLibrary::indexBooksText()
{
    if (!m_loaded || !m_writable)
        return tr("The library cannot be changed right now.");
    if (!BookTextSearch::status(m_libraryPath).exists) {
        // Every format waiting, as Calibre's first indexing leaves them.
        QList<BookTextIndex::Job> formats;
        for (const Book &book : std::as_const(m_data.books)) {
            for (const BookFormat &format : book.formats)
                formats.append({ book.id, format.format });
        }
        QString error;
        bool made = false;
        const QString language = effectiveConfig().interfaceLanguage;
        if (!CommitEngine::withLibraryLock(
                [&] { made = BookTextIndex::create(m_libraryPath, formats, language, &error); }))
            return tr("Calibre is open on this computer: close it to change the library.");
        if (!made)
            return error;
        emit textIndexChanged();
    }
    if (m_data.preferences.value(QStringLiteral("fts_enabled")) != QLatin1String("true")) {
        // Calibre's own switch, which its next start follows: its saves then
        // keep the index too. The save starts the indexing.
        CommitEngine::Request request;
        request.preferences = { { QStringLiteral("fts_enabled"), QStringLiteral("true") } };
        return queueChange(request);
    }
    startTextIndexing();
    return {};
}

BookTextIndexer *BookLibrary::textIndexer()
{
    return &m_textIndexer;
}

void BookLibrary::startTextIndexing()
{
    if (!m_loaded || !m_writable || m_textIndexer.running()
        || m_data.preferences.value(QStringLiteral("fts_enabled")) != QLatin1String("true"))
        return;
    QList<BookTextIndex::Task> tasks;
    for (const BookTextIndex::Job &job : BookTextIndex::waiting(m_libraryPath, nullptr)) {
        BookTextIndex::Task task{ job.book, job.format, {} };
        if (const Book *book = findBook(job.book)) {
            for (const BookFormat &format : book->formats) {
                if (format.format.compare(job.format, Qt::CaseInsensitive) == 0 && QFileInfo::exists(format.file))
                    task.path = format.file;
            }
        }
        tasks.append(task);
    }
    m_textIndexer.start(m_libraryPath, effectiveConfig().interfaceLanguage, tasks);
}

QString BookLibrary::saveToDisk(const QVariantList &bookIds, const QString &folder)
{
    if (!m_loaded)
        return tr("No library is open.");
    if (m_saveToDiskWatcher.isRunning())
        return tr("Books are already being saved to disk.");
    QList<int> ids;
    for (const QVariant &id : bookIds) {
        if (findBook(id.toInt()))
            ids.append(id.toInt());
    }
    if (ids.isEmpty())
        return tr("There is no book to save.");
    const CalibreConfig config = effectiveConfig();
    Opf::Settings opf;
    opf.interfaceLanguage = config.interfaceLanguage;
    const QString version = QCoreApplication::applicationVersion();
    opf.producer = version.isEmpty() ? QStringLiteral("Cousoir") : QStringLiteral("Cousoir (%1)").arg(version);
    opf.producerId = QStringLiteral("cousoir");
    const SaveToDisk::Options options = SaveToDisk::Options::load(CalibreConfig::configDirectory());
    m_saveToDiskFolder = QDir::cleanPath(folder);
    // A copy: the library shown may change meanwhile.
    m_saveToDiskWatcher.setFuture(QtConcurrent::run([data = m_data, ids, folder = m_saveToDiskFolder, options, opf] {
        return SaveToDisk::save(data, ids, folder, options, opf);
    }));
    return {};
}

QStringList BookLibrary::dragUrls(const QVariantList &bookIds) const
{
    // input_format_order of Calibre's preferences, its default.
    static const QStringList order{ QStringLiteral("EPUB"), QStringLiteral("AZW3"), QStringLiteral("MOBI"),
                                    QStringLiteral("LIT"),  QStringLiteral("PRC"),  QStringLiteral("FB2"),
                                    QStringLiteral("HTML"), QStringLiteral("HTM"),  QStringLiteral("XHTM"),
                                    QStringLiteral("SHTML"), QStringLiteral("XHTML"), QStringLiteral("ZIP"),
                                    QStringLiteral("DOCX"), QStringLiteral("ODT"),  QStringLiteral("RTF"),
                                    QStringLiteral("PDF"),  QStringLiteral("TXT") };
    QStringList urls;
    for (const QVariant &id : bookIds) {
        const Book *book = findBook(id.toInt());
        if (!book)
            continue;
        const BookFormat *best = nullptr;
        qsizetype bestRank = order.size();
        for (const BookFormat &format : book->formats) {
            if (format.file.isEmpty() || !QFileInfo::exists(format.file))
                continue;
            const qsizetype rank = order.contains(format.format) ? order.indexOf(format.format) : order.size();
            if (!best || rank < bestRank) {
                best = &format;
                bestRank = rank;
            }
        }
        if (best)
            // Encoded, as a URI list holds them.
            urls.append(QString::fromLatin1(QUrl::fromLocalFile(best->file).toEncoded()));
    }
    return urls;
}

QString BookLibrary::createLibrary(const QString &folder) const
{
    QString error;
    return NewLibrary::create(QDir::cleanPath(folder), &error) ? QString() : error;
}

QVariantList BookLibrary::readingRows() const
{
    QList<const Book *> read;
    for (const Book &book : std::as_const(m_data.books)) {
        if (book.readEpoch > 0)
            read.append(&book);
    }
    std::sort(read.begin(), read.end(), [](const Book *a, const Book *b) { return a->readEpoch > b->readEpoch; });
    const QString coverKey = coverKeyOf(m_libraryPath);
    const QDate today = QDate::currentDate();
    QVariantList rows;
    for (const Book *book : std::as_const(read)) {
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(qint64(book->readEpoch * 1000));
        const qint64 days = when.date().daysTo(today);
        rows.append(QVariantMap{
            { QStringLiteral("bookId"), book->id },
            { QStringLiteral("title"), book->title },
            { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
            { QStringLiteral("cover"), book->coverFile.isEmpty()
                  ? QString() : CoverCache::url(coverKey, book->id, book->coverModified) },
            { QStringLiteral("fraction"), book->readFraction },
            { QStringLiteral("progress"), BookDisplay::progress(book->readFraction) },
            { QStringLiteral("when"), days <= 0 ? tr("Today")
                                      : days == 1 ? tr("Yesterday")
                                      : days < 7 ? tr("%n days ago", "", int(days))
                                                 : BookDisplay::date(when) },
            // Calibre's viewer leaves the last pages a little short of the end.
            { QStringLiteral("finished"), book->readFraction >= 0.98 },
        });
    }
    return rows;
}

QVariantMap BookLibrary::searchBooksText(const QString &query) const
{
    QString error;
    const QList<BookTextSearch::Hit> hits = query.trimmed().isEmpty()
        ? QList<BookTextSearch::Hit>()
        : BookTextSearch::find(m_libraryPath, query, true, effectiveConfig().interfaceLanguage, 200, &error);
    QVariantList rows;
    QSet<int> shown;
    for (const BookTextSearch::Hit &hit : hits) {
        const Book *book = findBook(hit.bookId);
        // A book Calibre indexed and the library no longer has; or one
        // already shown from another of its formats, ranked higher.
        if (!book || shown.contains(book->id))
            continue;
        shown.insert(book->id);
        rows.append(QVariantMap{ { QStringLiteral("bookId"), book->id },
                                 { QStringLiteral("title"), book->title },
                                 { QStringLiteral("authors"), book->authors.join(QStringLiteral(" & ")) },
                                 { QStringLiteral("format"), hit.format },
                                 { QStringLiteral("snippet"), hit.snippet } });
    }
    return { { QStringLiteral("rows"), rows }, { QStringLiteral("error"), error } };
}

int BookLibrary::bookOfFile(const QString &path) const
{
    const QString wanted = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (const Book &book : m_data.books) {
        for (const BookFormat &format : book.formats) {
            if (!format.file.isEmpty() && QDir::cleanPath(format.file) == wanted)
                return book.id;
        }
    }
    return 0;
}

QString BookLibrary::readableFormat(int bookId) const
{
    const Book *book = findBook(bookId);
    if (!book || !readerAvailable())
        return {};
    QStringList wanted{ QStringLiteral("EPUB"), QStringLiteral("KEPUB"), QStringLiteral("AZW3"), QStringLiteral("MOBI"),
                        QStringLiteral("AZW"), QStringLiteral("FB2"), QStringLiteral("FBZ"), QStringLiteral("PDF"),
                        QStringLiteral("TXT") };
#ifdef COUSOIR_READER
    if (!BookReader::converter().isEmpty())
        wanted += BookReader::convertedFormats();
#endif
    for (const QString &name : std::as_const(wanted)) {
        for (const BookFormat &format : book->formats) {
            if (format.format == name && !format.file.isEmpty())
                return format.format;
        }
    }
    return {};
}

int BookLibrary::saveReading(int bookId, const std::optional<ReadPosition> &position, const QList<Annotation> &annotations)
{
    if (!m_writable || !findBook(bookId))
        return -1;
    CommitEngine::Request request;
    if (position && !position->cfi.isEmpty())
        request.readPositions.append({ bookId, *position });
    for (const Annotation &annotation : annotations)
        request.annotations.append({ bookId, annotation });
    if (request.readPositions.isEmpty() && request.annotations.isEmpty())
        return 0;
    return queueChange(request).isEmpty() ? m_lastChangeId : -1;
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
         db.rows(QStringLiteral("SELECT annot_type, annot_id, timestamp, annot_data, searchable_text, user_type, user "
                                "FROM annotations WHERE book=? AND format=? "
                                "AND ((user_type='local' AND user='viewer') OR user_type='kobo')"),
                 { bookId, format.toUpper() })) {
        found.append({ format.toUpper(), row.value(0).toString(), row.value(1).toString(), row.value(2).toDouble(),
                       row.value(3).toString().toUtf8(), row.value(4).toString(), row.value(5).toString(),
                       row.value(6).toString() });
    }
    return found;
}
