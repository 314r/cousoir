#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// Calibre's text rules, ported from src/calibre/ebooks/metadata/__init__.py
// and src/calibre/utils/localization.py of Calibre 9.16. Calibre's own output
// for the same names is checked by tests/calibre_text_test.cpp.
namespace CalibreText {

// The tweaks the rules read, with Calibre's defaults from
// resources/default_tweaks.py. tweaks.json in the Calibre configuration
// folder overrides them.
struct Tweaks
{
    // "library_order" moves leading articles to the end; "strictly_alphabetic"
    // keeps titles as they are.
    QString titleSeriesSorting = QStringLiteral("library_order");
    // An ISO 639 code; empty uses the interface language.
    QString defaultLanguageForTitleSort;
    // Python regular expressions per ISO 639-2 code, matched case-insensitively
    // at the start of a title.
    QHash<QString, QStringList> perLanguageTitleSortArticles;
    // "comma", "nocomma", "invert" or "copy".
    QString authorSortCopyMethod = QStringLiteral("comma");
    QStringList authorNameSuffixes;
    QStringList authorNamePrefixes;
    QStringList authorNameCopywords;
    bool authorUseSurnamePrefixes = false;
    QStringList authorSurnamePrefixes;

    static Tweaks defaults();
};

// title_sort(): `title` without its quotes and with a leading article moved
// to the end, "Petit Prince, Le". The articles are those of `language`, an
// ISO 639 code or a locale name; empty takes the tweak's language, then
// `interfaceLanguage`, as Calibre's SQL triggers do.
QString titleSort(const QString &title, const QString &language, const Tweaks &tweaks,
                  const QString &interfaceLanguage);
// The same with Calibre's `order` argument in place of the tweak.
QString titleSort(const QString &title, const QString &language, const Tweaks &tweaks,
                  const QString &interfaceLanguage, const QString &order);

// author_to_author_sort(): "Cook, Glen" for "Glen Cook". `method` empty uses
// the tweak's.
QString authorToAuthorSort(const QString &author, const Tweaks &tweaks, const QString &method = {});

// The SQL function of the same name: the database stores the commas of a name
// as "|".
QString sqlAuthorToAuthorSort(const QString &author, const Tweaks &tweaks);

// canonicalize_lang(): the ISO 639-2 code Calibre stores for "fr", "fr_FR",
// "fra" or "French"; empty when it knows no such language.
QString canonicalizeLanguage(const QString &raw);

// The English name of an ISO 639 code, or the code itself.
QString languageName(const QString &code);

// Lower case without accents, for searching "berenice" in "Bérénice".
QString foldForSearch(const QString &text);

// ascii_text(): `text` in ASCII, through Unihandecode's table as Calibre uses
// it for every interface language but Japanese, Korean and Vietnamese.
QString asciiText(const QString &text);
// Whether asciiText() is what Calibre does in `interfaceLanguage`: the
// three languages above have tables of their own.
bool hasAsciiTable(const QString &interfaceLanguage);
// sanitize_file_name(): `name` with the characters Windows, macOS or Linux
// refuse replaced by "_", and no leading dot or trailing dot or space.
QString sanitizeFileName(const QString &name);
// ascii_filename(): asciiText() made a safe file name.
QString asciiFilename(const QString &text);

// Calibre's "Unknown", the title and author of a book without one, and
// "Cover", in its interface language.
QString unknownText(const QString &interfaceLanguage);
QString coverText(const QString &interfaceLanguage);

} // namespace CalibreText
