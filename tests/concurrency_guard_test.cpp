#include <QtTest>

#include <QDirIterator>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QTemporaryDir>

#include <unistd.h>

#include "concurrency_guard.h"

namespace {

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

void age(const QString &path, int seconds)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-seconds), QFileDevice::FileModificationTime));
}

// Answers Dropbox's command socket protocol with `status`.
class FakeDropbox : public QObject
{
public:
    explicit FakeDropbox(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            QLocalSocket *socket = m_server.nextPendingConnection();
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                m_request += socket->readAll();
                if (!m_request.endsWith("done\n"))
                    return;
                socket->write(status.isEmpty() ? QByteArray("notok\nno such file\ndone\n")
                                               : "ok\nstatus\t" + status + "\ndone\n");
                socket->flush();
            });
        });
    }

    QByteArray status = "up to date";
    QByteArray m_request;

private:
    QLocalServer m_server;
};

} // namespace

class ConcurrencyGuardTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void detectsCalibre();
    void findsConflictCopies();
    void parsesDropboxAnswers();
    void readsDropboxRoots();
    void findsSharedFolders_data();
    void findsSharedFolders();
    void recognisesNetworkFileSystems();
    void writableWhenEverythingIsQuiet();
    void readOnlyUntilLoadedAndKnown();
    void readOnlyWhileCalibreRuns();
    void readOnlyWithJournal();
    void readOnlyWhileDropboxSyncs();
    void readOnlyWithoutDropbox();
    void readOnlyAfterRecentChange();
    void noWaitForLocalLibrary();
    void readOnlyAfterRecentChangeWhenShared_data();
    void readOnlyAfterRecentChangeWhenShared();
    void followsSharingChoice();
    void onlyThisComputerInDropbox();
    void readOnlyUntilConflictSeen();
    void ownChangeIsNoReasonToWait();
    void readOnlyWhileEditedElsewhere_data();
    void readOnlyWhileEditedElsewhere();

private:
    ConcurrencyGuard::Environment environment() const;
    std::unique_ptr<ConcurrencyGuard> guard(bool loaded = true);
    std::unique_ptr<ConcurrencyGuard> guard(const ConcurrencyGuard::Environment &environment);
    // Makes the library at `relative` under the root, its database changed
    // `seconds` ago, and the one the guards check.
    void makeLibrary(const QString &relative, int seconds);
    // Writes `marker` under the root: a folder when it ends with "/".
    void makeMarker(const QString &marker);

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_library;
};

void ConcurrencyGuardTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("cousoir-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("concurrency-guard"));
}

void ConcurrencyGuardTest::init()
{
    QSettings().clear();
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    // A library inside a Dropbox folder, older than the cool-down.
    m_library = m_root->filePath(QStringLiteral("Dropbox/Calibre"));
    QVERIFY(QDir().mkpath(m_library));
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("metadata.db")), "db"));
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), 600);
    QVERIFY(writeFile(m_root->filePath(QStringLiteral("info.json")),
                      "{\"personal\": {\"path\": \"" + m_root->filePath(QStringLiteral("Dropbox")).toUtf8() + "\"}}"));
    QVERIFY(writeFile(m_root->filePath(QStringLiteral("unix")), "Num RefCount Protocol Flags Type St Inode Path\n"));
}

ConcurrencyGuard::Environment ConcurrencyGuardTest::environment() const
{
    ConcurrencyGuard::Environment environment;
    environment.procNetUnix = m_root->filePath(QStringLiteral("unix"));
    environment.dropboxSocket = m_root->filePath(QStringLiteral("dropbox.sock"));
    environment.dropboxInfo = m_root->filePath(QStringLiteral("info.json"));
    environment.intervalMs = 60000;
    // Whatever disk the temporary folder is on.
    environment.fileSystemType = "ext4";
    return environment;
}

std::unique_ptr<ConcurrencyGuard> ConcurrencyGuardTest::guard(bool loaded)
{
    auto result = std::make_unique<ConcurrencyGuard>(environment());
    result->setLibraryReadable(loaded);
    result->setSchemaKnown(loaded);
    result->setLibraryPath(m_library);
    return result;
}

std::unique_ptr<ConcurrencyGuard> ConcurrencyGuardTest::guard(const ConcurrencyGuard::Environment &environment)
{
    auto result = std::make_unique<ConcurrencyGuard>(environment);
    result->setLibraryReadable(true);
    result->setSchemaKnown(true);
    result->setLibraryPath(m_library);
    return result;
}

void ConcurrencyGuardTest::makeLibrary(const QString &relative, int seconds)
{
    m_library = m_root->filePath(relative);
    QVERIFY(QDir().mkpath(m_library));
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("metadata.db")), "db"));
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), seconds);
}

void ConcurrencyGuardTest::makeMarker(const QString &marker)
{
    if (marker.endsWith(u'/'))
        QVERIFY(QDir().mkpath(m_root->filePath(marker)));
    else if (!marker.isEmpty())
        QVERIFY(writeFile(m_root->filePath(marker), "x"));
}

void ConcurrencyGuardTest::detectsCalibre()
{
    const QString path = m_root->filePath(QStringLiteral("unix"));
    QVERIFY(!ConcurrencyGuard::calibreRunning(path, 1000));
    // The GUI's lock alone is not the database lock; another user's is not ours.
    QVERIFY(writeFile(path, "Num RefCount Protocol Flags Type St Inode Path\n"
                            "0000: 00000002 00000000 00010000 0001 01 4242 @calibre-singleinstance-1000-GUI\n"
                            "0000: 00000002 00000000 00010000 0001 01 4243 @calibre-singleinstance-1001-db\n"
                            "0000: 00000002 00000000 00010000 0001 01 4244 @xcalibre-singleinstance-1000-db\n"));
    QVERIFY(!ConcurrencyGuard::calibreRunning(path, 1000));
    QVERIFY(writeFile(path, "Num RefCount Protocol Flags Type St Inode Path\n"
                            "0000: 00000002 00000000 00010000 0001 01 4245 @calibre-singleinstance-1000-db\n"));
    QVERIFY(ConcurrencyGuard::calibreRunning(path, 1000));
    QVERIFY(!ConcurrencyGuard::calibreRunning(m_root->filePath(QStringLiteral("missing")), 1000));
}

void ConcurrencyGuardTest::findsConflictCopies()
{
    for (const char *name : { "metadata (conflicted copy 2024-11-14).db",
                              "metadata (Copie en conflit de Pierre 2024-11-14).db",
                              "metadata (Pierre's conflicted copy 2024-11-14).db",
                              "metadata (case clash from 2024-11-14 101500).db",
                              "metadata.sync-conflict-20241114-101500-ABCDEFG.db",
                              "metadata.sync-conflict-20241114-101500-.db",
                              "metadata_conflict-20241114-101500.db",
                              "notes (Case Conflict).txt", "metadata.db", "metadata_db_prefs_backup.json",
                              "Conflict (2024).epub" }) {
        QVERIFY(writeFile(QDir(m_library).filePath(QString::fromUtf8(name)), "x"));
    }
    QCOMPARE(ConcurrencyGuard::conflictCopies(m_library),
             (QStringList{ QStringLiteral("metadata (Copie en conflit de Pierre 2024-11-14).db"),
                           QStringLiteral("metadata (Pierre's conflicted copy 2024-11-14).db"),
                           QStringLiteral("metadata (case clash from 2024-11-14 101500).db"),
                           QStringLiteral("metadata (conflicted copy 2024-11-14).db"),
                           QStringLiteral("metadata.sync-conflict-20241114-101500-.db"),
                           QStringLiteral("metadata.sync-conflict-20241114-101500-ABCDEFG.db"),
                           QStringLiteral("metadata_conflict-20241114-101500.db"),
                           QStringLiteral("notes (Case Conflict).txt") }));
}

void ConcurrencyGuardTest::parsesDropboxAnswers()
{
    QCOMPARE(ConcurrencyGuard::dropboxStatusRequest(QStringLiteral("/a b/metadata.db")),
             QByteArray("icon_overlay_file_status\npath\t/a b/metadata.db\ndone\n"));
    QCOMPARE(ConcurrencyGuard::parseDropboxStatus("ok\nstatus\tup to date\ndone\n"), QStringLiteral("up to date"));
    QCOMPARE(ConcurrencyGuard::parseDropboxStatus("ok\nstatus\tsyncing\ndone\n"), QStringLiteral("syncing"));
    QCOMPARE(ConcurrencyGuard::parseDropboxStatus("notok\nThe 'path' argument does not exist\ndone\n"), QString());
}

void ConcurrencyGuardTest::readsDropboxRoots()
{
    QCOMPARE(ConcurrencyGuard::dropboxRoots(m_root->filePath(QStringLiteral("info.json"))),
             QStringList{ m_root->filePath(QStringLiteral("Dropbox")) });
    QVERIFY(ConcurrencyGuard::dropboxRoots(m_root->filePath(QStringLiteral("none.json"))).isEmpty());
}

void ConcurrencyGuardTest::findsSharedFolders_data()
{
    QTest::addColumn<QString>("library");
    QTest::addColumn<QString>("marker");
    QTest::addColumn<QByteArray>("type");
    QTest::addColumn<QString>("service");
    QTest::addColumn<QString>("root");
    const auto row = [](const char *name, const char *library, const char *marker, const char *type,
                        const char *service, const char *root) {
        QTest::newRow(name) << QString::fromUtf8(library) << QString::fromUtf8(marker) << QByteArray(type)
                            << QString::fromUtf8(service) << QString::fromUtf8(root);
    };
    row("dropbox", "Dropbox/Calibre", "", "ext4", "dropbox", "Dropbox");
    row("syncthing", "Sync/Books/Calibre", "Sync/.stfolder/", "ext4", "syncthing", "Sync");
    row("syncthing before 0.14.38", "Sync/Calibre", "Sync/.stfolder", "ext4", "syncthing", "Sync");
    row("syncthing library", "Calibre", "Calibre/.stfolder/", "ext4", "syncthing", "Calibre");
    row("nextcloud", "Nextcloud/Calibre", "Nextcloud/.sync_0a1b2c3d4e5f.db", "ext4", "nextcloud", "Nextcloud");
    row("older nextcloud", "Nextcloud/Calibre", "Nextcloud/._sync_0a1b2c3d4e5f.db", "ext4", "nextcloud", "Nextcloud");
    row("csync", "ownCloud/Calibre", "ownCloud/.csync_journal.db", "ext4", "nextcloud", "ownCloud");
    row("network", "Share/Calibre", "", "nfs4", "network", "");
    row("local", "Books/Calibre", "", "ext4", "", "");
}

void ConcurrencyGuardTest::findsSharedFolders()
{
    QFETCH(QString, library);
    QFETCH(QString, marker);
    QFETCH(QByteArray, type);
    QFETCH(QString, service);
    QFETCH(QString, root);
    makeLibrary(library, 600);
    makeMarker(marker);
    const ConcurrencyGuard::Sharing found = ConcurrencyGuard::findSharing(
        m_library, ConcurrencyGuard::dropboxRoots(m_root->filePath(QStringLiteral("info.json"))), type);
    QCOMPARE(found.service, service);
    QCOMPARE(found.root, root.isEmpty() ? QString() : QFileInfo(m_root->filePath(root)).canonicalFilePath());
    QCOMPARE(found.fileSystemType, service == QLatin1String("network") || service.isEmpty() ? type : QByteArray());
}

void ConcurrencyGuardTest::recognisesNetworkFileSystems()
{
    for (const char *type : { "nfs", "nfs4", "cifs", "smb3", "9p", "virtiofs", "fuse.sshfs", "fuse.rclone",
                              "fuse.gvfsd-fuse" })
        QVERIFY2(ConcurrencyGuard::networkFileSystem(type), type);
    for (const char *type : { "ext4", "btrfs", "xfs", "tmpfs", "overlay", "fuseblk", "fuse.mergerfs", "fuse.gocryptfs",
                              "" })
        QVERIFY2(!ConcurrencyGuard::networkFileSystem(type), type);
}

void ConcurrencyGuardTest::writableWhenEverythingIsQuiet()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    auto checks = guard();
    QTRY_COMPARE(checks->state(), QStringLiteral("writable"));
    QVERIFY(checks->writable());
    QCOMPARE(checks->dropboxStatus(), QStringLiteral("up to date"));
    QVERIFY(checks->readOnlyReason().isEmpty());
    QVERIFY(dropbox.m_request.contains(QDir(m_library).filePath(QStringLiteral("metadata.db")).toUtf8()));
}

void ConcurrencyGuardTest::readOnlyUntilLoadedAndKnown()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    auto checks = guard(false);
    QCOMPARE(checks->state(), QStringLiteral("unreadable"));
    checks->setLibraryReadable(true);
    QCOMPARE(checks->state(), QStringLiteral("schema"));
    QVERIFY(checks->readOnlyReason().contains(QStringLiteral("Calibre version")));
    checks->setSchemaKnown(true);
    QTRY_COMPARE(checks->state(), QStringLiteral("writable"));
}

void ConcurrencyGuardTest::readOnlyWhileCalibreRuns()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    QVERIFY(writeFile(m_root->filePath(QStringLiteral("unix")),
                      "Num RefCount Protocol Flags Type St Inode Path\n0000: 00000002 00000000 00010000 0001 01 1 "
                      "@calibre-singleinstance-" + QByteArray::number(::getuid()) + "-db\n"));
    auto checks = guard();
    QCOMPARE(checks->state(), QStringLiteral("calibre"));
    QCOMPARE(checks->readOnlyReason(), QStringLiteral("Read-only: Calibre is open on this computer"));
}

void ConcurrencyGuardTest::readOnlyWithJournal()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("metadata.db-journal")), ""));
    auto checks = guard();
    QCOMPARE(checks->state(), QStringLiteral("journal"));
}

void ConcurrencyGuardTest::readOnlyWhileDropboxSyncs()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    dropbox.status = "syncing";
    auto checks = guard();
    QTRY_COMPARE(checks->dropboxStatus(), QStringLiteral("syncing"));
    QCOMPARE(checks->state(), QStringLiteral("dropbox"));
    QCOMPARE(checks->readOnlyReason(), QStringLiteral("Read-only: Dropbox is syncing"));

    dropbox.status = "up to date";
    dropbox.m_request.clear();
    checks->refresh();
    QTRY_COMPARE(checks->state(), QStringLiteral("writable"));
}

void ConcurrencyGuardTest::readOnlyWithoutDropbox()
{
    // Inside a Dropbox folder with no Dropbox answering.
    auto checks = guard();
    QTRY_COMPARE(checks->dropboxStatus(), QStringLiteral("unavailable"));
    QCOMPARE(checks->state(), QStringLiteral("dropbox"));
    QCOMPARE(checks->readOnlyReason(), QStringLiteral("Read-only: Dropbox is not running"));

    // Outside of one, Dropbox does not matter.
    m_library = m_root->filePath(QStringLiteral("Local"));
    QVERIFY(QDir().mkpath(m_library));
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("metadata.db")), "db"));
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), 600);
    checks->setLibraryPath(m_library);
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    QVERIFY(checks->dropboxStatus().isEmpty());
}

void ConcurrencyGuardTest::readOnlyAfterRecentChange()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), 40);
    auto checks = guard();
    QTRY_COMPARE(checks->state(), QStringLiteral("recent"));
    QVERIFY2(checks->readOnlyReason().startsWith(QStringLiteral("Read-only: library changed 4")),
             qPrintable(checks->readOnlyReason()));
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), 121);
    checks->refresh();
    QTRY_COMPARE(checks->state(), QStringLiteral("writable"));
}

void ConcurrencyGuardTest::noWaitForLocalLibrary()
{
    // Calibre just closed on a library no other computer changes.
    makeLibrary(QStringLiteral("Books/Calibre"), 10);
    auto checks = guard();
    QVERIFY(checks->syncService().isEmpty());
    QVERIFY(!checks->shared());
    QCOMPARE(checks->state(), QStringLiteral("writable"));
}

void ConcurrencyGuardTest::readOnlyAfterRecentChangeWhenShared_data()
{
    QTest::addColumn<QString>("marker");
    QTest::addColumn<QByteArray>("type");
    QTest::newRow("syncthing") << QStringLiteral("Shared/.stfolder/") << QByteArray("ext4");
    QTest::newRow("network") << QString() << QByteArray("nfs4");
}

void ConcurrencyGuardTest::readOnlyAfterRecentChangeWhenShared()
{
    QFETCH(QString, marker);
    QFETCH(QByteArray, type);
    makeLibrary(QStringLiteral("Shared/Calibre"), 40);
    makeMarker(marker);
    ConcurrencyGuard::Environment shared = environment();
    shared.fileSystemType = type;
    auto checks = guard(shared);
    QVERIFY(checks->shared());
    QCOMPARE(checks->state(), QStringLiteral("recent"));

    // Heartbeats only go where a sync service carries them.
    checks->setLibraryUuid(QStringLiteral("library-id"));
    checks->noteWrite();
    const bool beats = QDirIterator(m_root->path(), { QStringLiteral(".cousoir") }, QDir::Dirs | QDir::Hidden,
                                    QDirIterator::Subdirectories).hasNext();
    QCOMPARE(beats, !marker.isEmpty());
}

void ConcurrencyGuardTest::followsSharingChoice()
{
    makeLibrary(QStringLiteral("Books/Calibre"), 40);
    const QString key = QStringLiteral("Library/sharing");
    {
        auto checks = guard();
        QCOMPARE(checks->state(), QStringLiteral("writable"));
        QSignalSpy changed(checks.get(), &ConcurrencyGuard::sharingChanged);
        checks->setSharing(QStringLiteral("shared"));
        QCOMPARE(changed.size(), 1);
        QVERIFY(checks->shared());
        QCOMPARE(checks->state(), QStringLiteral("recent"));
        QCOMPARE(QSettings().value(key).toStringList(),
                 QStringList{ QStringLiteral("shared|") + QFileInfo(m_library).canonicalFilePath() });
        checks->setSharing(QStringLiteral("sometimes"));
        QCOMPARE(checks->sharing(), QStringLiteral("shared"));
    }

    // Kept for the library.
    auto again = guard();
    QCOMPARE(again->sharing(), QStringLiteral("shared"));
    QCOMPARE(again->state(), QStringLiteral("recent"));
    again->setSharing(QStringLiteral("local"));
    QVERIFY(!again->shared());
    QCOMPARE(again->state(), QStringLiteral("writable"));
    again->setSharing(QString());
    QVERIFY(QSettings().value(key).toStringList().isEmpty());
}

void ConcurrencyGuardTest::onlyThisComputerInDropbox()
{
    // A Dropbox folder no Dropbox answers for any more.
    age(QDir(m_library).filePath(QStringLiteral("metadata.db")), 40);
    auto checks = guard();
    QCOMPARE(checks->syncService(), QStringLiteral("dropbox"));
    QTRY_COMPARE(checks->dropboxStatus(), QStringLiteral("unavailable"));
    QCOMPARE(checks->state(), QStringLiteral("dropbox"));

    checks->setSharing(QStringLiteral("local"));
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    checks->setLibraryUuid(QStringLiteral("library-id"));
    checks->noteWrite();
    QVERIFY(!QFileInfo::exists(m_root->filePath(QStringLiteral("Dropbox/.cousoir"))));

    // A sync under way still waits.
    FakeDropbox dropbox(environment().dropboxSocket);
    dropbox.status = "syncing";
    checks->refresh();
    QTRY_COMPARE(checks->state(), QStringLiteral("dropbox"));
}

void ConcurrencyGuardTest::readOnlyUntilConflictSeen()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    const QString copy = QStringLiteral("metadata (conflicted copy 2024-11-14).db");
    QVERIFY(writeFile(QDir(m_library).filePath(copy), "old"));
    // A conflict copy of another file is listed but does not block.
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("notes (conflicted copy).txt")), "x"));
    auto checks = guard();
    QTRY_COMPARE(checks->state(), QStringLiteral("conflict"));
    QCOMPARE(checks->readOnlyReason(), QStringLiteral("Read-only: conflict copy of the database"));
    QCOMPARE(checks->conflicts().size(), 2);

    checks->acknowledgeConflict(copy);
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    QVERIFY(checks->conflicts().first().toMap().value(QStringLiteral("acknowledged")).toBool());

    // The same name with other contents is a new conflict.
    QVERIFY(writeFile(QDir(m_library).filePath(copy), "newer"));
    checks->refresh();
    QCOMPARE(checks->state(), QStringLiteral("conflict"));
}

void ConcurrencyGuardTest::ownChangeIsNoReasonToWait()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    const QString database = QDir(m_library).filePath(QStringLiteral("metadata.db"));
    age(database, 10);
    auto checks = guard();
    QTRY_COMPARE(checks->state(), QStringLiteral("recent"));
    // The file Cousoir wrote itself.
    checks->setOwnChangeKey(ConcurrencyGuard::changeKey(database));
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    // Dropbox sending it is no reason either.
    dropbox.status = "syncing";
    checks->refresh();
    QTRY_COMPARE(checks->dropboxStatus(), QStringLiteral("syncing"));
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    // Another change after it counts again.
    QVERIFY(writeFile(database, "changed elsewhere"));
    checks->refresh();
    QCOMPARE(checks->state(), QStringLiteral("dropbox"));
    dropbox.status = "up to date";
    checks->refresh();
    QTRY_COMPARE(checks->state(), QStringLiteral("recent"));
}

void ConcurrencyGuardTest::readOnlyWhileEditedElsewhere_data()
{
    QTest::addColumn<QString>("library");
    QTest::addColumn<QString>("marker");
    QTest::addColumn<QString>("root");
    QTest::newRow("dropbox") << QStringLiteral("Dropbox/Calibre") << QString() << QStringLiteral("Dropbox");
    QTest::newRow("syncthing") << QStringLiteral("Sync/Calibre") << QStringLiteral("Sync/.stfolder/")
                               << QStringLiteral("Sync");
    QTest::newRow("nextcloud") << QStringLiteral("Nextcloud/Calibre")
                               << QStringLiteral("Nextcloud/.sync_0a1b2c3d4e5f.db") << QStringLiteral("Nextcloud");
}

void ConcurrencyGuardTest::readOnlyWhileEditedElsewhere()
{
    QFETCH(QString, library);
    QFETCH(QString, marker);
    QFETCH(QString, root);
    makeLibrary(library, 600);
    makeMarker(marker);
    FakeDropbox dropbox(environment().dropboxSocket);
    ConcurrencyGuard::Environment here = environment();
    here.host = QStringLiteral("laptop");
    ConcurrencyGuard::Environment there = environment();
    there.host = QStringLiteral("desktop");
    ConcurrencyGuard laptop(here);
    ConcurrencyGuard desktop(there);
    for (ConcurrencyGuard *checks : { &laptop, &desktop }) {
        checks->setLibraryReadable(true);
        checks->setSchemaKnown(true);
        checks->setLibraryUuid(QStringLiteral("library-id"));
        checks->setLibraryPath(m_library);
    }
    QTRY_COMPARE(laptop.state(), QStringLiteral("writable"));
    QCOMPARE(laptop.syncRoot(), QFileInfo(m_root->filePath(root)).canonicalFilePath());

    // The desktop's heartbeat after a change keeps the laptop read-only.
    desktop.noteWrite();
    const QString folder = ConcurrencyGuard::heartbeatFolder(laptop.syncRoot(), QStringLiteral("library-id"));
    QVERIFY(QFileInfo::exists(QDir(folder).filePath(QStringLiteral("desktop.json"))));
    laptop.refresh();
    QCOMPARE(laptop.state(), QStringLiteral("elsewhere"));
    QCOMPARE(laptop.readOnlyReason(), QStringLiteral("Read-only: Cousoir is editing on desktop"));
    // Not the desktop itself.
    desktop.refresh();
    QTRY_COMPARE(desktop.state(), QStringLiteral("writable"));

    // An old heartbeat does not count.
    QVERIFY(writeFile(QDir(folder).filePath(QStringLiteral("desktop.json")),
                      R"({"host":"desktop","updated":"2020-01-01T00:00:00Z","lastWrite":"2020-01-01T00:00:00Z"})"));
    laptop.refresh();
    QCOMPARE(laptop.state(), QStringLiteral("writable"));
}

QTEST_GUILESS_MAIN(ConcurrencyGuardTest)
#include "concurrency_guard_test.moc"
