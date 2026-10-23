#include "metadata_downloader.h"

#include <QDir>
#include <QImageReader>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>

#include "book.h"
#include "book_library.h"
#include "calibre_config.h"
#include "calibre_job.h"

MetadataDownloader::MetadataDownloader(QObject *parent)
    : QObject(parent)
    , m_program(MetadataDownload::program())
    , m_web(new OpenLibrary::Client(this))
{
    connect(m_web, &OpenLibrary::Client::identified, this,
            [this](const QList<MetadataDownload::Found> &books, const QString &error) {
                if (!error.isEmpty()) {
                    setState(QStringLiteral("failed"), error);
                } else if (books.isEmpty()) {
                    setState(QStringLiteral("failed"), tr("Open Library found nothing for this book."));
                } else {
                    m_identified.books = books;
                    setState(QStringLiteral("found"));
                }
            });
    connect(m_web, &OpenLibrary::Client::coverFound, this, [this](const MetadataDownload::Cover &cover) {
        m_covers.append(cover);
        emit coversChanged();
    });
    connect(m_web, &OpenLibrary::Client::coversFinished, this, [this](const QString &error) {
        m_coversRunning = false;
        m_coversError = error;
        // The largest first once all are in, as Calibre orders them.
        std::stable_sort(m_covers.begin(), m_covers.end(), [](const auto &a, const auto &b) {
            return qint64(a.width) * a.height > qint64(b.width) * b.height;
        });
        emit coversChanged();
    });
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

QString MetadataDownloader::source() const
{
    return native() ? QStringLiteral("openlibrary") : QStringLiteral("calibre");
}

bool MetadataDownloader::native() const
{
    return m_native || m_program.isEmpty();
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
    if (m_bookId == 0)
        return covers;
    // Kept unless another is picked, as in Calibre's dialog.
    const QString own = coverUrl();
    const QSize size = own.isEmpty() ? QSize() : QImageReader(QUrl(own).toLocalFile()).size();
    covers.append(QVariantMap{
        { QStringLiteral("source"), own.isEmpty() ? tr("No cover") : tr("Current cover") },
        { QStringLiteral("width"), size.width() },
        { QStringLiteral("height"), size.height() },
        { QStringLiteral("url"), own },
        { QStringLiteral("own"), true },
    });
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

QString MetadataDownloader::coversError() const
{
    return m_coversError;
}

QString MetadataDownloader::search(int bookId)
{
    if (!m_library)
        return tr("No library is open.");
    const Book *book = m_library->data().book(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    cancel();
    m_bookId = bookId;
    // The dialog's: the book's title, authors and identifiers as they are.
    m_query = { book->title, book->authors, book->identifiers };
    if (native()) {
        QString error;
        if (!makeFolder(&error)) {
            setState(QStringLiteral("failed"), error);
            return {};
        }
        // Calibre's preferences for downloads, when Calibre left some.
        const OpenLibrary::Preferences preferences = OpenLibrary::Preferences::load(CalibreConfig::configDirectory());
        m_identified.ignoredFields = preferences.ignoredFields;
        m_identified.appendComments = preferences.appendComments;
        setState(QStringLiteral("searching"));
        m_web->identify(m_query, book->languages.value(0), m_library->interfaceLanguage(), preferences);
        return {};
    }
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
    if (m_state != QLatin1String("found") || result < 0 || result >= m_identified.books.size())
        return;
    // Those asked for before stop.
    stopCovers();
    // The dialog's: the book's title and authors, the identifiers found.
    m_query.identifiers = m_identified.books.at(result).identifiers;
    m_covers.clear();
    m_coversError.clear();
    if (native()) {
        m_coversRunning = true;
        emit coversChanged();
        m_web->covers(result, m_folder->path());
        return;
    }
    start(true);
}

QString MetadataDownloader::apply(int result, const QStringList &fields, const QString &coverUrl)
{
    if (!m_library || m_state != QLatin1String("found") || result < 0 || result >= m_identified.books.size())
        return tr("There is nothing downloaded to save.");
    QVariantMap changes = m_library->downloadedChanges(m_bookId, m_identified.books.at(result), fields,
                                                       m_identified.appendComments);
    if (!coverUrl.isEmpty()) {
        const QString file = QUrl(coverUrl).toLocalFile();
        const auto found = std::find_if(m_covers.cbegin(), m_covers.cend(),
                                        [&file](const MetadataDownload::Cover &cover) { return cover.file == file; });
        if (found == m_covers.cend())
            return tr("That cover is no longer there.");
        changes.insert(QStringLiteral("coverFile"), found->file);
    }
    // The edit has read the cover when it returns.
    const QString error = changes.isEmpty() ? QString() : m_library->updateBook(m_bookId, changes);
    if (error.isEmpty())
        cancel();
    return error;
}

void MetadataDownloader::cancel()
{
    stopProcess();
    m_web->stop();
    m_coverPoll.stop();
    m_folder.reset();
    m_identified = {};
    m_covers.clear();
    m_coversError.clear();
    m_coversRunning = false;
    m_bookId = 0;
    setState(QStringLiteral("idle"));
    emit coversChanged();
}

void MetadataDownloader::stopCovers()
{
    if (!m_coversRunning)
        return;
    stopProcess();
    m_web->stopCovers();
    m_coverPoll.stop();
    m_coversRunning = false;
}

void MetadataDownloader::setProgram(const QString &program)
{
    m_program = program.isEmpty() ? MetadataDownload::program() : program;
}

void MetadataDownloader::setNative(bool native)
{
    const QString before = source();
    m_native = native;
    if (source() != before)
        emit sourceChanged();
}

void MetadataDownloader::setSite(const OpenLibrary::Site &site)
{
    m_web->setSite(site);
}

bool MetadataDownloader::makeFolder(QString *error)
{
    // A folder of the library's cache, for both jobs, or for the covers.
    const QString base = QDir(BookLibrary::cacheFolder(m_library->libraryPath())).filePath(QStringLiteral("downloads"));
    QDir().mkpath(base);
    CalibreJob::removeStaleFolders(base);
    m_folder = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
    if (m_folder->isValid())
        return true;
    *error = tr("Cannot create a folder for the download in %1.").arg(base);
    return false;
}

void MetadataDownloader::start(bool covers)
{
    QString error;
    if (!covers)
        makeFolder(&error);
    if (error.isEmpty()) {
        if (covers)
            MetadataDownload::prepareCovers(m_folder->path(), m_query, &error);
        else
            MetadataDownload::prepareIdentify(m_folder->path(), m_query, &error);
    }
    if (!error.isEmpty()) {
        if (covers) {
            m_coversError = error;
            emit coversChanged();
        } else {
            setState(QStringLiteral("failed"), error);
        }
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
            m_coversError = tr("Cannot run Calibre to download covers.");
            emit coversChanged();
        } else {
            setState(QStringLiteral("failed"), tr("Cannot run Calibre to download metadata."));
        }
    });
    // Before it starts: a start that fails at once says so on its own.
    if (covers) {
        m_coversRunning = true;
        m_coverPoll.start();
        emit coversChanged();
    } else {
        setState(QStringLiteral("searching"));
    }
    m_process->start(m_program, MetadataDownload::arguments(m_folder->path()));
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
        if (MetadataDownload::readCovers(m_folder->path(), &covers, &error)) {
            m_covers = covers;
        } else {
            // Those it saved before it stopped, and why.
            m_covers = MetadataDownload::coversFound(m_folder->path());
            m_coversError = error;
        }
        emit coversChanged();
        return;
    }
    MetadataDownload::Identified identified;
    if (!MetadataDownload::readIdentified(m_folder->path(), &identified, &error)) {
        setState(QStringLiteral("failed"), error);
        return;
    }
    if (identified.books.isEmpty()) {
        // The sources' log tells whether they could be asked.
        const QString log = CalibreJob::logTail(m_folder->path());
        setState(QStringLiteral("failed"), log.isEmpty() ? tr("Calibre's sources found nothing for this book.")
                                                         : tr("Calibre's sources found nothing for this book. "
                                                              "Their log ends with: %1").arg(log));
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
