#include "device_library.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <utility>

#include <signal.h>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#endif

#include "book_library.h"
#include "calibre_config.h"
#include "calibre_lock.h"
#include "concurrency_guard.h"
#include "kobo_send.h"

namespace {

// The filesystems a Kobo's storage comes as: no other volume is looked into,
// which keeps network shares out.
bool removable(const QStorageInfo &storage)
{
    static const QList<QByteArray> types{ "vfat", "msdos", "fat", "exfat", "fuseblk" };
    return types.contains(storage.fileSystemType().toLower());
}

QList<Volume> mountedVolumes()
{
    QList<Volume> volumes;
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && storage.isReady() && removable(storage))
            volumes.append({ storage.rootPath(), QString::fromLocal8Bit(storage.device()) });
    }
    return volumes;
}

#ifdef Q_OS_LINUX
const QString udisks = QStringLiteral("org.freedesktop.UDisks2");

// Unmounts the Kobo's filesystem through UDisks2, as a file manager's
// "Safely remove" does, then powers its drive off, which takes the Kobo out
// of its USB mode.
// The block device's object in UDisks2, which spells "-" as "_2d".
QString udisksBlock(const QString &node)
{
    QString name;
    for (const char c : QFileInfo(node).fileName().toUtf8()) {
        if (std::isalnum(uchar(c)))
            name += QLatin1Char(c);
        else
            name += QStringLiteral("_%1").arg(uchar(c), 2, 16, QLatin1Char('0'));
    }
    return QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + name;
}

void udisksEject(const Kobo::Device &device, std::function<void(const QString &)> done)
{
    const QString block = udisksBlock(device.node);
    QDBusConnection bus = QDBusConnection::systemBus();
    QDBusMessage unmount = QDBusMessage::createMethodCall(udisks, block, udisks + QStringLiteral(".Filesystem"),
                                                          QStringLiteral("Unmount"));
    unmount << QVariantMap();
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(unmount, 120000));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [=](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        if (call->isError()) {
            done(QObject::tr("Cannot eject %1: %2").arg(device.name, call->error().message()));
            return;
        }
        QDBusMessage get = QDBusMessage::createMethodCall(udisks, block, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("Get"));
        get << udisks + QStringLiteral(".Block") << QStringLiteral("Drive");
        const QDBusMessage reply = QDBusConnection::systemBus().call(get);
        const QDBusObjectPath drive = reply.arguments().isEmpty()
            ? QDBusObjectPath()
            : qvariant_cast<QDBusObjectPath>(qvariant_cast<QDBusVariant>(reply.arguments().first()).variant());
        // Unmounted is enough to unplug it; powered off, the Kobo leaves its
        // USB mode by itself.
        if (!drive.path().isEmpty() && drive.path() != QLatin1String("/")) {
            QDBusMessage powerOff = QDBusMessage::createMethodCall(udisks, drive.path(), udisks + QStringLiteral(".Drive"),
                                                                   QStringLiteral("PowerOff"));
            powerOff << QVariantMap();
            QDBusConnection::systemBus().asyncCall(powerOff);
        }
        done({});
    });
}
#else
void udisksEject(const Kobo::Device &device, std::function<void(const QString &)> done)
{
    done(QObject::tr("Eject %1 from the system's file manager.").arg(device.name));
}
#endif

// Folders a send left behind, a crash or a kill, a day old.
void removeStaleFolders(const QString &base)
{
    const QDateTime dayAgo = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &folder : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (folder.lastModified() < dayAgo)
            QDir(folder.filePath()).removeRecursively();
    }
}

// As the library shows the size of its files.
QString sizeText(qint64 size)
{
    return QLocale::c().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

const Book *bookIn(const LibraryData &data, int bookId)
{
    for (const Book &book : data.books) {
        if (book.id == bookId)
            return &book;
    }
    return nullptr;
}

QStringList formatsOf(const Book &book)
{
    QStringList formats;
    for (const BookFormat &format : book.formats)
        formats.append(format.format);
    return formats;
}

} // namespace

DeviceLibrary::DeviceLibrary(QObject *parent)
    : QObject(parent)
    , m_volumes(mountedVolumes)
    , m_program(KoboSend::program())
    , m_ejector(udisksEject)
    , m_configDirectory(CalibreConfig::configDirectory())
{
    connect(&m_scanTimer, &QTimer::timeout, this, &DeviceLibrary::scan);
    m_scanTimer.start(2000);
    QTimer::singleShot(0, this, &DeviceLibrary::scan);
}

DeviceLibrary::~DeviceLibrary()
{
    stopProcess();
    if (m_copy)
        m_copy->waitForFinished();
}

BookLibrary *DeviceLibrary::library() const
{
    return m_library;
}

void DeviceLibrary::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    cancel();
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library) {
        connect(m_library, &BookLibrary::libraryChanged, this, &DeviceLibrary::rematch);
        // A send waits for the changes before it.
        connect(m_library, &BookLibrary::savingChanged, this, &DeviceLibrary::startNext);
        connect(m_library, &BookLibrary::loadingChanged, this, &DeviceLibrary::startNext);
        connect(m_library, &BookLibrary::libraryPathChanged, this, [this] { cancel(); });
    }
    rematch();
    emit libraryChanged();
}

QVariantList DeviceLibrary::devices() const
{
    QVariantList list;
    for (const Connected &connected : m_devices) {
        const Kobo::Device &device = connected.device;
        list.append(QVariantMap{
            { QStringLiteral("root"), device.root },
            { QStringLiteral("name"), device.name },
            { QStringLiteral("freeBytes"), device.freeBytes },
            { QStringLiteral("free"), sizeText(device.freeBytes) },
            { QStringLiteral("totalBytes"), device.totalBytes },
            { QStringLiteral("firmware"), device.firmware },
        });
    }
    return list;
}

QString DeviceLibrary::currentRoot() const
{
    return m_currentRoot;
}

void DeviceLibrary::setCurrentRoot(const QString &root)
{
    if (m_currentRoot == root)
        return;
    m_currentRoot = root;
    emit currentChanged();
    emit rowsChanged();
}

QVariantMap DeviceLibrary::current() const
{
    for (const QVariant &device : devices()) {
        if (device.toMap().value(QStringLiteral("root")).toString() == m_currentRoot)
            return device.toMap();
    }
    return {};
}

QVariantList DeviceLibrary::rows() const
{
    const Connected *connected = find(m_currentRoot);
    if (!connected)
        return {};
    QList<Kobo::DeviceBook> books = connected->books;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(books.begin(), books.end(), [&collator](const Kobo::DeviceBook &a, const Kobo::DeviceBook &b) {
        return collator.compare(a.title, b.title) < 0;
    });
    QVariantList rows;
    for (const Kobo::DeviceBook &book : std::as_const(books)) {
        rows.append(QVariantMap{
            { QStringLiteral("lpath"), book.lpath },
            { QStringLiteral("title"), book.title },
            { QStringLiteral("authors"), book.authors.join(QLatin1String(" & ")) },
            { QStringLiteral("format"), book.format },
            { QStringLiteral("size"), sizeText(book.size) },
            { QStringLiteral("bookId"), book.bookId },
            { QStringLiteral("purchased"), book.purchased },
            { QStringLiteral("imported"), book.imported },
        });
    }
    return rows;
}

bool DeviceLibrary::loading() const
{
    const Connected *connected = find(m_currentRoot);
    return connected && connected->loading;
}

QString DeviceLibrary::lastError() const
{
    const Connected *connected = find(m_currentRoot);
    return connected ? connected->error : QString();
}

QVariantMap DeviceLibrary::onDevice() const
{
    QVariantMap map;
    for (const Connected &connected : m_devices) {
        for (const Kobo::DeviceBook &book : connected.books) {
            if (!book.bookId)
                continue;
            const QString key = QString::number(book.bookId);
            QStringList names = map.value(key).toStringList();
            if (!names.contains(connected.device.name))
                names.append(connected.device.name);
            map.insert(key, names);
        }
    }
    return map;
}

bool DeviceLibrary::available() const
{
    return !m_program.isEmpty();
}

bool DeviceLibrary::writable() const
{
    return m_writable;
}

QString DeviceLibrary::readOnlyReason() const
{
    return m_writable ? QString() : tr("Calibre is open on this computer: it handles the Kobo meanwhile.");
}

QVariantList DeviceLibrary::jobs() const
{
    QVariantList jobs;
    for (qsizetype index = 0; index < m_jobs.size(); ++index) {
        const Job &job = m_jobs.at(index);
        QVariantList bookIds;
        for (const int bookId : job.bookIds)
            bookIds.append(bookId);
        jobs.append(QVariantMap{
            { QStringLiteral("bookIds"), bookIds },
            { QStringLiteral("titles"), job.titles },
            { QStringLiteral("name"), job.name },
            { QStringLiteral("root"), job.root },
            { QStringLiteral("running"), index == 0 && (m_process || m_copy || m_folder) },
            { QStringLiteral("progress"), job.progress },
        });
    }
    return jobs;
}

bool DeviceLibrary::busy() const
{
    return !m_jobs.isEmpty();
}

QString DeviceLibrary::sendFormat(int bookId) const
{
    if (!m_library)
        return {};
    const Book *book = bookIn(m_library->data(), bookId);
    return book ? Kobo::sendFormat(formatsOf(*book), Kobo::formatMap(m_configDirectory)) : QString();
}

QString DeviceLibrary::send(const QVariantList &bookIds, const QString &root)
{
    if (!m_library)
        return tr("No library is open.");
    if (!available())
        return tr("Sending books needs Calibre, which is not installed.");
    if (!m_writable)
        return readOnlyReason();
    const Connected *connected = find(root);
    if (!connected)
        return tr("The device is no longer connected.");
    Job job{ {}, {}, root, connected->device.name };
    const QStringList formatMap = Kobo::formatMap(m_configDirectory);
    for (const QVariant &value : bookIds) {
        const Book *book = bookIn(m_library->data(), value.toInt());
        if (!book)
            return tr("This book is no longer in the library.");
        if (Kobo::sendFormat(formatsOf(*book), formatMap).isEmpty())
            return tr("“%1” has none of the formats %2 reads: convert it to KEPUB first.").arg(book->title, job.name);
        if (!job.bookIds.contains(book->id)) {
            job.bookIds.append(book->id);
            job.titles.append(book->title);
        }
    }
    if (job.bookIds.isEmpty())
        return {};
    m_jobs.append(job);
    emit jobsChanged();
    startNext();
    return {};
}

QString DeviceLibrary::remove(const QStringList &lpaths)
{
    if (!m_writable)
        return readOnlyReason();
    const Connected *connected = find(m_currentRoot);
    if (!connected)
        return tr("The device is no longer connected.");
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.root == m_currentRoot)
            return tr("Wait until the books being sent to %1 are there.").arg(connected->device.name);
    }
    QList<Kobo::DeviceBook> books;
    for (const Kobo::DeviceBook &book : connected->books) {
        if (lpaths.contains(book.lpath)) {
            if (book.purchased)
                return tr("“%1” was bought from Kobo: remove it on the Kobo.").arg(book.title);
            books.append(book);
        }
    }
    QString error;
    const Kobo::Device device = connected->device;
    if (!Kobo::removeBooks(device, books, &error))
        return error;
    load(device.root);
    return {};
}

QString DeviceLibrary::eject(const QString &root)
{
    const Connected *connected = find(root);
    if (!connected)
        return tr("The device is no longer connected.");
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.root == root)
            return tr("Wait until the books being sent to %1 are there.").arg(connected->device.name);
    }
    const Kobo::Device device = connected->device;
    // Out of sight at once, back if ejecting fails.
    m_ejected.append(root);
    m_devices.removeIf([&root](const Connected &candidate) { return candidate.device.root == root; });
    emit devicesChanged();
    emit rowsChanged();
    if (m_currentRoot == root)
        emit currentChanged();
    QPointer<DeviceLibrary> self(this);
    m_ejector(device, [self, device](const QString &error) {
        if (!self)
            return;
        if (error.isEmpty()) {
            emit self->ejected(device.name);
            return;
        }
        self->m_ejected.removeAll(device.root);
        self->scan();
        emit self->failed(error);
    });
    return {};
}

void DeviceLibrary::cancel(int bookId)
{
    const auto matches = [bookId](const Job &job) { return bookId == 0 || job.bookIds.contains(bookId); };
    // A copy under way ends; a batch being made ready is dropped with its
    // folder.
    if ((m_process || m_folder) && !m_copy && matches(m_jobs.first())) {
        stopProcess();
        m_folder.reset();
    }
    const qsizetype from = m_process || m_folder ? 1 : 0;
    const qsizetype before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin() + from, m_jobs.end(), matches), m_jobs.end());
    if (m_jobs.size() != before)
        emit jobsChanged();
    startNext();
}

void DeviceLibrary::refresh()
{
    for (const Connected &connected : std::as_const(m_devices))
        load(connected.device.root);
    scan();
}

void DeviceLibrary::setVolumeProvider(VolumeProvider provider, int intervalMs)
{
    m_volumes = std::move(provider);
    m_scanTimer.start(intervalMs);
    scan();
}

void DeviceLibrary::setProgram(const QString &program)
{
    const bool wasAvailable = available();
    m_program = program.isEmpty() ? KoboSend::program() : program;
    if (available() != wasAvailable)
        emit availableChanged();
}

void DeviceLibrary::setEjector(Ejector ejector)
{
    m_ejector = std::move(ejector);
}

void DeviceLibrary::setSysRoot(const QString &sysRoot)
{
    m_sysRoot = sysRoot;
}

void DeviceLibrary::setConfigDirectory(const QString &directory)
{
    m_configDirectory = directory;
}

void DeviceLibrary::scan()
{
    updateWritable();
    QList<Volume> kobos;
    QStringList roots;
    for (const Volume &volume : m_volumes()) {
        const QString root = QDir::cleanPath(volume.root);
        if (Kobo::isKobo(root)) {
            kobos.append({ root, volume.node });
            roots.append(root);
        }
    }
    // An ejected Kobo comes back once plugged in again.
    m_ejected.removeIf([&roots](const QString &root) { return !roots.contains(root); });
    bool changed = false;
    const qsizetype before = m_devices.size();
    m_devices.removeIf([&roots](const Connected &connected) { return !roots.contains(connected.device.root); });
    changed = m_devices.size() != before;
    for (const Volume &volume : std::as_const(kobos)) {
        if (m_ejected.contains(volume.root))
            continue;
        if (Connected *known = find(volume.root)) {
            const QStorageInfo storage(volume.root);
            if (storage.bytesAvailable() != known->device.freeBytes) {
                known->device.freeBytes = storage.bytesAvailable();
                changed = true;
            }
            continue;
        }
        m_devices.append({ Kobo::describe(volume.root, volume.node, m_sysRoot), {}, {}, true });
        changed = true;
        load(volume.root);
    }
    if (!changed)
        return;
    emit devicesChanged();
    emit rowsChanged();
    if (!m_currentRoot.isEmpty())
        emit currentChanged();
}

void DeviceLibrary::load(const QString &root)
{
    Connected *connected = find(root);
    if (!connected)
        return;
    connected->loading = true;
    const Kobo::Device device = connected->device;
    auto *watcher = new QFutureWatcher<std::pair<QList<Kobo::DeviceBook>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
        watcher->deleteLater();
        Connected *loaded = find(root);
        if (!loaded)
            return;
        const auto [books, error] = watcher->result();
        loaded->books = books;
        loaded->error = error;
        loaded->loading = false;
        loaded->device.freeBytes = QStorageInfo(root).bytesAvailable();
        if (m_library)
            Kobo::match(&loaded->books, m_library->data());
        emit devicesChanged();
        emit rowsChanged();
    });
    watcher->setFuture(QtConcurrent::run([device] {
        QString error;
        QList<Kobo::DeviceBook> books = Kobo::books(device, &error);
        return std::pair{ books, error };
    }));
}

void DeviceLibrary::rematch()
{
    if (!m_library)
        return;
    for (Connected &connected : m_devices)
        Kobo::match(&connected.books, m_library->data());
    emit rowsChanged();
}

void DeviceLibrary::updateWritable()
{
    const bool writable = !(ConcurrencyGuard::calibreRunningHere() && !CalibreLock::heldHere());
    if (writable == m_writable)
        return;
    m_writable = writable;
    emit writableChanged();
}

const DeviceLibrary::Connected *DeviceLibrary::find(const QString &root) const
{
    for (const Connected &connected : m_devices) {
        if (connected.device.root == root)
            return &connected;
    }
    return nullptr;
}

DeviceLibrary::Connected *DeviceLibrary::find(const QString &root)
{
    return const_cast<Connected *>(std::as_const(*this).find(root));
}

void DeviceLibrary::startNext()
{
    if (m_process || m_copy || m_folder || m_jobs.isEmpty() || !m_library || m_library->saving()
        || m_library->loading())
        return;
    const Job job = m_jobs.first();
    const Connected *connected = find(job.root);
    const auto fail = [this, &job](const QString &error) {
        m_jobs.removeFirst();
        m_folder.reset();
        emit jobsChanged();
        emit failed(error);
        startNext();
    };
    if (!connected) {
        fail(tr("%1 is no longer connected; “%2” was not sent.").arg(job.name, job.titles.join(QLatin1String("”, “"))));
        return;
    }
    const QString base = QDir(BookLibrary::cacheFolder(m_library->libraryPath())).filePath(QStringLiteral("sends"));
    QDir().mkpath(base);
    removeStaleFolders(base);
    m_folder = std::make_shared<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
    if (!m_folder->isValid()) {
        fail(tr("Cannot create a folder for sending in %1.").arg(base));
        return;
    }
    emit jobsChanged();
    // Copying the library's database and the Kobo's takes a moment.
    const LibraryData data = m_library->data();
    const Kobo::Device device = connected->device;
    const QStringList formatMap = Kobo::formatMap(m_configDirectory);
    const QString folder = m_folder->path();
    auto *watcher = new QFutureWatcher<QString>(this);
    // A batch stopped meanwhile keeps its folder until it is ready, then
    // drops it.
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kept = m_folder, folder]() mutable {
        watcher->deleteLater();
        const QString error = watcher->result();
        const bool current = m_folder == kept && !m_jobs.isEmpty();
        kept.reset();
        if (!current)
            return;
        if (!error.isEmpty()) {
            const Job job = m_jobs.takeFirst();
            m_folder.reset();
            emit jobsChanged();
            emit failed(tr("“%1” was not sent to %2: %3").arg(job.titles.join(QLatin1String("”, “")), job.name, error));
            startNext();
            return;
        }
        m_process = new QProcess(this);
        m_process->setProcessEnvironment(KoboSend::environment(folder, m_library ? m_library->interfaceLanguage() : QString()));
        m_process->setStandardErrorFile(QProcess::nullDevice());
        // A session of its own: stopping it stops Calibre's workers with it.
        m_process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
        connect(m_process, &QProcess::readyReadStandardOutput, this, &DeviceLibrary::readProgress);
        connect(m_process, &QProcess::finished, this, &DeviceLibrary::finishJob);
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart)
                return;
            const Job job = m_jobs.takeFirst();
            m_process->deleteLater();
            m_process = nullptr;
            m_folder.reset();
            emit jobsChanged();
            emit failed(tr("Cannot run Calibre to send “%1”.").arg(job.titles.join(QLatin1String("”, “"))));
            startNext();
        });
        m_process->start(m_program, KoboSend::arguments(folder));
    });
    watcher->setFuture(QtConcurrent::run([data, bookIds = job.bookIds, device, formatMap, folder] {
        QString error;
        KoboSend::prepare(data, bookIds, device, formatMap, folder, &error);
        return error;
    }));
}

void DeviceLibrary::readProgress()
{
    if (!m_process)
        return;
    bool changed = false;
    while (m_process->canReadLine()) {
        const double fraction = KoboSend::progress(m_process->readLine());
        if (fraction >= 0 && !m_jobs.isEmpty()) {
            // Calibre's part, then the copy.
            m_jobs.first().progress = 0.8 * fraction;
            changed = true;
        }
    }
    if (changed)
        emit jobsChanged();
}

void DeviceLibrary::finishJob()
{
    readProgress();
    const int exitCode = m_process->exitStatus() == QProcess::NormalExit ? m_process->exitCode() : -1;
    m_process->deleteLater();
    m_process = nullptr;
    const KoboSend::Outcome outcome = KoboSend::outcome(m_folder->path(), exitCode);
    const Job job = m_jobs.first();
    const Connected *connected = find(job.root);
    if (!outcome.ok || !connected) {
        finishCopy(false, outcome.ok ? tr("%1 is no longer connected.").arg(job.name) : outcome.error);
        return;
    }
    // The books Calibre found nothing to send of are told apart.
    Job &running = m_jobs.first();
    running.copying = true;
    running.unsendable = outcome.unsendable;
    QStringList titles;
    for (const KoboSend::Sent &sent : outcome.sent) {
        const qsizetype index = running.bookIds.indexOf(sent.bookId);
        if (index >= 0)
            titles.append(running.titles.at(index));
    }
    running.titles = titles;
    const Kobo::Device device = connected->device;
    const QString folder = m_folder->path();
    m_copy = new QFutureWatcher<QString>(this);
    connect(m_copy, &QFutureWatcherBase::finished, this, [this] {
        const QString error = m_copy->result();
        m_copy->deleteLater();
        m_copy = nullptr;
        finishCopy(error.isEmpty(), error);
    });
    QPointer<DeviceLibrary> self(this);
    m_copy->setFuture(QtConcurrent::run([folder, outcome, device, self] {
        QString error;
        KoboSend::apply(folder, outcome, device, &error, [self](double fraction) {
            QMetaObject::invokeMethod(self, [self, fraction] {
                if (self && !self->m_jobs.isEmpty()) {
                    self->m_jobs.first().progress = 0.8 + 0.2 * fraction;
                    emit self->jobsChanged();
                }
            }, Qt::QueuedConnection);
        });
        return error;
    }));
}

void DeviceLibrary::finishCopy(bool ok, const QString &error)
{
    const Job job = m_jobs.takeFirst();
    m_folder.reset();
    emit jobsChanged();
    if (!ok)
        emit failed(tr("“%1” was not sent to %2: %3").arg(job.titles.join(QLatin1String("”, “")), job.name, error));
    else if (!job.unsendable.isEmpty())
        emit failed(tr("“%1” was not sent to %2: it has none of the formats the Kobo reads.")
                        .arg(job.unsendable.join(QLatin1String("”, “")), job.name));
    else if (!job.titles.isEmpty())
        emit sent(job.titles, job.name);
    load(job.root);
    startNext();
}

void DeviceLibrary::stopProcess()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    const pid_t pid = pid_t(m_process->processId());
    if (pid > 0)
        ::kill(-pid, SIGTERM);
    if (!m_process->waitForFinished(3000) && pid > 0) {
        ::kill(-pid, SIGKILL);
        m_process->waitForFinished(1000);
    }
    delete m_process;
    m_process = nullptr;
}
