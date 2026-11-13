#include "new_library.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QUuid>

#include "sqlite_db.h"

namespace NewLibrary {

bool create(const QString &folder, QString *error)
{
    const QDir dir(folder);
    const bool existed = dir.exists();
    if (existed && !dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        *error = QObject::tr("%1 is not empty: a new library needs a folder of its own.").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    QFile sql(QStringLiteral(":/cousoir/calibre/new_library.sql"));
    if (!sql.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cousoir was built without Calibre's schema for new libraries.");
        return false;
    }
    if (!QDir().mkpath(folder)) {
        *error = QObject::tr("Cannot create the folder %1.").arg(QDir::toNativeSeparators(folder));
        return false;
    }
    // Made aside, then given its name: a library is there whole or not.
    const QString database = dir.filePath(QStringLiteral("metadata.db"));
    const QString making = dir.filePath(QStringLiteral(".metadata.db.cousoir-new"));
    const auto undo = [&] {
        QFile::remove(making);
        if (!existed)
            QDir().rmdir(folder);
        return false;
    };
    {
        QFile file(making);
        if (!file.open(QIODevice::WriteOnly)) {
            *error = QObject::tr("Cannot write in %1: %2").arg(QDir::toNativeSeparators(folder), file.errorString());
            return undo();
        }
    }
    {
        SqliteDb db;
        // Python's str(uuid4()), as Calibre gives each library its own.
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        if (!db.open(making, SqliteDb::Mode::ReadWrite, error)
            || !db.exec(QStringLiteral("BEGIN;") + QString::fromUtf8(sql.readAll()) + QStringLiteral("COMMIT;"), error)
            || !db.run(QStringLiteral("UPDATE library_id SET uuid=?"), { uuid }, error)) {
            db.close();
            return undo();
        }
    }
    if (!QFile::rename(making, database)) {
        *error = QObject::tr("Cannot create %1.").arg(QDir::toNativeSeparators(database));
        return undo();
    }
    return true;
}

} // namespace NewLibrary
