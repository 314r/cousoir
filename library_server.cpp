#include "library_server.h"

// Calibre's content server names a downloaded book as
// src/calibre/srv/code.py book_filename() does: the title and authors cut
// to 30 characters, the book's id, in ASCII.

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QNetworkInterface>
#include <QPointer>
#include <QRunnable>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include "book.h"
#include "calibre_text.h"
#include "opds.h"

namespace {

// The most a request's line and headers may take.
constexpr qsizetype maximumHeader = 16 * 1024;
// The time a client has to send its request.
constexpr int requestTimeout = 30000;
// The connections served at once; others are closed unanswered.
constexpr int maximumConnections = 64;
// A file is sent this much at a time, while less than twice that waits.
constexpr qint64 chunk = 256 * 1024;
// The height of a thumbnail, as the web page's covers show it.
constexpr int thumbnailHeight = 300;

const char *const headProperty = "cousoirHead";
const char *const requestProperty = "cousoirRequest";

QByteArray statusText(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
}

// A query string's values, as a form sends them: "+" for a space.
QHash<QString, QString> queryItems(const QByteArray &query)
{
    QHash<QString, QString> items;
    for (const QByteArray &pair : query.split('&')) {
        if (pair.isEmpty())
            continue;
        const qsizetype equal = pair.indexOf('=');
        QByteArray key = equal < 0 ? pair : pair.left(equal);
        QByteArray value = equal < 0 ? QByteArray() : pair.mid(equal + 1);
        key.replace('+', ' ');
        value.replace('+', ' ');
        items.insert(QUrl::fromPercentEncoding(key), QUrl::fromPercentEncoding(value));
    }
    return items;
}

QByteArray encode(const QString &value)
{
    return QUrl::toPercentEncoding(value);
}

QList<BookSort::Level> sortLevels(const QString &sort)
{
    if (sort == QLatin1String("timestamp"))
        return { { QStringLiteral("timestamp"), false } };
    if (sort == QLatin1String("authors"))
        return { { QStringLiteral("authors"), true }, { QStringLiteral("title"), true } };
    if (sort == QLatin1String("series"))
        return { { QStringLiteral("series"), true }, { QStringLiteral("title"), true } };
    return { { QStringLiteral("title"), true } };
}

// The book's id from a path's segment, 0 for none.
int bookId(const QString &segment)
{
    bool ok = false;
    const int id = segment.toInt(&ok);
    return ok && id > 0 ? id : 0;
}

class ThumbnailJob : public QRunnable
{
public:
    ThumbnailJob(QString cover, QString cached, std::function<void(QByteArray)> done)
        : m_cover(std::move(cover)), m_cached(std::move(cached)), m_done(std::move(done))
    {
    }

    void run() override
    {
        QByteArray bytes;
        QFile cached(m_cached);
        if (!m_cached.isEmpty() && cached.open(QIODevice::ReadOnly))
            bytes = cached.readAll();
        if (bytes.isEmpty()) {
            QImageReader reader(m_cover);
            reader.setAutoTransform(true);
            QSize size = reader.size();
            if (size.isValid() && size.height() > thumbnailHeight) {
                size.scale(INT_MAX, thumbnailHeight, Qt::KeepAspectRatio);
                reader.setScaledSize(size);
            }
            QImage image;
            if (reader.read(&image)) {
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 85);
                if (!m_cached.isEmpty()) {
                    QDir().mkpath(QFileInfo(m_cached).absolutePath());
                    QSaveFile out(m_cached);
                    if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size())
                        out.commit();
                }
            }
        }
        m_done(bytes);
    }

private:
    QString m_cover;
    QString m_cached;
    std::function<void(QByteArray)> m_done;
};

} // namespace

LibraryServer::LibraryServer(QObject *parent) : QObject(parent) {}

LibraryServer::~LibraryServer() = default;

void LibraryServer::setCatalog(const Catalog &catalog)
{
    m_catalog = catalog;
}

void LibraryServer::setPassword(const QString &password)
{
    m_password = password;
}

QString LibraryServer::listen(quint16 port, const QHostAddress &address)
{
    close();
    m_server = new QTcpServer(this);
    m_server->setMaxPendingConnections(maximumConnections);
    connect(m_server, &QTcpServer::newConnection, this, &LibraryServer::accept);
    if (!m_server->listen(address, port)) {
        const QString error = m_server->serverError() == QAbstractSocket::AddressInUseError
            ? tr("The port %1 is taken by another program.").arg(port)
            : m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return error;
    }
    return {};
}

void LibraryServer::close()
{
    if (!m_server)
        return;
    m_server->close();
    // The answers on their way end with the server.
    delete m_server;
    m_server = nullptr;
}

bool LibraryServer::isListening() const
{
    return m_server && m_server->isListening();
}

quint16 LibraryServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

QList<qsizetype> LibraryServer::findBooks(const LibraryData &data, const CalibreSearch::Index &index,
                                          const BookSort::Sorter *sorter, const QString &query,
                                          const QList<BookSort::Level> &sort, QString *error)
{
    QList<qsizetype> rows;
    rows.reserve(data.books.size());
    if (query.trimmed().isEmpty()) {
        for (qsizetype row = 0; row < data.books.size(); ++row)
            rows.append(row);
    } else {
        const CalibreSearch::Result result = CalibreSearch::search(index, query);
        if (error)
            *error = result.error;
        for (qsizetype row = 0; row < result.rows.size(); ++row) {
            if (result.rows.testBit(row))
                rows.append(row);
        }
    }
    if (sorter)
        sorter->sort(sort, &rows);
    return rows;
}

QStringList LibraryServer::addresses(quint16 port)
{
    QStringList addresses;
    for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
        if (address.isLoopback() || address.isLinkLocal() || address.protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        addresses.append(QStringLiteral("http://%1:%2/").arg(address.toString()).arg(port));
    }
    return addresses;
}

void LibraryServer::accept()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            break;
        // Sockets are the server's children, and end with it.
        if (m_server->findChildren<QTcpSocket *>(Qt::FindDirectChildrenOnly).size() > maximumConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        auto *timeout = new QTimer(socket);
        timeout->setObjectName(QStringLiteral("timeout"));
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
        timeout->start(requestTimeout);
    }
}

void LibraryServer::read(QTcpSocket *socket)
{
    // One request a connection: once it is in, the rest is left unread.
    if (socket->property("cousoirAnswered").toBool())
        return;
    QByteArray buffer = socket->property("cousoirBuffer").toByteArray() + socket->readAll();
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (buffer.size() > maximumHeader) {
            socket->setProperty("cousoirAnswered", true);
            reply(socket, 431, "text/plain", "Request too large\n");
            return;
        }
        socket->setProperty("cousoirBuffer", buffer);
        return;
    }
    socket->setProperty("cousoirAnswered", true);
    socket->setProperty("cousoirBuffer", QVariant());
    if (auto *timeout = socket->findChild<QTimer *>(QStringLiteral("timeout")))
        timeout->stop();

    const QList<QByteArray> lines = buffer.left(end).split('\n');
    const QList<QByteArray> request = lines.first().trimmed().split(' ');
    if (request.size() != 3 || !request.at(2).startsWith("HTTP/1.")) {
        reply(socket, 400, "text/plain", "Bad request\n");
        return;
    }
    QHash<QByteArray, QByteArray> headers;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const qsizetype colon = lines.at(i).indexOf(':');
        if (colon > 0)
            headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
    }
    answer(socket, request.at(0), request.at(1), headers);
}

void LibraryServer::answer(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                           const QHash<QByteArray, QByteArray> &headers)
{
    // For the log: what was asked, noted where the answer is written.
    socket->setProperty(requestProperty, QString::fromLatin1(method + ' ' + target));
    if (method != "GET" && method != "HEAD") {
        reply(socket, 405, "text/plain", "Only GET and HEAD\n", { { "Allow", "GET, HEAD" } });
        return;
    }
    socket->setProperty(headProperty, method == "HEAD");
    if (!m_password.isEmpty()) {
        const QByteArray authorization = headers.value("authorization");
        const QByteArray credentials = authorization.startsWith("Basic ")
            ? QByteArray::fromBase64(authorization.mid(6).trimmed())
            : QByteArray();
        const qsizetype colon = credentials.indexOf(':');
        if (colon < 0 || QString::fromUtf8(credentials.mid(colon + 1)) != m_password) {
            reply(socket, 401, "text/plain", "A password is needed\n",
                  { { "WWW-Authenticate", "Basic realm=\"Cousoir\", charset=\"UTF-8\"" } });
            return;
        }
    }
    if (!m_catalog.data || !m_catalog.find) {
        reply(socket, 503, "text/plain", "The library is not open\n");
        return;
    }
    const LibraryData &data = *m_catalog.data;

    const qsizetype question = target.indexOf('?');
    const QString path = QUrl::fromPercentEncoding(question < 0 ? target : target.left(question));
    const QHash<QString, QString> query = queryItems(question < 0 ? QByteArray() : target.mid(question + 1));
    const QStringList segments = path.split(u'/', Qt::SkipEmptyParts);
    const int offset = std::max(0, query.value(QStringLiteral("offset")).toInt());
    const QByteArray navigation = "application/atom+xml;profile=opds-catalog;kind=navigation;charset=utf-8";
    const QByteArray acquisition = "application/atom+xml;profile=opds-catalog;kind=acquisition;charset=utf-8";

    if (segments.isEmpty()) {
        const QString q = query.value(QStringLiteral("q"));
        QString error;
        const QList<qsizetype> rows = m_catalog.find(q, sortLevels(q.isEmpty() ? QStringLiteral("timestamp") : QStringLiteral("title")), &error);
        reply(socket, 200, "text/html;charset=utf-8",
              Opds::webPage(data, error.isEmpty() ? rows : QList<qsizetype>{}, q, offset, m_catalog.name, error));
        return;
    }
    if (segments.first() == QLatin1String("opds")) {
        const QString section = segments.value(1);
        if (segments.size() == 1) {
            reply(socket, 200, navigation, Opds::rootFeed(data, m_catalog.name));
        } else if (segments.size() == 2 && section == QLatin1String("books")) {
            const QString q = query.value(QStringLiteral("q"));
            const QString sort = query.value(QStringLiteral("sort"));
            const QString title = query.value(QStringLiteral("title"));
            QByteArray self = "/opds/books?";
            if (!q.isEmpty())
                self += "q=" + encode(q) + '&';
            if (!sort.isEmpty())
                self += "sort=" + encode(sort) + '&';
            if (!title.isEmpty())
                self += "title=" + encode(title) + '&';
            self.chop(1);
            QString error;
            const QList<qsizetype> rows = m_catalog.find(q, sortLevels(sort), &error);
            const QString feedTitle = !title.isEmpty() ? title : !q.isEmpty() ? tr("Search: %1").arg(q) : tr("Books");
            reply(socket, 200, acquisition,
                  Opds::booksFeed(data, error.isEmpty() ? rows : QList<qsizetype>{}, feedTitle, QString::fromLatin1(self), offset));
        } else if (segments.size() == 2 && section == QLatin1String("search.xml")) {
            reply(socket, 200, "application/opensearchdescription+xml;charset=utf-8", Opds::openSearch(m_catalog.name));
        } else if (segments.size() == 2) {
            const QByteArray feed = Opds::categoryFeed(data, section, m_catalog.interfaceLanguage);
            if (feed.isEmpty()) {
                reply(socket, 404, "text/plain", "Not found\n");
                return;
            }
            reply(socket, 200, navigation, feed);
        } else {
            reply(socket, 404, "text/plain", "Not found\n");
            return;
        }
        return;
    }

    const Book *book = segments.size() >= 2 ? data.book(bookId(segments.at(1))) : nullptr;
    if (book && segments.size() == 3 && segments.first() == QLatin1String("get")) {
        const QString format = segments.at(2).toUpper();
        for (const BookFormat &file : book->formats) {
            if (file.format == format && !file.file.isEmpty()) {
                // book_filename() of src/calibre/srv/code.py.
                const QString authors = CalibreText::authorsToString(book->authors.isEmpty()
                                                                         ? QStringList{ tr("Unknown") }
                                                                         : book->authors);
                const QString name = QStringLiteral("%1 - %2_%3.%4")
                                         .arg(book->title.left(30), authors.left(30))
                                         .arg(book->id)
                                         .arg(format.toLower());
                sendFile(socket, file.file, Opds::mimeType(format).toLatin1(),
                         CalibreText::asciiFilename(name).replace(u'"', u'_'));
                return;
            }
        }
    } else if (book && segments.size() == 2 && segments.first() == QLatin1String("cover") && !book->coverFile.isEmpty()) {
        sendFile(socket, book->coverFile, "image/jpeg", {});
        return;
    } else if (book && segments.size() == 2 && segments.first() == QLatin1String("thumb") && !book->coverFile.isEmpty()) {
        sendThumbnail(socket, book->id);
        return;
    }
    reply(socket, 404, "text/plain", "Not found\n");
}

void LibraryServer::reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
                          const QList<QPair<QByteArray, QByteArray>> &extra)
{
    QByteArray header = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    header += "Content-Type: " + type + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    header += "Connection: close\r\n";
    header += "Server: Cousoir\r\n";
    for (const auto &[name, value] : extra)
        header += name + ": " + value + "\r\n";
    header += "\r\n";
    socket->write(header);
    if (!socket->property(headProperty).toBool())
        socket->write(body);
    socket->disconnectFromHost();
    emit served(socket->property(requestProperty).toString(), status);
}

void LibraryServer::sendFile(QTcpSocket *socket, const QString &path, const QByteArray &type, const QString &downloadName)
{
    auto *file = new QFile(path, socket);
    if (!file->open(QIODevice::ReadOnly)) {
        reply(socket, 404, "text/plain", "The file cannot be read\n");
        return;
    }
    QList<QPair<QByteArray, QByteArray>> extra;
    if (!downloadName.isEmpty())
        extra.append({ "Content-Disposition", "attachment; filename=\"" + downloadName.toLatin1() + '"' });
    QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(file->size())
        + "\r\nConnection: close\r\nServer: Cousoir\r\n";
    for (const auto &[name, value] : extra)
        header += name + ": " + value + "\r\n";
    header += "\r\n";
    socket->write(header);
    emit served(socket->property(requestProperty).toString(), 200);
    if (socket->property(headProperty).toBool() || file->size() == 0) {
        socket->disconnectFromHost();
        return;
    }
    // The file goes as the client takes it, without reading it all at once.
    const auto more = [socket, file] {
        while (socket->bytesToWrite() < 2 * chunk && !file->atEnd()) {
            const QByteArray bytes = file->read(chunk);
            if (bytes.isEmpty())
                break;
            socket->write(bytes);
        }
        if (file->atEnd() || file->error() != QFileDevice::NoError) {
            QObject::disconnect(socket, &QTcpSocket::bytesWritten, file, nullptr);
            file->close();
            socket->disconnectFromHost();
        }
    };
    connect(socket, &QTcpSocket::bytesWritten, file, more);
    more();
}

void LibraryServer::sendThumbnail(QTcpSocket *socket, int bookId)
{
    const Book *book = m_catalog.data->book(bookId);
    const QString cached = m_catalog.thumbnailFolder.isEmpty()
        ? QString()
        : QDir(m_catalog.thumbnailFolder)
              .filePath(QStringLiteral("%1-%2-0x%3.jpg").arg(book->id).arg(book->coverModified).arg(thumbnailHeight));
    QPointer<QTcpSocket> target(socket);
    QPointer<LibraryServer> self(this);
    auto *job = new ThumbnailJob(book->coverFile, cached, [target, self](const QByteArray &bytes) {
        // The application outlives the job, unlike the server and the socket.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [target, self, bytes] {
                if (!target || !self)
                    return;
                if (bytes.isEmpty())
                    self->reply(target, 500, "text/plain", "The cover cannot be read\n");
                else
                    self->reply(target, 200, "image/jpeg", bytes, { { "Cache-Control", "max-age=3600" } });
            },
            Qt::QueuedConnection);
    });
    QThreadPool::globalInstance()->start(job);
}
