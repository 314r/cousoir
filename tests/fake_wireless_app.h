#pragma once

#include <QTcpSocket>

#include <memory>

#include "wireless.h"

// An app that connects to Calibre's wireless device server as KOReader's
// Calibre plugin does (plugins/calibre.koplugin/wireless.lua), for the
// tests: its books in memory, each its metadata and bytes, and every
// message it got.

namespace FakeWireless {

using PythonJson::Value;

inline Value dict(std::initializer_list<std::pair<QString, Value>> fields)
{
    Value value = Value::dict();
    for (const auto &[key, field] : fields)
        value.insert(key, field);
    return value;
}

class FakeApp : public QObject
{
public:
    struct Stored
    {
        Value metadata;
        QByteArray data;
    };

    QString deviceUuid;
    QString password;
    // The app's answer to the next book sent, an ERROR when set.
    QString refuse;
    QList<Stored> books;
    QList<QPair<int, Value>> received;
    bool closed = false;

    void connectTo(quint16 port)
    {
        m_socket = std::make_unique<QTcpSocket>();
        connect(m_socket.get(), &QTcpSocket::readyRead, this, &FakeApp::read);
        connect(m_socket.get(), &QTcpSocket::disconnected, this, [this] { closed = true; });
        m_socket->connectToHost(QHostAddress::LocalHost, port);
    }

    int count(int opcode) const
    {
        int found = 0;
        for (const auto &message : received)
            found += message.first == opcode;
        return found;
    }

    const Value *last(int opcode) const
    {
        for (qsizetype i = received.size() - 1; i >= 0; --i) {
            if (received.at(i).first == opcode)
                return &received.at(i).second;
        }
        return nullptr;
    }

private:
    void reply(int opcode, const Value &argument) { m_socket->write(Wireless::frame(opcode, argument)); }

    qsizetype indexOf(const QString &lpath) const
    {
        for (qsizetype i = 0; i < books.size(); ++i) {
            if (books.at(i).metadata.text(QStringLiteral("lpath")) == lpath)
                return i;
        }
        return -1;
    }

    void read()
    {
        m_buffer += m_socket->readAll();
        while (true) {
            if (m_binaryLeft > 0) {
                const QByteArray part = m_buffer.left(m_binaryLeft);
                m_buffer.remove(0, part.size());
                books[m_binaryBook].data += part;
                m_binaryLeft -= part.size();
                if (m_binaryLeft > 0)
                    return;
            }
            int opcode = 0;
            Value argument;
            const int taken = Wireless::takeMessage(&m_buffer, &opcode, &argument);
            if (taken <= 0)
                return;
            received.append({ opcode, argument });
            handle(opcode, argument);
        }
    }

    void handle(int opcode, const Value &argument)
    {
        switch (opcode) {
        case Wireless::GetInitializationInfo: {
            Value extensions;
            extensions.kind = Value::Kind::List;
            for (const char *extension : { "epub", "mobi", "pdf" })
                extensions.items.append(Value::from(QString::fromLatin1(extension)));
            const QString challenge = argument.text(QStringLiteral("passwordChallenge"));
            reply(Wireless::Ok,
                  dict({ { QStringLiteral("appName"), Value::from(QStringLiteral("KOReader")) },
                         { QStringLiteral("acceptedExtensions"), extensions },
                         { QStringLiteral("cacheUsesLpaths"), Value::from(true) },
                         { QStringLiteral("canAcceptLibraryInfo"), Value::from(true) },
                         { QStringLiteral("canDeleteMultipleBooks"), Value::from(true) },
                         { QStringLiteral("canReceiveBookBinary"), Value::from(true) },
                         { QStringLiteral("canSendOkToSendbook"), Value::from(true) },
                         { QStringLiteral("canStreamBooks"), Value::from(true) },
                         { QStringLiteral("canStreamMetadata"), Value::from(true) },
                         { QStringLiteral("canUseCachedMetadata"), Value::from(true) },
                         { QStringLiteral("coverHeight"), Value::from(240) },
                         { QStringLiteral("deviceKind"), Value::from(QStringLiteral("Kobo_Libra2")) },
                         { QStringLiteral("deviceName"), Value::from(QStringLiteral("KOReader (Kobo_Libra2)")) },
                         { QStringLiteral("passwordHash"),
                           Value::from(password.isEmpty() || challenge.isEmpty()
                                           ? QString()
                                           : Wireless::passwordHash(password, challenge)) },
                         { QStringLiteral("maxBookContentPacketLen"), Value::from(4096) },
                         { QStringLiteral("versionOK"), Value::from(true) } }));
            break;
        }
        case Wireless::GetDeviceInformation: {
            Value info = dict({ { QStringLiteral("device_name"), Value::from(QStringLiteral("KOReader (Kobo_Libra2)")) } });
            if (!deviceUuid.isEmpty())
                info.insert(QStringLiteral("device_store_uuid"), Value::from(deviceUuid));
            reply(Wireless::Ok, dict({ { QStringLiteral("device_info"), info },
                                       { QStringLiteral("version"), Value::from(QStringLiteral("1")) },
                                       { QStringLiteral("device_version"), Value::from(QStringLiteral("1")) } }));
            break;
        }
        case Wireless::SetCalibreDeviceInfo:
            deviceUuid = argument.text(QStringLiteral("device_store_uuid"));
            reply(Wireless::Ok, Value::dict());
            break;
        case Wireless::FreeSpace:
            reply(Wireless::Ok, dict({ { QStringLiteral("free_space_on_device"), Value::from(qint64(1) << 30) } }));
            break;
        case Wireless::SetLibraryInfo:
            reply(Wireless::Ok, Value::dict());
            break;
        case Wireless::GetBookCount:
            reply(Wireless::Ok, dict({ { QStringLiteral("willStream"), Value::from(true) },
                                       { QStringLiteral("willScan"), Value::from(true) },
                                       { QStringLiteral("count"), Value::from(int(books.size())) } }));
            for (qsizetype i = 0; i < books.size(); ++i) {
                const Value &book = books.at(i).metadata;
                reply(Wireless::Ok,
                      dict({ { QStringLiteral("priKey"), Value::from(int(i + 1)) },
                             { QStringLiteral("uuid"), Value::from(book.text(QStringLiteral("uuid"))) },
                             { QStringLiteral("lpath"), Value::from(book.text(QStringLiteral("lpath"))) },
                             { QStringLiteral("last_modified"), Value::from(book.text(QStringLiteral("last_modified"))) } }));
            }
            break;
        case Wireless::Noop:
            if (argument.find(QStringLiteral("priKey")))
                reply(Wireless::Ok, books.at(argument.find(QStringLiteral("priKey"))->integer - 1).metadata);
            else if (!argument.find(QStringLiteral("count")))
                reply(Wireless::Ok, Value::dict());
            break;
        case Wireless::SendBook: {
            if (!refuse.isEmpty()) {
                reply(Wireless::Error, dict({ { QStringLiteral("message"), Value::from(refuse) } }));
                break;
            }
            reply(Wireless::Ok, Value::dict());
            Value metadata = *argument.find(QStringLiteral("metadata"));
            metadata.insert(QStringLiteral("lpath"), Value::from(argument.text(QStringLiteral("lpath"))));
            qsizetype index = indexOf(argument.text(QStringLiteral("lpath")));
            if (index < 0) {
                books.append({ metadata, {} });
                index = books.size() - 1;
            } else {
                books[index] = { metadata, {} };
            }
            m_binaryBook = index;
            m_binaryLeft = argument.find(QStringLiteral("length"))->integer;
            break;
        }
        case Wireless::SendBookMetadata: {
            const Value &book = *argument.find(QStringLiteral("data"));
            const qsizetype index = indexOf(book.text(QStringLiteral("lpath")));
            if (index >= 0)
                books[index].metadata = book;
            break;
        }
        case Wireless::DeleteBook:
            reply(Wireless::Ok, Value::dict());
            for (const Value &lpath : argument.find(QStringLiteral("lpaths"))->items) {
                const qsizetype index = indexOf(lpath.string);
                const QString uuid = index >= 0 ? books.at(index).metadata.text(QStringLiteral("uuid")) : QString();
                if (index >= 0)
                    books.removeAt(index);
                reply(Wireless::Ok, dict({ { QStringLiteral("uuid"), Value::from(uuid) } }));
            }
            break;
        default:
            break;
        }
    }

    std::unique_ptr<QTcpSocket> m_socket;
    QByteArray m_buffer;
    qsizetype m_binaryBook = -1;
    qint64 m_binaryLeft = 0;
};

} // namespace FakeWireless
