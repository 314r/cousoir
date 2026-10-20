#include "concurrency_guard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSysInfo>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "calibre_db.h"
#include "calibre_lock.h"

namespace {

const char acknowledgedKey[] = "Library/acknowledgedConflicts";

// A conflict copy is known by its name, size and time: Dropbox making a new
// one under the same name counts again.
QString conflictKey(const QString &file, qint64 size, qint64 modifiedMs)
{
    return QStringLiteral("%1|%2|%3").arg(file).arg(size).arg(modifiedMs);
}

uint currentUid()
{
#ifdef Q_OS_UNIX
    return uint(::getuid());
#else
    return 0;
#endif
}

} // namespace

ConcurrencyGuard::Environment ConcurrencyGuard::Environment::system()
{
    Environment environment;
    environment.host = QSysInfo::machineHostName();
    environment.dropboxSocket = QDir::homePath() + QStringLiteral("/.dropbox/command_socket");
    environment.dropboxInfo = QDir::homePath() + QStringLiteral("/.dropbox/info.json");
    return environment;
}

ConcurrencyGuard::ConcurrencyGuard(QObject *parent)
    : ConcurrencyGuard(Environment::system(), parent)
{
}

ConcurrencyGuard::ConcurrencyGuard(const Environment &environment, QObject *parent)
    : QObject(parent)
    , m_environment(environment)
{
    m_timer.setInterval(m_environment.intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &ConcurrencyGuard::refresh);
}

QString ConcurrencyGuard::libraryPath() const
{
    return m_libraryPath;
}

void ConcurrencyGuard::setLibraryPath(const QString &path)
{
    const QString cleaned = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (m_libraryPath == cleaned)
        return;
    m_libraryPath = cleaned;
    m_dropboxStatus.clear();
    if (m_socket)
        m_socket->abort();
    emit libraryPathChanged();
    if (m_libraryPath.isEmpty())
        m_timer.stop();
    else
        m_timer.start();
    refresh();
}

bool ConcurrencyGuard::libraryReadable() const
{
    return m_libraryReadable;
}

void ConcurrencyGuard::setLibraryReadable(bool readable)
{
    if (m_libraryReadable == readable)
        return;
    m_libraryReadable = readable;
    evaluate();
}

bool ConcurrencyGuard::schemaKnown() const
{
    return m_schemaKnown;
}

void ConcurrencyGuard::setSchemaKnown(bool known)
{
    if (m_schemaKnown == known)
        return;
    m_schemaKnown = known;
    evaluate();
}

QString ConcurrencyGuard::libraryUuid() const
{
    return m_libraryUuid;
}

void ConcurrencyGuard::setLibraryUuid(const QString &uuid)
{
    if (m_libraryUuid == uuid)
        return;
    m_libraryUuid = uuid;
    evaluate();
}

QString ConcurrencyGuard::ownChangeKey() const
{
    return m_ownChangeKey;
}

void ConcurrencyGuard::setOwnChangeKey(const QString &key)
{
    if (m_ownChangeKey == key)
        return;
    m_ownChangeKey = key;
    evaluate();
}

QString ConcurrencyGuard::changeKey(const QString &path)
{
    const FileIdentity identity = CalibreDb::identify(path);
    return identity.exists ? QStringLiteral("%1:%2:%3").arg(identity.size).arg(identity.modifiedMs).arg(identity.inode)
                           : QString();
}

QString ConcurrencyGuard::heartbeatFolder(const QString &dropboxRoot, const QString &uuid)
{
    return QDir(dropboxRoot).filePath(QStringLiteral(".cousoir/libraries/%1/sessions").arg(uuid));
}

QString ConcurrencyGuard::dropboxRoot() const
{
    if (m_libraryPath.isEmpty())
        return {};
    const QString library = QFileInfo(m_libraryPath).canonicalFilePath();
    for (const QString &root : dropboxRoots(m_environment.dropboxInfo)) {
        const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
        if (!canonicalRoot.isEmpty() && (library == canonicalRoot || library.startsWith(canonicalRoot + u'/')))
            return canonicalRoot;
    }
    return {};
}

void ConcurrencyGuard::noteWrite()
{
    m_lastWrite = QDateTime::currentDateTimeUtc();
    writeHeartbeat();
}

void ConcurrencyGuard::writeHeartbeat()
{
    // Only while this computer's change is recent: an idle Cousoir keeps no
    // other computer from editing.
    const QString root = dropboxRoot();
    if (root.isEmpty() || m_libraryUuid.isEmpty() || !m_lastWrite.isValid()
        || m_lastWrite.secsTo(QDateTime::currentDateTimeUtc()) > m_environment.editingSeconds)
        return;
    const QString folder = heartbeatFolder(root, m_libraryUuid);
    QDir().mkpath(folder);
    QSaveFile file(QDir(folder).filePath(m_environment.host + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{
        { QStringLiteral("host"), m_environment.host },
        { QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
        { QStringLiteral("lastWrite"), m_lastWrite.toString(Qt::ISODate) },
    }).toJson(QJsonDocument::Compact));
    file.commit();
}

QString ConcurrencyGuard::editingElsewhere() const
{
    const QString root = dropboxRoot();
    if (root.isEmpty() || m_libraryUuid.isEmpty())
        return {};
    const QDir folder(heartbeatFolder(root, m_libraryUuid));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QString &file : folder.entryList({ QStringLiteral("*.json") }, QDir::Files)) {
        QFile heartbeat(folder.filePath(file));
        if (!heartbeat.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject beat = QJsonDocument::fromJson(heartbeat.readAll()).object();
        const QString host = beat.value(QLatin1String("host")).toString();
        if (host.isEmpty() || host == m_environment.host)
            continue;
        const QDateTime updated = QDateTime::fromString(beat.value(QLatin1String("updated")).toString(), Qt::ISODate);
        const QDateTime lastWrite = QDateTime::fromString(beat.value(QLatin1String("lastWrite")).toString(), Qt::ISODate);
        if (updated.isValid() && lastWrite.isValid() && updated.secsTo(now) <= m_environment.heartbeatSeconds
            && lastWrite.secsTo(now) <= m_environment.editingSeconds)
            return host;
    }
    return {};
}

bool ConcurrencyGuard::writable() const
{
    return m_state == QLatin1String("writable");
}

QString ConcurrencyGuard::state() const
{
    return m_state;
}

QString ConcurrencyGuard::readOnlyReason() const
{
    return m_reason;
}

QString ConcurrencyGuard::dropboxStatus() const
{
    return m_dropboxStatus;
}

QVariantList ConcurrencyGuard::conflicts() const
{
    return m_conflicts;
}

void ConcurrencyGuard::refresh()
{
    evaluate();
    askDropbox();
    // About once a minute.
    if (++m_ticks % std::max(1, 60000 / std::max(1, m_environment.intervalMs)) == 0)
        writeHeartbeat();
}

void ConcurrencyGuard::acknowledgeConflict(const QString &file)
{
    for (const QVariant &value : std::as_const(m_conflicts)) {
        const QVariantMap conflict = value.toMap();
        if (conflict.value(QStringLiteral("file")).toString() != file)
            continue;
        QSettings settings;
        QStringList acknowledged = settings.value(QLatin1String(acknowledgedKey)).toStringList();
        const QString key = conflictKey(file, conflict.value(QStringLiteral("size")).toLongLong(),
                                        conflict.value(QStringLiteral("modifiedMs")).toLongLong());
        if (!acknowledged.contains(key)) {
            acknowledged.append(key);
            settings.setValue(QLatin1String(acknowledgedKey), acknowledged);
        }
    }
    evaluate();
}

bool ConcurrencyGuard::calibreRunning(const QString &procNetUnix, uint uid)
{
    QFile file(procNetUnix);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    // create_single_instance_mutex() in utils/lock.py binds
    // "\0calibre-singleinstance-<uid>-db", which /proc lists with an "@".
    const QByteArray name = "@calibre-singleinstance-" + QByteArray::number(uid) + "-db";
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.endsWith(name) && (line.size() == name.size() || line.at(line.size() - name.size() - 1) == ' '))
            return true;
    }
    return false;
}

bool ConcurrencyGuard::calibreRunningHere()
{
    return calibreRunning(Environment::system().procNetUnix, currentUid());
}

QStringList ConcurrencyGuard::conflictCopies(const QString &folder)
{
    // Dropbox names a conflict copy in its user's language.
    static const QRegularExpression pattern(
        QStringLiteral("\\((conflicted copy|copie en conflit|case conflict|konflikt|copia en conflicto"
                       "|copia in conflitto|kopia w konflikcie|cópia em conflito)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList copies;
    const QStringList files = QDir(folder).entryList(QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString &file : files) {
        if (pattern.match(file).hasMatch())
            copies.append(file);
    }
    return copies;
}

QStringList ConcurrencyGuard::dropboxRoots(const QString &infoPath)
{
    QFile file(infoPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList roots;
    const QJsonObject accounts = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &account : accounts) {
        const QString path = account.toObject().value(QLatin1String("path")).toString();
        if (!path.isEmpty())
            roots.append(QDir::cleanPath(path));
    }
    return roots;
}

QByteArray ConcurrencyGuard::dropboxStatusRequest(const QString &path)
{
    return "icon_overlay_file_status\npath\t" + path.toUtf8() + "\ndone\n";
}

QString ConcurrencyGuard::parseDropboxStatus(const QByteArray &answer)
{
    // "ok\nstatus\tup to date\ndone\n"; "notok\n<message>\ndone\n" otherwise.
    const QList<QByteArray> lines = answer.split('\n');
    if (lines.isEmpty() || lines.first() != "ok")
        return {};
    for (const QByteArray &line : lines) {
        if (line.startsWith("status\t"))
            return QString::fromUtf8(line.mid(7));
    }
    return {};
}

bool ConcurrencyGuard::inDropbox() const
{
    return !dropboxRoot().isEmpty();
}

void ConcurrencyGuard::askDropbox()
{
    if (m_socket || !inDropbox())
        return;
    auto *socket = new QLocalSocket(this);
    m_socket = socket;
    m_answer.clear();
    const QString database = QDir(m_libraryPath).filePath(QStringLiteral("metadata.db"));
    connect(socket, &QLocalSocket::connected, socket, [socket, database] {
        socket->write(dropboxStatusRequest(database));
    });
    connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
        m_answer += socket->readAll();
        if (m_answer.endsWith("done\n"))
            finishDropbox(parseDropboxStatus(m_answer));
    });
    connect(socket, &QLocalSocket::errorOccurred, socket, [this] { finishDropbox(QStringLiteral("unavailable")); });
    // A Dropbox busy indexing can take a while to answer; not answering at
    // all counts as not running.
    QTimer::singleShot(3000, socket, [this] { finishDropbox(QStringLiteral("unavailable")); });
    socket->connectToServer(m_environment.dropboxSocket);
}

void ConcurrencyGuard::finishDropbox(const QString &status)
{
    if (!m_socket)
        return;
    QLocalSocket *socket = m_socket;
    m_socket = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    // "notok" comes back for a file Dropbox does not know yet.
    m_dropboxStatus = status.isEmpty() ? QStringLiteral("unknown") : status;
    evaluate();
}

void ConcurrencyGuard::evaluate()
{
    const QDir library(m_libraryPath);
    const QString database = library.filePath(QStringLiteral("metadata.db"));

    const QStringList acknowledged = QSettings().value(QLatin1String(acknowledgedKey)).toStringList();
    QVariantList conflicts;
    bool unseenConflict = false;
    if (!m_libraryPath.isEmpty()) {
        for (const QString &file : conflictCopies(m_libraryPath)) {
            const QFileInfo info(library.filePath(file));
            const qint64 modifiedMs = info.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch();
            const bool isDatabase = file.startsWith(QLatin1String("metadata")) && file.endsWith(QLatin1String(".db"));
            const bool seen = acknowledged.contains(conflictKey(file, info.size(), modifiedMs));
            unseenConflict = unseenConflict || (isDatabase && !seen);
            conflicts.append(QVariantMap{
                { QStringLiteral("file"), file },
                { QStringLiteral("size"), info.size() },
                { QStringLiteral("modifiedMs"), modifiedMs },
                { QStringLiteral("modified"), info.fileTime(QFileDevice::FileModificationTime) },
                { QStringLiteral("database"), isDatabase },
                { QStringLiteral("acknowledged"), seen },
            });
        }
    }
    if (m_conflicts != conflicts) {
        m_conflicts = conflicts;
        emit conflictsChanged();
    }

    QString state = QStringLiteral("writable");
    QString reason;
    const bool dropbox = inDropbox();
    const FileIdentity identity = CalibreDb::identify(database);
    const qint64 age = (QDateTime::currentMSecsSinceEpoch() - identity.modifiedMs) / 1000;
    const bool ownChange = !m_ownChangeKey.isEmpty() && changeKey(database) == m_ownChangeKey;
    const QString elsewhere = dropbox ? editingElsewhere() : QString();
    if (m_libraryPath.isEmpty() || !m_libraryReadable) {
        state = QStringLiteral("unreadable");
        reason = tr("Read-only: the library cannot be read");
    } else if (!m_schemaKnown) {
        state = QStringLiteral("schema");
        reason = tr("Read-only: the database is from a Calibre version Cousoir does not know");
    } else if (calibreRunning(m_environment.procNetUnix, currentUid()) && !CalibreLock::heldHere()) {
        state = QStringLiteral("calibre");
        reason = tr("Read-only: Calibre is open on this computer");
    } else if (QFileInfo::exists(database + QStringLiteral("-journal"))
               || QFileInfo::exists(database + QStringLiteral("-wal"))) {
        state = QStringLiteral("journal");
        reason = tr("Read-only: Calibre is writing to the library");
    } else if (dropbox && m_dropboxStatus != QLatin1String("up to date")
               && m_dropboxStatus != QLatin1String("unwatched")
               // Dropbox sending Cousoir's own change is no reason to wait.
               && !(ownChange && m_dropboxStatus == QLatin1String("syncing"))) {
        state = QStringLiteral("dropbox");
        reason = m_dropboxStatus.isEmpty() ? tr("Read-only: waiting for Dropbox")
            : m_dropboxStatus == QLatin1String("unavailable") ? tr("Read-only: Dropbox is not running")
            : m_dropboxStatus == QLatin1String("syncing") ? tr("Read-only: Dropbox is syncing")
            : tr("Read-only: Dropbox says “%1”").arg(m_dropboxStatus);
    } else if (!elsewhere.isEmpty()) {
        state = QStringLiteral("elsewhere");
        reason = tr("Read-only: Cousoir is editing on %1").arg(elsewhere);
    } else if (identity.exists && age < m_environment.coolDownSeconds && !ownChange) {
        state = QStringLiteral("recent");
        reason = tr("Read-only: library changed %1 s ago").arg(std::max<qint64>(0, age));
    } else if (unseenConflict) {
        state = QStringLiteral("conflict");
        reason = tr("Read-only: Dropbox conflict copy");
    }
    if (!dropbox && !m_dropboxStatus.isEmpty())
        m_dropboxStatus.clear();

    if (m_state == state && m_reason == reason && m_reportedDropboxStatus == m_dropboxStatus)
        return;
    m_state = state;
    m_reason = reason;
    m_reportedDropboxStatus = m_dropboxStatus;
    emit stateChanged();
}
