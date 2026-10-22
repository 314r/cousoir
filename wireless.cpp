#include "wireless.h"

// Ported from Calibre 9.16, src/calibre/devices/smart_device_app/driver.py,
// Copyright (C) Charles Haley and Kovid Goyal, GPL v3: the opcodes, the
// framing (_json_encode(), _read_string_from_net()), the answer to a
// datagram (ConnectionListener.run()), open(), get_device_information() and
// _update_driveinfo_record(), set_library_info(), books() with its metadata
// cache (_metadata_in_cache(), _set_known_metadata(), _read_metadata_cache(),
// _write_metadata_cache()), upload_books() and _put_file(), delete_books(),
// sync_booklists() and _metadata_already_on_device(), free_space(), eject()
// and the NOOPs of detect_managed_devices(), and _startup_on_demand()'s
// choice of ports.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUuid>

#include "calibre_languages.h"

using PythonJson::Value;

namespace Wireless {

namespace {

Value list(const QList<Value> &items)
{
    Value value;
    value.kind = Value::Kind::List;
    value.items = items;
    return value;
}

Value dict(std::initializer_list<std::pair<QString, Value>> fields)
{
    Value value = Value::dict();
    for (const auto &[key, field] : fields)
        value.insert(key, field);
    return value;
}

} // namespace

const QList<quint16> &broadcastPorts()
{
    static const QList<quint16> ports{ 54982, 48123, 39001, 44044, 59678 };
    return ports;
}

const Tables &tables()
{
    static const Tables loaded = [] {
        Tables result;
        QFile file(QStringLiteral(":/cousoir/wireless/calibre.json"));
        if (!file.open(QIODevice::ReadOnly))
            return result;
        const Value json = PythonJson::parse(file.readAll());
        if (const Value *version = json.find(QStringLiteral("calibreVersion"))) {
            for (const Value &part : version->items)
                result.calibreVersion.append(int(part.integer));
        }
        if (const Value *extensions = json.find(QStringLiteral("validExtensions"))) {
            for (const Value &extension : extensions->items)
                result.validExtensions.append(extension.string);
        }
        if (const Value *mimes = json.find(QStringLiteral("mimeTypes"))) {
            for (const auto &[extension, mime] : mimes->fields)
                result.mimeTypes.insert(extension, mime.string);
        }
        result.pubdateFormat = json.text(QStringLiteral("pubdateFormat"));
        result.timestampFormat = json.text(QStringLiteral("timestampFormat"));
        result.lastModifiedFormat = json.text(QStringLiteral("lastModifiedFormat"));
        if (const Value *fields = json.find(QStringLiteral("fieldMetadata")))
            result.fieldMetadata = *fields;
        return result;
    }();
    return loaded;
}

QByteArray frame(int opcode, const Value &argument)
{
    PythonJson::Format format;
    format.ensureAscii = true;
    const QByteArray json = PythonJson::dumps(list({ Value::from(opcode), argument }), format).toUtf8();
    return QByteArray::number(json.size()) + json;
}

int takeMessage(QByteArray *buffer, int *opcode, Value *argument)
{
    const qsizetype start = buffer->indexOf('[');
    const qsizetype digits = start < 0 ? buffer->size() : start;
    for (qsizetype i = 0; i < digits; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(buffer->at(i))))
            return -1;
    }
    if (start < 0)
        return digits > 18 ? -1 : 0;
    bool ok = false;
    const qint64 length = buffer->left(start).toLongLong(&ok);
    if (!ok || length < 2)
        return -1;
    if (buffer->size() - start < length)
        return 0;
    const QByteArray json = buffer->mid(start, length);
    buffer->remove(0, start + length);
    const Value message = PythonJson::parse(json, &ok);
    if (!ok || message.kind != Value::Kind::List || message.items.size() != 2
        || message.items.at(0).kind != Value::Kind::Integer)
        return -1;
    *opcode = int(message.items.at(0).integer);
    *argument = message.items.at(1);
    return 1;
}

QByteArray broadcastReply(const QString &hostName, int contentServerPort, int port)
{
    return QStringLiteral("calibre wireless device client (on %1);%2,%3")
        .arg(hostName.section(u'.', 0, 0))
        .arg(contentServerPort)
        .arg(port)
        .toUtf8();
}

QString passwordHash(const QString &password, const QString &challenge)
{
    return QString::fromLatin1(QCryptographicHash::hash((password + challenge).toUtf8(), QCryptographicHash::Sha1).toHex());
}

} // namespace Wireless

namespace {

// isoformat(now()): the time in UTC, to the microsecond.
QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz000+00:00"));
}

QDateTime parseDate(const QString &text)
{
    if (text.isEmpty() || text == QLatin1String("None"))
        return {};
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}

bool flag(const Value &value, const QString &key)
{
    const Value *field = value.find(key);
    return field && PythonJson::truthy(*field);
}

qint64 number(const Value &value, const QString &key, qint64 fallback = 0)
{
    const Value *field = value.find(key);
    if (!field)
        return fallback;
    if (field->kind == Value::Kind::Integer)
        return field->integer;
    if (field->kind == Value::Kind::Float)
        return qint64(field->real);
    if (field->kind == Value::Kind::String) {
        bool ok = false;
        const qint64 parsed = field->string.toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

QString extensionOf(const QString &lpath)
{
    const qsizetype dot = lpath.lastIndexOf(u'.');
    return dot < 0 || dot < lpath.lastIndexOf(u'/') ? QString() : lpath.mid(dot);
}

// A thumbnail's height, or -1 for none.
int thumbnailHeight(const Value &book)
{
    const Value *thumbnail = book.find(QStringLiteral("thumbnail"));
    if (!thumbnail || thumbnail->kind != Value::Kind::List || thumbnail->items.size() < 2)
        return -1;
    return int(thumbnail->items.at(1).integer);
}

} // namespace

WirelessConnection::WirelessConnection(QTcpSocket *socket, const Settings &settings, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_settings(settings)
{
    m_socket->setParent(this);
    m_timeout.setSingleShot(true);
    m_noop.setSingleShot(true);
    connect(m_socket, &QTcpSocket::readyRead, this, &WirelessConnection::readMore);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] { fail(tr("The reader disconnected.")); });
    connect(m_socket, &QTcpSocket::bytesWritten, this, [this] {
        if (m_file) {
            m_timeout.start(m_settings.timeoutSeconds * 1000);
            writeChunk();
        }
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] { fail(tr("The reader stopped answering.")); });
    connect(&m_noop, &QTimer::timeout, this, &WirelessConnection::sendNoop);
    enqueue([this] { handshake(); });
}

WirelessConnection::~WirelessConnection()
{
    if (!m_closed)
        writeCache();
}

bool WirelessConnection::isOpen() const
{
    return m_open && !m_closed;
}

bool WirelessConnection::isBusy() const
{
    return m_operation;
}

QString WirelessConnection::name() const
{
    // get_gui_name()
    if (!m_app.deviceName.isEmpty())
        return m_app.deviceName;
    if (!m_app.deviceKind.isEmpty())
        return m_app.deviceKind;
    return m_app.appName.isEmpty() ? tr("Wireless device") : m_app.appName;
}

const Wireless::App &WirelessConnection::app() const
{
    return m_app;
}

const Value &WirelessConnection::driveInfo() const
{
    return m_driveInfo;
}

QString WirelessConnection::deviceUuid() const
{
    return m_driveInfo.text(QStringLiteral("device_store_uuid"));
}

qint64 WirelessConnection::freeSpace() const
{
    return m_freeSpace;
}

const QList<Value> &WirelessConnection::books() const
{
    return m_books;
}

std::optional<Value> WirelessConnection::knownBook(const QString &lpath) const
{
    const auto found = m_known.constFind(lpath);
    if (found == m_known.cend())
        return std::nullopt;
    return *found;
}

QString WirelessConnection::lpathOfUuid(const QString &uuid, const QString &extension) const
{
    // _uuid_in_cache(), for "Replace books with the same calibre ID".
    for (const Value &book : m_cache) {
        const QString lpath = book.text(QStringLiteral("lpath"));
        if (book.text(QStringLiteral("uuid")) == uuid && lpath.endsWith(extension) && m_known.contains(lpath))
            return lpath;
    }
    return {};
}

void WirelessConnection::send(const QList<Wireless::Outgoing> &books, Done done)
{
    enqueue([this, books, done] {
        m_operation = true;
        m_operationDone = done;
        const auto shared = std::make_shared<QList<Wireless::Outgoing>>(books);
        // upload_books()'s sanity_check(): the books fit.
        request(Wireless::FreeSpace, Value::dict(), 1, [this, shared](const QList<Message> &replies) {
            m_freeSpace = number(replies.constFirst().argument, QStringLiteral("free_space_on_device"));
            qint64 total = 0;
            for (const Wireless::Outgoing &book : *shared)
                total += QFileInfo(book.file).size();
            if (total > m_freeSpace) {
                finish(false, tr("The reader has no room for these books."));
                return;
            }
            sendBook(shared, 0, total);
        });
    });
}

void WirelessConnection::remove(const QStringList &lpaths, Done done)
{
    enqueue([this, lpaths, done] {
        m_operation = true;
        m_operationDone = done;
        Value paths;
        paths.kind = Value::Kind::List;
        for (const QString &lpath : lpaths)
            paths.items.append(Value::from(lpath));
        // delete_books(): an answer, then one for each book.
        request(Wireless::DeleteBook, Wireless::dict({ { QStringLiteral("lpaths"), paths } }), 1 + int(lpaths.size()),
                [this, lpaths](const QList<Message> &) {
                    for (const QString &lpath : lpaths) {
                        m_books.removeIf([&lpath](const Value &book) { return book.text(QStringLiteral("lpath")) == lpath; });
                        forget(lpath);
                    }
                    emit booksChanged();
                    syncBooklists({}, [this] { finish(true, {}); });
                });
    });
}

void WirelessConnection::updateMetadata(const QList<Value> &books, Done done)
{
    enqueue([this, books, done] {
        m_operation = true;
        m_operationDone = done;
        syncBooklists(books, [this] { finish(true, {}); });
    });
}

void WirelessConnection::eject()
{
    enqueue([this] {
        m_ejecting = true;
        request(Wireless::Noop, Wireless::dict({ { QStringLiteral("ejecting"), Value::from(true) } }), 1,
                [this](const QList<Message> &) { close(tr("The reader was let go.")); });
    });
}

void WirelessConnection::close(const QString &reason)
{
    fail(reason.isEmpty() ? tr("The reader was let go.") : reason);
}

void WirelessConnection::enqueue(std::function<void()> task)
{
    m_queue.push_back(std::move(task));
    if (!m_running)
        next();
}

void WirelessConnection::next()
{
    if (m_closed)
        return;
    if (m_queue.empty()) {
        m_running = false;
        if (m_open)
            m_noop.start(m_settings.noopIntervalMs);
        return;
    }
    m_running = true;
    m_noop.stop();
    const std::function<void()> task = std::move(m_queue.front());
    m_queue.pop_front();
    task();
}

void WirelessConnection::request(int opcode, const Value &argument, int replies, Replies done)
{
    post(opcode, argument);
    m_expected = replies;
    m_replies.clear();
    m_done = std::move(done);
    if (replies == 0) {
        const Replies callback = std::move(m_done);
        callback({});
        return;
    }
    m_timeout.start(m_settings.timeoutSeconds * 1000);
    feed();
}

void WirelessConnection::post(int opcode, const Value &argument)
{
    if (!m_closed)
        m_socket->write(Wireless::frame(opcode, argument));
}

void WirelessConnection::readMore()
{
    m_buffer += m_socket->readAll();
    feed();
}

void WirelessConnection::feed()
{
    while (m_expected > 0 && !m_closed) {
        Message message;
        const int taken = Wireless::takeMessage(&m_buffer, &message.opcode, &message.argument);
        if (taken == 0)
            return;
        if (taken < 0) {
            fail(tr("The reader answered what Cousoir cannot read."));
            return;
        }
        m_replies.append(message);
        if (m_replies.size() < m_expected)
            continue;
        m_expected = 0;
        m_timeout.stop();
        const Replies done = std::move(m_done);
        const QList<Message> replies = std::move(m_replies);
        done(replies);
        return;
    }
}

void WirelessConnection::fail(const QString &reason)
{
    if (m_closed)
        return;
    m_closed = true;
    m_timeout.stop();
    m_noop.stop();
    m_queue.clear();
    m_file.reset();
    m_socket->disconnect(this);
    m_socket->abort();
    writeCache();
    if (m_operation) {
        m_operation = false;
        if (const Done done = std::exchange(m_operationDone, {}))
            done(false, reason);
        emit finished(false, reason);
    }
    emit closed(reason);
}

void WirelessConnection::sendNoop()
{
    // detect_managed_devices(): an idle app is asked whether it is there.
    if (m_running || !m_open || m_closed)
        return;
    enqueue([this] { request(Wireless::Noop, Value::dict(), 1, [this](const QList<Message> &) { next(); }); });
}

void WirelessConnection::handshake()
{
    using Wireless::dict;
    const Wireless::Tables &tables = Wireless::tables();
    const QString challenge = m_settings.password.isEmpty() ? QString() : isoNow();
    Value version;
    version.kind = Value::Kind::List;
    for (const int part : tables.calibreVersion)
        version.items.append(Value::from(part));
    const Value argument = dict({
        { QStringLiteral("serverProtocolVersion"), Value::from(1) },
        { QStringLiteral("validExtensions"), Value::from(tables.validExtensions) },
        { QStringLiteral("passwordChallenge"), Value::from(challenge) },
        { QStringLiteral("currentLibraryName"), Value::from(m_settings.libraryName) },
        { QStringLiteral("currentLibraryUUID"), Value::from(m_settings.libraryUuid) },
        { QStringLiteral("pubdateFormat"), Value::from(tables.pubdateFormat) },
        { QStringLiteral("timestampFormat"), Value::from(tables.timestampFormat) },
        { QStringLiteral("lastModifiedFormat"), Value::from(tables.lastModifiedFormat) },
        { QStringLiteral("calibre_version"), version },
        { QStringLiteral("canSupportUpdateBooks"), Value::from(true) },
        { QStringLiteral("canSupportLpathChanges"), Value::from(true) },
    });
    request(Wireless::GetInitializationInfo, argument, 1, [this, challenge](const QList<Message> &replies) {
        const Message &reply = replies.constFirst();
        const Value &info = reply.argument;
        if (reply.opcode != Wireless::Ok || !flag(info, QStringLiteral("versionOK"))
            || number(info, QStringLiteral("maxBookContentPacketLen")) <= 0) {
            fail(tr("The reader speaks another version of Calibre's protocol."));
            return;
        }
        if (!flag(info, QStringLiteral("canStreamBooks")) || !flag(info, QStringLiteral("canStreamMetadata"))
            || !flag(info, QStringLiteral("canReceiveBookBinary")) || !flag(info, QStringLiteral("canDeleteMultipleBooks"))) {
            fail(tr("The app on the reader is too old: update it."));
            return;
        }
        m_app.appName = info.text(QStringLiteral("appName"));
        m_app.deviceKind = info.text(QStringLiteral("deviceKind"));
        m_app.deviceName = info.text(QStringLiteral("deviceName"));
        if (const Value *version = info.find(QStringLiteral("ccVersionNumber")))
            m_app.version = version->kind == Value::Kind::String ? version->string : QString::number(version->integer);
        m_app.cacheUsesLpaths = flag(info, QStringLiteral("cacheUsesLpaths"));
        m_app.canUseCachedMetadata = flag(info, QStringLiteral("canUseCachedMetadata"));
        m_app.canSendOkToSendbook = flag(info, QStringLiteral("canSendOkToSendbook"));
        m_app.canAcceptLibraryInfo = flag(info, QStringLiteral("canAcceptLibraryInfo"));
        m_app.useUuidFileNames = flag(info, QStringLiteral("useUuidFileNames"));
        m_app.maxBookContentPacketLen = number(info, QStringLiteral("maxBookContentPacketLen"), 4096);
        m_app.coverHeight = int(number(info, QStringLiteral("coverHeight"), 160));
        if (info.find(QStringLiteral("coverWidth")))
            m_app.coverWidth = int(number(info, QStringLiteral("coverWidth"), m_app.coverHeight * 3 / 4));
        if (const Value *extensions = info.find(QStringLiteral("acceptedExtensions"))) {
            for (const Value &extension : extensions->items) {
                if (extension.kind == Value::Kind::String && !extension.string.isEmpty())
                    m_app.acceptedExtensions.append(extension.string.toLower());
            }
        }
        if (const Value *lengths = info.find(QStringLiteral("extensionPathLengths"))) {
            for (const auto &[extension, length] : lengths->fields)
                m_app.extensionPathLengths.insert(extension, int(length.integer));
        }
        if (m_app.acceptedExtensions.isEmpty()) {
            fail(tr("The reader takes no books."));
            return;
        }
        if (!m_settings.password.isEmpty()
            && info.text(QStringLiteral("passwordHash")) != Wireless::passwordHash(m_settings.password, challenge)) {
            post(Wireless::DisplayMessage,
                 Wireless::dict({ { QStringLiteral("messageKind"), Value::from(1) },
                                  { QStringLiteral("currentLibraryName"), Value::from(m_settings.libraryName) },
                                  { QStringLiteral("currentLibraryUUID"), Value::from(m_settings.libraryUuid) } }));
            m_socket->flush();
            fail(tr("The reader gave a wrong password."));
            return;
        }
        // get_device_information(): the app's driveinfo, completed, sent back.
        request(Wireless::GetDeviceInformation, Value::dict(), 1, [this](const QList<Message> &replies) {
            const Value *info = replies.constFirst().argument.find(QStringLiteral("device_info"));
            m_driveInfo = info && info->kind == Value::Kind::Dict ? *info : Value::dict();
            if (m_driveInfo.text(QStringLiteral("device_store_uuid")).isEmpty())
                m_driveInfo.insert(QStringLiteral("device_store_uuid"),
                                   Value::from(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (m_driveInfo.text(QStringLiteral("device_name")).isEmpty())
                m_driveInfo.insert(QStringLiteral("device_name"), Value::from(name()));
            m_driveInfo.insert(QStringLiteral("location_code"), Value::from(QStringLiteral("main")));
            m_driveInfo.insert(QStringLiteral("last_library_uuid"), Value::from(m_settings.libraryUuid));
            QStringList version;
            for (const int part : Wireless::tables().calibreVersion)
                version.append(QString::number(part));
            m_driveInfo.insert(QStringLiteral("calibre_version"), Value::from(version.join(u'.')));
            m_driveInfo.insert(QStringLiteral("date_last_connected"), Value::from(isoNow()));
            m_driveInfo.insert(QStringLiteral("prefix"), Value::from(QString()));
            request(Wireless::SetCalibreDeviceInfo, m_driveInfo, 1, [this](const QList<Message> &) {
                readCache();
                request(Wireless::FreeSpace, Value::dict(), 1, [this](const QList<Message> &replies) {
                    m_freeSpace = number(replies.constFirst().argument, QStringLiteral("free_space_on_device"));
                    if (!m_app.canAcceptLibraryInfo) {
                        listBooks();
                        return;
                    }
                    // set_library_info(): the library and its fields, their
                    // names in Calibre's interface language.
                    Value fields = Wireless::tables().fieldMetadata;
                    const std::shared_ptr<const CalibreLanguages::Names> names
                        = CalibreLanguages::names(m_settings.interfaceLanguage);
                    for (auto &[key, field] : fields.fields) {
                        const QString fieldName = field.text(QStringLiteral("name"));
                        if (!fieldName.isEmpty())
                            field.insert(QStringLiteral("name"), Value::from(names->translate(fieldName)));
                    }
                    const Value info = Wireless::dict({
                        { QStringLiteral("libraryName"), Value::from(m_settings.libraryName) },
                        { QStringLiteral("libraryUuid"), Value::from(m_settings.libraryUuid) },
                        { QStringLiteral("fieldMetadata"), fields },
                        { QStringLiteral("otherInfo"),
                          Wireless::dict({ { QStringLiteral("id_link_rules"), Value::dict() } }) },
                    });
                    request(Wireless::SetLibraryInfo, info, 1, [this](const QList<Message> &) { listBooks(); });
                });
            });
        });
    });
}

void WirelessConnection::listBooks()
{
    using Wireless::dict;
    const Value argument = dict({
        { QStringLiteral("canStream"), Value::from(true) },
        { QStringLiteral("canScan"), Value::from(true) },
        { QStringLiteral("willUseCachedMetadata"), Value::from(m_app.canUseCachedMetadata) },
        { QStringLiteral("supportsSync"), Value::from(false) },
        { QStringLiteral("canSupportBookFormatSync"), Value::from(true) },
    });
    const auto ready = [this] {
        m_open = true;
        emit booksChanged();
        emit opened();
        next();
    };
    request(Wireless::GetBookCount, argument, 1, [this, ready](const QList<Message> &replies) {
        if (replies.constFirst().opcode != Wireless::Ok) {
            fail(tr("The reader did not list its books."));
            return;
        }
        m_books.clear();
        const int count = int(number(replies.constFirst().argument, QStringLiteral("count")));
        if (!m_app.canUseCachedMetadata) {
            receiveBooks(count, ready);
            return;
        }
        // Each book by its uuid, path and time: those the cache knows as
        // they are come from it, the others are asked for.
        m_expected = count;
        m_replies.clear();
        m_done = [this, ready](const QList<Message> &stubs) {
            QList<qint64> wanted;
            QSet<QString> lpaths;
            for (const Message &stub : stubs) {
                const Value &book = stub.argument;
                const QString uuid = book.text(QStringLiteral("uuid"));
                const QString lpath = book.text(QStringLiteral("lpath"));
                const QString key = uuid + (lpath.isEmpty() ? book.text(QStringLiteral("extension")) : lpath);
                const auto cached = m_cache.constFind(key);
                const QDateTime modified = parseDate(book.text(QStringLiteral("last_modified")));
                if (!uuid.isEmpty() && cached != m_cache.cend() && modified.isValid()
                    && parseDate(cached->text(QStringLiteral("last_modified"))) == modified) {
                    m_cacheUsed.insert(key, QDateTime::currentDateTimeUtc());
                    m_books.append(*cached);
                    lpaths.insert(cached->text(QStringLiteral("lpath")));
                } else {
                    wanted.append(number(book, QStringLiteral("priKey")));
                }
            }
            if (m_app.cacheUsesLpaths) {
                // Books no longer on the app leave the cache.
                for (const QString &lpath : m_known.keys()) {
                    if (!lpaths.contains(lpath))
                        forget(lpath);
                }
            }
            post(Wireless::Noop, Wireless::dict({ { QStringLiteral("count"), Value::from(qint64(wanted.size())) } }));
            for (const qint64 key : wanted)
                post(Wireless::Noop, Wireless::dict({ { QStringLiteral("priKey"), Value::from(key) } }));
            receiveBooks(int(wanted.size()), ready);
        };
        if (count == 0) {
            const Replies done = std::move(m_done);
            m_expected = 0;
            done({});
            return;
        }
        m_timeout.start(m_settings.timeoutSeconds * 1000);
        feed();
    });
}

void WirelessConnection::receiveBooks(int count, std::function<void()> then)
{
    m_expected = count;
    m_replies.clear();
    m_done = [this, then](const QList<Message> &replies) {
        for (const Message &reply : replies) {
            if (reply.opcode != Wireless::Ok) {
                fail(tr("The reader did not send its books' metadata."));
                return;
            }
            Value book = reply.argument;
            book.remove(QStringLiteral("_series_sort_"));
            const bool fresh = book.find(QStringLiteral("_new_book_"));
            book.remove(QStringLiteral("_new_book_"));
            setBook(book);
            if (!fresh)
                remember(book);
        }
        then();
    };
    if (count == 0) {
        const Replies done = std::move(m_done);
        m_expected = 0;
        done({});
        return;
    }
    m_timeout.start(m_settings.timeoutSeconds * 1000);
    feed();
}

void WirelessConnection::sendBook(const std::shared_ptr<QList<Wireless::Outgoing>> &books, int index, qint64 total)
{
    if (index >= books->size()) {
        // books_uploaded(): the booklists synced once the books are there.
        syncBooklists({}, [this] { finish(true, {}); });
        return;
    }
    const Wireless::Outgoing &book = books->at(index);
    const qint64 length = QFileInfo(book.file).size();
    Value metadata = book.metadata;
    metadata.insert(QStringLiteral("lpath"), Value::from(book.lpath));
    metadata.insert(QStringLiteral("size"), Value::from(length));
    using Wireless::dict;
    const Value argument = dict({
        { QStringLiteral("lpath"), Value::from(book.lpath) },
        { QStringLiteral("length"), Value::from(length) },
        { QStringLiteral("metadata"), metadata },
        { QStringLiteral("thisBook"), Value::from(index) },
        { QStringLiteral("totalBooks"), Value::from(int(books->size())) },
        { QStringLiteral("willStreamBooks"), Value::from(true) },
        { QStringLiteral("willStreamBinary"), Value::from(true) },
        { QStringLiteral("wantsSendOkToSendbook"), Value::from(m_app.canSendOkToSendbook) },
        { QStringLiteral("canSupportLpathChanges"), Value::from(true) },
    });
    request(Wireless::SendBook, argument, m_app.canSendOkToSendbook ? 1 : 0,
            [this, books, index, total, metadata, length](const QList<Message> &replies) mutable {
                const Wireless::Outgoing &book = books->at(index);
                QString lpath = book.lpath;
                if (!replies.isEmpty()) {
                    const Message &reply = replies.constFirst();
                    if (reply.opcode == Wireless::Error) {
                        finish(false, tr("The reader refused %1: %2")
                                          .arg(lpath, reply.argument.text(QStringLiteral("message"))));
                        return;
                    }
                    const QString changed = reply.argument.text(QStringLiteral("lpath"));
                    if (!changed.isEmpty())
                        lpath = changed;
                }
                metadata.insert(QStringLiteral("lpath"), Value::from(lpath));
                metadata.remove(QStringLiteral("_series_sort_"));
                remember(metadata);
                setBook(metadata);
                streamFile(book.file, length, [this, books, index, total](bool ok) {
                    if (!ok) {
                        fail(tr("Cannot read %1.").arg(books->at(index).file));
                        return;
                    }
                    emit progress(double(index + 1) / double(books->size()));
                    sendBook(books, index + 1, total);
                });
            });
}

void WirelessConnection::streamFile(const QString &path, qint64 length, std::function<void(bool)> done)
{
    m_file = std::make_unique<QFile>(path);
    if (!m_file->open(QIODevice::ReadOnly)) {
        m_file.reset();
        done(false);
        return;
    }
    m_fileLeft = length;
    m_fileDone = std::move(done);
    m_timeout.start(m_settings.timeoutSeconds * 1000);
    writeChunk();
}

void WirelessConnection::writeChunk()
{
    // The file a piece at a time, as the socket takes it.
    constexpr qint64 window = 1 << 20;
    while (m_file && m_fileLeft > 0 && m_socket->bytesToWrite() < window) {
        const QByteArray chunk = m_file->read(std::min<qint64>(m_fileLeft, 64 * 1024));
        if (chunk.isEmpty()) {
            m_file.reset();
            const std::function<void(bool)> done = std::move(m_fileDone);
            done(false);
            return;
        }
        m_socket->write(chunk);
        m_fileLeft -= chunk.size();
    }
    if (m_file && m_fileLeft == 0) {
        m_file.reset();
        m_timeout.stop();
        const std::function<void(bool)> done = std::move(m_fileDone);
        done(true);
    }
}

void WirelessConnection::syncBooklists(const QList<Value> &books, std::function<void()> then)
{
    // sync_booklists(): the metadata the app does not have already.
    QList<Value> sending;
    for (const Value &book : books) {
        const auto known = m_known.constFind(book.text(QStringLiteral("lpath")));
        const bool already = known != m_known.cend()
            && known->text(QStringLiteral("uuid")) == book.text(QStringLiteral("uuid"))
            && parseDate(known->text(QStringLiteral("last_modified")))
                == parseDate(book.text(QStringLiteral("last_modified")))
            && thumbnailHeight(*known) == thumbnailHeight(book);
        if (!already)
            sending.append(book);
    }
    using Wireless::dict;
    post(Wireless::SendBooklists, dict({ { QStringLiteral("count"), Value::from(qint64(sending.size())) },
                                         { QStringLiteral("collections"), Value::dict() },
                                         { QStringLiteral("willStreamMetadata"), Value::from(true) },
                                         { QStringLiteral("supportsSync"), Value::from(false) } }));
    for (qsizetype i = 0; i < sending.size(); ++i) {
        Value stored = sending.at(i);
        stored.remove(QStringLiteral("_series_sort_"));
        remember(stored);
        setBook(stored);
        post(Wireless::SendBookMetadata, dict({ { QStringLiteral("index"), Value::from(qint64(i)) },
                                                { QStringLiteral("count"), Value::from(qint64(sending.size())) },
                                                { QStringLiteral("data"), sending.at(i) },
                                                { QStringLiteral("supportsSync"), Value::from(false) } }));
    }
    if (!sending.isEmpty())
        emit booksChanged();
    writeCache();
    // _sync_booklists() asks for the room left after.
    request(Wireless::FreeSpace, Value::dict(), 1, [this, then](const QList<Message> &replies) {
        m_freeSpace = number(replies.constFirst().argument, QStringLiteral("free_space_on_device"), m_freeSpace);
        then();
    });
}

void WirelessConnection::finish(bool ok, const QString &error)
{
    m_operation = false;
    if (const Done done = std::exchange(m_operationDone, {}))
        done(ok, error);
    emit finished(ok, error);
    next();
}

QString WirelessConnection::cacheKey(const Value &book) const
{
    // _make_metadata_cache_key()
    const QString uuid = book.text(QStringLiteral("uuid"));
    const QString lpath = book.text(QStringLiteral("lpath"));
    if (uuid.isEmpty() || lpath.isEmpty())
        return {};
    return uuid + (m_app.cacheUsesLpaths ? lpath : extensionOf(lpath));
}

void WirelessConnection::remember(const Value &book)
{
    // _set_known_metadata(): another book at the same place leaves.
    const QString lpath = book.text(QStringLiteral("lpath"));
    const auto previous = m_known.constFind(lpath);
    if (previous != m_known.cend() && previous->text(QStringLiteral("uuid")) != book.text(QStringLiteral("uuid"))) {
        const QString uuid = previous->text(QStringLiteral("uuid"));
        m_cache.remove(uuid + extensionOf(lpath));
        m_cache.remove(uuid + lpath);
    }
    m_known.insert(lpath, book);
    const QString key = cacheKey(book);
    if (!key.isEmpty()) {
        m_cache.insert(key, book);
        m_cacheUsed.insert(key, QDateTime::currentDateTimeUtc());
    }
}

void WirelessConnection::forget(const QString &lpath)
{
    const auto known = m_known.constFind(lpath);
    if (known == m_known.cend())
        return;
    const QString key = cacheKey(*known);
    m_known.erase(known);
    m_cache.remove(key);
    m_cacheUsed.remove(key);
}

void WirelessConnection::readCache()
{
    // _read_metadata_cache(): the books this app had, by its uuid.
    m_cache.clear();
    m_cacheUsed.clear();
    m_known.clear();
    if (m_settings.cacheFolder.isEmpty())
        return;
    QFile file(QDir(m_settings.cacheFolder).filePath(deviceUuid() + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const Value cache = PythonJson::parse(file.readAll());
    for (const auto &[key, entry] : cache.fields) {
        const Value *book = entry.find(QStringLiteral("book"));
        if (!book || book->kind != Value::Kind::Dict)
            continue;
        m_cache.insert(key, *book);
        m_cacheUsed.insert(key, parseDate(entry.text(QStringLiteral("last_used"))));
        m_known.insert(book->text(QStringLiteral("lpath")), *book);
    }
}

void WirelessConnection::writeCache()
{
    // _write_metadata_cache(): entries unused for 30 days go.
    if (m_settings.cacheFolder.isEmpty() || deviceUuid().isEmpty())
        return;
    const QDateTime purge = QDateTime::currentDateTimeUtc().addDays(-30);
    Value cache = Value::dict();
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        const QDateTime used = m_cacheUsed.value(it.key());
        if (used.isValid() && used < purge)
            continue;
        cache.insert(it.key(),
                     Wireless::dict({ { QStringLiteral("book"), it.value() },
                                      { QStringLiteral("last_used"),
                                        Value::from((used.isValid() ? used : QDateTime::currentDateTimeUtc())
                                                        .toString(Qt::ISODateWithMs)) } }));
    }
    QDir().mkpath(m_settings.cacheFolder);
    QSaveFile file(QDir(m_settings.cacheFolder).filePath(deviceUuid() + QStringLiteral(".json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(PythonJson::dumps(cache).toUtf8());
        file.commit();
    }
}

void WirelessConnection::setBook(const Value &book)
{
    const QString lpath = book.text(QStringLiteral("lpath"));
    for (Value &existing : m_books) {
        if (existing.text(QStringLiteral("lpath")) == lpath) {
            existing = book;
            return;
        }
    }
    m_books.append(book);
}

WirelessServer::WirelessServer(QObject *parent)
    : QObject(parent)
{
}

WirelessServer::~WirelessServer()
{
    stop();
}

bool WirelessServer::start(const Settings &settings, QString *error)
{
    stop();
    m_settings = settings;
    m_server = new QTcpServer(this);
    // _startup_on_demand(): the port asked for, or 9090 then random ones.
    bool listening = false;
    if (settings.port != 0) {
        listening = m_server->listen(QHostAddress::Any, settings.port);
    } else {
        for (int attempt = 0; attempt < 100 && !listening; ++attempt) {
            const quint16 port = attempt == 0 ? Wireless::defaultPort
                                              : quint16(QRandomGenerator::global()->bounded(8192, 65526));
            listening = m_server->listen(QHostAddress::Any, port);
        }
    }
    if (!listening) {
        *error = settings.port != 0 ? tr("Cannot listen on port %1: %2").arg(settings.port).arg(m_server->errorString())
                                    : tr("Cannot find a free port to listen on.");
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QTcpServer::newConnection, this, &WirelessServer::accept);
    if (settings.discovery) {
        // The first of Calibre's five ports another program has not taken.
        m_udp = new QUdpSocket(this);
        bool bound = false;
        for (const quint16 port : settings.broadcastPorts.isEmpty() ? Wireless::broadcastPorts() : settings.broadcastPorts) {
            if ((bound = m_udp->bind(QHostAddress::AnyIPv4, port)))
                break;
        }
        if (bound) {
            connect(m_udp, &QUdpSocket::readyRead, this, &WirelessServer::answerDatagrams);
        } else {
            delete m_udp;
            m_udp = nullptr;
        }
    }
    return true;
}

void WirelessServer::stop()
{
    if (m_connection)
        m_connection->close();
    delete m_udp;
    m_udp = nullptr;
    delete m_server;
    m_server = nullptr;
}

bool WirelessServer::isListening() const
{
    return m_server && m_server->isListening();
}

quint16 WirelessServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

quint16 WirelessServer::broadcastPort() const
{
    return m_udp ? m_udp->localPort() : 0;
}

WirelessConnection *WirelessServer::connection() const
{
    return m_connection;
}

void WirelessServer::setLibrary(const WirelessConnection::Settings &settings)
{
    m_settings.connection = settings;
}

void WirelessServer::accept()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (m_connection) {
            // One app at a time: the others hear who has Cousoir.
            socket->write(Wireless::frame(
                Wireless::CalibreBusy,
                Wireless::dict({ { QStringLiteral("otherDevice"), Value::from(m_connection->name()) } })));
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        auto *connection = new WirelessConnection(socket, m_settings.connection, this);
        m_connection = connection;
        connect(connection, &WirelessConnection::opened, this, [this, connection] { emit connected(connection); });
        connect(connection, &WirelessConnection::closed, this, [this, connection](const QString &reason) {
            if (m_connection == connection)
                m_connection = nullptr;
            connection->deleteLater();
            emit disconnected(reason);
        });
    }
}

void WirelessServer::answerDatagrams()
{
    // ConnectionListener.run(): any datagram is answered with the ports.
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_udp->receiveDatagram(1024);
        if (!datagram.isValid())
            continue;
        m_udp->writeDatagram(Wireless::broadcastReply(QHostInfo::localHostName(), m_settings.contentServerPort, port()),
                             datagram.senderAddress(), quint16(datagram.senderPort()));
    }
}
