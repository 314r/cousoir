#include "device_files.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>

#include "mtp.h"

QList<DeviceFiles::Entry> DeviceFiles::entries() const
{
    QList<Entry> list = m_entries.values();
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.path < b.path; });
    return list;
}

const DeviceFiles::Entry *DeviceFiles::find(const QString &path) const
{
    const auto found = m_entries.constFind(key(path));
    return found == m_entries.cend() ? nullptr : &found.value();
}

QList<DeviceFiles::Entry> DeviceFiles::children(const QString &folder) const
{
    const QString parent = key(folder);
    QList<Entry> list;
    for (const Entry &entry : m_entries) {
        if (key(parentOf(entry.path)) == parent)
            list.append(entry);
    }
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.path < b.path; });
    return list;
}

quint32 DeviceFiles::storageId() const
{
    return m_storageId;
}

QString DeviceFiles::storageName() const
{
    return m_storageName;
}

qint64 DeviceFiles::freeBytes() const
{
    return m_freeBytes;
}

qint64 DeviceFiles::totalBytes() const
{
    return m_totalBytes;
}

bool DeviceFiles::ignored(const QStringList &path, bool kindle)
{
    if (path.isEmpty())
        return false;
    QStringList lower;
    for (const QString &name : path)
        lower.append(name.toLower());
    if (kindle && lower.last().endsWith(QLatin1String(".sdr")))
        return true;
    static const QSet<QString> top{
        QStringLiteral("alarms"),     QStringLiteral("dcim"),        QStringLiteral("movies"),
        QStringLiteral("music"),      QStringLiteral("notifications"), QStringLiteral("screenshots"),
        QStringLiteral("pictures"),   QStringLiteral("ringtones"),   QStringLiteral("samsung"),
        QStringLiteral("sony"),       QStringLiteral("htc"),         QStringLiteral("bluetooth"),
        QStringLiteral("fonts"),      QStringLiteral("games"),       QStringLiteral("lost.dir"),
        QStringLiteral("video"),      QStringLiteral("whatsapp"),    QStringLiteral("image"),
        QStringLiteral("com.zinio.mobile.android.reader"),
    };
    const QString &first = lower.first();
    if (top.contains(first))
        return true;
    if (first.startsWith(u'.') && first != QLatin1String(".tolino") && first != QLatin1String(".notebooks"))
        return true;
    // A Kindle's system/thumbnails holds the covers of the books sent.
    if (first == QLatin1String("system") && !kindle)
        return true;
    if (lower.size() > 1) {
        if (first == QLatin1String("android")) {
            if (lower.at(1) != QLatin1String("data"))
                return true;
            if (lower.size() > 2 && lower.at(2) != QLatin1String("com.amazon.kindle"))
                return true;
        } else if (first == QLatin1String("system") && lower.at(1) != QLatin1String("thumbnails")) {
            return true;
        }
    }
    return false;
}

void DeviceFiles::clear()
{
    m_entries.clear();
}

void DeviceFiles::add(const Entry &entry)
{
    m_entries.insert(key(entry.path), entry);
}

void DeviceFiles::drop(const QString &path)
{
    m_entries.remove(key(path));
}

QString DeviceFiles::key(const QString &path)
{
    return path.toLower();
}

QString DeviceFiles::parentOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

QString DeviceFiles::nameOf(const QString &path)
{
    return path.mid(path.lastIndexOf(u'/') + 1);
}

MtpFiles::MtpFiles(Mtp::Session *session, bool kindle)
    : m_session(session)
    , m_kindle(kindle)
{
}

bool MtpFiles::scan(QString *error)
{
    clear();
    QList<Mtp::Storage> storages = m_session->storages(error);
    std::sort(storages.begin(), storages.end(), [](const Mtp::Storage &a, const Mtp::Storage &b) { return a.id < b.id; });
    storages.removeIf([](const Mtp::Storage &storage) { return !storage.writable; });
    if (storages.isEmpty()) {
        if (error && error->isEmpty())
            *error = QObject::tr("The device has no storage it lets write to.");
        return false;
    }
    const Mtp::Storage &main = storages.first();
    m_storageId = main.id;
    m_storageName = main.description;
    m_freeBytes = main.freeBytes;
    m_totalBytes = main.capacity;
    return scanFolder(Mtp::Root, {}, error);
}

bool MtpFiles::scanFolder(quint32 parent, const QString &path, QString *error)
{
    QString listError;
    const QList<Mtp::Object> objects = m_session->children(m_storageId, parent, &listError);
    if (!listError.isEmpty()) {
        if (error)
            *error = listError;
        return false;
    }
    for (const Mtp::Object &object : objects) {
        const QString child = path.isEmpty() ? object.name : path + u'/' + object.name;
        add({ child, object.folder, object.size, object.modified, object.id, object.parent == Mtp::Root ? 0 : object.parent });
        if (object.folder && !ignored(child.split(u'/'), m_kindle) && !scanFolder(object.id, child, error))
            return false;
    }
    return true;
}

bool MtpFiles::read(const QString &path, const QString &localPath, QString *error)
{
    const Entry *entry = find(path);
    if (!entry || entry->folder) {
        if (error)
            *error = QObject::tr("There is no file %1 on the device.").arg(path);
        return false;
    }
    return m_session->download(entry->id, localPath, error);
}

bool MtpFiles::write(const QString &path, const QString &localPath, QString *error,
                     const std::function<void(qint64, qint64)> &progress)
{
    const QString folder = parentOf(path);
    const Entry *parent = folder.isEmpty() ? nullptr : find(folder);
    if (!folder.isEmpty() && (!parent || !parent->folder)) {
        if (error)
            *error = QObject::tr("There is no folder %1 on the device.").arg(folder);
        return false;
    }
    const quint32 parentId = parent ? parent->id : 0;
    if (const Entry *existing = find(path)) {
        if (existing->folder) {
            if (error)
                *error = QObject::tr("%1 is a folder on the device.").arg(path);
            return false;
        }
        if (!m_session->remove(existing->id, error))
            return false;
        drop(path);
    }
    // Under the name in the folder's case.
    const QString name = nameOf(path);
    const QString placed = parent ? parent->path + u'/' + name : name;
    Mtp::Object created;
    if (!m_session->upload(m_storageId, parentId, name, localPath, &created, error, progress))
        return false;
    add({ placed, false, QFileInfo(localPath).size(), QDateTime::currentSecsSinceEpoch(), created.id, parentId });
    return true;
}

bool MtpFiles::makeFolder(const QString &path, QString *error)
{
    if (const Entry *existing = find(path))
        return existing->folder;
    const QString folder = parentOf(path);
    const Entry *parent = folder.isEmpty() ? nullptr : find(folder);
    if (!folder.isEmpty() && (!parent || !parent->folder)) {
        if (error)
            *error = QObject::tr("There is no folder %1 on the device.").arg(folder);
        return false;
    }
    quint32 id = 0;
    const QString name = nameOf(path);
    if (!m_session->makeFolder(m_storageId, parent ? parent->id : 0, name, &id, error))
        return false;
    add({ parent ? parent->path + u'/' + name : name, true, 0, QDateTime::currentSecsSinceEpoch(), id, parent ? parent->id : 0 });
    return true;
}

bool MtpFiles::remove(const QString &path, QString *error)
{
    const Entry *entry = find(path);
    if (!entry)
        return true;
    if (!m_session->remove(entry->id, error))
        return false;
    drop(path);
    return true;
}

FolderFiles::FolderFiles(const QString &root, bool kindle)
    : m_root(QDir::cleanPath(root))
    , m_kindle(kindle)
{
}

bool FolderFiles::scan(QString *error)
{
    clear();
    if (!QFileInfo(m_root).isDir()) {
        if (error)
            *error = QObject::tr("The device is no longer connected.");
        return false;
    }
    m_nextId = 1;
    m_storageId = 0x10001;
    m_storageName = QStringLiteral("Internal Storage");
    const QStorageInfo storage(m_root);
    m_freeBytes = storage.bytesAvailable();
    m_totalBytes = storage.bytesTotal();
    scanFolder({}, 0);
    return true;
}

void FolderFiles::scanFolder(const QString &path, quint32 parent)
{
    const QDir folder(path.isEmpty() ? m_root : QDir(m_root).filePath(path));
    const QFileInfoList infos = folder.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : infos) {
        const QString child = path.isEmpty() ? info.fileName() : path + u'/' + info.fileName();
        const quint32 id = m_nextId++;
        add({ child, info.isDir(), info.isDir() ? 0 : info.size(), info.lastModified().toSecsSinceEpoch(), id, parent });
        if (info.isDir() && !ignored(child.split(u'/'), m_kindle))
            scanFolder(child, id);
    }
}

QString FolderFiles::actual(const QString &path) const
{
    const Entry *entry = find(path);
    if (entry)
        return QDir(m_root).filePath(entry->path);
    const QString folder = parentOf(path);
    const Entry *parent = folder.isEmpty() ? nullptr : find(folder);
    const QString placed = parent ? parent->path + u'/' + nameOf(path) : path;
    return QDir(m_root).filePath(placed);
}

bool FolderFiles::read(const QString &path, const QString &localPath, QString *error)
{
    const Entry *entry = find(path);
    QFile::remove(localPath);
    if (!entry || entry->folder || !QFile::copy(QDir(m_root).filePath(entry->path), localPath)) {
        if (error)
            *error = QObject::tr("Cannot copy %1 from the device.").arg(path);
        return false;
    }
    return true;
}

bool FolderFiles::write(const QString &path, const QString &localPath, QString *error,
                        const std::function<void(qint64, qint64)> &progress)
{
    const QString folder = parentOf(path);
    const Entry *parent = folder.isEmpty() ? nullptr : find(folder);
    if (!folder.isEmpty() && (!parent || !parent->folder)) {
        if (error)
            *error = QObject::tr("There is no folder %1 on the device.").arg(folder);
        return false;
    }
    const QString target = actual(path);
    const Entry *existing = find(path);
    const quint32 id = existing ? existing->id : m_nextId++;
    QFile::remove(target);
    if (!QFile::copy(localPath, target)) {
        if (error)
            *error = QObject::tr("Cannot copy %1 to the device.").arg(path);
        return false;
    }
    const qint64 size = QFileInfo(target).size();
    if (progress)
        progress(size, size);
    add({ QDir(m_root).relativeFilePath(target), false, size, QFileInfo(target).lastModified().toSecsSinceEpoch(), id,
          parent ? parent->id : 0 });
    return true;
}

bool FolderFiles::makeFolder(const QString &path, QString *error)
{
    if (const Entry *existing = find(path))
        return existing->folder;
    const QString folder = parentOf(path);
    const Entry *parent = folder.isEmpty() ? nullptr : find(folder);
    if (!folder.isEmpty() && (!parent || !parent->folder)) {
        if (error)
            *error = QObject::tr("There is no folder %1 on the device.").arg(folder);
        return false;
    }
    const QString target = actual(path);
    if (!QDir().mkdir(target)) {
        if (error)
            *error = QObject::tr("Cannot make the folder %1 on the device.").arg(path);
        return false;
    }
    add({ QDir(m_root).relativeFilePath(target), true, 0, QDateTime::currentSecsSinceEpoch(), m_nextId++,
          parent ? parent->id : 0 });
    return true;
}

bool FolderFiles::remove(const QString &path, QString *error)
{
    const Entry *entry = find(path);
    if (!entry)
        return true;
    const QString target = QDir(m_root).filePath(entry->path);
    if (entry->folder ? !QDir().rmdir(target) : !QFile::remove(target)) {
        if (error)
            *error = QObject::tr("Cannot delete %1 on the device.").arg(path);
        return false;
    }
    drop(path);
    return true;
}

QString FolderFiles::root() const
{
    return m_root;
}
