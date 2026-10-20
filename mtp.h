#pragma once

#include <QList>
#include <QString>

#include <functional>

struct LIBMTP_mtpdevice_struct;

// A reader that speaks MTP, as a Kindle Paperwhite does, through libmtp, the
// library Calibre's MTP driver uses: its storages, the files and folders in
// them by object id, and the copies to and from it. A session is MTP's one
// connection to the device at a time: Cousoir opens one for each thing it
// does, so that Calibre or a file manager can have the device in between.
// Calls block; they belong on a thread of their own.
namespace Mtp {

// An MTP device on USB, found without opening it.
struct RawDevice
{
    quint32 bus = 0;
    quint8 number = 0;
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString vendor;
    QString product;
};

// The devices libmtp finds, by its list of known devices and by asking
// those it does not know whether they speak MTP.
QList<RawDevice> detect(QString *error);

struct Storage
{
    quint32 id = 0;
    QString description;
    qint64 capacity = 0;
    qint64 freeBytes = 0;
    bool writable = true;
};

struct Object
{
    quint32 id = 0;
    quint32 parent = 0;
    quint32 storage = 0;
    QString name;
    bool folder = false;
    qint64 size = 0;
    // Seconds since the epoch.
    qint64 modified = 0;
};

// The parent that stands for a storage's root when listing it.
constexpr quint32 Root = 0xffffffff;

class Session
{
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Opens the device on USB bus `bus` as number `number`. Letting go of it
    // leaves it on the bus, ready to be opened again.
    bool open(quint32 bus, quint8 number, QString *error);
    void close();
    bool isOpen() const;

    QString friendlyName() const;
    QString modelName() const;
    QString manufacturer() const;
    // As Calibre has it: the one the device gives over MTP, or else over USB.
    QString serialNumber() const;

    // Its storages, the first its main memory, with their room left now.
    QList<Storage> storages(QString *error);
    // The files and folders in folder `parent` of storage `storage`.
    QList<Object> children(quint32 storage, quint32 parent, QString *error);
    // Copies object `id` to `localPath`.
    bool download(quint32 id, const QString &localPath, QString *error);
    // Copies `localPath` into folder `parent` as `name`; `parent` is 0 for the
    // root, as Calibre's driver sends to it. `progress` hears of the bytes
    // sent.
    bool upload(quint32 storage, quint32 parent, const QString &name, const QString &localPath, Object *created,
                QString *error, const std::function<void(qint64, qint64)> &progress = {});
    // Makes folder `name` in `parent`, 0 for the root.
    bool makeFolder(quint32 storage, quint32 parent, const QString &name, quint32 *id, QString *error);
    bool remove(quint32 id, QString *error);

private:
    // libmtp's errors since the last call, and none left after.
    QString errors();

    LIBMTP_mtpdevice_struct *m_device = nullptr;
    QString m_friendlyName;
    QString m_modelName;
    QString m_manufacturer;
    QString m_serialNumber;
};

} // namespace Mtp
