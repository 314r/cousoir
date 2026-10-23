#include "conversion_service.h"

#include <QDir>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

#include "book_library.h"
#include "calibre_job.h"
#include "conversion.h"
#include "kepub.h"

ConversionService::ConversionService(QObject *parent)
    : QObject(parent)
    , m_program(Conversion::program())
{
}

ConversionService::~ConversionService()
{
    stopProcess();
}

BookLibrary *ConversionService::library() const
{
    return m_library;
}

void ConversionService::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    cancel();
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library) {
        // A conversion waits for the changes before it; its folder waits for
        // its own change.
        connect(m_library, &BookLibrary::savingChanged, this, [this] {
            if (!m_library->saving()) {
                releaseFolders();
                startNext();
            }
        });
        connect(m_library, &BookLibrary::loadingChanged, this, &ConversionService::startNext);
        connect(m_library, &BookLibrary::libraryPathChanged, this, [this] { cancel(); });
    }
    emit libraryChanged();
}

bool ConversionService::calibre() const
{
    return !m_program.isEmpty() && !m_native;
}

namespace {

// The book's EPUB, for Cousoir's own KEPUB.
QString epubOf(const Book &book)
{
    for (const BookFormat &format : book.formats) {
        if (format.format == QLatin1String("EPUB") && !format.file.isEmpty())
            return format.file;
    }
    return {};
}

} // namespace

QVariantList ConversionService::jobs() const
{
    QVariantList jobs;
    for (qsizetype index = 0; index < m_jobs.size(); ++index) {
        const Job &job = m_jobs.at(index);
        jobs.append(QVariantMap{
            { QStringLiteral("bookId"), job.bookId },
            { QStringLiteral("title"), job.title },
            { QStringLiteral("format"), job.format },
            { QStringLiteral("running"), index == 0 && (m_process || m_nativeJob) },
            { QStringLiteral("progress"), job.progress },
        });
    }
    return jobs;
}

bool ConversionService::busy() const
{
    return !m_jobs.isEmpty();
}

QStringList ConversionService::formatsFor(int bookId) const
{
    if (!m_library)
        return {};
    const Book *book = m_library->data().book(bookId);
    if (!book)
        return {};
    if (!calibre())
        return epubOf(*book).isEmpty() ? QStringList() : QStringList{ QStringLiteral("KEPUB") };
    return !Conversion::inputFormats(*book).isEmpty() ? Conversion::outputFormats() : QStringList();
}

QString ConversionService::convert(int bookId, const QString &format)
{
    if (!m_library)
        return tr("No library is open.");
    if (!m_library->writable())
        return tr("The library cannot be changed right now.");
    const Book *book = m_library->data().book(bookId);
    if (!book)
        return tr("This book is no longer in the library.");
    const QString upper = format.toUpper();
    if (!Conversion::outputFormats().contains(upper))
        return tr("Cousoir does not convert to %1.").arg(format);
    if (!calibre() && (upper != QLatin1String("KEPUB") || epubOf(*book).isEmpty()))
        return upper == QLatin1String("KEPUB") ? tr("Without Calibre, Cousoir makes a KEPUB of an EPUB only.")
                                               : tr("Converting to %1 needs Calibre, which is not installed.").arg(upper);
    if (calibre() && Conversion::inputFormats(*book).isEmpty())
        return tr("Calibre converts none of this book's formats.");
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.bookId == bookId && job.format == upper)
            return {};
    }
    m_jobs.append({ bookId, book->title, upper, 0 });
    emit jobsChanged();
    startNext();
    return {};
}

void ConversionService::cancel(int bookId)
{
    const auto matches = [bookId](const Job &job) { return bookId == 0 || job.bookId == bookId; };
    if (m_process && matches(m_jobs.first())) {
        stopProcess();
        m_folder.reset();
    }
    // The running one stays when it is another book's, or Cousoir's own,
    // which ends in a moment.
    const qsizetype from = m_process || m_nativeJob ? 1 : 0;
    const qsizetype before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin() + from, m_jobs.end(), matches), m_jobs.end());
    if (m_jobs.size() != before)
        emit jobsChanged();
    startNext();
}

void ConversionService::setNative(bool native)
{
    m_native = native;
    emit calibreChanged();
}

void ConversionService::setProgram(const QString &program)
{
    m_program = program.isEmpty() ? Conversion::program() : program;
}

void ConversionService::startNext()
{
    if (m_process || m_nativeJob || m_jobs.isEmpty() || !m_library || m_library->saving() || m_library->loading())
        return;
    const Job job = m_jobs.first();
    const Book *book = m_library->data().book(job.bookId);
    QString error;
    if (!book)
        error = tr("“%1” is no longer in the library.").arg(job.title);
    else if (!m_library->writable())
        error = tr("The library cannot be changed right now; “%1” was not converted.").arg(job.title);
    const QString base = QDir(BookLibrary::cacheFolder(m_library->libraryPath())).filePath(QStringLiteral("conversions"));
    std::shared_ptr<QTemporaryDir> folder;
    if (error.isEmpty()) {
        QDir().mkpath(base);
        CalibreJob::removeStaleFolders(base);
        folder = std::make_shared<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
        if (!folder->isValid())
            error = tr("Cannot create a folder for the conversion in %1.").arg(base);
        else if (calibre())
            Conversion::prepare(m_library->data(), *book, job.format, m_library->interfaceLanguage(), folder->path(),
                                &error);
    }
    if (!error.isEmpty()) {
        m_jobs.removeFirst();
        emit jobsChanged();
        emit failed(error);
        startNext();
        return;
    }

    m_folder = folder;
    if (!calibre()) {
        startNative(*book);
        return;
    }
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(CalibreJob::environment(folder->path(), m_library->interfaceLanguage()));
    m_process->setStandardErrorFile(QProcess::nullDevice());
    // A session of its own: stopping it stops Calibre's worker with it.
    m_process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ConversionService::readProgress);
    connect(m_process, &QProcess::finished, this, &ConversionService::finishJob);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
        if (processError != QProcess::FailedToStart)
            return;
        const Job job = m_jobs.takeFirst();
        m_process->deleteLater();
        m_process = nullptr;
        m_folder.reset();
        emit jobsChanged();
        emit failed(tr("Cannot run Calibre to convert “%1”.").arg(job.title));
        startNext();
    });
    m_process->start(m_program, Conversion::arguments(folder->path()));
    emit jobsChanged();
}

void ConversionService::readProgress()
{
    if (!m_process)
        return;
    bool changed = false;
    while (m_process->canReadLine()) {
        const double fraction = CalibreJob::progress(m_process->readLine());
        if (fraction >= 0 && !m_jobs.isEmpty()) {
            m_jobs.first().progress = fraction;
            changed = true;
        }
    }
    if (changed)
        emit jobsChanged();
}

void ConversionService::finishJob()
{
    readProgress();
    const int exitCode = m_process->exitStatus() == QProcess::NormalExit ? m_process->exitCode() : -1;
    m_process->deleteLater();
    m_process = nullptr;
    const Job job = m_jobs.takeFirst();
    const std::shared_ptr<QTemporaryDir> folder = std::exchange(m_folder, nullptr);
    const Conversion::Outcome outcome = Conversion::outcome(folder->path(), exitCode);
    QString error = outcome.error;
    if (outcome.ok)
        error = m_library ? m_library->addConversion(job.bookId, job.format, outcome) : tr("No library is open.");
    emit jobsChanged();
    if (error.isEmpty()) {
        m_handedOver.append(folder);
        emit converted(job.bookId, job.title, job.format);
    } else {
        emit failed(tr("“%1” was not converted to %2: %3").arg(job.title, job.format, error));
    }
    startNext();
}

void ConversionService::startNative(const Book &book)
{
    // What the conversion reads of the book, taken now: the library may
    // change while it runs.
    const Kepub::Options options = Kepub::optionsFor(book);
    const QString epub = epubOf(book);
    const QString output = QDir(m_folder->path()).filePath(QStringLiteral("output.kepub"));
    m_nativeJob = new QFutureWatcher<QString>(this);
    connect(m_nativeJob, &QFutureWatcher<QString>::finished, this, &ConversionService::finishNative);
    m_nativeJob->setFuture(QtConcurrent::run([epub, output, options] {
        QString error;
        return Kepub::kepubify(epub, output, options, &error) ? QString() : error;
    }));
    emit jobsChanged();
}

void ConversionService::finishNative()
{
    const QString error = m_nativeJob->result();
    m_nativeJob->deleteLater();
    m_nativeJob = nullptr;
    const Job job = m_jobs.takeFirst();
    const std::shared_ptr<QTemporaryDir> folder = std::exchange(m_folder, nullptr);
    QString failure = error;
    if (failure.isEmpty()) {
        Conversion::Outcome outcome;
        outcome.ok = true;
        outcome.inputFormat = QStringLiteral("epub");
        outcome.file = QDir(folder->path()).filePath(QStringLiteral("output.kepub"));
        failure = m_library ? m_library->addConversion(job.bookId, job.format, outcome) : tr("No library is open.");
    }
    emit jobsChanged();
    if (failure.isEmpty()) {
        m_handedOver.append(folder);
        emit converted(job.bookId, job.title, job.format);
    } else {
        emit failed(tr("“%1” was not converted to %2: %3").arg(job.title, job.format, failure));
    }
    startNext();
}

void ConversionService::stopProcess()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    CalibreJob::stopProcessGroup(m_process);
    delete m_process;
    m_process = nullptr;
}

void ConversionService::releaseFolders()
{
    m_handedOver.clear();
}
