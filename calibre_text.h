#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

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
    // The series index of a book added to a series without one: "next",
    // "first_free", "next_free", "last_free", or a number.
    QString seriesIndexAutoIncrement = QStringLiteral("next");
    std::optional<double> seriesIndexAutoIncrementNumber;
    // The list's sort: the value of a number a book lacks, and how many of
    // the last sorts it keeps for ties.
    double valueForUndefinedNumbers = 0;
    int maximumResortLevels = 5;

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

// string_to_authors(): "A & B", "A and B" or "A with B" as names; "&&" is
// an "&" in a name.
QStringList stringToAuthors(const QString &text);
// authors_to_string(): the names joined by " & ", an "&" in one doubled.
QString authorsToString(const QStringList &authors);

// check_isbn(): the ISBN-10 or ISBN-13 in `text`, without its separators,
// if its check digit is right; empty otherwise. `simple` only drops dashes
// and spaces, rejecting any other character.
QString checkIsbn(const QString &text, bool simple = false);

// str.isspace(): Qt's whitespace and the four information separators, the
// whitespace of str.strip() and str.split().
bool isSpace(QChar character);
// str.strip(): `text` without Python's leading and trailing whitespace.
QString strip(const QString &text);
// str.split(): the words of `text` between runs of whitespace.
QStringList split(const QString &text);
// ' '.join(text.split()), as normalize_whitespace() does too: each run of
// whitespace one space, none at either end.
QString collapseSpaces(const QString &text);
// Python's \s in a str pattern as a QRegularExpression class, and \S: Qt's
// are ASCII without Unicode properties, and miss the information separators
// with them.
extern const QString pythonSpace;
extern const QString pythonNonSpace;

// check_doi(): the first "10.NNNN/…" in `text`, empty for none.
QString checkDoi(const QString &text);

// fuzzy_title() of db/utils.py: a title as Calibre compares it to find a
// book already in the library: lower case, without punctuation or a leading
// article.
QString fuzzyTitle(const QString &title, const Tweaks &tweaks, const QString &interfaceLanguage);

// fmt_sidx(): "2" for a whole series index, "2.5" otherwise.
QString seriesIndexText(double index);

// _get_next_series_num_for_list(): the index a book added to a series takes,
// after the indices its books have.
double nextSeriesIndex(QList<double> indices, const Tweaks &tweaks);

// calibre_langcode_to_name(): the name of an ISO 639 code in Calibre's
// interface language, as Calibre's tables and translations have it (see
// CalibreLanguages), or the code itself.
QString languageName(const QString &code, const QString &interfaceLanguage);

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
// The ISO 639-2 code of a MOBI header's language code, empty for none.
QString mobiLanguage(int code);

// clean_ascii_chars(): no ASCII control characters but tab, line feed and
// carriage return.
QString cleanAsciiChars(const QString &text);
// clean_xml_chars(): only the characters XML allows, as Calibre's native
// version keeps them.
QString cleanXmlChars(const QString &text);
// replace_entities(): HTML's named and numeric character references
// replaced, as Calibre's fast_html_entities does it.
QString replaceEntities(const QString &text);

} // namespace CalibreText
