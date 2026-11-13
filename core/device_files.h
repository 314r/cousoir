#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Mtp {
class Session;
}

// The files of a reader's main storage, by path from its root, / separated,
// found whatever the case of their names, as Calibre's MTP driver finds them:
// over MTP for a Kindle Paperwhite, or in a folder that stands for one in the
// tests. Its folders are read once by scan(), then follow the changes made
// through it.
class DeviceFiles
{
public:
    struct Entry
    {
        QString path;
        bool folder = false;
        qint64 size = 0;
        // Seconds since the epoch.
        qint64 modified = 0;
        // MTP's object ids; the storage's root is the parent 0.
        quint32 id = 0;
        quint32 parent = 0;
    };

    virtual ~DeviceFiles() = default;

    // Reads every file and folder again, but in the folders Calibre ignores,
    // which are there without their contents.
    virtual bool scan(QString *error) = 0;
    QList<Entry> entries() const;
    const Entry *find(const QString &path) const;
    // The files and folders right in `folder`, "" for the root.
    QList<Entry> children(const QString &folder) const;

    // Copies the file at `path` to `localPath`.
    virtual bool read(const QString &path, const QString &localPath, QString *error) = 0;
    // Puts `localPath` at `path`, replacing the file there; its folder must
    // be there.
    virtual bool write(const QString &path, const QString &localPath, QString *error,
                       const std::function<void(qint64, qint64)> &progress = {}) = 0;
    // Makes the folder at `path`, whose parent must be there; nothing if it
    // is.
    virtual bool makeFolder(const QString &path, QString *error) = 0;
    // Deletes a file or an empty folder.
    virtual bool remove(const QString &path, QString *error) = 0;

    // The storage: MTP's id and name, and its room.
    quint32 storageId() const;
    QString storageName() const;
    qint64 freeBytes() const;
    qint64 totalBytes() const;

    // is_folder_ignored() of Calibre's MTP driver, with its default rules,
    // for the folder at `path`, split; `kindle` for an Amazon device.
    static bool ignored(const QStringList &path, bool kindle);

protected:
    void clear();
    void add(const Entry &entry);
    void drop(const QString &path);
    static QString key(const QString &path);
    static QString parentOf(const QString &path);
    static QString nameOf(const QString &path);

    QHash<QString, Entry> m_entries;
    quint32 m_storageId = 0;
    QString m_storageName;
    qint64 m_freeBytes = 0;
    qint64 m_totalBytes = 0;
};

// A Kindle's storage over MTP, through a session opened before and kept open
// while it is used.
class MtpFiles : public DeviceFiles
{
public:
    // `kindle` for Calibre's rules for Amazon devices.
    MtpFiles(Mtp::Session *session, bool kindle);
    bool scan(QString *error) override;
    bool read(const QString &path, const QString &localPath, QString *error) override;
    bool write(const QString &path, const QString &localPath, QString *error,
               const std::function<void(qint64, qint64)> &progress = {}) override;
    bool makeFolder(const QString &path, QString *error) override;
    bool remove(const QString &path, QString *error) override;

private:
    bool scanFolder(quint32 parent, const QString &path, QString *error);

    Mtp::Session *m_session;
    bool m_kindle;
};

// A folder that stands for a device's storage, with made-up object ids.
class FolderFiles : public DeviceFiles
{
public:
    FolderFiles(const QString &root, bool kindle);
    bool scan(QString *error) override;
    bool read(const QString &path, const QString &localPath, QString *error) override;
    bool write(const QString &path, const QString &localPath, QString *error,
               const std::function<void(qint64, qint64)> &progress = {}) override;
    bool makeFolder(const QString &path, QString *error) override;
    bool remove(const QString &path, QString *error) override;

private:
    void scanFolder(const QString &path, quint32 parent);
    QString actual(const QString &path) const;

    QString m_root;
    bool m_kindle;
    quint32 m_nextId = 1;
};
