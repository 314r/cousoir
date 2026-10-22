#include "calibre_notes.h"

// Ported from Calibre 9.16, src/calibre/db/notes/connect.py, Copyright (C)
// Kovid Goyal, GPL v3: Notes.set_note(), set_backup_for(),
// path_to_retired_dir_for_item(), retire_entry(), remove_retired_entry(),
// trim_retired_dir() and hash_key().

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <algorithm>

#include <xxhash.h>

#include "calibre_db.h"
#include "calibre_fts.h"
#include "sqlite_db.h"

namespace CalibreNotes {

namespace {

// Between the parts of a note's backup.
const QByteArray separator("\0\x1c\0", 3);
constexpr int maxRetiredItems = 256;

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        *error = QObject::tr("Cannot write %1.").arg(path);
        return false;
    }
    return true;
}

// trim_retired_dir(): the oldest notes kept aside go past 256.
void trimRetired(const QString &retired)
{
    QStringList items;
    for (const QString &name : QDir(retired).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        items.append(QDir(retired).filePath(name));
    const qsizetype extra = items.size() - maxRetiredItems;
    if (extra <= 0)
        return;
    const auto key = [](const QString &path) {
        const QFileInfo noteId(QDir(path).filePath(QStringLiteral("note_id")));
        QFile file(noteId.filePath());
        const int id = file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toInt() : 0;
        return std::pair(noteId.lastModified().toMSecsSinceEpoch(), id);
    };
    std::sort(items.begin(), items.end(), [&key](const QString &a, const QString &b) { return key(a) < key(b); });
    for (qsizetype i = 0; i < extra; ++i)
        QDir(items.at(i)).removeRecursively();
}

} // namespace

QString hashKey(const QString &key)
{
    const QByteArray bytes = key.toUtf8();
    const XXH64_hash_t hash = XXH3_64bits(bytes.constData(), size_t(bytes.size()));
    return QStringLiteral("%1").arg(quint64(hash), 16, 16, QLatin1Char('0'));
}

bool apply(const QString &libraryPath, const QList<Change> &changes, const QString &interfaceLanguage, QString *error)
{
    const QDir folder(CalibreDb::notesFolder(libraryPath));
    const QString path = folder.filePath(QStringLiteral("notes.db"));
    if (!QFileInfo::exists(path)) {
        *error = QObject::tr("This library has no notes yet: open it once in Calibre 7 or later.");
        return false;
    }
    SqliteDb db;
    if (!db.open(path, SqliteDb::Mode::ReadWrite, error) || !CalibreFts::registerTokenizers(db.handle(), interfaceLanguage, error)
        || !db.exec(QStringLiteral("BEGIN IMMEDIATE"), error))
        return false;
    const auto undo = [&db](const QString &message, QString *error) {
        db.exec(QStringLiteral("ROLLBACK"), nullptr);
        if (error->isEmpty())
            *error = message;
        return false;
    };
    const QString backups = folder.filePath(QStringLiteral("backup"));
    const QString retired = folder.filePath(QStringLiteral("retired"));
    for (const Change &change : changes) {
        // path_to_retired_dir_for_item(): by the field and the item's name
        // in lower case.
        const QString retiredFolder
            = QDir(retired).filePath(hashKey(change.field + u' ' + change.itemValue.toLower()));
        const QString backup = QDir(backups).filePath(change.field + u'/' + QString::number(change.item));
        const QVariant noteId = db.value(QStringLiteral("SELECT id FROM notes WHERE item=%1 AND colname='%2'")
                                             .arg(change.item)
                                             .arg(QString(change.field).replace(u'\'', QStringLiteral("''"))));
        if (noteId.isValid()
            && !db.rows(QStringLiteral("SELECT resource FROM notes_resources_link WHERE note=?"), { noteId }).isEmpty())
            return undo(QObject::tr("This note has images: change it in Calibre."), error);
        // The item's name first, as a search finds it.
        const QString searchable = change.itemValue + u'\n' + change.searchable;
        if (change.html.isEmpty()) {
            if (!noteId.isValid())
                continue;
            if (!db.run(QStringLiteral("DELETE FROM notes WHERE id=?"), { noteId }, error))
                return undo({}, error);
            // retire_entry(): its backup kept aside, with its id.
            if (QFileInfo::exists(backup)) {
                QDir().mkpath(retiredFolder);
                const QString doc = QDir(retiredFolder).filePath(QStringLiteral("doc.html"));
                QFile::remove(doc);
                if (!QFile::rename(backup, doc)
                    || !writeFile(QDir(retiredFolder).filePath(QStringLiteral("note_id")), QByteArray::number(noteId.toInt()),
                                  error))
                    return undo(QObject::tr("Cannot keep the note removed aside."), error);
                trimRetired(retired);
            }
            continue;
        }
        if (!noteId.isValid()) {
            QDir(retiredFolder).removeRecursively();
            if (!db.run(QStringLiteral("INSERT INTO notes (item,colname,doc,searchable_text) VALUES (?,?,?,?)"),
                        { change.item, change.field, change.html, searchable }, error))
                return undo({}, error);
        } else if (!db.run(QStringLiteral("UPDATE notes SET doc=?,searchable_text=? WHERE id=?"),
                           { change.html, searchable, noteId }, error)) {
            return undo({}, error);
        }
        // set_backup_for(): the note, its text, its resources (none) and the
        // item's name.
        const QByteArray content = change.html.toUtf8() + separator + searchable.toUtf8() + separator + separator
            + change.itemValue.toUtf8();
        if (!writeFile(backup, content, error))
            return undo({}, error);
    }
    if (!db.exec(QStringLiteral("COMMIT"), error))
        return undo({}, error);
    return true;
}

} // namespace CalibreNotes
