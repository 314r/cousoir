#include "metadata_downloader.h"

#include <QDir>
#include <QImageReader>
#include <QLocale>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>

#include "book.h"
#include "book_library.h"
#include "calibre_job.h"

MetadataDownloader::MetadataDownloader(QObject *parent)
    : QObject(parent)
    , m_program(MetadataDownload::program())
{
    // single_covers() saves each cover as it comes.
    m_coverPoll.setInterval(250);
    connect(&m_coverPoll, &QTimer::timeout, this, &MetadataDownloader::readCovers);
}

MetadataDownloader::~MetadataDownloader()
{
    stopProcess();
}

BookLibrary *MetadataDownloader::library() const
{
    return m_library;
}

void MetadataDownloader::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    cancel();
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library)
        connect(m_library, &BookLibrary::libraryPathChanged, this, &MetadataDownloader::cancel);
    emit libraryChanged();
}

bool MetadataDownloader::available() const
{
    return !m_program.isEmpty();
}

QString MetadataDownloader::state() const
{
    return m_state;
}

int MetadataDownloader::bookId() const
{
    return m_bookId;
}

QString MetadataDownloader::error() const
{
    return m_error;
}

QVariantList MetadataDownloader::results() const
{
    QVariantList results;
    for (const MetadataDownload::Found &found : m_identified.books) {
        results.append(QVariantMap{
            { QStringLiteral("title"), found.title },
            { QStringLiteral("authors"), found.authors.join(QStringLiteral(" & ")) },
            // The year, as the dialog shows it.
            { QStringLiteral("published"), found.pubdate ? QString::number(found.pubdate->year()) : QString() },
            { QStringLiteral("publisher"), found.publisher },
            { QStringLiteral("hasCover"), found.hasCover },
            { QStringLiteral("hasSummary"), !found.comments.trimmed().isEmpty() },
        });
    }
    return results;
}

QVariantList MetadataDownloader::covers() const
{
    QVariantList covers;
    const QString own = coverUrl();
    if (!own.isEmpty()) {
        const QSize size = QImageReader(QUrl(own).toLocalFile()).size();
        covers.append(QVariantMap{
            { QStringLiteral("source"), tr("Current cover") },
            { QStringLiteral("width"), size.width() },
            { QStringLiteral("height"), size.height() },
            { QStringLiteral("url"), own },
            { QStringLiteral("own"), true },
        });
    }
    for (const MetadataDownload::Cover &cover : m_covers) {
        covers.append(QVariantMap{
            { QStringLiteral("source"), cover.source },
            { QStringLiteral("width"), cover.width },
            { QStringLiteral("height"), cover.height },
            { QStringLiteral("url"), QUrl::fromLocalFile(cover.file).toString() },
            { QStringLiteral("own"), false },
        });
    }
    return covers;
}

bool MetadataDownloader::searchingCovers() const
{
    return m_coversRunning;
}

QString MetadataDownloader::search(int bookId)
{
    if (!m_library)
        return tr("No library is open.");
    if (!available())
        return tr("Downloading metadata needs Calibre, which is not installed.");
    const Book *book = m_library->data().book(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    cancel();
    m_bookId = bookId;
    // The dialog's: the book's title, authors and identifiers as they are.
    m_query = { book->title, book->authors, book->identifiers };
    start(false);
    return {};
}

QVariantList MetadataDownloader::fields(int result) const
{
    if (!m_library || result < 0 || result >= m_identified.books.size())
        return {};
    return m_library->downloadFields(m_bookId, m_identified.books.at(result), m_identified.ignoredFields,
                                     m_identified.appendComments);
}

void MetadataDownloader::searchCovers(int result)
{
    if (m_state != QLatin1String("found") || result < 0 || result >= m_identified.books.size() || m_coversRunning)
        return;
    // The dialog's: the book's title and authors, the identifiers found.
    m_query.identifiers = m_identified.books.at(result).identifiers;
    m_covers.clear();
    start(true);
}

QString MetadataDownloader::apply(int result, const QStringList &fields, int cover)
{
    if (!m_library || m_state != QLatin1String("found") || result < 0 || result >= m_identified.books.size())
        return tr("There is nothing downloaded to save.");
    QVariantMap changes = m_library->downloadedChanges(m_bookId, m_identified.books.at(result), fields,
                                                       m_identified.appendComments);
    // The covers found come after the book's own.
    const int found = cover - (coverUrl().isEmpty() ? 0 : 1);
    if (cover >= 0 && found >= 0 && found < m_covers.size())
        changes.insert(QStringLiteral("coverFile"), m_covers.at(found).file);
    // The edit has read the cover when it returns.
    const QString error = changes.isEmpty() ? QString() : m_library->updateBook(m_bookId, changes);
    if (error.isEmpty())
        cancel();
    return error;
}

void MetadataDownloader::cancel()
{
    stopProcess();
    m_coverPoll.stop();
    m_folder.reset();
    m_identified = {};
    m_covers.clear();
    const bool coversWere = m_coversRunning;
    m_coversRunning = false;
    m_bookId = 0;
    setState(QStringLiteral("idle"));
    if (coversWere)
        emit coversChanged();
}

void MetadataDownloader::setProgram(const QString &program)
{
    const bool wasAvailable = available();
    m_program = program.isEmpty() ? MetadataDownload::program() : program;
    if (available() != wasAvailable)
        emit availableChanged();
}

void MetadataDownloader::start(bool covers)
{
    QString error;
    if (!covers) {
        // A folder of the library's cache, for both jobs.
        const QString base = QDir(BookLibrary::cacheFolder(m_library->libraryPath())).filePath(QStringLiteral("downloads"));
        QDir().mkpath(base);
        CalibreJob::removeStaleFolders(base);
        m_folder = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
        if (!m_folder->isValid())
            error = tr("Cannot create a folder for the download in %1.").arg(base);
    }
    if (error.isEmpty()) {
        if (covers)
            MetadataDownload::prepareCovers(m_folder->path(), m_query, &error);
        else
            MetadataDownload::prepareIdentify(m_folder->path(), m_query, &error);
    }
    if (!error.isEmpty()) {
        if (covers)
            return;
        setState(QStringLiteral("failed"), error);
        return;
    }
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(CalibreJob::environment(m_folder->path(), m_library->interfaceLanguage()));
    m_process->setStandardOutputFile(QProcess::nullDevice());
    m_process->setStandardErrorFile(QProcess::nullDevice());
    // A session of its own: stopping it stops what Calibre started.
    m_process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    connect(m_process, &QProcess::finished, this, &MetadataDownloader::finishJob);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart)
            return;
        m_process->deleteLater();
        m_process = nullptr;
        if (m_coversRunning) {
            m_coversRunning = false;
            m_coverPoll.stop();
            emit coversChanged();
        } else {
            setState(QStringLiteral("failed"), tr("Cannot run Calibre to download metadata."));
        }
    });
    m_process->start(m_program, MetadataDownload::arguments(m_folder->path()));
    if (covers) {
        m_coversRunning = true;
        m_coverPoll.start();
        emit coversChanged();
    } else {
        setState(QStringLiteral("searching"));
    }
}

void MetadataDownloader::finishJob()
{
    m_process->deleteLater();
    m_process = nullptr;
    QString error;
    if (m_coversRunning) {
        m_coverPoll.stop();
        m_coversRunning = false;
        QList<MetadataDownload::Cover> covers;
        if (MetadataDownload::readCovers(m_folder->path(), &covers, &error))
            m_covers = covers;
        else
            readCovers();
        emit coversChanged();
        return;
    }
    MetadataDownload::Identified identified;
    if (!MetadataDownload::readIdentified(m_folder->path(), &identified, &error)) {
        setState(QStringLiteral("failed"), error);
        return;
    }
    if (identified.books.isEmpty()) {
        setState(QStringLiteral("failed"), tr("Calibre's sources found nothing for this book."));
        return;
    }
    m_identified = identified;
    setState(QStringLiteral("found"));
}

void MetadataDownloader::readCovers()
{
    if (!m_folder)
        return;
    const QList<MetadataDownload::Cover> covers = MetadataDownload::coversFound(m_folder->path());
    if (covers.size() == m_covers.size())
        return;
    m_covers = covers;
    emit coversChanged();
}

void MetadataDownloader::stopProcess()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    CalibreJob::stopProcessGroup(m_process);
    delete m_process;
    m_process = nullptr;
}

void MetadataDownloader::setState(const QString &state, const QString &error)
{
    if (m_state == state && m_error == error)
        return;
    m_state = state;
    m_error = error;
    emit stateChanged();
}

QString MetadataDownloader::coverUrl() const
{
    const Book *book = m_library && m_bookId ? m_library->data().book(m_bookId) : nullptr;
    return book && !book->coverFile.isEmpty() ? QUrl::fromLocalFile(book->coverFile).toString() : QString();
}
