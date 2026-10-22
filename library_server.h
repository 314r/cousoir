#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QStringList>

#include <functional>

#include "book_sort.h"
#include "calibre_search.h"

class QTcpServer;
class QTcpSocket;
struct LibraryData;

// The library served on the local network, read only: its OPDS catalog for
// e-readers and reading apps, and a web page for a phone's or a computer's
// browser, from which to read the books and download them (see Opds for the
// addresses). A small HTTP/1.1 server of its own, which answers GET and
// HEAD and closes each connection after its answer; with a password, it asks
// for it by Basic authentication, any user name.
class LibraryServer : public QObject
{
    Q_OBJECT

public:
    // The books found by a search of Calibre's search language, sorted:
    // indexes into the library's books, and why a search failed.
    using Find = std::function<QList<qsizetype>(const QString &query, const QList<BookSort::Level> &sort, QString *error)>;

    // What the server serves, read at each request: the library's data,
    // kept alive and unchanged between requests by its owner.
    struct Catalog
    {
        const LibraryData *data = nullptr;
        Find find;
        QString name;
        QString interfaceLanguage;
        // Where the covers' thumbnails are kept; empty to keep none.
        QString thumbnailFolder;
    };

    explicit LibraryServer(QObject *parent = nullptr);
    ~LibraryServer() override;

    void setCatalog(const Catalog &catalog);
    void setPassword(const QString &password);

    // Starts listening on `port` of `address`, every network by default; 0
    // takes a free port. Returns why it cannot, empty once it listens.
    QString listen(quint16 port, const QHostAddress &address = QHostAddress::Any);
    void close();
    bool isListening() const;
    quint16 port() const;

    // A Find on a library's search index and sorter: every book for an
    // empty query.
    static QList<qsizetype> findBooks(const LibraryData &data, const CalibreSearch::Index &index,
                                      const BookSort::Sorter *sorter, const QString &query,
                                      const QList<BookSort::Level> &sort, QString *error);

    // The addresses at which other devices of the network reach `port`:
    // http://192.168.1.20:8090/, one per address of this computer.
    static QStringList addresses(quint16 port);

signals:
    // A request answered, for a log: its method and target, and the
    // answer's status.
    void served(const QString &request, int status);

private:
    void accept();
    void read(QTcpSocket *socket);
    void answer(QTcpSocket *socket, const QByteArray &method, const QByteArray &target, const QHash<QByteArray, QByteArray> &headers);
    void reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
               const QList<QPair<QByteArray, QByteArray>> &extra = {});
    void sendFile(QTcpSocket *socket, const QString &file, const QByteArray &type, const QString &downloadName);
    void sendThumbnail(QTcpSocket *socket, int bookId);

    QTcpServer *m_server = nullptr;
    Catalog m_catalog;
    QString m_password;
};
