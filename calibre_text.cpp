// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/ebooks/metadata/__init__.py: title_sort(),
//   get_title_sort_pat(), quote_pairs, author_to_author_sort(),
//   remove_bracketed_text(), string_to_authors(), authors_to_string(),
//   check_isbn() and check_doi()
// - resources/default_tweaks.py: the title sort articles and the author sort
//   word lists
// - src/calibre/__init__.py: sanitize_file_name()
// - src/calibre/utils/filenames.py: ascii_filename()
// - src/calibre/ebooks/html_entities.c: replace_entities()
// - src/calibre/db/__init__.py: _get_next_series_num_for_list()
// - src/calibre/db/utils.py: fuzzy_title()
// - src/calibre/utils/localization.py: canonicalize_lang()
// - src/calibre/utils/cleantext.py: clean_xml_chars() and clean_ascii_chars()
//
// The tables it includes, calibre_unidecode.inc and calibre_strings.inc,
// are Calibre's too: see their own headers.
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_text.h"

#include "calibre_languages.h"

#include <QHash>
#include <QLocale>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

#include "calibre_strings.inc"
#include "calibre_unidecode.inc"

// quote_pairs: an opening quote and the quotes that may close it.
QString closingQuotes(QChar opening)
{
    static const QHash<QChar, QString> pairs{
        { u'"', QStringLiteral("\"") },
        { u'\'', QStringLiteral("'") },
        { u'“', QStringLiteral("”“") },
        { u'”', QStringLiteral("””") },
        { u'„', QStringLiteral("”“") },
        { u'‚', QStringLiteral("’‘") },
        { u'’', QStringLiteral("’‘") },
        { u'‘', QStringLiteral("’‘") },
        { u'‹', QStringLiteral("›") },
        { u'›', QStringLiteral("‹") },
        { u'《', QStringLiteral("》") },
        { u'〈', QStringLiteral("〉") },
        { u'»', QStringLiteral("«»") },
        { u'«', QStringLiteral("«»") },
        { u'「', QStringLiteral("」") },
        { u'『', QStringLiteral("』") },
    };
    return pairs.value(opening);
}

// Drops a quote at the start of `title`, and the one closing it at the end.
void stripQuotes(QString &title)
{
    if (title.isEmpty())
        return;
    const QString closing = closingQuotes(title.front());
    if (closing.isEmpty())
        return;
    title.remove(0, 1);
    if (!title.isEmpty() && closing.contains(title.back()))
        title.chop(1);
}

// remove_bracketed_text(): the text outside (), [] and {}. A closing bracket
// without its opening one is dropped too.
QString removeBracketedText(const QString &text)
{
    QHash<QChar, int> open;
    int total = 0;
    QString result;
    for (const QChar character : text) {
        if (character == u'(' || character == u'[' || character == u'{') {
            ++open[character];
            ++total;
        } else if (character == u')' || character == u']' || character == u'}') {
            const QChar opening = character == u')' ? u'(' : character == u']' ? u'[' : u'{';
            if (open.value(opening) > 0) {
                --open[opening];
                --total;
            }
        } else if (total < 1) {
            result.append(character);
        }
    }
    return result;
}

QSet<QString> lowered(const QStringList &words)
{
    QSet<QString> result;
    for (const QString &word : words)
        result.insert(word.toLower());
    return result;
}

// The words lower-cased, each also with a dot after it, as the author name
// prefixes and suffixes are matched.
QSet<QString> loweredWithDots(const QStringList &words)
{
    QSet<QString> result = lowered(words);
    for (const QString &word : words)
        result.insert(word.toLower() + u'.');
    return result;
}

// get_title_sort_pat(): the articles of the language, or of English when the
// tweak has none for it, as one case-insensitive alternation anchored at the
// start. Compiled patterns are kept, as Calibre does.
QRegularExpression titleSortPattern(const QString &language, const CalibreText::Tweaks &tweaks)
{
    const auto articles = tweaks.perLanguageTitleSortArticles.constFind(language);
    const QStringList list = articles != tweaks.perLanguageTitleSortArticles.cend()
        ? *articles
        : tweaks.perLanguageTitleSortArticles.value(QStringLiteral("eng"),
              { QStringLiteral("A\\s+"), QStringLiteral("The\\s+"), QStringLiteral("An\\s+") });
    const QString pattern = list.isEmpty()
        ? QStringLiteral("^$")
        : QStringLiteral("^(%1)").arg(list.join(u'|'));

    static QMutex mutex;
    static QHash<QString, QRegularExpression> compiled;
    QMutexLocker locker(&mutex);
    auto found = compiled.constFind(pattern);
    if (found != compiled.cend())
        return *found;
    QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption
                                               | QRegularExpression::UseUnicodePropertiesOption);
    if (!expression.isValid()) {
        expression = QRegularExpression(QStringLiteral("^(A|The|An)\\s+"), QRegularExpression::CaseInsensitiveOption
                                            | QRegularExpression::UseUnicodePropertiesOption);
    }
    compiled.insert(pattern, expression);
    return expression;
}

// Every language Qt knows, by lower-case English name, for canonicalize_lang()
// given a name.
const QHash<QString, QLocale::Language> &languagesByName()
{
    static const QHash<QString, QLocale::Language> names = [] {
        QHash<QString, QLocale::Language> result;
        for (int value = QLocale::AnyLanguage + 1; value <= QLocale::LastLanguage; ++value) {
            const auto language = QLocale::Language(value);
            const QString name = QLocale::languageToString(language).toLower();
            if (!name.isEmpty())
                result.insert(name, language);
        }
        return result;
    }();
    return names;
}

QString isoCode(QLocale::Language language)
{
    if (language == QLocale::AnyLanguage || language == QLocale::C)
        return {};
    return QLocale::languageToCode(language, QLocale::ISO639Part3);
}

} // namespace

namespace CalibreText {

#define PYTHON_SPACES "\\t\\n\\x0b\\f\\r\\x1c-\\x20\\x85\\xa0\\x{1680}\\x{2000}-\\x{200a}\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}"
const QString pythonSpace = QStringLiteral("[" PYTHON_SPACES "]");
const QString pythonNonSpace = QStringLiteral("[^" PYTHON_SPACES "]");
#undef PYTHON_SPACES

bool isSpace(QChar character)
{
    return character.isSpace() || (character.unicode() >= 0x1c && character.unicode() <= 0x1f);
}

QString strip(const QString &text)
{
    qsizetype start = 0;
    qsizetype end = text.size();
    while (start < end && isSpace(text.at(start)))
        ++start;
    while (end > start && isSpace(text.at(end - 1)))
        --end;
    return text.mid(start, end - start);
}

QStringList split(const QString &text)
{
    QStringList tokens;
    QString token;
    for (const QChar character : text) {
        if (isSpace(character)) {
            if (!token.isEmpty())
                tokens.append(std::exchange(token, QString()));
        } else {
            token.append(character);
        }
    }
    if (!token.isEmpty())
        tokens.append(token);
    return tokens;
}

QString collapseSpaces(const QString &text)
{
    return split(text).join(u' ');
}

Tweaks Tweaks::defaults()
{
    Tweaks tweaks;
    // per_language_title_sort_articles of resources/default_tweaks.py.
    tweaks.perLanguageTitleSortArticles = {
        { QStringLiteral("eng"), { QStringLiteral("A\\s+"), QStringLiteral("The\\s+"), QStringLiteral("An\\s+") } },
        { QStringLiteral("epo"), { QStringLiteral("La\\s+"), QStringLiteral("L'"), QStringLiteral("L´") } },
        { QStringLiteral("spa"), { QStringLiteral("El\\s+"), QStringLiteral("La\\s+"), QStringLiteral("Lo\\s+"),
                                   QStringLiteral("Los\\s+"), QStringLiteral("Las\\s+"), QStringLiteral("Un\\s+"),
                                   QStringLiteral("Una\\s+"), QStringLiteral("Unos\\s+"), QStringLiteral("Unas\\s+") } },
        { QStringLiteral("fra"), { QStringLiteral("Le\\s+"), QStringLiteral("La\\s+"), QStringLiteral("L'"),
                                   QStringLiteral("L´"), QStringLiteral("L’"), QStringLiteral("Les\\s+"),
                                   QStringLiteral("Un\\s+"), QStringLiteral("Une\\s+"), QStringLiteral("Des\\s+"),
                                   QStringLiteral("De\\s+(La\\s+)?"), QStringLiteral("D'"), QStringLiteral("D´"),
                                   QStringLiteral("D’") } },
        { QStringLiteral("pol"), {} },
        { QStringLiteral("ita"), { QStringLiteral("Lo\\s+"), QStringLiteral("Il\\s+"), QStringLiteral("L'"),
                                   QStringLiteral("L´"), QStringLiteral("La\\s+"), QStringLiteral("Gli\\s+"),
                                   QStringLiteral("I\\s+"), QStringLiteral("Le\\s+"), QStringLiteral("Uno\\s+"),
                                   QStringLiteral("Un\\s+"), QStringLiteral("Una\\s+"), QStringLiteral("Un'"),
                                   QStringLiteral("Un´"), QStringLiteral("Dei\\s+"), QStringLiteral("Degli\\s+"),
                                   QStringLiteral("Delle\\s+"), QStringLiteral("Del\\s+"), QStringLiteral("Della\\s+"),
                                   QStringLiteral("Dello\\s+"), QStringLiteral("Dell'"), QStringLiteral("Dell´") } },
        { QStringLiteral("por"), { QStringLiteral("A\\s+"), QStringLiteral("O\\s+"), QStringLiteral("Os\\s+"),
                                   QStringLiteral("As\\s+"), QStringLiteral("Um\\s+"), QStringLiteral("Uns\\s+"),
                                   QStringLiteral("Uma\\s+"), QStringLiteral("Umas\\s+") } },
        { QStringLiteral("ron"), { QStringLiteral("Un\\s+"), QStringLiteral("O\\s+"), QStringLiteral("Nişte\\s+") } },
        { QStringLiteral("deu"), { QStringLiteral("Der\\s+"), QStringLiteral("Die\\s+"), QStringLiteral("Das\\s+"),
                                   QStringLiteral("Den\\s+"), QStringLiteral("Ein\\s+"), QStringLiteral("Eine\\s+"),
                                   QStringLiteral("Einen\\s+"), QStringLiteral("Dem\\s+"), QStringLiteral("Des\\s+"),
                                   QStringLiteral("Einem\\s+"), QStringLiteral("Eines\\s+") } },
        { QStringLiteral("nld"), { QStringLiteral("De\\s+"), QStringLiteral("Het\\s+"), QStringLiteral("Een\\s+"),
                                   QStringLiteral("'n\\s+"), QStringLiteral("'s\\s+"), QStringLiteral("Ene\\s+"),
                                   QStringLiteral("Ener\\s+"), QStringLiteral("Enes\\s+"), QStringLiteral("Den\\s+"),
                                   QStringLiteral("Der\\s+"), QStringLiteral("Des\\s+"), QStringLiteral("'t\\s+") } },
        { QStringLiteral("swe"), { QStringLiteral("En\\s+"), QStringLiteral("Ett\\s+"), QStringLiteral("Det\\s+"),
                                   QStringLiteral("Den\\s+"), QStringLiteral("De\\s+") } },
        { QStringLiteral("tur"), { QStringLiteral("Bir\\s+") } },
        { QStringLiteral("afr"), { QStringLiteral("'n\\s+"), QStringLiteral("Die\\s+") } },
        { QStringLiteral("ell"), { QStringLiteral("O\\s+"), QStringLiteral("I\\s+"), QStringLiteral("To\\s+"),
                                   QStringLiteral("Ta\\s+"), QStringLiteral("Tus\\s+"), QStringLiteral("Tis\\s+"),
                                   QStringLiteral("'Enas\\s+"), QStringLiteral("'Mia\\s+"), QStringLiteral("'Ena\\s+"),
                                   QStringLiteral("'Enan\\s+") } },
        { QStringLiteral("hun"), { QStringLiteral("A\\s+"), QStringLiteral("Az\\s+"), QStringLiteral("Egy\\s+") } },
    };
    tweaks.authorNameSuffixes = { QStringLiteral("Jr"), QStringLiteral("Sr"), QStringLiteral("Inc"),
                                  QStringLiteral("Ph.D"), QStringLiteral("Phd"), QStringLiteral("MD"),
                                  QStringLiteral("M.D"), QStringLiteral("I"), QStringLiteral("II"),
                                  QStringLiteral("III"), QStringLiteral("IV"), QStringLiteral("Junior"),
                                  QStringLiteral("Senior") };
    tweaks.authorNamePrefixes = { QStringLiteral("Mr"), QStringLiteral("Mrs"), QStringLiteral("Ms"),
                                  QStringLiteral("Dr"), QStringLiteral("Prof") };
    tweaks.authorNameCopywords = { QStringLiteral("Agency"), QStringLiteral("Corporation"), QStringLiteral("Company"),
                                   QStringLiteral("Co."), QStringLiteral("Council"), QStringLiteral("Committee"),
                                   QStringLiteral("Inc."), QStringLiteral("Institute"), QStringLiteral("National"),
                                   QStringLiteral("Society"), QStringLiteral("Club"), QStringLiteral("Team"),
                                   QStringLiteral("Software"), QStringLiteral("Games"), QStringLiteral("Entertainment"),
                                   QStringLiteral("Media"), QStringLiteral("Studios") };
    tweaks.authorSurnamePrefixes = { QStringLiteral("da"), QStringLiteral("de"), QStringLiteral("di"),
                                     QStringLiteral("la"), QStringLiteral("le"), QStringLiteral("van"),
                                     QStringLiteral("von") };
    return tweaks;
}

QString titleSort(const QString &title, const QString &language, const Tweaks &tweaks,
                  const QString &interfaceLanguage)
{
    return titleSort(title, language, tweaks, interfaceLanguage, tweaks.titleSeriesSorting);
}

QString titleSort(const QString &title, const QString &language, const Tweaks &tweaks,
                  const QString &interfaceLanguage, const QString &order)
{
    QString result = strip(title);
    if (order == QLatin1String("strictly_alphabetic"))
        return result;
    stripQuotes(result);

    QString wanted = language;
    if (wanted.isEmpty())
        wanted = tweaks.defaultLanguageForTitleSort.isEmpty() ? interfaceLanguage
                                                              : tweaks.defaultLanguageForTitleSort;
    const QRegularExpressionMatch match = titleSortPattern(canonicalizeLanguage(wanted), tweaks).match(result);
    if (match.hasMatch() && match.capturedLength(1) > 0) {
        const QString article = match.captured(1);
        result = result.mid(article.size()) + QStringLiteral(", ") + article;
        stripQuotes(result);
    }
    return strip(result);
}

QString authorToAuthorSort(const QString &author, const Tweaks &tweaks, const QString &method)
{
    if (author.isEmpty())
        return {};
    const QString how = method.isEmpty() ? tweaks.authorSortCopyMethod : method;
    if (how == QLatin1String("copy"))
        return author;

    const QString bare = strip(removeBracketedText(author));
    if (how == QLatin1String("comma") && bare.contains(u','))
        return author;

    QStringList tokens = split(bare);
    if (tokens.size() < 2)
        return author;

    const QSet<QString> copywords = lowered(tweaks.authorNameCopywords);
    for (const QString &token : std::as_const(tokens)) {
        if (copywords.contains(token.toLower()))
            return author;
    }

    const QSet<QString> surnamePrefixes = lowered(tweaks.authorSurnamePrefixes);
    if (tweaks.authorUseSurnamePrefixes && tokens.size() == 2 && surnamePrefixes.contains(tokens.first().toLower()))
        return author;

    const QSet<QString> prefixes = loweredWithDots(tweaks.authorNamePrefixes);
    qsizetype first = 0;
    while (first < tokens.size() && prefixes.contains(tokens.at(first).toLower()))
        ++first;
    if (first == tokens.size())
        return author;

    const QSet<QString> suffixes = loweredWithDots(tweaks.authorNameSuffixes);
    qsizetype last = tokens.size() - 1;
    while (last >= first && suffixes.contains(tokens.at(last).toLower()))
        --last;
    if (last < first)
        return author;

    const QString suffix = tokens.mid(last + 1).join(u' ');

    if (tweaks.authorUseSurnamePrefixes && last > first && surnamePrefixes.contains(tokens.at(last - 1).toLower())) {
        tokens[last - 1] += u' ' + tokens.at(last);
        --last;
    }

    QStringList sorted{ tokens.at(last) };
    sorted += tokens.mid(first, last - first);
    const qsizetype names = sorted.size();
    if (!suffix.isEmpty())
        sorted.append(suffix);
    if (how != QLatin1String("nocomma") && names > 1)
        sorted.first() += u',';
    return sorted.join(u' ');
}

QString sqlAuthorToAuthorSort(const QString &author, const Tweaks &tweaks)
{
    if (author.isEmpty())
        return {};
    QString name = author;
    return authorToAuthorSort(name.replace(u'|', u','), tweaks);
}

QString canonicalizeLanguage(const QString &raw)
{
    // Calibre's own table when Calibre is there.
    if (const std::optional<QString> code = CalibreLanguages::canonicalize(raw))
        return *code;
    QString code = raw.toLower().trimmed();
    code.replace(u'_', u'-');
    code = code.section(u'-', 0, 0).section(u'@', 0, 0).trimmed();
    if (code.isEmpty())
        return {};
    if (code.size() == 2) {
        const QString converted = isoCode(QLocale::codeToLanguage(code, QLocale::ISO639Part1));
        if (!converted.isEmpty())
            return converted;
    } else if (code.size() == 3) {
        // Calibre accepts any ISO 639-3 code; Qt only knows the languages it
        // has locale data for, which include every language with title sort
        // articles.
        if (QLocale::codeToLanguage(code, QLocale::ISO639Part3) != QLocale::AnyLanguage)
            return code;
    }
    return isoCode(languagesByName().value(code, QLocale::AnyLanguage));
}

QStringList stringToAuthors(const QString &text)
{
    if (text.isEmpty())
        return {};
    static const QRegularExpression conjunctions(QStringLiteral("(?i),?") + pythonSpace + QStringLiteral("+(and|with)")
                                                 + pythonSpace + u'+');
    QString raw = text;
    raw.replace(QStringLiteral("&&"), QString(QChar(0xffff)));
    raw.replace(conjunctions, QStringLiteral("&"));
    QStringList authors;
    for (QString author : raw.split(u'&')) {
        author = strip(author).replace(QChar(0xffff), u'&');
        if (!author.isEmpty())
            authors.append(author);
    }
    return authors;
}

QString authorsToString(const QStringList &authors)
{
    QStringList escaped;
    for (const QString &author : authors) {
        if (!author.isEmpty())
            escaped.append(QString(author).replace(u'&', QStringLiteral("&&")));
    }
    return escaped.join(QStringLiteral(" & "));
}

QString checkIsbn(const QString &text, bool simple)
{
    QString isbn = text.toUpper();
    if (simple) {
        isbn = isbn.remove(u'-').trimmed().remove(u' ');
    } else {
        static const QRegularExpression other(QStringLiteral("[^0-9X]"));
        isbn.remove(other);
    }
    if (isbn.size() != 10 && isbn.size() != 13)
        return {};
    static const QRegularExpression same(QStringLiteral("^(\\d)\\1{9,12}$"));
    if (same.match(isbn).hasMatch())
        return {};
    const auto digit = [&isbn](qsizetype i) { return isbn.at(i).digitValue(); };
    if (isbn.size() == 10) {
        // check_digit_for_isbn10()
        int sum = 0;
        for (int i = 0; i < 9; ++i) {
            if (digit(i) < 0)
                return {};
            sum += (i + 1) * digit(i);
        }
        const int check = sum % 11;
        const QChar expected = check == 10 ? QChar(u'X') : QChar(u'0' + check);
        return isbn.at(9) == expected ? isbn : QString();
    }
    // check_digit_for_isbn13()
    int sum = 0;
    for (int i = 0; i < 12; ++i) {
        if (digit(i) < 0)
            return {};
        sum += (i % 2 ? 3 : 1) * digit(i);
    }
    const int check = (10 - sum % 10) % 10;
    return isbn.at(12) == QChar(u'0' + check) ? isbn : QString();
}

QString fuzzyTitle(const QString &title, const Tweaks &tweaks, const QString &interfaceLanguage)
{
    static const QRegularExpression punctuation(QStringLiteral("[\\[\\](){}<>'\";,:#]"));
    static const QRegularExpression separators(QStringLiteral("[-._]"));
    static const QRegularExpression spaces(pythonSpace + u'+');
    QString result = strip(title).toLower();
    result.remove(punctuation);
    // get_title_sort_pat() without a language: the tweak's, else Calibre's.
    const QString language = tweaks.defaultLanguageForTitleSort.isEmpty() ? interfaceLanguage
                                                                          : tweaks.defaultLanguageForTitleSort;
    result.remove(titleSortPattern(canonicalizeLanguage(language), tweaks));
    result.replace(separators, QStringLiteral(" "));
    result.replace(spaces, QStringLiteral(" "));
    return result;
}

double nextSeriesIndex(QList<double> indices, const Tweaks &tweaks)
{
    const QString &rule = tweaks.seriesIndexAutoIncrement;
    const double fallback = tweaks.seriesIndexAutoIncrementNumber.value_or(1.0);
    if (indices.isEmpty())
        return fallback;
    std::sort(indices.begin(), indices.end());
    if (rule == QLatin1String("next"))
        return std::floor(indices.last()) + 1;
    if (rule == QLatin1String("first_free")) {
        for (int i = 1; i < 10000; ++i) {
            if (!indices.contains(i))
                return i;
        }
    }
    if (rule == QLatin1String("next_free")) {
        for (int i = int(std::ceil(indices.first())); i < 10000; ++i) {
            if (!indices.contains(i))
                return i;
        }
    }
    if (rule == QLatin1String("last_free")) {
        for (int i = int(std::ceil(indices.last())); i > 0; --i) {
            if (!indices.contains(i))
                return i;
        }
        return indices.last() + 1;
    }
    return fallback;
}

QString checkDoi(const QString &text)
{
    static const QRegularExpression doi(QStringLiteral("10\\.\\d{4}/\\S+"), QRegularExpression::UseUnicodePropertiesOption);
    return doi.match(text).captured(0);
}

QString languageAsIso6391(const QString &language)
{
    const QString code = canonicalizeLanguage(language);
    if (code.isEmpty())
        return {};
    if (const std::shared_ptr<const CalibreLanguages::Iso639> table = CalibreLanguages::iso639();
        table && !table->threeToTwo.isEmpty())
        return table->threeToTwo.value(code);
    // Without Calibre's table, Qt's, which has the languages with two letter
    // codes it knows.
    const QLocale::Language qtLanguage = QLocale::codeToLanguage(code, QLocale::ISO639Part3);
    return qtLanguage == QLocale::AnyLanguage ? QString() : QLocale::languageToCode(qtLanguage, QLocale::ISO639Part1);
}

QByteArray mobiLanguageCode(const QString &language)
{
    const QString two = languageAsIso6391(language);
    int main = mobiCodes[0].main;
    int sub = mobiCodes[0].sub;
    for (const auto &entry : mobiCodes) {
        if (!two.isEmpty() && two == QLatin1String(entry.language)) {
            main = entry.main;
            sub = entry.sub;
        }
    }
    // pack('>HBB', 0, sub, main).
    return QByteArray("\0\0", 2) + char(sub) + char(main);
}

QString languageName(const QString &code, const QString &interfaceLanguage)
{
    return CalibreLanguages::names(interfaceLanguage)->name(code);
}

QString asciiText(const QString &text)
{
    // Unihandecoder.decode(): NFKC, then each character outside ASCII by
    // the table; a character outside the table's plane is "?".
    const QString normalized = text.normalized(QString::NormalizationForm_KC);
    QString result;
    result.reserve(normalized.size());
    for (qsizetype i = 0; i < normalized.size(); ++i) {
        const char16_t code = normalized.at(i).unicode();
        if (code < 0x80) {
            result.append(QChar(code));
        } else if (QChar::isHighSurrogate(code) && i + 1 < normalized.size()
                   && normalized.at(i + 1).isLowSurrogate()) {
            result.append(u'?');
            ++i;
        } else if (QChar::isSurrogate(code)) {
            result.append(u'?');
        } else {
            result.append(QString::fromUtf8(unidecodeStrings[unidecodeIndex[code - 0x80]]));
        }
    }
    return result;
}

bool hasAsciiTable(const QString &interfaceLanguage)
{
    // get_udc() picks Unihandecoder(lang=get_lang()).
    const QString language = interfaceLanguage.toLower();
    return !language.startsWith(QLatin1String("ja")) && !language.startsWith(QLatin1String("kr"))
        && language != QLatin1String("korean") && !language.startsWith(QLatin1String("vn"))
        && language != QLatin1String("vietnum");
}

QString sanitizeFileName(const QString &name)
{
    QString one;
    one.reserve(name.size());
    for (const QChar character : name) {
        const char16_t code = character.unicode();
        const bool bad = code < 32 || code == u'\\' || code == u'|' || code == u'?' || code == u'*' || code == u'<'
            || code == u'"' || code == u':' || code == u'>' || code == u'+' || code == u'/';
        one.append(bad ? QChar(u'_') : character);
    }
    // re.sub(r'\s', ' ', one).strip()
    for (QChar &character : one) {
        if (isSpace(character))
            character = u' ';
    }
    one = strip(one);
    // os.path.splitext() ignores the dots a name starts with.
    qsizetype dot = one.lastIndexOf(u'.');
    qsizetype leading = 0;
    while (leading < one.size() && one.at(leading) == u'.')
        ++leading;
    if (dot < leading)
        dot = -1;
    QString base = dot < 0 ? one : one.left(dot);
    const QString extension = dot < 0 ? QString() : one.mid(dot);
    static const QRegularExpression onlyDots(QStringLiteral("^\\.+$"));
    if (onlyDots.match(base).hasMatch())
        base = QStringLiteral("_");
    base.replace(QStringLiteral(".."), QStringLiteral("_"));
    one = base + extension;
    if (!one.isEmpty() && (one.back() == u'.' || one.back() == u' '))
        one.back() = u'_';
    if (one.startsWith(u'.'))
        one[0] = u'_';
    return one;
}

QString asciiFilename(const QString &text)
{
    QString ascii = asciiText(text);
    ascii.replace(u'?', u'_');
    for (QChar &character : ascii) {
        if (character.unicode() < 32)
            character = u'_';
    }
    return sanitizeFileName(ascii);
}

namespace {

struct CalibreStrings
{
    const char *unknown = "Unknown";
    const char *cover = "Cover";
};

// Calibre's translations as get_lc_messages_path() finds them: the language
// as given, then without its script, then without its country.
CalibreStrings calibreStringsFor(const QString &interfaceLanguage)
{
    QString language = interfaceLanguage;
    language.replace(u'-', u'_');
    language = language.section(u'.', 0, 0);
    const QStringList candidates = { language, language.section(u'@', 0, 0), language.section(u'_', 0, 0) };
    for (const QString &candidate : candidates) {
        for (const auto &entry : calibreStrings) {
            if (candidate == QLatin1String(entry.language))
                return { entry.unknown, entry.cover };
        }
    }
    return {};
}

} // namespace

QString unknownText(const QString &interfaceLanguage)
{
    return QString::fromUtf8(calibreStringsFor(interfaceLanguage).unknown);
}

QString coverText(const QString &interfaceLanguage)
{
    return QString::fromUtf8(calibreStringsFor(interfaceLanguage).cover);
}

QString mobiLanguage(int code)
{
    for (const auto &entry : mobiLanguages) {
        if (entry.code == code)
            return QString::fromLatin1(entry.language) == QLatin1String("und") ? QString()
                                                                              : QString::fromLatin1(entry.language);
    }
    return {};
}

QString cleanAsciiChars(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (const QChar character : text) {
        const char16_t code = character.unicode();
        if ((code < 32 && code != u'\t' && code != u'\n' && code != u'\r') || code == 127)
            continue;
        result.append(character);
    }
    return result;
}

QString cleanXmlChars(const QString &text)
{
    const auto allowed = [](char32_t code) {
        return (code >= 0x20 && code <= 0x7e) || code == 0x9 || code == 0xa || code == 0xd || code == 0x85
            || (code >= 0xa0 && code <= 0xd7ff) || (code >= 0xe000 && code <= 0xfdcf)
            || (code >= 0xfdf0 && code <= 0xfffd) || (code > 0xffff && code <= 0x10ffff);
    };
    QString result;
    result.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        // A pair is one character beyond the BMP; a lone surrogate is none.
        if (character.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
            result.append(character);
            result.append(text.at(++index));
        } else if (!character.isSurrogate() && allowed(character.unicode())) {
            result.append(character);
        }
    }
    return result;
}

namespace {

// convert_entity(): the text of the reference between "&" and ";", or
// nothing when it is none.
std::optional<QString> convertEntity(QStringView name)
{
    if (name.startsWith(u'#')) {
        if (name.size() < 2)
            return std::nullopt;
        const bool hex = name.at(1) == u'x' || name.at(1) == u'X';
        QStringView digits = name.mid(hex ? 2 : 1);
        if (digits.isEmpty())
            return std::nullopt;
        quint32 code = 0;
        for (const QChar character : digits) {
            const char16_t c = character.unicode();
            quint32 digit;
            if (c >= u'0' && c <= u'9')
                digit = c - u'0';
            else if (hex && c >= u'a' && c <= u'f')
                digit = 10 + c - u'a';
            else if (hex && c >= u'A' && c <= u'F')
                digit = 10 + c - u'A';
            else
                return std::nullopt;
            code = code * (hex ? 16 : 10) + digit;
        }
        if (code == 0 || code >= 0x110000)
            return QString();
        if (code >= 0xd800 && code <= 0xdfff)
            return QString(QChar::ReplacementCharacter);
        const char32_t point = code;
        return QString::fromUcs4(&point, 1);
    }
    const QByteArray key = name.toLatin1();
    const auto end = std::end(htmlEntities);
    const auto found = std::lower_bound(std::begin(htmlEntities), end, key, [](const auto &entry, const QByteArray &k) {
        return qstrcmp(entry.name, k.constData()) < 0;
    });
    if (found == end || qstrcmp(found->name, key.constData()) != 0)
        return std::nullopt;
    return QString::fromUtf8(found->value);
}

} // namespace

QString replaceEntities(const QString &text)
{
    QString result;
    result.reserve(text.size());
    qsizetype position = 0;
    while (position < text.size()) {
        const qsizetype amp = text.indexOf(u'&', position);
        if (amp < 0) {
            result += QStringView(text).mid(position);
            break;
        }
        result += QStringView(text).mid(position, amp - position);
        // process_entity(): letters and digits, a "#" first, up to ";". The
        // character that ends a run that is no reference is kept with it.
        qsizetype scan = amp + 1;
        bool closed = false;
        while (scan < text.size()) {
            const QChar c = text.at(scan++);
            const char16_t code = c.unicode();
            if ((code >= u'a' && code <= u'z') || (code >= u'A' && code <= u'Z') || (code >= u'0' && code <= u'9')
                || (code == u'#' && scan == amp + 2))
                continue;
            closed = code == u';';
            break;
        }
        const qsizetype length = scan - amp;
        std::optional<QString> converted;
        if (closed && length >= 3 && length <= 64)
            converted = convertEntity(QStringView(text).mid(amp + 1, length - 2));
        result += converted ? *converted : QStringView(text).mid(amp, length).toString();
        position = scan;
    }
    // Calibre gives back its input for an empty result.
    return result.isEmpty() ? text : result;
}

QString foldForSearch(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar character : decomposed) {
        if (character.category() == QChar::Mark_NonSpacing)
            continue;
        switch (character.unicode()) {
        case u'œ': case u'Œ':
            folded += QStringLiteral("oe");
            break;
        case u'æ': case u'Æ':
            folded += QStringLiteral("ae");
            break;
        case u'ß':
            folded += QStringLiteral("ss");
            break;
        case u'ø': case u'Ø':
            folded += u'o';
            break;
        case u'ł': case u'Ł':
            folded += u'l';
            break;
        case u'đ': case u'Đ':
            folded += u'd';
            break;
        default:
            folded += character;
        }
    }
    return folded.toCaseFolded();
}

} // namespace CalibreText
