#include <QtTest>

#include <QCryptographicHash>
#include <QNetworkDatagram>
#include <QTemporaryDir>
#include <QUdpSocket>

#include <memory>

#include "wireless.h"

// Calibre's wireless device connection, Cousoir's server side, against an
// app that answers as KOReader's Calibre plugin does
// (plugins/calibre.koplugin/wireless.lua): the handshake, the books listed
// with the metadata cache, books sent and removed, metadata sent, a book
// refused, the password, a second app turned away, and the datagrams that
// find the server. tests/golden/run.sh compares the same with Calibre.

#include "fake_wireless_app.h"

using PythonJson::Value;
using FakeWireless::FakeApp;
using FakeWireless::dict;

namespace {

Value bookMetadata(const QString &title, const QString &uuid, const QString &modified)
{
    return dict({ { QStringLiteral("title"), Value::from(title) },
                  { QStringLiteral("authors"), Value::from(QStringList{ QStringLiteral("Boris Vian") }) },
                  { QStringLiteral("uuid"), Value::from(uuid) },
                  { QStringLiteral("last_modified"), Value::from(modified) },
                  { QStringLiteral("thumbnail"), Value::null() },
                  { QStringLiteral("_series_sort_"), Value::from(QString()) } });
}

} // namespace

class WirelessTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void framesAsCalibre();
    void answersDatagrams();
    void sendsListsAndRemoves();
    void asksOnlyForBooksItDoesNotKnow();
    void sendsChangedMetadata();
    void tellsWhyABookWasRefused();
    void tellsWhatWaitedWhenTheAppGoes();
    void namesTheCacheSafely();
    void checksThePassword();
    void turnsASecondAppAway();

private:
    // Starts the server and connects `app`, until the server listed it.
    WirelessConnection *connect(FakeApp *app, const QString &password = {});
    QString writeBook(const QString &name, const QByteArray &data);

    std::unique_ptr<QTemporaryDir> m_folder;
    std::unique_ptr<WirelessServer> m_server;
};

void WirelessTest::init()
{
    m_folder = std::make_unique<QTemporaryDir>();
    m_server = std::make_unique<WirelessServer>();
}

WirelessConnection *WirelessTest::connect(FakeApp *app, const QString &password)
{
    if (!m_server->isListening()) {
        WirelessServer::Settings settings;
        settings.port = 0;
        settings.discovery = false;
        settings.connection.password = password;
        settings.connection.libraryName = QStringLiteral("Library");
        settings.connection.libraryUuid = QStringLiteral("25ea370f-2ec1-46fe-9add-33ad18e029d3");
        settings.connection.cacheFolder = m_folder->filePath(QStringLiteral("cache"));
        settings.connection.noopIntervalMs = 200;
        QString error;
        if (!m_server->start(settings, &error))
            qWarning() << error;
    }
    WirelessConnection *opened = nullptr;
    const QMetaObject::Connection link = QObject::connect(m_server.get(), &WirelessServer::connected,
                                                          [&opened](WirelessConnection *connection) { opened = connection; });
    app->connectTo(m_server->port());
    (void)QTest::qWaitFor([&] { return opened || app->closed; }, 10000);
    QObject::disconnect(link);
    return opened;
}

QString WirelessTest::writeBook(const QString &name, const QByteArray &data)
{
    const QString path = m_folder->filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(data);
    return path;
}

void WirelessTest::framesAsCalibre()
{
    // _json_encode() and json.dumps(): the length, then ASCII.
    const QByteArray frame
        = Wireless::frame(Wireless::SendBookMetadata, dict({ { QStringLiteral("title"), Value::from(QStringLiteral("Écume")) } }));
    QCOMPARE(frame, QByteArrayLiteral("29[16, {\"title\": \"\\u00c9cume\"}]"));

    // Messages taken as they come, a piece at a time.
    QByteArray buffer = frame.left(10);
    int opcode = 0;
    Value argument;
    QCOMPARE(Wireless::takeMessage(&buffer, &opcode, &argument), 0);
    buffer += frame.mid(10) + Wireless::frame(Wireless::Ok, Value::dict());
    QCOMPARE(Wireless::takeMessage(&buffer, &opcode, &argument), 1);
    QCOMPARE(opcode, int(Wireless::SendBookMetadata));
    QCOMPARE(argument.text(QStringLiteral("title")), QStringLiteral("Écume"));
    QCOMPARE(Wireless::takeMessage(&buffer, &opcode, &argument), 1);
    QCOMPARE(opcode, int(Wireless::Ok));
    QVERIFY(buffer.isEmpty());
    buffer = QByteArrayLiteral("x[0, {}]");
    QCOMPARE(Wireless::takeMessage(&buffer, &opcode, &argument), -1);

    // ConnectionListener's answer, KOReader's challenge.
    QCOMPARE(Wireless::broadcastReply(QStringLiteral("desk.local"), 8090, 9090),
             QByteArrayLiteral("calibre wireless device client (on desk);8090,9090"));
    QCOMPARE(Wireless::passwordHash(QStringLiteral("secret"), QStringLiteral("2026-10-22T20:00:23.261702+00:00")),
             QString::fromLatin1(QCryptographicHash::hash("secret2026-10-22T20:00:23.261702+00:00",
                                                          QCryptographicHash::Sha1).toHex()));
    QVERIFY(Wireless::tables().validExtensions.contains(QStringLiteral("epub")));
    QCOMPARE(Wireless::tables().mimeTypes.value(QStringLiteral("epub")), QStringLiteral("application/epub+zip"));
}

void WirelessTest::answersDatagrams()
{
    // A port of its own, not one of Calibre's, which Calibre may have.
    QUdpSocket probe;
    QVERIFY(probe.bind(QHostAddress::LocalHost, 0));
    QUdpSocket spare;
    QVERIFY(spare.bind(QHostAddress::LocalHost, 0));
    const quint16 port = spare.localPort();
    spare.close();
    WirelessServer::Settings settings;
    settings.broadcastPorts = { port };
    settings.contentServerPort = 8090;
    QString error;
    QVERIFY2(m_server->start(settings, &error), qPrintable(error));
    QCOMPARE(m_server->broadcastPort(), port);
    probe.writeDatagram("hello", QHostAddress::LocalHost, port);
    QTRY_VERIFY(probe.hasPendingDatagrams());
    const QByteArray answer = probe.receiveDatagram().data();
    QVERIFY2(answer.startsWith("calibre wireless device client (on "), answer.constData());
    QVERIFY2(answer.endsWith(QStringLiteral(";8090,%1").arg(m_server->port()).toUtf8()), answer.constData());
}

void WirelessTest::sendsListsAndRemoves()
{
    FakeApp app;
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    QCOMPARE(connection->name(), QStringLiteral("KOReader (Kobo_Libra2)"));
    QCOMPARE(connection->app().acceptedExtensions, (QStringList{ "epub", "mobi", "pdf" }));
    QCOMPARE(connection->app().coverHeight, 240);
    QVERIFY(!connection->deviceUuid().isEmpty());
    QCOMPARE(app.deviceUuid, connection->deviceUuid());
    QCOMPARE(app.last(Wireless::SetLibraryInfo)->text(QStringLiteral("libraryName")), QStringLiteral("Library"));
    QVERIFY(connection->books().isEmpty());

    // Two books, the second larger than a packet, each after its metadata.
    QByteArray large(100000, 'x');
    for (qsizetype i = 0; i < large.size(); ++i)
        large[i] = char(i % 251);
    QSignalSpy finished(connection, &WirelessConnection::finished);
    connection->send({ { QStringLiteral("Vian, Boris/Ecume.epub"), bookMetadata(QStringLiteral("L'Écume des jours"), QStringLiteral("u1"), QStringLiteral("2026-10-19T08:38:30+00:00")),
                         writeBook(QStringLiteral("a.epub"), "small") },
                       { QStringLiteral("Big.pdf"), bookMetadata(QStringLiteral("Big"), QStringLiteral("u2"), QStringLiteral("2026-10-19T08:38:31+00:00")),
                         writeBook(QStringLiteral("b.pdf"), large) } });
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(finished.at(0).at(0).toBool());
    QCOMPARE(app.books.size(), 2);
    QCOMPARE(app.books.at(0).data, QByteArrayLiteral("small"));
    QCOMPARE(app.books.at(1).data, large);
    QCOMPARE(app.books.at(1).metadata.find(QStringLiteral("size"))->integer, large.size());
    QCOMPARE(connection->books().size(), 2);
    // Then, as Calibre after a send: the booklists, and the room left.
    QCOMPARE(app.received.at(app.received.size() - 2).first, int(Wireless::SendBooklists));
    QCOMPARE(app.received.constLast().first, int(Wireless::FreeSpace));

    // Removed, each answered.
    connection->remove({ QStringLiteral("Big.pdf") });
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY(finished.at(1).at(0).toBool());
    QCOMPARE(app.books.size(), 1);
    QCOMPARE(connection->books().size(), 1);

    // Idle, it is asked now and then whether it is there; let go, it goes.
    QTRY_VERIFY(app.count(Wireless::Noop) > 1);
    QSignalSpy closed(connection, &WirelessConnection::closed);
    connection->eject();
    QTRY_COMPARE(closed.size(), 1);
    QVERIFY(PythonJson::truthy(*app.last(Wireless::Noop)->find(QStringLiteral("ejecting"))));
}

void WirelessTest::asksOnlyForBooksItDoesNotKnow()
{
    FakeApp app;
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    QSignalSpy finished(connection, &WirelessConnection::finished);
    connection->send({ { QStringLiteral("a.epub"), bookMetadata(QStringLiteral("A"), QStringLiteral("u1"), QStringLiteral("2026-10-19T08:38:30+00:00")),
                         writeBook(QStringLiteral("a.epub"), "a") } });
    QTRY_COMPARE(finished.size(), 1);
    QSignalSpy closed(connection, &WirelessConnection::closed);
    connection->eject();
    QTRY_COMPARE(closed.size(), 1);

    // Back with that book and one from elsewhere: only the latter is asked.
    FakeApp again;
    again.deviceUuid = app.deviceUuid;
    again.books = app.books;
    again.books.append({ bookMetadata(QStringLiteral("B"), QStringLiteral("u9"), QStringLiteral("2026-01-01T00:00:00+00:00")), "b" });
    again.books.last().metadata.insert(QStringLiteral("lpath"), Value::from(QStringLiteral("b.epub")));
    QTRY_VERIFY(!m_server->connection());
    connection = connect(&again);
    QVERIFY(connection);
    QCOMPARE(again.count(Wireless::Noop), 2);
    QCOMPARE(again.received.at(again.received.size() - 2).second.find(QStringLiteral("count"))->integer, 1);
    QCOMPARE(again.received.constLast().second.find(QStringLiteral("priKey"))->integer, 2);
    QCOMPARE(connection->books().size(), 2);
    QVERIFY(connection->knownBook(QStringLiteral("a.epub")));
    QCOMPARE(connection->lpathOfUuid(QStringLiteral("u1"), QStringLiteral(".epub")), QStringLiteral("a.epub"));
}

void WirelessTest::sendsChangedMetadata()
{
    FakeApp app;
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    QSignalSpy finished(connection, &WirelessConnection::finished);
    const Value sent = bookMetadata(QStringLiteral("A"), QStringLiteral("u1"), QStringLiteral("2026-10-19T08:38:30+00:00"));
    connection->send({ { QStringLiteral("a.epub"), sent, writeBook(QStringLiteral("a.epub"), "a") } });
    QTRY_COMPARE(finished.size(), 1);

    // What the app has already does not go again; a change does.
    Value changed = sent;
    changed.insert(QStringLiteral("lpath"), Value::from(QStringLiteral("a.epub")));
    connection->updateMetadata({ changed });
    QTRY_COMPARE(finished.size(), 2);
    QCOMPARE(app.count(Wireless::SendBookMetadata), 0);
    changed.insert(QStringLiteral("title"), Value::from(QStringLiteral("A, again")));
    changed.insert(QStringLiteral("last_modified"), Value::from(QStringLiteral("2026-10-20T08:00:00+00:00")));
    connection->updateMetadata({ changed });
    QTRY_COMPARE(finished.size(), 3);
    QCOMPARE(app.count(Wireless::SendBookMetadata), 1);
    QCOMPARE(app.books.constFirst().metadata.text(QStringLiteral("title")), QStringLiteral("A, again"));
}

void WirelessTest::tellsWhyABookWasRefused()
{
    FakeApp app;
    app.refuse = QStringLiteral("No space left on device");
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    QSignalSpy finished(connection, &WirelessConnection::finished);
    connection->send({ { QStringLiteral("a.epub"), bookMetadata(QStringLiteral("A"), QStringLiteral("u1"), QStringLiteral("2026-10-19T08:38:30+00:00")),
                         writeBook(QStringLiteral("a.epub"), "a") } });
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.at(0).at(0).toBool());
    QVERIFY(finished.at(0).at(1).toString().contains(QStringLiteral("No space left on device")));
    QVERIFY(connection->isOpen());
}

void WirelessTest::tellsWhatWaitedWhenTheAppGoes()
{
    FakeApp app;
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    // The app stops answering during a send, another one waiting.
    app.silent = true;
    QList<bool> ended;
    const auto book = [this](const QString &name) {
        return Wireless::Outgoing{ name, bookMetadata(name, QStringLiteral("u-") + name, QStringLiteral("2026-10-19T08:38:30+00:00")),
                                   writeBook(name, "x") };
    };
    connection->send({ book(QStringLiteral("a.epub")) }, [&ended](bool ok, const QString &) { ended.append(ok); });
    connection->send({ book(QStringLiteral("b.epub")) }, [&ended](bool ok, const QString &) { ended.append(ok); });
    QTRY_VERIFY(app.count(Wireless::FreeSpace) >= 2);
    app.disconnectNow();
    QTRY_COMPARE(ended, (QList<bool>{ false, false }));
    // Asked once it is gone, at once.
    bool told = false;
    connection->remove({ QStringLiteral("a.epub") }, [&told](bool ok, const QString &) { told = !ok; });
    QVERIFY(told);
}

void WirelessTest::namesTheCacheSafely()
{
    // A uuid the app makes up names no file outside the cache.
    FakeApp app;
    app.deviceUuid = QStringLiteral("../../escaped");
    WirelessConnection *connection = connect(&app);
    QVERIFY(connection);
    QSignalSpy closed(connection, &WirelessConnection::closed);
    connection->eject();
    QTRY_COMPARE(closed.size(), 1);
    QVERIFY(!QFileInfo::exists(m_folder->filePath(QStringLiteral("escaped.json"))));
    QVERIFY(!QFileInfo::exists(QDir(m_folder->path()).filePath(QStringLiteral("../escaped.json"))));
    const QStringList files = QDir(m_folder->filePath(QStringLiteral("cache"))).entryList(QDir::Files);
    QCOMPARE(files.size(), 1);
    QVERIFY(files.constFirst().startsWith(QLatin1String("wireless_device_")));
}

void WirelessTest::checksThePassword()
{
    FakeApp wrong;
    wrong.password = QStringLiteral("guess");
    QVERIFY(!connect(&wrong, QStringLiteral("secret")));
    QTRY_VERIFY(wrong.closed);
    QCOMPARE(wrong.last(Wireless::DisplayMessage)->find(QStringLiteral("messageKind"))->integer, 1);

    FakeApp right;
    right.password = QStringLiteral("secret");
    QTRY_VERIFY(!m_server->connection());
    QVERIFY(connect(&right));
}

void WirelessTest::turnsASecondAppAway()
{
    FakeApp first;
    QVERIFY(connect(&first));
    FakeApp second;
    second.connectTo(m_server->port());
    QTRY_VERIFY(second.count(Wireless::CalibreBusy) == 1);
    QCOMPARE(second.last(Wireless::CalibreBusy)->text(QStringLiteral("otherDevice")), QStringLiteral("KOReader (Kobo_Libra2)"));
    QTRY_VERIFY(second.closed);
    QVERIFY(m_server->connection()->isOpen());
}

QTEST_GUILESS_MAIN(WirelessTest)
#include "wireless_test.moc"
