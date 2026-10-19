#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

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
    // Notes in the journal that the database whose SHA-256 is `sha256` is
    // about to replace the library's: from then on a recovery finishes the
    // change if the library's database has that hash.
    bool aboutToPublish(const QByteArray &sha256, QString *error);
    // Makes the change final: kept files and empty folders go, and the
    // journal.
    bool finish(QString *error);
    // Forgets the change without undoing it, as a crash would, leaving the
    // journal for recover(). For tests.
    void abandon();

    // Rolls back or finishes the change a crash left in `journal`, for the
    // library database at `database`. Returns false with `error` when a file
    // could not be put back; `done` says what was done, empty without a
    // journal.
    static bool recover(const QString &journal, const QString &database, QString *done, QString *error);

private:
    struct Step
    {
        QString op;
        QString path;
        QString other;
    };

    bool log(const Step &step, QString *error);
    static bool undo(const Step &step, QString *error);
    static void cleanUp(const QList<Step> &steps);
    static QString keptName(const QString &path);

    QString m_journal;
    QList<Step> m_steps;
    bool m_finished = false;
};
