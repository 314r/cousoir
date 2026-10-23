#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <functional>

// A web server on 127.0.0.1 for the tests, standing in for a site: each GET
// is answered by `handler` from its URL, and noted in `requests`.
class FakeHttpServer : public QObject
{
public:
    struct Answer
    {
        int status = 404;
        QByteArray body;
        QByteArray type = "application/json";
    };

    std::function<Answer(const QUrl &url)> handler;
    // The paths and queries asked, in order.
    QStringList requests;

    explicit FakeHttpServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
        m_server.listen(QHostAddress::LocalHost);
    }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

private:
    void serve(QTcpSocket *socket)
    {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
            if (!buffer.contains("\r\n\r\n")) {
                socket->setProperty("buffer", buffer);
                return;
            }
            const QByteArray target = buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
            requests.append(QString::fromLatin1(target));
            const Answer answer = handler ? handler(QUrl(url() + QString::fromLatin1(target))) : Answer{};
            socket->write("HTTP/1.1 " + QByteArray::number(answer.status) + (answer.status == 200 ? " OK" : " Error")
                          + "\r\nContent-Type: " + answer.type + "\r\nContent-Length: "
                          + QByteArray::number(answer.body.size()) + "\r\nConnection: close\r\n\r\n" + answer.body);
            socket->disconnectFromHost();
        });
    }

    QTcpServer m_server;
};
