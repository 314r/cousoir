#include "calibre_text.h"

#include <QHash>
#include <QLocale>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>

namespace {

// Python's str.isspace(), which str.strip() and str.split() use: Qt's spaces
// and the four information separators.
bool isPythonSpace(QChar character)
{
    return character.isSpace() || (character.unicode() >= 0x1c && character.unicode() <= 0x1f);
}

QString pythonStrip(const QString &text)
{
    qsizetype start = 0;
    qsizetype end = text.size();
    while (start < end && isPythonSpace(text.at(start)))
        ++start;
    while (end > start && isPythonSpace(text.at(end - 1)))
        --end;
    return text.mid(start, end - start);
}

QStringList pythonSplit(const QString &text)
{
    QStringList tokens;
    QString token;
    for (const QChar character : text) {
        if (isPythonSpace(character)) {
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
    QString result = pythonStrip(title);
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
    return pythonStrip(result);
}

QString authorToAuthorSort(const QString &author, const Tweaks &tweaks, const QString &method)
{
    if (author.isEmpty())
        return {};
    const QString how = method.isEmpty() ? tweaks.authorSortCopyMethod : method;
    if (how == QLatin1String("copy"))
        return author;

    const QString bare = pythonStrip(removeBracketedText(author));
    if (how == QLatin1String("comma") && bare.contains(u','))
        return author;

    QStringList tokens = pythonSplit(bare);
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

    QSet<QString> prefixes = lowered(tweaks.authorNamePrefixes);
    for (const QString &prefix : lowered(tweaks.authorNamePrefixes))
        prefixes.insert(prefix + u'.');
    qsizetype first = 0;
    while (first < tokens.size() && prefixes.contains(tokens.at(first).toLower()))
        ++first;
    if (first == tokens.size())
        return author;

    QSet<QString> suffixes = lowered(tweaks.authorNameSuffixes);
    for (const QString &suffix : lowered(tweaks.authorNameSuffixes))
        suffixes.insert(suffix + u'.');
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

QString languageName(const QString &code)
{
    const QLocale::Language language = QLocale::codeToLanguage(code, QLocale::AnyLanguageCode);
    if (language == QLocale::AnyLanguage)
        return code;
    return QLocale::languageToString(language);
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
