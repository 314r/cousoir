#include <QtTest>

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
    void writableWhenEverythingIsQuiet();
    void readOnlyUntilLoadedAndKnown();
    void readOnlyWhileCalibreRuns();
    void readOnlyWithJournal();
    void readOnlyWhileDropboxSyncs();
    void readOnlyWithoutDropbox();
    void readOnlyAfterRecentChange();
    void readOnlyUntilConflictSeen();

private:
    ConcurrencyGuard::Environment environment() const;
    std::unique_ptr<ConcurrencyGuard> guard(bool loaded = true);

    std::unique_ptr<QTemporaryDir> m_root;
    QString m_library;
};

void ConcurrencyGuardTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("reliure-tests"));
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
                              "notes (Case Conflict).txt", "metadata.db", "metadata_db_prefs_backup.json" }) {
        QVERIFY(writeFile(QDir(m_library).filePath(QString::fromUtf8(name)), "x"));
    }
    QCOMPARE(ConcurrencyGuard::conflictCopies(m_library),
             (QStringList{ QStringLiteral("metadata (Copie en conflit de Pierre 2024-11-14).db"),
                           QStringLiteral("metadata (conflicted copy 2024-11-14).db"),
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

void ConcurrencyGuardTest::readOnlyUntilConflictSeen()
{
    FakeDropbox dropbox(environment().dropboxSocket);
    const QString copy = QStringLiteral("metadata (conflicted copy 2024-11-14).db");
    QVERIFY(writeFile(QDir(m_library).filePath(copy), "old"));
    // A conflict copy of another file is listed but does not block.
    QVERIFY(writeFile(QDir(m_library).filePath(QStringLiteral("notes (conflicted copy).txt")), "x"));
    auto checks = guard();
    QTRY_COMPARE(checks->state(), QStringLiteral("conflict"));
    QCOMPARE(checks->conflicts().size(), 2);

    checks->acknowledgeConflict(copy);
    QCOMPARE(checks->state(), QStringLiteral("writable"));
    QVERIFY(checks->conflicts().first().toMap().value(QStringLiteral("acknowledged")).toBool());

    // The same name with other contents is a new conflict.
    QVERIFY(writeFile(QDir(m_library).filePath(copy), "newer"));
    checks->refresh();
    QCOMPARE(checks->state(), QStringLiteral("conflict"));
}

QTEST_GUILESS_MAIN(ConcurrencyGuardTest)
#include "concurrency_guard_test.moc"
