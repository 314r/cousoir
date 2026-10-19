#include "file_transaction.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRandomGenerator>

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// Flushes a file, or a folder's entries, to the disk.
void sync(const QString &path)
{
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    Q_UNUSED(path);
#endif
}

bool rename(const QString &from, const QString &to, QString *error)
{
    // std::rename replaces `to`; QFile::rename would refuse it.
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) != 0) {
        if (error)
            *error = QObject::tr("Cannot move %1 to %2: %3").arg(from, to, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    return true;
}

// remove_dir_if_empty(ignore_metadata_caches=True): a folder left with only
// .DS_Store or Thumbs.db goes too.
void removeFolderIfEmpty(const QString &folder)
{
    QDir dir(folder);
    if (!dir.exists())
        return;
    const QStringList entries = dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString lower = entry.toLower();
        if (lower != QLatin1String(".ds_store") && lower != QLatin1String("thumbs.db"))
            return;
    }
    for (const QString &entry : entries)
        QFileInfo(dir.filePath(entry)).isDir() ? QDir(dir.filePath(entry)).removeRecursively()
                                                : QFile::remove(dir.filePath(entry));
    QDir().rmdir(folder);
}

QString newName(const QString &kept)
{
    // The kept name with "-new-" in place of "-old-": where a write puts the
    // file before it takes its place.
    QFileInfo info(kept);
    return info.dir().filePath(info.fileName().replace(QLatin1String("-old-"), QLatin1String("-new-")));
}

} // namespace

FileTransaction::FileTransaction(const QString &journal)
    : m_journal(journal)
{
}

FileTransaction::~FileTransaction()
{
    // A transaction dropped half way is undone; its journal stays if that
    // fails, for the next start.
    if (!m_finished && !m_steps.isEmpty())
        rollback(nullptr);
}

QString FileTransaction::keptName(const QString &path)
{
    const QFileInfo info(path);
    const QString id = QString::number(QRandomGenerator::global()->generate(), 16);
    return info.dir().filePath(QStringLiteral(".~reliure-%1-old-%2").arg(id, info.fileName()));
}

bool FileTransaction::log(const Step &step, QString *error)
{
    QDir().mkpath(QFileInfo(m_journal).absolutePath());
    QFile file(m_journal);
    if (!file.open(QIODevice::Append)) {
        if (error)
            *error = QObject::tr("Cannot write the journal %1: %2").arg(m_journal, file.errorString());
        return false;
    }
    const QJsonObject line{ { QStringLiteral("op"), step.op }, { QStringLiteral("path"), step.path },
                            { QStringLiteral("other"), step.other } };
    file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    file.flush();
#ifdef Q_OS_UNIX
    ::fsync(file.handle());
#endif
    m_steps.append(step);
    return true;
}

bool FileTransaction::makeFolder(const QString &folder, QString *error)
{
    // The missing folders, from the outermost, each undone on its own.
    QStringList missing;
    for (QString path = QDir::cleanPath(folder); !QFileInfo::exists(path); path = QFileInfo(path).absolutePath())
        missing.prepend(path);
    for (const QString &path : std::as_const(missing)) {
        if (!log({ QStringLiteral("mkdir"), path, {} }, error))
            return false;
        if (!QDir().mkdir(path)) {
            if (error)
                *error = QObject::tr("Cannot create the folder %1.").arg(path);
            return false;
        }
    }
    return true;
}

bool FileTransaction::move(const QString &from, const QString &to, QString *error)
{
    if (QFileInfo::exists(to)) {
        if (error)
            *error = QObject::tr("%1 is already there.").arg(to);
        return false;
    }
    return makeFolder(QFileInfo(to).absolutePath(), error) && log({ QStringLiteral("move"), from, to }, error)
        && rename(from, to, error);
}

bool FileTransaction::write(const QString &path, const QByteArray &contents, QString *error)
{
    const QString kept = QFileInfo::exists(path) ? keptName(path) : QString();
    const QString fresh = newName(kept.isEmpty() ? keptName(path) : kept);
    if (!makeFolder(QFileInfo(path).absolutePath(), error)
        || !log({ QStringLiteral("write"), path, kept.isEmpty() ? fresh : kept }, error))
        return false;
    QFile file(fresh);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.flush()) {
        if (error)
            *error = QObject::tr("Cannot write %1: %2").arg(fresh, file.errorString());
        return false;
    }
#ifdef Q_OS_UNIX
    ::fsync(file.handle());
#endif
    file.close();
    // Keep the file's permissions.
    if (!kept.isEmpty())
        QFile::setPermissions(fresh, QFileInfo(path).permissions());
    if (!kept.isEmpty() && !rename(path, kept, error))
        return false;
    return rename(fresh, path, error);
}

bool FileTransaction::remove(const QString &path, QString *error)
{
    if (!QFileInfo::exists(path))
        return true;
    const QString kept = keptName(path);
    return log({ QStringLiteral("remove"), path, kept }, error) && rename(path, kept, error);
}

bool FileTransaction::addScratch(const QString &path, QString *error)
{
    return log({ QStringLiteral("scratch"), path, {} }, error);
}

void FileTransaction::removeFolderIfEmpty(const QString &folder)
{
    // Done at finish(): until then the folder may be needed for a rollback.
    log({ QStringLiteral("rmdir"), folder, {} }, nullptr);
}

bool FileTransaction::undo(const Step &step, QString *error)
{
    if (step.op == QLatin1String("move")) {
        if (QFileInfo::exists(step.other) && !QFileInfo::exists(step.path))
            return rename(step.other, step.path, error);
    } else if (step.op == QLatin1String("mkdir")) {
        QDir().rmdir(step.path);
    } else if (step.op == QLatin1String("write")) {
        // `other` is the old file when there was one, else the new one on
        // its way in.
        const QFileInfo other(step.other);
        if (other.fileName().contains(QLatin1String("-old-"))) {
            QFile::remove(newName(step.other));
            if (QFileInfo::exists(step.other))
                return rename(step.other, step.path, error);
        } else {
            QFile::remove(step.other);
            QFile::remove(step.path);
        }
    } else if (step.op == QLatin1String("remove")) {
        if (QFileInfo::exists(step.other))
            return rename(step.other, step.path, error);
    } else if (step.op == QLatin1String("scratch")) {
        QFile::remove(step.path);
    }
    return true;
}

void FileTransaction::cleanUp(const QList<Step> &steps)
{
    for (const Step &step : steps) {
        if ((step.op == QLatin1String("write") || step.op == QLatin1String("remove"))
            && QFileInfo(step.other).fileName().contains(QLatin1String("-old-"))) {
            // A kept folder is an empty one Reliure cleared.
            if (QFileInfo(step.other).isDir())
                QDir(step.other).removeRecursively();
            else
                QFile::remove(step.other);
        } else if (step.op == QLatin1String("scratch")) {
            QFile::remove(step.path);
        }
    }
    for (const Step &step : steps) {
        if (step.op == QLatin1String("rmdir"))
            ::removeFolderIfEmpty(step.path);
    }
}

bool FileTransaction::rollback(QString *error)
{
    bool ok = true;
    for (qsizetype index = m_steps.size() - 1; index >= 0; --index) {
        QString stepError;
        if (!undo(m_steps.at(index), &stepError)) {
            ok = false;
            if (error && error->isEmpty())
                *error = stepError;
        }
    }
    m_steps.clear();
    m_finished = true;
    if (ok)
        QFile::remove(m_journal);
    return ok;
}

bool FileTransaction::aboutToPublish(const QByteArray &sha256, QString *error)
{
    return log({ QStringLiteral("publish"), {}, QString::fromLatin1(sha256.toHex()) }, error);
}

bool FileTransaction::finish(QString *)
{
    cleanUp(m_steps);
    QStringList folders;
    for (const Step &step : std::as_const(m_steps)) {
        if (step.op == QLatin1String("move"))
            folders << QFileInfo(step.path).absolutePath() << QFileInfo(step.other).absolutePath();
    }
    folders.removeDuplicates();
    for (const QString &folder : std::as_const(folders))
        sync(folder);
    m_steps.clear();
    m_finished = true;
    QFile::remove(m_journal);
    return true;
}

void FileTransaction::abandon()
{
    m_steps.clear();
    m_finished = true;
}

bool FileTransaction::recover(const QString &journal, const QString &database, QString *done, QString *error)
{
    QFile file(journal);
    if (!file.open(QIODevice::ReadOnly))
        return true;
    QList<Step> steps;
    QByteArray published;
    while (!file.atEnd()) {
        const QJsonObject line = QJsonDocument::fromJson(file.readLine()).object();
        if (line.isEmpty())
            continue;
        const Step step{ line.value(QLatin1String("op")).toString(), line.value(QLatin1String("path")).toString(),
                         line.value(QLatin1String("other")).toString() };
        if (step.op == QLatin1String("publish"))
            published = step.other.toLatin1();
        steps.append(step);
    }
    file.close();

    QByteArray current;
    if (!published.isEmpty()) {
        QFile db(database);
        if (db.open(QIODevice::ReadOnly))
            current = QCryptographicHash::hash(db.readAll(), QCryptographicHash::Sha256).toHex();
    }
    if (!published.isEmpty() && current == published) {
        cleanUp(steps);
        if (done)
            *done = QObject::tr("finished a change that was interrupted after the database was saved");
        QFile::remove(journal);
        return true;
    }
    bool ok = true;
    for (qsizetype index = steps.size() - 1; index >= 0; --index) {
        QString stepError;
        if (!undo(steps.at(index), &stepError)) {
            ok = false;
            if (error && error->isEmpty())
                *error = stepError;
        }
    }
    if (done)
        *done = QObject::tr("undid a change that was interrupted before the database was saved");
    if (ok)
        QFile::remove(journal);
    return ok;
}
