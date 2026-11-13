#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Writing the notes Calibre 7 keeps on items, authors, series, tags and
// publishers, in the library's .calnotes folder, as its Notes class writes
// them: the note in notes.db, indexed with Calibre's tokenizer, a backup of
// it beside, and a note removed kept aside ("retired"), for Calibre to bring
// back should the item come back. Notes with images are left to Calibre.
namespace CalibreNotes {

struct Change
{
    // Calibre's field: "authors", "series", "tags", "publisher".
    QString field;
    int item = 0;
    // The item's name.
    QString itemValue;
    // The note's HTML; empty to remove it.
    QString html;
    // Its text, as a search finds it.
    QString searchable;
};

// set_note() for each change, in one transaction of notes.db. False, with
// `error`, when the library has no notes.db (Calibre 7 never opened it) or
// a note to change has images.
bool apply(const QString &libraryPath, const QList<Change> &changes, const QString &interfaceLanguage, QString *error);

// hash_key(): the xxh3 64 bits digest of `key` in hexadecimal.
QString hashKey(const QString &key);

} // namespace CalibreNotes
