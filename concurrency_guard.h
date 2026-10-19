#pragma once

#include <QDateTime>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

// Whether the library may be written now, and if not why. Writing is safe
// only while nothing else can be writing: no Calibre on this computer, no
// Calibre journal, Dropbox done syncing the database, no recent change from
// elsewhere and no Dropbox conflict copy of the database left unseen. The
// checks run again every couple of seconds.
class ConcurrencyGuard : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY libraryPathChanged)
    // Set by the session from the library model: whether its last load
    // worked and whether the database has a structure Reliure knows.
    Q_PROPERTY(bool libraryReadable READ libraryReadable WRITE setLibraryReadable NOTIFY stateChanged)
    Q_PROPERTY(bool schemaKnown READ schemaKnown WRITE setSchemaKnown NOTIFY stateChanged)
    Q_PROPERTY(bool writable READ writable NOTIFY stateChanged)
    // "writable", "unreadable", "schema", "calibre", "journal", "dropbox",
    // "recent" or "conflict": the first check that fails, in that order.
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

        static Environment system();
    };

    explicit ConcurrencyGuard(QObject *parent = nullptr);
    explicit ConcurrencyGuard(const Environment &environment, QObject *parent = nullptr);

    QString libraryPath() const;
    void setLibraryPath(const QString &path);
    bool libraryReadable() const;
    void setLibraryReadable(bool readable);
    bool schemaKnown() const;
    void setSchemaKnown(bool known);
    bool writable() const;
    QString state() const;
    QString readOnlyReason() const;
    QString dropboxStatus() const;
    QVariantList conflicts() const;

    // Runs every check now; Dropbox answers later.
    Q_INVOKABLE void refresh();
    // Stops counting a conflict copy against writing; another copy, or this
    // one changed, counts again.
    Q_INVOKABLE void acknowledgeConflict(const QString &file);

    // Whether a Calibre program that can write the library, the GUI,
    // calibredb or the content server, holds Calibre's lock for `uid`. Linux
    // only: Calibre binds an abstract socket there.
    static bool calibreRunning(const QString &procNetUnix, uint uid);
    // The same for this user on this computer.
    static bool calibreRunningHere();
    // Dropbox's conflict copies in `folder`, by file name.
    static QStringList conflictCopies(const QString &folder);
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

private:
    void evaluate();
    void askDropbox();
    void finishDropbox(const QString &status);
    bool inDropbox() const;

    Environment m_environment;
    QString m_libraryPath;
    bool m_libraryReadable = false;
    bool m_schemaKnown = false;
    QString m_state = QStringLiteral("unreadable");
    QString m_reason;
    QString m_dropboxStatus;
    // The status the last stateChanged() went out with.
    QString m_reportedDropboxStatus;
    QVariantList m_conflicts;
    QTimer m_timer;
    QPointer<QLocalSocket> m_socket;
    QByteArray m_answer;
};
