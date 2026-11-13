#pragma once

#include <QString>

#include "calibre_text.h"

// What Cousoir reads of Calibre's own configuration on this computer: the
// library Calibre opens, its interface language and the tweaks that change
// how titles and authors sort. Calibre's settings can differ per computer
// even though the library is shared.
struct CalibreConfig
{
    QString libraryPath;
    // Calibre's get_lang(): CALIBRE_OVERRIDE_LANG, then the language picked
    // in Calibre's preferences, then the system locale. It drives title_sort
    // in Calibre's SQL triggers.
    QString interfaceLanguage;
    CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    // Adding books, as set in Calibre's preferences: how a file's name gives
    // its title and authors, whether the file's own metadata is read, the
    // tags every new book gets, and whether names read "Last First".
    QString filenamePattern = QStringLiteral("(?P<title>.+) - (?P<author>[^_]+)");
    bool readFileMetadata = true;
    QStringList newBookTags;
    bool swapAuthorNames = false;
    // Sorting: numbers in texts compared as numbers (numeric_collation).
    bool numericCollation = false;

    // CALIBRE_CONFIG_DIRECTORY, as Calibre honours it, or Calibre's default
    // folder for this platform.
    static QString configDirectory();
    // The library Cousoir opens unless told otherwise: COUSOIR_LIBRARY_PATH,
    // else the one Calibre opens. Empty when there is none.
    static QString defaultLibraryPath();
    // Reads global.py.json and tweaks.json from `directory`; missing or
    // unreadable files keep Calibre's defaults.
    static CalibreConfig load(const QString &directory = configDirectory());
};
