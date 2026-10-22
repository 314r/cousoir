#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include "python_json.h"

class QFile;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

// Calibre's wireless device connection, its server's side, as Calibre 9.16's
// SMART_DEVICE_APP driver speaks it (devices/smart_device_app/driver.py):
// reader apps, KOReader, Calibre Companion and others, find the server by a
// UDP datagram to one of five ports, connect over TCP, and from then on
// answer what the server asks, each message its length in digits followed by
// a JSON array, [opcode, {arguments}]; a book's bytes follow the request
// that sends it. The server lists the app's books, keeping their metadata
// so as to ask only for the books it does not know, sends books with their
// metadata and cover, removes them, sends metadata changed since, and lets
// the app go.
namespace Wireless {

enum Opcode {
    Ok = 0,
    SetCalibreDeviceInfo = 1,
    SetCalibreDeviceName = 2,
    GetDeviceInformation = 3,
    TotalSpace = 4,
    FreeSpace = 5,
    GetBookCount = 6,
    SendBooklists = 7,
    SendBook = 8,
    GetInitializationInfo = 9,
    BookDone = 11,
    Noop = 12,
    DeleteBook = 13,
    GetBookFileSegment = 14,
    GetBookMetadata = 15,
    SendBookMetadata = 16,
    DisplayMessage = 17,
    CalibreBusy = 18,
    SetLibraryInfo = 19,
    Error = 20,
    GetCollections = 21,
    UpdateCollections = 22,
};

// The port Calibre listens on first, and those it waits for datagrams on.
constexpr quint16 defaultPort = 9090;
const QList<quint16> &broadcastPorts();

// What Calibre tells every app alike (assets/wireless/calibre.json, from
// tools/oracle/wireless_oracle.py).
struct Tables
{
    QList<int> calibreVersion;
    QStringList validExtensions;
    QHash<QString, QString> mimeTypes;
    QString pubdateFormat;
    QString timestampFormat;
    QString lastModifiedFormat;
    // The library's standard fields, their names in English.
    PythonJson::Value fieldMetadata;
};
const Tables &tables();

// A message as sent: its length, then [opcode, argument] as Python's
// json.dumps() writes it, in ASCII.
QByteArray frame(int opcode, const PythonJson::Value &argument);

// Takes the first message off `buffer`: 1 when one was taken, 0 when more
// bytes are needed, -1 when the bytes are no message.
int takeMessage(QByteArray *buffer, int *opcode, PythonJson::Value *argument);

// The answer to a datagram: the server's name for this computer, the port
// of its content server and its own.
QByteArray broadcastReply(const QString &hostName, int contentServerPort, int port);

// The SHA-1 the app answers a password's challenge with.
QString passwordHash(const QString &password, const QString &challenge);

// What an app said of itself on connecting.
struct App
{
    QString appName;
    QString deviceKind;
    QString deviceName;
    QString version;
    // The extensions it takes, in the order it prefers them.
    QStringList acceptedExtensions;
    // How many characters a path leaves for each extension.
    QHash<QString, int> extensionPathLengths;
    bool cacheUsesLpaths = false;
    bool canUseCachedMetadata = false;
    bool canSendOkToSendbook = false;
    bool canAcceptLibraryInfo = false;
    bool useUuidFileNames = false;
    int coverHeight = 160;
    // Set when the app wants covers of exactly this width.
    std::optional<int> coverWidth;
    qint64 maxBookContentPacketLen = 4096;
};

// A book to send: where it goes on the app, its metadata as the app keeps
// it (a book's entry in metadata.calibre), and the file sent.
struct Outgoing
{
    QString lpath;
    PythonJson::Value metadata;
    QString file;
};

} // namespace Wireless

// An app connected to Cousoir, from its handshake to its leaving. What it is
// asked goes one request at a time, in the order asked; between them a NOOP
// every few seconds tells it Cousoir is still there.
class WirelessConnection : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        QString password;
        QString libraryName;
        QString libraryUuid;
        // Calibre's, for the names of the library's fields.
        QString interfaceLanguage;
        // Where the books' metadata the apps hold is kept, by app.
        QString cacheFolder;
        // Seconds without an answer before the app counts as gone.
        int timeoutSeconds = 60;
        int noopIntervalMs = 5000;
    };

    WirelessConnection(QTcpSocket *socket, const Settings &settings, QObject *parent = nullptr);
    ~WirelessConnection() override;

    bool isOpen() const;
    bool isBusy() const;
    // get_gui_name(): "Wireless device: KOReader (Kobo_Libra2)".
    QString name() const;
    const Wireless::App &app() const;
    // The app's driveinfo, as sent with SET_CALIBRE_DEVICE_INFO.
    const PythonJson::Value &driveInfo() const;
    QString deviceUuid() const;
    qint64 freeSpace() const;
    // The books on the app: the metadata it keeps for each.
    const QList<PythonJson::Value> &books() const;
    // The metadata sent or received of the book at `lpath`, if known.
    std::optional<PythonJson::Value> knownBook(const QString &lpath) const;
    // A book already on the app with this uuid and extension, if it is
    // known: a book sent again replaces it.
    QString lpathOfUuid(const QString &uuid, const QString &extension) const;

    // Each waits for those asked before; `done`, when given, is told how it
    // ended, as finished() is.
    using Done = std::function<void(bool ok, const QString &error)>;
    void send(const QList<Wireless::Outgoing> &books, Done done = {});
    void remove(const QStringList &lpaths, Done done = {});
    // sync_booklists(): the metadata given, of books on the app, sent
    // where the app does not have it already, then the room left.
    void updateMetadata(const QList<PythonJson::Value> &books, Done done = {});
    // The app told to disconnect, then the connection closed.
    void eject();
    // The connection closed at once.
    void close(const QString &reason = {});

signals:
    // The app said who it is, and its books were listed.
    void opened();
    void booksChanged();
    void progress(double fraction);
    // A send, removal or update came to its end.
    void finished(bool ok, const QString &error);
    void closed(const QString &reason);

private:
    struct Message
    {
        int opcode = -1;
        PythonJson::Value argument;
    };
    using Replies = std::function<void(const QList<Message> &)>;

    // A task, and whom to tell should the connection close before it ends.
    void enqueue(std::function<void()> task, Done done = {});
    void next();
    // Sends a request; `done` gets the `replies` messages that answer it,
    // none for 0.
    void request(int opcode, const PythonJson::Value &argument, int replies, Replies done);
    void post(int opcode, const PythonJson::Value &argument);
    void readMore();
    void feed();
    void fail(const QString &reason);
    void sendNoop();

    void handshake();
    void listBooks();
    void receiveBooks(int count, std::function<void()> then);
    void sendBook(const std::shared_ptr<QList<Wireless::Outgoing>> &books, int index, qint64 total);
    void streamFile(const QString &path, qint64 length, std::function<void(bool)> done);
    void writeChunk();
    void syncBooklists(const QList<PythonJson::Value> &books, std::function<void()> then);
    void finish(bool ok, const QString &error);

    // The metadata cache, Calibre's device_book_cache: by uuid and lpath,
    // or uuid and extension when the app keys its own so.
    QString cacheKey(const PythonJson::Value &book) const;
    void remember(const PythonJson::Value &book);
    void forget(const QString &lpath);
    QString cacheFile() const;
    void readCache();
    void writeCache();
    void setBook(const PythonJson::Value &book);

    QTcpSocket *m_socket;
    Settings m_settings;
    QByteArray m_buffer;
    std::deque<std::pair<std::function<void()>, Done>> m_queue;
    bool m_running = false;
    int m_expected = 0;
    QList<Message> m_replies;
    Replies m_done;
    QTimer m_timeout;
    QTimer m_noop;
    bool m_open = false;
    bool m_closed = false;
    // A send, removal or update under way, and whom to tell its end.
    bool m_operation = false;
    Done m_operationDone;
    bool m_ejecting = false;
    Wireless::App m_app;
    PythonJson::Value m_driveInfo;
    qint64 m_freeSpace = 0;
    QList<PythonJson::Value> m_books;
    // known_metadata: what the app has of each book, by lpath.
    QHash<QString, PythonJson::Value> m_known;
    // device_book_cache, by cacheKey().
    QHash<QString, PythonJson::Value> m_cache;
    QHash<QString, QDateTime> m_cacheUsed;
    // The file being sent and how much of it is left to write.
    std::unique_ptr<QFile> m_file;
    qint64 m_fileLeft = 0;
    std::function<void(bool)> m_fileDone;
};

// The server: Cousoir listening for apps, on Calibre's port or another, and
// answering their datagrams; one app at a time, others told it is busy.
class WirelessServer : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        // The port to listen on; 0 for 9090, or another free one as
        // Calibre picks it.
        quint16 port = 0;
        // Whether to answer the apps' datagrams, and on which ports: the
        // first free of Calibre's when none are given.
        bool discovery = true;
        QList<quint16> broadcastPorts;
        // The port of the library's own server, which the answer names.
        int contentServerPort = 8080;
        WirelessConnection::Settings connection;
    };

    explicit WirelessServer(QObject *parent = nullptr);
    ~WirelessServer() override;

    bool start(const Settings &settings, QString *error);
    void stop();
    bool isListening() const;
    quint16 port() const;
    // The port datagrams are answered on, 0 when none could be had.
    quint16 broadcastPort() const;
    WirelessConnection *connection() const;
    // For a change of library: the apps that connect from now on.
    void setLibrary(const WirelessConnection::Settings &settings);

signals:
    // An app connected and was listed.
    void connected(WirelessConnection *connection);
    void disconnected(const QString &reason);

private:
    void accept();
    void answerDatagrams();

    Settings m_settings;
    QTcpServer *m_server = nullptr;
    QUdpSocket *m_udp = nullptr;
    QPointer<WirelessConnection> m_connection;
};
