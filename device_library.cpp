#include "device_library.h"

#include <QClipboard>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimeZone>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <utility>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#endif

#include "book.h"
#include "book_library.h"
#include "calibre_config.h"
#include "calibre_job.h"
#include "calibre_lock.h"
#include "concurrency_guard.h"
#include "cousoir_paths.h"
#include "conversion.h"
#include "device_files.h"
#include "kindle.h"
#include "kindle_send.h"
#include "kobo_send.h"
#include "mtp.h"

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

// As the library shows the size of its files.
// The notes worth showing: a highlight of nothing, which Calibre keeps, says
// nothing.
QList<ReaderNote> shownNotes(const ReadingState &reading)
{
    QList<ReaderNote> notes = reading.notes;
    notes.removeIf([](const ReaderNote &note) {
        return note.type != QLatin1String("Bookmark") && note.text.trimmed().isEmpty()
            && note.annotation.trimmed().isEmpty();
    });
    return notes;
}

// How far a book is read, in a few words.
QString stateText(const ReadingState &reading)
{
    if (reading.status == 2)
        return QObject::tr("Finished");
    if (reading.status == 1)
        return reading.percent ? QObject::tr("%1 % read").arg(*reading.percent) : QObject::tr("Being read");
    return {};
}

// A date the reader wrote, in UTC unless it says otherwise, as a local day.
QString dayText(const QString &written)
{
    QDateTime date = QDateTime::fromString(written, Qt::ISODateWithMs);
    if (!date.isValid())
        return written.left(10);
    if (date.timeSpec() == Qt::LocalTime && !written.endsWith(u'Z') && !written.contains(u'+'))
        date.setTimeZone(QTimeZone::UTC);
    return date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}

// Time spent reading: hours and minutes, or minutes.
QString durationText(qint64 seconds)
{
    if (seconds < 60)
        return {};
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return QObject::tr("%1 min").arg(minutes);
    return QObject::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString sizeText(qint64 size)
{
    return QLocale::c().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

QStringList formatsOf(const Book &book)
{
    QStringList formats;
    for (const BookFormat &format : book.formats)
        formats.append(format.format);
    return formats;
}

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed() : QString();
}

// The Kindles on USB that MTP reaches, from Linux's list of USB devices:
// Amazon's, but those that come as a disk.
QList<KindleSlot> connectedKindles()
{
    QList<KindleSlot> kindles;
    const QDir usb(QStringLiteral("/sys/bus/usb/devices"));
    for (const QString &name : usb.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.contains(u':'))
            continue;
        const QDir device(usb.filePath(name));
        bool ok = false;
        if (readText(device.filePath(QStringLiteral("idVendor"))).toUShort(&ok, 16) != Kindle::VendorId || !ok)
            continue;
        bool disk = false;
        for (const QString &interface : device.entryList({ name + QStringLiteral(":*") }, QDir::Dirs))
            disk = disk || readText(device.filePath(interface + QStringLiteral("/bInterfaceClass"))) == QLatin1String("08");
        if (disk)
            continue;
        kindles.append({ readText(device.filePath(QStringLiteral("busnum"))).toUInt(),
                       quint8(readText(device.filePath(QStringLiteral("devnum"))).toUInt()),
                         readText(device.filePath(QStringLiteral("idProduct"))).toUShort(nullptr, 16), {} });
    }
    return kindles;
}

QString keyOf(const KindleSlot &slot)
{
    return slot.folder.isEmpty() ? QStringLiteral("mtp:%1:%2").arg(slot.bus).arg(slot.number)
                                 : QStringLiteral("mtpdir:") + QDir::cleanPath(slot.folder);
}

} // namespace

struct DeviceLibrary::KindleLink
{
    std::unique_ptr<Mtp::Session> session;
    std::unique_ptr<DeviceFiles> files;
    KindleSend::Device device;

    // Opens the Kindle at `slot` and lists its files.
    static std::shared_ptr<KindleLink> open(const KindleSlot &slot, QString *error)
    {
        auto link = std::make_shared<KindleLink>();
        if (!slot.folder.isEmpty()) {
            link->files = std::make_unique<FolderFiles>(slot.folder, true);
            link->device = KindleSend::standIn();
        } else {
            link->session = std::make_unique<Mtp::Session>();
            if (!link->session->open(slot.bus, slot.number, error))
                return nullptr;
            link->device = { link->session->friendlyName(), link->session->modelName(), link->session->serialNumber(),
                             Kindle::VendorId, slot.productId };
            link->files = std::make_unique<MtpFiles>(link->session.get(), true);
        }
        if (!link->files->scan(error))
            return nullptr;
        return link;
    }
};

DeviceLibrary::DeviceLibrary(QObject *parent)
    : QObject(parent)
    , m_volumes(mountedVolumes)
    , m_kindles(connectedKindles)
    , m_program(SendJob::program())
    , m_ejector(udisksEject)
    , m_configDirectory(CalibreConfig::configDirectory())
{
    // Under test, only the readers a test makes up: never those plugged in,
    // which a test would otherwise read, write to and send books to.
    if (QStandardPaths::isTestModeEnabled()) {
        m_volumes = [] { return QList<Volume>(); };
        m_kindles = [] { return QList<KindleSlot>(); };
    }
    m_kindlePool.setMaxThreadCount(1);
    connect(&m_scanTimer, &QTimer::timeout, this, &DeviceLibrary::scan);
    m_scanTimer.start(2000);
    QTimer::singleShot(0, this, &DeviceLibrary::scan);
}

DeviceLibrary::~DeviceLibrary()
{
    stopProcess();
    if (m_copy)
        m_copy->waitForFinished();
    m_kindlePool.waitForDone();
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
        list.append(QVariantMap{
            { QStringLiteral("root"), connected.root },
            { QStringLiteral("name"), connected.name },
            { QStringLiteral("kind"), connected.kind == Kind::Kobo ? QStringLiteral("kobo") : QStringLiteral("kindle") },
            { QStringLiteral("ejectable"), connected.kind == Kind::Kobo },
            { QStringLiteral("free"), sizeText(connected.freeBytes) },
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
    QList<DeviceBook> books = connected->books;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(books.begin(), books.end(), [&collator](const DeviceBook &a, const DeviceBook &b) {
        return collator.compare(a.title, b.title) < 0;
    });
    QVariantList rows;
    for (const DeviceBook &book : std::as_const(books)) {
        rows.append(QVariantMap{
            { QStringLiteral("lpath"), book.lpath },
            { QStringLiteral("title"), book.title },
            { QStringLiteral("authors"), book.authors.join(QLatin1String(" & ")) },
            { QStringLiteral("format"), book.format },
            { QStringLiteral("size"), sizeText(book.size) },
            { QStringLiteral("bookId"), book.bookId },
            { QStringLiteral("purchased"), book.purchased },
            { QStringLiteral("reading"), book.reading.status == 2 ? QStringLiteral("✓")
                  : book.reading.status == 1 ? (book.reading.percent ? QStringLiteral("%1 %").arg(*book.reading.percent)
                                                                     : QStringLiteral("…"))
                                             : QString() },
            { QStringLiteral("notes"), int(shownNotes(book.reading).size()) },
        });
    }
    return rows;
}

QVariantList DeviceLibrary::reading(int bookId) const
{
    QVariantList copies;
    const Book *libraryBook = m_library && bookId > 0 ? m_library->data().book(bookId) : nullptr;
    if (!libraryBook)
        return copies;
    const auto copy = [&copies](const QString &name, const QString &format, const ReadingState &reading,
                                const QString &title, const QStringList &authors, const QDateTime &seen) {
        if (reading.status == 0 && reading.notes.isEmpty())
            return;
        const QList<ReaderNote> shown = shownNotes(reading);
        QVariantList notes;
        const ReaderNote *previous = nullptr;
        for (const ReaderNote &note : shown) {
            const bool newChapter = !previous || previous->chapter != note.chapter
                || previous->chapterTitle != note.chapterTitle;
            previous = &note;
            notes.append(QVariantMap{
                { QStringLiteral("newChapter"), newChapter },
                { QStringLiteral("type"), note.type },
                { QStringLiteral("text"), note.text },
                { QStringLiteral("annotation"), note.annotation },
                { QStringLiteral("chapter"), note.chapterTitle },
                { QStringLiteral("progress"), note.chapterProgress },
                { QStringLiteral("created"), dayText(note.created) },
                { QStringLiteral("position"), note.position },
            });
        }
        copies.append(QVariantMap{
            { QStringLiteral("name"), name },
            { QStringLiteral("format"), format },
            { QStringLiteral("connected"), !seen.isValid() },
            { QStringLiteral("seen"), seen.isValid() ? seen.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")) : QString() },
            { QStringLiteral("status"), reading.status },
            { QStringLiteral("percent"), reading.percent.value_or(-1) },
            { QStringLiteral("state"), stateText(reading) },
            { QStringLiteral("lastRead"), reading.lastRead.isEmpty() ? QString() : dayText(reading.lastRead) },
            { QStringLiteral("timeSpent"), durationText(reading.secondsSpent) },
            { QStringLiteral("notes"), notes },
            { QStringLiteral("markdown"), DeviceBooks::notesMarkdown(title, authors, shown) },
        });
    };
    QSet<QString> connected;
    for (const Connected &device : m_devices) {
        // A reader still being read, or that cannot be, tells what it told.
        if (!device.loading && device.error.isEmpty())
            connected.insert(deviceId(device));
        for (const DeviceBook &book : device.books + device.clippings) {
            if (book.bookId == bookId)
                copy(device.name, book.format, book.reading, libraryBook->title, libraryBook->authors, {});
        }
    }
    // Readers away, as last seen.
    for (const ReadingLog::Entry &entry : readingLog().value(libraryBook->uuid)) {
        if (!connected.contains(entry.device))
            copy(entry.deviceName, entry.format, entry.reading, libraryBook->title, libraryBook->authors, entry.seen);
    }
    return copies;
}

void DeviceLibrary::copyNotes(const QString &markdown) const
{
    QGuiApplication::clipboard()->setText(markdown);
}

QString DeviceLibrary::saveNotes(const QString &markdown, const QUrl &file) const
{
    QSaveFile out(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!out.open(QIODevice::WriteOnly) || out.write(markdown.toUtf8()) < 0 || !out.commit())
        return tr("Cannot write %1: %2").arg(QFileInfo(out.fileName()).fileName(), out.errorString());
    return {};
}

QString DeviceLibrary::deviceId(const Connected &connected) const
{
    const QString serial = connected.kind == Kind::Kobo ? connected.kobo.serial : connected.serial;
    return serial.isEmpty() ? connected.name : serial;
}

void DeviceLibrary::matchKindle(Connected &connected)
{
    DeviceBooks::match(&connected.books, m_library->data());
    connected.clippings = connected.allClippings;
    DeviceBooks::match(&connected.clippings, m_library->data());
    Kindle::attachClippings(&connected.books, &connected.clippings);
}

const ReadingLog::Log &DeviceLibrary::readingLog() const
{
    const QString path = m_library ? CousoirPaths::readingFile(m_library->libraryPath()) : QString();
    if (path != m_readingLogPath) {
        m_readingLogPath = path;
        m_readingLog = path.isEmpty() ? ReadingLog::Log() : ReadingLog::read(path);
    }
    return m_readingLog;
}

void DeviceLibrary::recordReading(const Connected &connected)
{
    // Only once the library is in: before, no book matches it, and the
    // reader's entries would all go.
    if (!m_library || !m_library->loaded() || m_library->libraryPath().isEmpty() || !connected.error.isEmpty()
        || connected.loading)
        return;
    readingLog();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QHash<int, const Book *> byId;
    for (const Book &book : m_library->data().books)
        byId.insert(book.id, &book);
    QHash<QString, QList<ReadingLog::Entry>> entries;
    for (const DeviceBook &book : connected.books + connected.clippings) {
        if (book.bookId <= 0 || (book.reading.status == 0 && book.reading.notes.isEmpty()))
            continue;
        const Book *libraryBook = byId.value(book.bookId);
        if (!libraryBook || libraryBook->uuid.isEmpty())
            continue;
        entries[libraryBook->uuid].append({ deviceId(connected), connected.name, book.lpath, book.format, now,
                                            book.reading });
    }
    // Written when the reader tells something new, or first today.
    if (ReadingLog::sameEntries(m_readingLog, deviceId(connected), entries, now.date()))
        return;
    ReadingLog::Log updated = m_readingLog;
    ReadingLog::replaceDevice(&updated, deviceId(connected), entries);
    QString error;
    if (ReadingLog::write(m_readingLogPath, updated, &error))
        m_readingLog = updated;
    else
        qWarning("%s", qPrintable(error));
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
        for (const DeviceBook &book : connected.books) {
            if (!book.bookId)
                continue;
            const QString key = QString::number(book.bookId);
            QStringList names = map.value(key).toStringList();
            if (!names.contains(connected.name))
                names.append(connected.name);
            map.insert(key, names);
        }
    }
    return map;
}

int DeviceLibrary::deviceCount() const
{
    return int(m_devices.size());
}

bool DeviceLibrary::available() const
{
    // A Kobo needs no Calibre.
    return true;
}

bool DeviceLibrary::sendsNatively() const
{
    return m_program.isEmpty() || m_native;
}

void DeviceLibrary::setNative(bool native)
{
    m_native = native;
}

bool DeviceLibrary::writable() const
{
    return m_writable;
}

QString DeviceLibrary::readOnlyReason() const
{
    return m_writable ? QString() : tr("Calibre is open on this computer: it handles the readers meanwhile.");
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

QStringList DeviceLibrary::formatMap(const Connected &connected) const
{
    return connected.kind == Kind::Kobo ? Kobo::formatMap(m_configDirectory)
                                        : Kindle::formatMap(m_configDirectory, connected.serial);
}

QString DeviceLibrary::sendFormat(int bookId, const QString &root) const
{
    const Connected *connected = find(root);
    if (!m_library || !connected)
        return {};
    const Book *book = m_library->data().book(bookId);
    return book ? DeviceBooks::sendFormat(formatsOf(*book), formatMap(*connected)) : QString();
}

QString DeviceLibrary::convertFormat(const QString &root) const
{
    const Connected *connected = find(root);
    if (!connected)
        return {};
    // auto_convert of Calibre's sync_to_device(): the first of the driver's
    // formats it converts to.
    const QStringList outputs = Conversion::outputFormats();
    for (const QString &format : formatMap(*connected)) {
        if (outputs.contains(format.toUpper()))
            return format.toUpper();
    }
    return {};
}

QString DeviceLibrary::send(const QVariantList &bookIds, const QString &root)
{
    if (!m_library)
        return tr("No library is open.");
    if (!m_writable)
        return readOnlyReason();
    const Connected *connected = find(root);
    if (connected && connected->kind == Kind::Kindle && m_program.isEmpty())
        return tr("Sending books to a Kindle needs Calibre, which is not installed.");
    if (!connected)
        return tr("The device is no longer connected.");
    if (connected->kind == Kind::Kindle && connected->loading)
        return tr("Wait until %1 is read.").arg(connected->name);
    if (connected->kind == Kind::Kindle && !connected->error.isEmpty())
        return connected->error;
    Job job{ {}, {}, root, connected->name, connected->kind };
    const QStringList formats = formatMap(*connected);
    for (const QVariant &value : bookIds) {
        const Book *book = m_library->data().book(value.toInt());
        if (!book)
            return tr("This book is no longer in the library.");
        if (DeviceBooks::sendFormat(formatsOf(*book), formats).isEmpty())
            return tr("“%1” has none of the formats %2 reads.").arg(book->title, job.name);
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
    Connected *connected = find(m_currentRoot);
    if (!connected)
        return tr("The device is no longer connected.");
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.root == m_currentRoot)
            return tr("Wait until the books being sent to %1 are there.").arg(connected->name);
    }
    QList<DeviceBook> books;
    for (const DeviceBook &book : connected->books) {
        if (lpaths.contains(book.lpath)) {
            if (book.purchased)
                return tr("“%1” was bought from Kobo: remove it on the Kobo.").arg(book.title);
            books.append(book);
        }
    }
    const QString root = connected->root;
    if (connected->kind == Kind::Kobo) {
        QString error;
        if (!Kobo::removeBooks(connected->kobo, books, &error))
            return error;
        load(root);
        return {};
    }
    // Over MTP, on its thread.
    connected->loading = true;
    emit rowsChanged();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
        watcher->deleteLater();
        if (!watcher->result().isEmpty())
            emit failed(watcher->result());
        load(root);
    });
    watcher->setFuture(QtConcurrent::run(&m_kindlePool, [slot = connected->slot, books] {
        QString error;
        const std::shared_ptr<KindleLink> link = KindleLink::open(slot, &error);
        if (link)
            Kindle::removeBooks(*link->files, books, &error);
        return error;
    }));
    return {};
}

QString DeviceLibrary::eject(const QString &root)
{
    const Connected *connected = find(root);
    if (!connected)
        return tr("The device is no longer connected.");
    if (connected->kind == Kind::Kindle)
        return tr("%1 needs no ejecting: unplug it once nothing is being sent to it.").arg(connected->name);
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.root == root)
            return tr("Wait until the books being sent to %1 are there.").arg(connected->name);
    }
    const Kobo::Device device = connected->kobo;
    // Out of sight at once, back if ejecting fails.
    m_ejected.append(root);
    m_devices.removeIf([&root](const Connected &candidate) { return candidate.root == root; });
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
    // folder, and a Kindle's connection with it.
    if ((m_process || m_folder) && !m_copy && matches(m_jobs.first())) {
        stopProcess();
        m_folder.reset();
        m_jobs.first().link.reset();
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
        load(connected.root);
    scan();
}

void DeviceLibrary::setVolumeProvider(VolumeProvider provider, int intervalMs)
{
    m_volumes = std::move(provider);
    m_scanTimer.start(intervalMs);
    scan();
}

void DeviceLibrary::setKindleProvider(KindleProvider provider)
{
    m_kindles = std::move(provider);
    scan();
}

void DeviceLibrary::setProgram(const QString &program)
{
    const bool wasAvailable = available();
    m_program = program.isEmpty() ? SendJob::program() : program;
    if (available() != wasAvailable)
        emit availableChanged();
}

void DeviceLibrary::setEjector(Ejector ejector)
{
    m_ejector = std::move(ejector);
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
    const QList<KindleSlot> kindles = m_kindles();
    for (const KindleSlot &slot : kindles)
        roots.append(keyOf(slot));
    // An ejected Kobo comes back once plugged in again.
    m_ejected.removeIf([&roots](const QString &root) { return !roots.contains(root); });
    const qsizetype before = m_devices.size();
    m_devices.removeIf([&roots](const Connected &connected) { return !roots.contains(connected.root); });
    bool changed = m_devices.size() != before;
    for (const Volume &volume : std::as_const(kobos)) {
        if (m_ejected.contains(volume.root))
            continue;
        if (Connected *known = find(volume.root)) {
            const qint64 free = QStorageInfo(volume.root).bytesAvailable();
            if (free != known->freeBytes) {
                known->freeBytes = free;
                changed = true;
            }
            continue;
        }
        Connected connected;
        connected.root = volume.root;
        connected.kobo = Kobo::describe(volume.root, volume.node);
        connected.name = connected.kobo.name;
        connected.freeBytes = connected.kobo.freeBytes;
        m_devices.append(connected);
        changed = true;
        load(volume.root);
    }
    for (const KindleSlot &slot : kindles) {
        if (find(keyOf(slot)))
            continue;
        Connected connected;
        connected.kind = Kind::Kindle;
        connected.root = keyOf(slot);
        connected.name = tr("Kindle");
        connected.slot = slot;
        m_devices.append(connected);
        changed = true;
        load(connected.root);
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
    if (connected->kind == Kind::Kobo) {
        connected->loading = true;
        const Kobo::Device device = connected->kobo;
        auto *watcher = new QFutureWatcher<std::pair<QList<DeviceBook>, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
            watcher->deleteLater();
            Connected *loaded = find(root);
            if (!loaded)
                return;
            const auto [books, error] = watcher->result();
            loaded->books = books;
            loaded->error = error;
            loaded->loading = false;
            loaded->freeBytes = QStorageInfo(root).bytesAvailable();
            if (m_library) {
                DeviceBooks::match(&loaded->books, m_library->data());
                recordReading(*loaded);
            }
            emit devicesChanged();
            emit rowsChanged();
        });
        watcher->setFuture(QtConcurrent::run([device] {
            QString error;
            QList<DeviceBook> books = Kobo::books(device, &error);
            return std::pair{ books, error };
        }));
        return;
    }

    // Calibre, open on the computer, has the Kindle's one MTP connection.
    if (!m_writable) {
        connected->books.clear();
        connected->clippings.clear();
        connected->allClippings.clear();
        connected->error = readOnlyReason();
        emit rowsChanged();
        return;
    }
    struct Read
    {
        QList<DeviceBook> books;
        QList<DeviceBook> clippings;
        QString error;
        bool opened = false;
        QString name;
        QString serial;
        qint64 freeBytes = 0;
    };
    connected->loading = true;
    auto *watcher = new QFutureWatcher<Read>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, root] {
        watcher->deleteLater();
        Connected *loaded = find(root);
        if (!loaded)
            return;
        const Read read = watcher->result();
        loaded->books = read.books;
        loaded->allClippings = read.clippings;
        loaded->error = read.error;
        loaded->loading = false;
        if (read.opened) {
            loaded->name = read.name;
            loaded->serial = read.serial;
            loaded->freeBytes = read.freeBytes;
        }
        if (m_library) {
            matchKindle(*loaded);
            recordReading(*loaded);
        }
        emit devicesChanged();
        emit rowsChanged();
    });
    watcher->setFuture(QtConcurrent::run(&m_kindlePool, [slot = connected->slot] {
        Read read;
        const std::shared_ptr<KindleLink> link = KindleLink::open(slot, &read.error);
        if (!link)
            return read;
        read.opened = true;
        // As Calibre does whenever it connects: the covers Amazon blanked
        // come back.
        Kindle::restoreThumbnails(*link->files, &read.error);
        read.books = Kindle::books(*link->files, &read.error);
        read.clippings = Kindle::clippings(*link->files, &read.error);
        read.name = Kindle::name(Kindle::readDriveInfo(*link->files), link->device.friendlyName, link->device.modelName);
        read.serial = link->device.serial;
        read.freeBytes = link->files->freeBytes();
        return read;
    }));
}

void DeviceLibrary::rematch()
{
    if (!m_library)
        return;
    for (Connected &connected : m_devices) {
        if (connected.kind == Kind::Kindle)
            matchKindle(connected);
        else
            DeviceBooks::match(&connected.books, m_library->data());
        if (!connected.loading)
            recordReading(connected);
    }
    emit rowsChanged();
}

void DeviceLibrary::updateWritable()
{
    const bool writable = !(ConcurrencyGuard::calibreRunningHere() && !CalibreLock::heldHere());
    if (writable == m_writable)
        return;
    m_writable = writable;
    emit writableChanged();
    // Calibre gone, the Kindles can be read.
    for (const Connected &connected : std::as_const(m_devices)) {
        if (connected.kind == Kind::Kindle)
            load(connected.root);
    }
}

const DeviceLibrary::Connected *DeviceLibrary::find(const QString &root) const
{
    for (const Connected &connected : m_devices) {
        if (connected.root == root)
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
    const auto fail = [this](const QString &error) {
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
    CalibreJob::removeStaleFolders(base);
    m_folder = std::make_shared<QTemporaryDir>(QDir(base).filePath(QStringLiteral("XXXXXX")));
    if (!m_folder->isValid()) {
        fail(tr("Cannot create a folder for sending in %1.").arg(base));
        return;
    }
    emit jobsChanged();
    // Copying the library's database and the device's files takes a moment.
    using Prepared = std::pair<QString, std::shared_ptr<KindleLink>>;
    const LibraryData data = m_library->data();
    const QStringList formats = formatMap(*connected);
    const QString folder = m_folder->path();
    auto *watcher = new QFutureWatcher<Prepared>(this);
    // A batch stopped meanwhile keeps its folder until it is ready, then
    // drops it.
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, kept = m_folder, folder]() mutable {
        watcher->deleteLater();
        Prepared prepared = watcher->result();
        const bool current = m_folder == kept && !m_jobs.isEmpty();
        kept.reset();
        if (!current)
            return;
        if (!prepared.first.isEmpty()) {
            const Job job = m_jobs.takeFirst();
            m_folder.reset();
            emit jobsChanged();
            emit failed(tr("“%1” was not sent to %2: %3").arg(job.titles.join(QLatin1String("”, “")), job.name,
                                                              prepared.first));
            startNext();
            return;
        }
        m_jobs.first().link = std::move(prepared.second);
        if (m_jobs.first().kind == Kind::Kobo && sendsNatively())
            finishSend(0);
        else
            startProcess(folder);
    });
    if (job.kind == Kind::Kobo && sendsNatively()) {
        // Cousoir's own send: the KEPUBs made, the Kobo's lists written.
        watcher->setFuture(QtConcurrent::run([data, bookIds = job.bookIds, device = connected->kobo, formats, folder,
                                              config = m_configDirectory,
                                              language = m_library->interfaceLanguage()] {
            QString error;
            KoboSend::prepareNatively(data, bookIds, device, formats, config, language, folder, &error);
            return Prepared{ error, nullptr };
        }));
    } else if (job.kind == Kind::Kobo) {
        watcher->setFuture(QtConcurrent::run([data, bookIds = job.bookIds, device = connected->kobo, formats, folder] {
            QString error;
            KoboSend::prepare(data, bookIds, device, formats, folder, &error);
            return Prepared{ error, nullptr };
        }));
    } else {
        // The Kindle's connection stays open until its files are there.
        watcher->setFuture(QtConcurrent::run(&m_kindlePool, [data, bookIds = job.bookIds, slot = connected->slot, formats,
                                                             folder] {
            QString error;
            std::shared_ptr<KindleLink> link = KindleLink::open(slot, &error);
            if (link && !KindleSend::prepare(data, bookIds, *link->files, link->device, formats, folder, &error))
                link.reset();
            return Prepared{ error, link };
        }));
    }
}

void DeviceLibrary::startProcess(const QString &folder)
{
    m_process = new QProcess(this);
    m_process->setProcessEnvironment(CalibreJob::environment(folder, m_library ? m_library->interfaceLanguage() : QString()));
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
    m_process->start(m_program, SendJob::arguments(folder));
}

void DeviceLibrary::readProgress()
{
    if (!m_process)
        return;
    bool changed = false;
    while (m_process->canReadLine()) {
        const double fraction = CalibreJob::progress(m_process->readLine());
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
    finishSend(exitCode);
}

void DeviceLibrary::finishSend(int exitCode)
{
    Job &running = m_jobs.first();
    const QString folder = m_folder->path();
    const Connected *connected = find(running.root);
    // The books Calibre found nothing to send of are told apart.
    const auto keepSent = [&running](const QList<SendJob::Sent> &sent, const QStringList &unsendable) {
        running.unsendable = unsendable;
        QStringList titles;
        for (const SendJob::Sent &book : sent) {
            const qsizetype index = running.bookIds.indexOf(book.bookId);
            if (index >= 0)
                titles.append(running.titles.at(index));
        }
        running.titles = titles;
    };
    const auto progress = [self = QPointer<DeviceLibrary>(this)](double fraction) {
        QMetaObject::invokeMethod(self, [self, fraction] {
            if (self && !self->m_jobs.isEmpty()) {
                self->m_jobs.first().progress = 0.8 + 0.2 * fraction;
                emit self->jobsChanged();
            }
        }, Qt::QueuedConnection);
    };
    QFuture<QString> copying;
    if (running.kind == Kind::Kobo) {
        const KoboSend::Outcome outcome = KoboSend::outcome(folder, exitCode);
        if (!outcome.ok || !connected) {
            finishCopy(false, outcome.ok ? tr("%1 is no longer connected.").arg(running.name) : outcome.error);
            return;
        }
        keepSent(outcome.sent, outcome.unsendable);
        copying = QtConcurrent::run([folder, outcome, device = connected->kobo, progress] {
            QString error;
            KoboSend::apply(folder, outcome, device, &error, progress);
            return error;
        });
    } else {
        const KindleSend::Outcome outcome = KindleSend::outcome(folder, exitCode);
        std::shared_ptr<KindleLink> link = std::exchange(running.link, nullptr);
        if (!outcome.ok || !link) {
            // Closed before finishCopy() reads the Kindle again.
            link.reset();
            finishCopy(false, outcome.ok ? tr("%1 is no longer connected.").arg(running.name) : outcome.error);
            return;
        }
        keepSent(outcome.sent, outcome.unsendable);
        // The connection closes on MTP's thread once the files are there.
        copying = QtConcurrent::run(&m_kindlePool, [folder, outcome, link, progress]() mutable {
            QString error;
            KindleSend::apply(folder, outcome, *link->files, &error, progress);
            link.reset();
            return error;
        });
    }
    m_copy = new QFutureWatcher<QString>(this);
    connect(m_copy, &QFutureWatcherBase::finished, this, [this] {
        const QString error = m_copy->result();
        m_copy->deleteLater();
        m_copy = nullptr;
        finishCopy(error.isEmpty(), error);
    });
    m_copy->setFuture(copying);
}

void DeviceLibrary::finishCopy(bool ok, const QString &error)
{
    const Job job = m_jobs.takeFirst();
    m_folder.reset();
    emit jobsChanged();
    if (!ok)
        emit failed(tr("“%1” was not sent to %2: %3").arg(job.titles.join(QLatin1String("”, “")), job.name, error));
    else if (!job.unsendable.isEmpty())
        emit failed(tr("“%1” was not sent to %2: it has none of the formats %2 reads.")
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
    CalibreJob::stopProcessGroup(m_process);
    delete m_process;
    m_process = nullptr;
}
