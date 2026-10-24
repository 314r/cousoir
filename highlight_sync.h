#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include "kobo_highlights.h"
#include "kobo_sync.h"

class BookLibrary;
class DeviceLibrary;

// Puts each Kobo's highlights into the library, as the Kobo's own
// annotations user, whenever the Kobo is read or the library it was read
// with opens, a KoboReader.sqlite file when asked; waits while the library
// cannot be written. What the Kobo was last read as is kept, a file per Kobo,
// and what is left out of the library is kept to be shown.
class HighlightSync : public QObject
{
    Q_OBJECT
    Q_PROPERTY(BookLibrary *library READ library WRITE setLibrary NOTIFY libraryChanged)
    Q_PROPERTY(DeviceLibrary *devices READ devices WRITE setDevices NOTIFY devicesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Changes that wait for the library to be writable.
    Q_PROPERTY(bool pending READ pending NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // Each Kobo met with this library: { device, name, seen, source, off,
    // highlights }.
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)

public:
    explicit HighlightSync(QObject *parent = nullptr);
    ~HighlightSync() override;

    BookLibrary *library() const;
    void setLibrary(BookLibrary *library);
    DeviceLibrary *devices() const;
    void setDevices(DeviceLibrary *devices);
    bool busy() const;
    bool pending() const;
    QString status() const;
    QVariantList sources() const;

    // Syncs every Kobo met with the library again, those taken out of it
    // too.
    Q_INVOKABLE void syncNow();
    // Syncs a Kobo's database, a backup for one: what it has goes in, what
    // it lacks stays. An error message, empty once on its way.
    Q_INVOKABLE QString importDatabase(const QUrl &file);
    // Takes all of a Kobo's highlights out of the library, and syncs it no
    // more. An error message, empty once on its way.
    Q_INVOKABLE QString removeFromLibrary(const QString &device);
    // A Kobo's name, by the user its highlights go by.
    Q_INVOKABLE QString nameOf(const QString &device) const;

    // What is left out of the library, by Kobo.
    QHash<QString, QList<KoboSync::Left>> left() const;

    // A Kobo read: kept, then synced.
    void take(const KoboHighlights::Snapshot &snapshot);

signals:
    void libraryChanged();
    void devicesChanged();
    void stateChanged();
    void sourcesChanged();
    void leftChanged();
    // Highlights went into the library.
    void synced(const QString &message);

private:
    struct Waiting
    {
        QString device;
        QString digest;
        int placed = 0;
    };

    QString libraryKey() const;
    void loadSnapshots();
    void openLibrary();
    // Plans, then saves, `snapshot` into the library.
    void queue(const KoboHighlights::Snapshot &snapshot);
    void runNext();
    void markSynced(const QString &device, const QString &digest);
    void finishPlan();
    void finishChange(int id, bool saved, const QString &error);
    void setStatus(const QString &status);

    QPointer<BookLibrary> m_library;
    QPointer<DeviceLibrary> m_devices;
    QHash<QString, KoboHighlights::Snapshot> m_snapshots;
    QHash<QString, QList<KoboSync::Left>> m_left;
    QList<KoboHighlights::Snapshot> m_queue;
    // The snapshot a plan is being made of.
    KoboHighlights::Snapshot m_planning;
    QFutureWatcher<KoboSync::Plan> m_watcher;
    QHash<int, Waiting> m_waiting;
    bool m_pending = false;
    QString m_status;
    // The library synced with on opening.
    QString m_openedPath;
};
