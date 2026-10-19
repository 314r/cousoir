#pragma once

#include <QString>

#include "calibre_text.h"

// What Reliure reads of Calibre's own configuration on this computer: the
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

    // CALIBRE_CONFIG_DIRECTORY, as Calibre honours it, or Calibre's default
    // folder for this platform.
    static QString configDirectory();
    // The library Reliure opens unless told otherwise: RELIURE_LIBRARY_PATH,
    // else the one Calibre opens. Empty when there is none.
    static QString defaultLibraryPath();
    // Reads global.py.json and tweaks.json from `directory`; missing or
    // unreadable files keep Calibre's defaults.
    static CalibreConfig load(const QString &directory = configDirectory());
};
