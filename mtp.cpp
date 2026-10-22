#include "mtp.h"

#include <QDir>
#include <QFile>
#include <QMutex>
#include <QObject>

#include <cstdlib>
#include <mutex>

#ifdef COUSOIR_HAVE_LIBMTP
#include <libmtp.h>

namespace {

// DEVICE_FLAG_FORCE_RESET_ON_CLOSE, from libmtp's private device-flags.h.
constexpr uint32_t ForceResetOnClose = 0x10000000;

// libmtp keeps global state: initialised once, and called from one thread
// at a time.
QMutex &lock()
{
    static QMutex mutex;
    return mutex;
}

void initialise()
{
    static std::once_flag once;
    std::call_once(once, [] { LIBMTP_Init(); });
}

QString take(char *text)
{
    const QString value = text ? QString::fromUtf8(text) : QString();
    std::free(text);
    return value;
}

// The serial number the device on `bus` as `number` gives over USB, from
// Linux's list of USB devices.
QString usbSerialNumber(quint32 bus, quint8 number)
{
    const QDir usb(QStringLiteral("/sys/bus/usb/devices"));
    for (const QString &name : usb.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile busFile(usb.filePath(name + QStringLiteral("/busnum")));
        QFile numberFile(usb.filePath(name + QStringLiteral("/devnum")));
        if (!busFile.open(QIODevice::ReadOnly) || !numberFile.open(QIODevice::ReadOnly))
            continue;
        if (busFile.readAll().trimmed().toUInt() != bus || numberFile.readAll().trimmed().toUInt() != number)
            continue;
        QFile serial(usb.filePath(name + QStringLiteral("/serial")));
        return serial.open(QIODevice::ReadOnly) ? QString::fromUtf8(serial.readAll()).trimmed() : QString();
    }
    return {};
}

struct Progress
{
    const std::function<void(qint64, qint64)> *callback;
};

int reportProgress(uint64_t const sent, uint64_t const total, void const *const data)
{
    const auto *progress = static_cast<const Progress *>(data);
    if (progress && progress->callback && *progress->callback)
        (*progress->callback)(qint64(sent), qint64(total));
    return 0;
}

} // namespace

namespace Mtp {

QList<RawDevice> detect(QString *error)
{
    const QMutexLocker locker(&lock());
    initialise();
    LIBMTP_raw_device_t *raw = nullptr;
    int count = 0;
    const LIBMTP_error_number_t result = LIBMTP_Detect_Raw_Devices(&raw, &count);
    QList<RawDevice> devices;
    if (result == LIBMTP_ERROR_NO_DEVICE_ATTACHED)
        return devices;
    if (result != LIBMTP_ERROR_NONE) {
        if (error)
            *error = QObject::tr("Cannot look for MTP devices.");
        return devices;
    }
    for (int i = 0; i < count; ++i) {
        const LIBMTP_raw_device_t &device = raw[i];
        devices.append({ device.bus_location, device.devnum, device.device_entry.vendor_id, device.device_entry.product_id });
    }
    std::free(raw);
    return devices;
}

Session::Session() = default;

Session::~Session()
{
    close();
}

bool Session::open(quint32 bus, quint8 number, QString *error)
{
    close();
    const QMutexLocker locker(&lock());
    initialise();
    LIBMTP_raw_device_t *raw = nullptr;
    int count = 0;
    if (LIBMTP_Detect_Raw_Devices(&raw, &count) != LIBMTP_ERROR_NONE) {
        if (error)
            *error = QObject::tr("The device is no longer connected.");
        return false;
    }
    for (int i = 0; i < count && !m_device; ++i) {
        if (raw[i].bus_location == bus && raw[i].devnum == number) {
            // libmtp gives Kindles (1949:9981 is both the Scribe and the
            // Paperwhite Signature Edition) the Android bug flags, whose USB
            // reset on release drops the Kindle off the bus until it is
            // plugged in again.
            raw[i].device_entry.device_flags &= ~ForceResetOnClose;
            m_device = LIBMTP_Open_Raw_Device_Uncached(&raw[i]);
        }
    }
    std::free(raw);
    if (!m_device) {
        if (error)
            *error = QObject::tr("Cannot open the device: another program may be using it.");
        return false;
    }
    m_friendlyName = take(LIBMTP_Get_Friendlyname(m_device));
    m_modelName = take(LIBMTP_Get_Modelname(m_device));
    m_serialNumber = take(LIBMTP_Get_Serialnumber(m_device));
    if (m_serialNumber.isEmpty())
        m_serialNumber = usbSerialNumber(bus, number);
    return true;
}

void Session::close()
{
    if (!m_device)
        return;
    const QMutexLocker locker(&lock());
    LIBMTP_Release_Device(m_device);
    m_device = nullptr;
}

QString Session::friendlyName() const
{
    return m_friendlyName;
}

QString Session::modelName() const
{
    return m_modelName;
}

QString Session::serialNumber() const
{
    return m_serialNumber;
}

QList<Storage> Session::storages(QString *error)
{
    QList<Storage> list;
    if (!m_device)
        return list;
    const QMutexLocker locker(&lock());
    if (LIBMTP_Get_Storage(m_device, LIBMTP_STORAGE_SORTBY_NOTSORTED) < 0) {
        if (error)
            *error = QObject::tr("Cannot read the device's storage: %1").arg(errors());
        return list;
    }
    for (LIBMTP_devicestorage_t *storage = m_device->storage; storage; storage = storage->next) {
        // Access capability 0 is read-write.
        list.append({ storage->id, QString::fromUtf8(storage->StorageDescription ? storage->StorageDescription : ""),
                      qint64(storage->MaxCapacity), qint64(storage->FreeSpaceInBytes), storage->AccessCapability == 0 });
    }
    return list;
}

QList<Object> Session::children(quint32 storage, quint32 parent, QString *error)
{
    QList<Object> list;
    if (!m_device)
        return list;
    const QMutexLocker locker(&lock());
    LIBMTP_Clear_Errorstack(m_device);
    LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(m_device, storage, parent);
    for (LIBMTP_file_t *file = files; file;) {
        list.append({ file->item_id, file->parent_id, QString::fromUtf8(file->filename ? file->filename : ""),
                      file->filetype == LIBMTP_FILETYPE_FOLDER, qint64(file->filesize), qint64(file->modificationdate) });
        LIBMTP_file_t *next = file->next;
        LIBMTP_destroy_file_t(file);
        file = next;
    }
    if (!files) {
        const QString stack = errors();
        if (!stack.isEmpty() && error)
            *error = QObject::tr("Cannot list the device's files: %1").arg(stack);
    }
    return list;
}

bool Session::download(quint32 id, const QString &localPath, QString *error)
{
    if (!m_device)
        return false;
    QFile file(localPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(localPath, file.errorString());
        return false;
    }
    const QMutexLocker locker(&lock());
    if (LIBMTP_Get_File_To_File_Descriptor(m_device, id, file.handle(), nullptr, nullptr) != 0) {
        if (error)
            *error = QObject::tr("Cannot copy a file from the device: %1").arg(errors());
        return false;
    }
    return true;
}

bool Session::upload(quint32 storage, quint32 parent, const QString &name, const QString &localPath, Object *created,
                     QString *error, const std::function<void(qint64, qint64)> &progress)
{
    if (!m_device)
        return false;
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read %1: %2").arg(localPath, file.errorString());
        return false;
    }
    QByteArray filename = name.toUtf8();
    LIBMTP_file_t metadata{};
    // In the root as Calibre's driver puts a file there.
    metadata.parent_id = parent ? parent : Root;
    metadata.storage_id = storage;
    metadata.item_id = 0;
    metadata.filename = filename.data();
    metadata.filetype = LIBMTP_FILETYPE_UNKNOWN;
    metadata.filesize = uint64_t(file.size());
    const Progress data{ &progress };
    const QMutexLocker locker(&lock());
    if (LIBMTP_Send_File_From_File_Descriptor(m_device, file.handle(), &metadata, reportProgress, &data) != 0) {
        if (error)
            *error = QObject::tr("Cannot copy %1 to the device: %2").arg(name, errors());
        return false;
    }
    if (created)
        *created = { metadata.item_id, parent, name, false, file.size(), 0 };
    return true;
}

bool Session::makeFolder(quint32 storage, quint32 parent, const QString &name, quint32 *id, QString *error)
{
    if (!m_device)
        return false;
    QByteArray folder = name.toUtf8();
    const QMutexLocker locker(&lock());
    const uint32_t made = LIBMTP_Create_Folder(m_device, folder.data(), parent, storage);
    if (made == 0) {
        if (error)
            *error = QObject::tr("Cannot make the folder %1 on the device: %2").arg(name, errors());
        return false;
    }
    if (id)
        *id = made;
    return true;
}

bool Session::remove(quint32 id, QString *error)
{
    if (!m_device)
        return false;
    const QMutexLocker locker(&lock());
    if (LIBMTP_Delete_Object(m_device, id) != 0) {
        if (error)
            *error = QObject::tr("Cannot delete a file on the device: %1").arg(errors());
        return false;
    }
    return true;
}

QString Session::errors()
{
    QStringList texts;
    for (LIBMTP_error_t *error = LIBMTP_Get_Errorstack(m_device); error; error = error->next) {
        if (error->error_text)
            texts.append(QString::fromUtf8(error->error_text).trimmed());
    }
    LIBMTP_Clear_Errorstack(m_device);
    return texts.join(QLatin1String("; "));
}

} // namespace Mtp

#else

// Without libmtp, there is no MTP device to find.
namespace Mtp {

QList<RawDevice> detect(QString *)
{
    return {};
}

Session::Session() = default;

Session::~Session() = default;

bool Session::open(quint32, quint8, QString *error)
{
    if (error)
        *error = QObject::tr("Cousoir was built without libmtp, which reaches MTP devices.");
    return false;
}

void Session::close() {}

QString Session::friendlyName() const
{
    return {};
}

QString Session::modelName() const
{
    return {};
}

QString Session::serialNumber() const
{
    return {};
}

QList<Storage> Session::storages(QString *)
{
    return {};
}

QList<Object> Session::children(quint32, quint32, QString *)
{
    return {};
}

bool Session::download(quint32, const QString &, QString *)
{
    return false;
}

bool Session::upload(quint32, quint32, const QString &, const QString &, Object *, QString *,
                     const std::function<void(qint64, qint64)> &)
{
    return false;
}

bool Session::makeFolder(quint32, quint32, const QString &, quint32 *, QString *)
{
    return false;
}

bool Session::remove(quint32, QString *)
{
    return false;
}

QString Session::errors()
{
    return {};
}

} // namespace Mtp

#endif
