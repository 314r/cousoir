#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Changes to files in the library that can be undone until the database that
// matches them is published. Each step goes to a journal, flushed to disk,
// before it happens; a crash leaves the journal, from which recover() puts
// the library back as it was, or finishes the change if the database had
// already been published. Files on their way in or out are kept beside their
// place under names starting with ".~", which Dropbox does not sync.
class FileTransaction
{
public:
    // `journal` is a file outside the library.
    explicit FileTransaction(const QString &journal);
    ~FileTransaction();
    FileTransaction(const FileTransaction &) = delete;
    FileTransaction &operator=(const FileTransaction &) = delete;

    // Moves a file or a folder to `to`, which must not exist, creating the
    // folders it needs. Both are in the library: a rename.
    bool move(const QString &from, const QString &to, QString *error);
    // Makes `folder` and the folders it needs.
    bool makeFolder(const QString &folder, QString *error);
    // Writes `contents` to `path`, replacing the file there.
    bool write(const QString &path, const QByteArray &contents, QString *error);
    // Copies the file `source`, from anywhere, to `path`, replacing the file
    // there.
    bool copy(const QString &source, const QString &path, QString *error);
    // Removes the file at `path`, if any.
    bool remove(const QString &path, QString *error);
    // Removes `folder` once the change is final, if nothing but the folder
    // caches of other systems is left in it.
    void removeFolderIfEmpty(const QString &folder);
    // A temporary file the change makes in the library, removed if it is
    // undone.
    bool addScratch(const QString &path, QString *error);

    // Puts every file back, in reverse order.
    bool rollback(QString *error);
    // Notes in the journal that the database whose contentHash() is `hash`
    // is about to replace the library's: from then on a recovery finishes the
    // change if the library's database has that hash. A journal of an
    // earlier Cousoir has the SHA-256 in hexadecimal, which recover() reads
    // too.
    bool aboutToPublish(const QString &hash, QString *error);
    // Makes the change final: kept files and empty folders go, and the
    // journal.
    void finish();
    // Forgets the change without undoing it, as a crash would, leaving the
    // journal for recover(). For tests.
    void abandon();

    // Rolls back or finishes the change a crash left in `journal`, for the
    // library database at `database`. Returns false with `error` when a file
    // could not be put back; `done` says what was done, empty without a
    // journal.
    static bool recover(const QString &journal, const QString &database, QString *done, QString *error);

    // Writes `contents` to `path`, replacing the file there, and flushes it
    // to the disk; outside any change.
    static bool writeDurably(const QString &path, const QByteArray &contents, QString *error);
    // Flushes a file, or a folder's entries, to the disk.
    static void sync(const QString &path);
    // How a file is copied. Through memory, a chunk at a time, leaves the
    // copy in the system's cache, for a copy read at once after. Sharing
    // its blocks, where the file system can, as btrfs and XFS do, is
    // instant but leaves nothing in the cache, so reading the copy then
    // goes to the disk: for a copy kept and not read, a backup.
    enum class Copy { ThroughMemory, SharingBlocks };
    // Copies the file `from` to `to`, replacing it. With `durable`, flushed
    // to the disk before it returns. Through memory, `hash` is set to the
    // contentHash() of what was copied.
    static bool copyFile(const QString &from, const QString &to, Copy how, bool durable, QString *error,
                         QString *hash = nullptr);
    // "xxh3:" and the XXH3-128 hash of the file's contents, in hexadecimal;
    // empty when it cannot be read.
    static QString contentHash(const QString &path);

private:
    struct Step
    {
        QString op;
        QString path;
        QString other;
    };

    bool log(const Step &step, QString *error);
    // Puts a new file at `path` once `fill` has written it under its
    // temporary name.
    bool place(const QString &path, const std::function<bool(const QString &, QString *)> &fill, QString *error);
    static bool undo(const Step &step, QString *error);
    // Undoes `steps` in reverse order, all of them even when one fails;
    // `error` is the first failure's.
    static bool undoAll(const QList<Step> &steps, QString *error);
    static void cleanUp(const QList<Step> &steps);
    static QString keptName(const QString &path);

    QString m_journal;
    QList<Step> m_steps;
    bool m_finished = false;
};
