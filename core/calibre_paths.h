#pragma once

#include <QString>

// Where Calibre keeps a book's files, ported from construct_path_name() and
// construct_file_name() in src/calibre/db/backend.py of Calibre 9.16.
namespace CalibrePaths {

// PATH_LIMIT: the longest name of a folder or file Calibre makes, except on
// Windows where it is 40.
inline constexpr int PathLimit = 100;

// "Boris Vian/L'Ecume des jours (1)": the book's folder, relative to the
// library. `unknown` names a book without an author, in Calibre's language.
QString constructPathName(int bookId, const QString &title, const QString &author, const QString &unknown);

// "L'Ecume des jours - Boris Vian": the name of the book's files, without
// their extension. `extensionLength` is the longest of their extensions,
// with its dot.
QString constructFileName(int bookId, const QString &title, const QString &author, int extensionLength,
                          const QString &unknown);

} // namespace CalibrePaths
