#include "highlight_sync.h"

#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QTemporaryDir>
#include <QtConcurrent>

#include "book_library.h"
#include "cousoir_paths.h"
#include "device_library.h"
#include "kobo.h"
#include "sqlite_db.h"

namespace {

const QString off = QStringLiteral("off");

} // namespace

HighlightSync::HighlightSync(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &HighlightSync::finishPlan);
}

HighlightSync::~HighlightSync()
{
    m_watcher.waitForFinished();
}

BookLibrary *HighlightSync::library() const
{
    return m_library;
}

void HighlightSync::setLibrary(BookLibrary *library)
{
    if (m_library == library)
        return;
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    m_openedPath.clear();
    if (m_library) {
        connect(m_library, &BookLibrary::libraryChanged, this, &HighlightSync::openLibrary);
        connect(m_library, &BookLibrary::changeFinished, this, &HighlightSync::finishChange);
        connect(m_library, &BookLibrary::writableChanged, this, [this] {
            if (!m_library->writable() || !std::exchange(m_pending, false))
                return;
            emit stateChanged();
            const QString key = libraryKey();
            for (const KoboHighlights::Snapshot &snapshot : std::as_const(m_snapshots)) {
                if (snapshot.synced.contains(key) && snapshot.synced.value(key) != off)
                    queue(snapshot);
            }
        });
    }
    emit libraryChanged();
    openLibrary();
}

DeviceLibrary *HighlightSync::devices() const
{
    return m_devices;
}

void HighlightSync::setDevices(DeviceLibrary *devices)
{
    if (m_devices == devices)
        return;
    if (m_devices)
        disconnect(m_devices, nullptr, this, nullptr);
    m_devices = devices;
    if (m_devices)
        connect(m_devices, &DeviceLibrary::koboHighlightsRead, this, &HighlightSync::take);
    emit devicesChanged();
}

bool HighlightSync::busy() const
{
    return m_watcher.isRunning() || !m_waiting.isEmpty();
}

bool HighlightSync::pending() const
{
    return m_pending;
}

QString HighlightSync::status() const
{
    return m_status;
}

QVariantList HighlightSync::sources() const
{
    const QString key = libraryKey();
    QVariantList list;
    for (const KoboHighlights::Snapshot &snapshot : m_snapshots) {
        if (key.isEmpty() || !snapshot.synced.contains(key))
            continue;
        int highlights = 0;
        for (const KoboHighlights::Book &book : snapshot.books) {
            for (const KoboHighlights::Highlight &highlight : book.highlights)
                highlights += !highlight.hidden;
        }
        list.append(QVariantMap{
            { QStringLiteral("device"), snapshot.device },
            { QStringLiteral("name"), snapshot.name },
            { QStringLiteral("seen"), QLocale().toString(snapshot.seen.toLocalTime(), QLocale::ShortFormat) },
            { QStringLiteral("source"), snapshot.source },
            { QStringLiteral("off"), snapshot.synced.value(key) == off },
            { QStringLiteral("highlights"), highlights },
        });
    }
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    return list;
}

QString HighlightSync::nameOf(const QString &device) const
{
    const auto found = m_snapshots.constFind(device);
    return found == m_snapshots.cend() || found->name.isEmpty() ? device : found->name;
}

QHash<QString, QList<KoboSync::Left>> HighlightSync::left() const
{
    return m_left;
}

QString HighlightSync::libraryKey() const
{
    return m_library && !m_library->libraryPath().isEmpty() ? CousoirPaths::libraryKey(m_library->libraryPath()) : QString();
}

void HighlightSync::loadSnapshots()
{
    for (const KoboHighlights::Snapshot &snapshot : KoboHighlights::snapshots()) {
        if (!m_snapshots.contains(snapshot.device))
            m_snapshots.insert(snapshot.device, snapshot);
    }
}

void HighlightSync::openLibrary()
{
    // Once a library is read, not each time it is saved.
    if (!m_library || !m_library->loaded() || m_library->libraryPath() == m_openedPath)
        return;
    m_openedPath = m_library->libraryPath();
    m_left.clear();
    emit leftChanged();
    loadSnapshots();
    emit sourcesChanged();
    const QString key = libraryKey();
    for (const KoboHighlights::Snapshot &snapshot : std::as_const(m_snapshots)) {
        if (snapshot.synced.contains(key) && snapshot.synced.value(key) != off)
            queue(snapshot);
    }
}

void HighlightSync::take(const KoboHighlights::Snapshot &snapshot)
{
    loadSnapshots();
    KoboHighlights::Snapshot kept = snapshot;
    kept.synced = m_snapshots.value(snapshot.device).synced;
    // Read with the library open: synced with it from now on.
    const QString key = libraryKey();
    if (!key.isEmpty() && m_library->loaded() && !kept.synced.contains(key))
        kept.synced.insert(key, QString());
    QString error;
    KoboHighlights::writeSnapshot(kept, &error);
    m_snapshots.insert(kept.device, kept);
    emit sourcesChanged();
    if (kept.synced.value(key) != off)
        queue(kept);
}

void HighlightSync::syncNow()
{
    loadSnapshots();
    const QString key = libraryKey();
    for (KoboHighlights::Snapshot &snapshot : m_snapshots) {
        if (!snapshot.synced.contains(key))
            continue;
        if (snapshot.synced.value(key) == off) {
            snapshot.synced.insert(key, QString());
            QString error;
            KoboHighlights::writeSnapshot(snapshot, &error);
        }
        queue(snapshot);
    }
    emit sourcesChanged();
    // The Kobos plugged in, read again.
    if (m_devices)
        m_devices->refresh();
}

QString HighlightSync::importDatabase(const QUrl &file)
{
    if (!m_library || !m_library->loaded())
        return tr("No library is open.");
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    SqliteDb db;
    QTemporaryDir copy;
    QString error;
    if (!QFileInfo(path).isFile())
        return tr("%1 is no file.").arg(QFileInfo(path).fileName());
    if (!Kobo::openDatabase(path, &db, &copy, &error))
        return error;
    QList<KoboHighlights::Book> books = KoboHighlights::read(db, &error);
    if (!error.isEmpty())
        return error;
    loadSnapshots();
    const KoboHighlights::Identity identity = KoboHighlights::identify(path);
    QString device = identity.serial;
    if (device.isEmpty()) {
        // A Kobo met already whose highlights these are: they share their
        // ids.
        QSet<QString> ids;
        for (const KoboHighlights::Book &book : std::as_const(books)) {
            for (const KoboHighlights::Highlight &highlight : book.highlights)
                ids.insert(highlight.id);
        }
        for (const KoboHighlights::Snapshot &snapshot : std::as_const(m_snapshots)) {
            for (const KoboHighlights::Book &book : snapshot.books) {
                for (const KoboHighlights::Highlight &highlight : book.highlights) {
                    if (device.isEmpty() && ids.contains(highlight.id))
                        device = snapshot.device;
                }
            }
        }
    }
    if (device.isEmpty())
        device = identity.name;
    KoboHighlights::Snapshot snapshot;
    snapshot.device = device;
    snapshot.name = m_snapshots.contains(device) ? m_snapshots.value(device).name : identity.name;
    snapshot.source = QStringLiteral("file");
    snapshot.seen = QDateTime::currentDateTimeUtc();
    snapshot.books = books;
    snapshot.synced = m_snapshots.value(device).synced;
    const QString key = libraryKey();
    snapshot.synced.insert(key, QString());
    // Kept unless the Kobo was read itself, which knows better.
    if (!m_snapshots.contains(device) || m_snapshots.value(device).source == QLatin1String("file")) {
        KoboHighlights::writeSnapshot(snapshot, &error);
        m_snapshots.insert(device, snapshot);
    } else {
        m_snapshots[device].synced.insert(key, QString());
        KoboHighlights::writeSnapshot(m_snapshots.value(device), &error);
    }
    emit sourcesChanged();
    queue(snapshot);
    return {};
}

QString HighlightSync::removeFromLibrary(const QString &device)
{
    if (!m_library || !m_library->loaded())
        return tr("No library is open.");
    if (m_library->removeAnnotationUser(KoboSync::userType, device) < 0)
        return tr("The library cannot be changed right now.");
    loadSnapshots();
    if (m_snapshots.contains(device)) {
        m_snapshots[device].synced.insert(libraryKey(), off);
        QString error;
        KoboHighlights::writeSnapshot(m_snapshots.value(device), &error);
    }
    m_queue.removeIf([&device](const KoboHighlights::Snapshot &queued) { return queued.device == device; });
    m_left.remove(device);
    emit leftChanged();
    emit sourcesChanged();
    return {};
}

void HighlightSync::queue(const KoboHighlights::Snapshot &snapshot)
{
    // The latest of a Kobo's, once.
    m_queue.removeIf([&snapshot](const KoboHighlights::Snapshot &queued) {
        return queued.device == snapshot.device && queued.source == snapshot.source;
    });
    m_queue.append(snapshot);
    runNext();
}

void HighlightSync::runNext()
{
    if (m_watcher.isRunning() || m_queue.isEmpty() || !m_library || !m_library->loaded())
        return;
    m_planning = m_queue.takeFirst();
    KoboSync::Input input;
    input.data = m_library->data();
    const QString cache = CousoirPaths::cacheFolder(m_library->libraryPath());
    input.snapshotDb = cache + QStringLiteral("/snapshot.db");
    input.cacheFile = cache + QStringLiteral("/kobo-placements.json");
    input.snapshot = m_planning;
    input.sent = KoboHighlights::readSent(m_planning.device);
    setStatus(tr("Reading the highlights of %1…").arg(nameOf(m_planning.device)));
    m_watcher.setFuture(QtConcurrent::run([input] { return KoboSync::plan(input); }));
    emit stateChanged();
}

void HighlightSync::finishPlan()
{
    const KoboSync::Plan plan = m_watcher.result();
    const KoboHighlights::Snapshot planned = std::exchange(m_planning, {});
    const QString digest = planned.digest();
    if (!m_library) {
        emit stateChanged();
        return;
    }
    m_left.insert(planned.device, plan.left);
    emit leftChanged();
    if (plan.changes.isEmpty() && plan.positions.isEmpty()) {
        markSynced(planned.device, digest);
        setStatus({});
    } else {
        const int id = m_library->saveDeviceAnnotations(plan.changes, plan.positions);
        if (id < 0) {
            m_pending = true;
            setStatus(tr("The highlights of %1 wait until the library can be changed.").arg(nameOf(planned.device)));
        } else {
            m_waiting.insert(id, { planned.device, digest, plan.placed, int(plan.positions.size()) });
        }
    }
    emit stateChanged();
    runNext();
}

void HighlightSync::finishChange(int id, bool saved, const QString &error)
{
    Q_UNUSED(error);
    if (!m_waiting.contains(id))
        return;
    const Waiting waiting = m_waiting.take(id);
    const QString name = nameOf(waiting.device);
    if (saved) {
        markSynced(waiting.device, waiting.digest);
        const QString message = waiting.placed > 0
            ? (waiting.placed == 1 ? tr("%1 highlight from %2 in the library") : tr("%1 highlights from %2 in the library"))
                  .arg(waiting.placed)
                  .arg(name)
            : waiting.places > 0
            ? (waiting.places == 1 ? tr("Where %2 stopped in %1 book, in the library") : tr("Where %2 stopped in %1 books, in the library"))
                  .arg(waiting.places)
                  .arg(name)
            : tr("The highlights of %1 updated in the library").arg(name);
        setStatus(message);
        emit synced(message);
    } else {
        m_pending = true;
        setStatus(tr("The highlights of %1 wait until the library can be changed.").arg(name));
    }
    emit stateChanged();
}

void HighlightSync::markSynced(const QString &device, const QString &digest)
{
    const QString key = libraryKey();
    auto found = m_snapshots.find(device);
    // Only what the Kobo was last read as: a file synced leaves it be.
    if (key.isEmpty() || found == m_snapshots.end() || found->digest() != digest || found->synced.value(key) == off)
        return;
    found->synced.insert(key, digest);
    QString error;
    KoboHighlights::writeSnapshot(*found, &error);
}

void HighlightSync::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit stateChanged();
}
