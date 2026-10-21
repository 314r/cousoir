#pragma once

#include <QDateTime>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

// Whether the library may be written now, and if not why. Writing is safe
// only while nothing else can be writing: no Calibre on this computer, no
// Calibre journal, Dropbox done syncing the database, and no conflict copy of
// the database left unseen. A library shared with other computers, through a
// sync service or a network folder, also waits for a recent change to settle
// and for another computer's Cousoir to stop editing. The checks run again
// every couple of seconds.
class ConcurrencyGuard : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY libraryPathChanged)
    // Set by the session from the library model: whether its last load
    // worked and whether the database has a structure Cousoir knows.
    Q_PROPERTY(bool libraryReadable READ libraryReadable WRITE setLibraryReadable NOTIFY stateChanged)
    Q_PROPERTY(bool schemaKnown READ schemaKnown WRITE setSchemaKnown NOTIFY stateChanged)
    // The library's id, which names its folder of heartbeats.
    Q_PROPERTY(QString libraryUuid READ libraryUuid WRITE setLibraryUuid NOTIFY stateChanged)
    // The metadata.db Cousoir itself last wrote, as "size:time:inode": that
    // change is no reason to wait.
    Q_PROPERTY(QString ownChangeKey READ ownChangeKey WRITE setOwnChangeKey NOTIFY stateChanged)
    Q_PROPERTY(bool writable READ writable NOTIFY stateChanged)
    // How the library reaches other computers, as found: "dropbox",
    // "syncthing", "nextcloud" (Nextcloud or ownCloud), "network", or empty.
    Q_PROPERTY(QString syncService READ syncService NOTIFY sharingChanged)
    // The synced folder that holds the heartbeats; empty on a network folder
    // or none.
    Q_PROPERTY(QString syncRoot READ syncRoot NOTIFY sharingChanged)
    // The user's word for this library, kept per library: "shared", "local",
    // or empty to go by syncService.
    Q_PROPERTY(QString sharing READ sharing WRITE setSharing NOTIFY sharingChanged)
    // Other computers may change the library: a recent change and another
    // computer's Cousoir keep it read-only.
    Q_PROPERTY(bool shared READ shared NOTIFY sharingChanged)
    // "writable", "unreadable", "schema", "calibre", "journal", "dropbox",
    // "elsewhere", "recent" or "conflict": the first check that fails, in
    // that order.
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString readOnlyReason READ readOnlyReason NOTIFY stateChanged)
    // Dropbox's word for metadata.db, like "up to date" or "syncing"; empty
    // when the library is not in a Dropbox folder, "unavailable" when Dropbox
    // does not answer.
    Q_PROPERTY(QString dropboxStatus READ dropboxStatus NOTIFY stateChanged)
    // The conflict copies at the library's root: { file, size, modified,
    // database, acknowledged }.
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY conflictsChanged)

public:
    // Where the checks look; tests point them elsewhere.
    struct Environment
    {
        QString procNetUnix = QStringLiteral("/proc/net/unix");
        QString dropboxSocket;
        QString dropboxInfo;
        // How long after a change from elsewhere the library stays read-only.
        int coolDownSeconds = 120;
        int intervalMs = 2000;
        // How long after its last change a Cousoir on another computer keeps
        // this one read-only, and how old its heartbeat may be.
        int editingSeconds = 600;
        int heartbeatSeconds = 180;
        // This computer's name in the heartbeats.
        QString host;
        // The library's file system, as QStorageInfo names it; empty to ask.
        QByteArray fileSystemType;

        static Environment system();
    };

    // Where the library is shared from: the service, its folder, and the
    // file system it was found on.
    struct Sharing
    {
        QString service;
        QString root;
        QByteArray fileSystemType;

        bool operator==(const Sharing &) const = default;
    };

    explicit ConcurrencyGuard(QObject *parent = nullptr);
    explicit ConcurrencyGuard(const Environment &environment, QObject *parent = nullptr);

    QString libraryPath() const;
    void setLibraryPath(const QString &path);
    bool libraryReadable() const;
    void setLibraryReadable(bool readable);
    bool schemaKnown() const;
    void setSchemaKnown(bool known);
    QString libraryUuid() const;
    void setLibraryUuid(const QString &uuid);
    QString ownChangeKey() const;
    void setOwnChangeKey(const QString &key);
    bool writable() const;
    QString state() const;
    QString readOnlyReason() const;
    QString dropboxStatus() const;
    QVariantList conflicts() const;
    QString syncService() const;
    QString syncRoot() const;
    QString sharing() const;
    void setSharing(const QString &sharing);
    bool shared() const;

    // Runs every check now; Dropbox answers later.
    Q_INVOKABLE void refresh();
    // Stops counting a conflict copy against writing; another copy, or this
    // one changed, counts again.
    Q_INVOKABLE void acknowledgeConflict(const QString &file);
    // Tells the other computers sharing the library through Dropbox,
    // Syncthing or Nextcloud that Cousoir just changed it here: their Cousoir
    // stays read-only for a while. The heartbeat is a file per computer in a
    // .cousoir folder at the root of the synced folder.
    Q_INVOKABLE void noteWrite();

    // "size:time:inode" of a file, as ownChangeKey takes it.
    static QString changeKey(const QString &path);
    // The heartbeat folder of a library in a synced folder.
    static QString heartbeatFolder(const QString &syncRoot, const QString &uuid);
    // How the library at `libraryPath` is shared: in one of `dropboxRoots`,
    // in a Syncthing or Nextcloud folder, found by the marker or journal at
    // its root, or on a network file system. `fileSystemType` stands for
    // QStorageInfo's when it is not empty.
    static Sharing findSharing(const QString &libraryPath, const QStringList &dropboxRoots,
                               const QByteArray &fileSystemType = {});
    // Whether QStorageInfo's `type` is a file system other computers write to.
    static bool networkFileSystem(const QByteArray &type);

    // Whether a Calibre program that can write the library, the GUI,
    // calibredb or the content server, holds Calibre's lock for `uid`. Linux
    // only: Calibre binds an abstract socket there.
    static bool calibreRunning(const QString &procNetUnix, uint uid);
    // The same for this user on this computer.
    static bool calibreRunningHere();
    // The conflict copies of Dropbox, Syncthing, Nextcloud and ownCloud in
    // `folder`, by file name.
    static QStringList conflictCopies(const QString &folder);
    // Whether `name` is the name of such a copy.
    static bool isConflictCopy(const QString &name);
    // The Dropbox folders of info.json.
    static QStringList dropboxRoots(const QString &infoPath);
    // The request Dropbox's command socket takes for a file's status, and its
    // answer's status; empty when the answer is not one.
    static QByteArray dropboxStatusRequest(const QString &path);
    static QString parseDropboxStatus(const QByteArray &answer);

signals:
    void libraryPathChanged();
    void stateChanged();
    void conflictsChanged();
    void sharingChanged();

private:
    void evaluate();
    void writeHeartbeat();
    // The computer editing the library elsewhere, if any.
    QString editingElsewhere() const;
    // Looks for the sync service again; true when it changed.
    bool detectSharing();
    // Where heartbeats go: the synced folder, while the library is shared.
    QString heartbeatRoot() const;
    // The library's path as the sharing choice is kept under.
    QString settingsPath() const;
    void askDropbox();
    void finishDropbox(const QString &status);
    bool inDropbox() const;

    Environment m_environment;
    QString m_libraryPath;
    bool m_libraryReadable = false;
    bool m_schemaKnown = false;
    QString m_libraryUuid;
    QString m_ownChangeKey;
    QDateTime m_lastWrite;
    int m_ticks = 0;
    QString m_state = QStringLiteral("unreadable");
    QString m_reason;
    QString m_dropboxStatus;
    // The status the last stateChanged() went out with.
    QString m_reportedDropboxStatus;
    QVariantList m_conflicts;
    Sharing m_found;
    QString m_sharingChoice;
    QTimer m_timer;
    QPointer<QLocalSocket> m_socket;
    QByteArray m_answer;
};
