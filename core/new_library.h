#pragma once

#include <QString>

// A new, empty Calibre library made without Calibre: its metadata.db as
// Calibre makes one (assets/calibre/new_library.sql, written from Calibre
// itself by tools/oracle/new_library_sql.py), with a uuid of its own.
// Calibre opens it as one of its own, and adds its notes folder then.
namespace NewLibrary {

// Makes the library in `folder`, which must not exist yet or be empty.
// False with `error`, nothing left behind.
bool create(const QString &folder, QString *error);

} // namespace NewLibrary
