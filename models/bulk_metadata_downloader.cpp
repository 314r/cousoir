#include "bulk_metadata_downloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "book_library.h"
#include "calibre_job.h"
#include "metadata_downloader.h"

namespace {

// As many books looked up at once: Calibre's sources each take their time,
// and the web services do not like more.
constexpr int parallelDownloads = 3;

} // namespace

BulkMetadataDownloader::BulkMetadataDownloader(QObject *parent)
    : QObject(parent)
{
}

BulkMetadataDownloader::~BulkMetadataDownloader()
{
    cancel();
}

BookLibrary *BulkMetadataDownloader::library() const
{
    return m_library;
}

void BulkMetadataDownloader::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    cancel();
    m_library = library;
    emit libraryChanged();
}

QString BulkMetadataDownloader::state() const
{
    return m_state;
}

QString BulkMetadataDownloader::source() const
{
    const QString program = m_programSet ? m_program : MetadataDownload::program();
    return m_native || program.isEmpty() ? QStringLiteral("openlibrary") : QStringLiteral("calibre");
}

bool BulkMetadataDownloader::covers() const
{
    return m_covers;
}

QVariantList BulkMetadataDownloader::rows() const
{
    QVariantList rows;
    for (const Entry &entry : m_entries) {
        rows.append(QVariantMap{ { QStringLiteral("bookId"), entry.bookId },
                                 { QStringLiteral("title"), entry.title },
                                 { QStringLiteral("authors"), entry.authors },
                                 { QStringLiteral("state"), entry.state },
                                 { QStringLiteral("error"), entry.error },
                                 { QStringLiteral("fields"), entry.fields },
                                 { QStringLiteral("cover"), entry.cover },
                                 { QStringLiteral("chosen"), entry.chosen } });
    }
    return rows;
}

int BulkMetadataDownloader::done() const
{
    return m_done;
}

int BulkMetadataDownloader::total() const
{
    return int(m_entries.size());
}

int BulkMetadataDownloader::chosenCount() const
{
    return int(std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry &entry) { return entry.chosen; }));
}

QString BulkMetadataDownloader::start(const QVariantList &bookIds, bool covers)
{
    if (!m_library)
        return tr("No library is open.");
    cancel();
    for (const QVariant &id : bookIds) {
        const Book *book = m_library->data().book(id.toInt());
        if (!book)
            continue;
        Entry entry;
        entry.bookId = book->id;
        entry.title = book->title;
        entry.authors = book->authors.join(QStringLiteral(" & "));
        m_entries.append(entry);
    }
    if (m_entries.isEmpty())
        return tr("There is no book to look up.");
    const QString base = QDir(BookLibrary::cacheFolder(m_library->libraryPath())).filePath(QStringLiteral("downloads"));
    QDir().mkpath(base);
    CalibreJob::removeStaleFolders(base);
    m_folder = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
    if (!m_folder->isValid()) {
        const QString error = tr("Cannot create a folder for the download in %1.").arg(base);
        m_entries.clear();
        m_folder.reset();
        return error;
    }
    m_covers = covers;
    setState(QStringLiteral("downloading"));
    for (int i = 0; i < parallelDownloads && i < m_entries.size(); ++i) {
        auto worker = std::make_shared<Worker>();
        worker->downloader = new MetadataDownloader(this);
        if (m_programSet)
            worker->downloader->setProgram(m_program);
        worker->downloader->setNative(m_native);
        if (m_site)
            worker->downloader->setSite(*m_site);
        worker->downloader->setLibrary(m_library);
        Worker *raw = worker.get();
        connect(worker->downloader, &MetadataDownloader::stateChanged, this, [this, raw] {
            const QString state = raw->downloader->state();
            if (raw->entry < 0)
                return;
            if (state == QLatin1String("found") && m_entries.at(raw->entry).state == QLatin1String("searching")) {
                found(raw);
            } else if (state == QLatin1String("failed")) {
                Entry &entry = m_entries[raw->entry];
                entry.state = QStringLiteral("failed");
                entry.error = raw->downloader->error();
                finish(raw);
            }
        });
        connect(worker->downloader, &MetadataDownloader::coversChanged, this, [this, raw] {
            if (raw->coversRunning && !raw->downloader->searchingCovers())
                coversDone(raw);
        });
        m_workers.append(worker);
    }
    emit rowsChanged();
    for (const auto &worker : std::as_const(m_workers))
        next(worker.get());
    return {};
}

void BulkMetadataDownloader::next(Worker *worker)
{
    worker->entry = -1;
    worker->coversRunning = false;
    if (m_next >= m_entries.size())
        return;
    const int index = m_next++;
    worker->entry = index;
    Entry &entry = m_entries[index];
    entry.state = QStringLiteral("searching");
    emit rowsChanged();
    const QString error = worker->downloader->search(entry.bookId);
    if (!error.isEmpty()) {
        entry.state = QStringLiteral("failed");
        entry.error = error;
        finish(worker);
    }
}

void BulkMetadataDownloader::found(Worker *worker)
{
    Entry &entry = m_entries[worker->entry];
    // The best match, its fields that differ and Calibre does not leave
    // aside, as the bulk download takes them.
    QStringList keys;
    for (const QVariant &field : worker->downloader->fields(0)) {
        const QVariantMap row = field.toMap();
        if (!row.value(QStringLiteral("chosen")).toBool())
            continue;
        keys.append(row.value(QStringLiteral("key")).toString());
        // On one line each, a description's paragraphs run together.
        entry.fields.append(
            QVariantMap{ { QStringLiteral("label"), row.value(QStringLiteral("label")) },
                         { QStringLiteral("current"), row.value(QStringLiteral("current")).toString().simplified() },
                         { QStringLiteral("found"), row.value(QStringLiteral("found")).toString().simplified() } });
    }
    entry.changes = keys.isEmpty() ? QVariantMap() : worker->downloader->changes(0, keys);
    if (!m_covers) {
        finish(worker);
        return;
    }
    entry.state = QStringLiteral("covers");
    emit rowsChanged();
    worker->downloader->searchCovers(0);
    if (worker->downloader->searchingCovers())
        worker->coversRunning = true;
    else
        coversDone(worker);
}

void BulkMetadataDownloader::coversDone(Worker *worker)
{
    worker->coversRunning = false;
    Entry &entry = m_entries[worker->entry];
    // The largest found, kept out of the download's folder, which goes
    // with the next book.
    QVariantMap best;
    qint64 bestArea = 0;
    for (const QVariant &cover : worker->downloader->covers()) {
        const QVariantMap map = cover.toMap();
        const qint64 area = qint64(map.value(QStringLiteral("width")).toInt()) * map.value(QStringLiteral("height")).toInt();
        if (!map.value(QStringLiteral("own")).toBool() && area > bestArea) {
            best = map;
            bestArea = area;
        }
    }
    if (!best.isEmpty()) {
        const QString source = QUrl(best.value(QStringLiteral("url")).toString()).toLocalFile();
        const QString kept = QDir(m_folder->path())
                                 .filePath(QStringLiteral("%1.%2").arg(entry.bookId).arg(QFileInfo(source).suffix()));
        QFile::remove(kept);
        if (QFile::copy(source, kept)) {
            entry.cover = QUrl::fromLocalFile(kept).toString();
            entry.changes.insert(QStringLiteral("coverFile"), kept);
        }
    }
    finish(worker);
}

void BulkMetadataDownloader::finish(Worker *worker)
{
    Entry &entry = m_entries[worker->entry];
    if (entry.state != QLatin1String("failed")) {
        entry.state = entry.changes.isEmpty() ? QStringLiteral("unchanged") : QStringLiteral("found");
        entry.chosen = !entry.changes.isEmpty();
    }
    ++m_done;
    worker->entry = -1;
    // The next book once this one's signals are over, unless all stopped.
    const int generation = m_generation;
    QMetaObject::invokeMethod(
        this,
        [this, worker, generation] {
            if (generation != m_generation)
                return;
            worker->downloader->cancel();
            next(worker);
            if (m_done == m_entries.size() && m_state == QLatin1String("downloading"))
                setState(QStringLiteral("done"));
        },
        Qt::QueuedConnection);
    emit rowsChanged();
}

void BulkMetadataDownloader::setChosen(int row, bool chosen)
{
    if (row < 0 || row >= m_entries.size() || m_entries.at(row).changes.isEmpty() || m_entries.at(row).chosen == chosen)
        return;
    m_entries[row].chosen = chosen;
    emit rowsChanged();
}

QString BulkMetadataDownloader::apply()
{
    if (!m_library || m_state != QLatin1String("done"))
        return tr("There is nothing downloaded to save.");
    QList<QPair<int, QVariantMap>> changes;
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.chosen)
            changes.append({ entry.bookId, entry.changes });
    }
    // The edits have read the covers when it returns.
    const QString error = changes.isEmpty() ? QString() : m_library->updateEachBook(changes);
    if (error.isEmpty())
        cancel();
    return error;
}

void BulkMetadataDownloader::cancel()
{
    for (const auto &worker : std::as_const(m_workers)) {
        worker->downloader->disconnect(this);
        worker->downloader->cancel();
        worker->downloader->deleteLater();
    }
    m_workers.clear();
    ++m_generation;
    m_entries.clear();
    m_next = 0;
    m_done = 0;
    m_folder.reset();
    emit rowsChanged();
    setState(QStringLiteral("idle"));
}

void BulkMetadataDownloader::setProgram(const QString &program)
{
    m_program = program;
    m_programSet = true;
}

void BulkMetadataDownloader::setNative(bool native)
{
    m_native = native;
}

void BulkMetadataDownloader::setSite(const OpenLibrary::Site &site)
{
    m_site = site;
}

void BulkMetadataDownloader::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}
