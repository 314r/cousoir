#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "calibre_languages.h"
#include "calibre_text.h"
#include "test_support.h"

namespace {

// What Calibre 9.16 computes for the same names, written by
// tools/oracle/text_oracle.py.
QJsonObject oracle()
{
    static const QJsonObject data = [] {
        QFile file(QStringLiteral(COUSOIR_TEST_DATA_DIR "/oracle/text.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

QJsonArray cases(const char *key)
{
    return oracle().value(QLatin1String(key)).toArray();
}

} // namespace

class CalibreTextTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void titleSort_data();
    void titleSort();
    void authorSort_data();
    void authorSort();
    void sqlAuthorSort();
    void canonicalizesLanguages();
    void titleSortFollowsTweaks();
    void namesLanguages();
    void foldsForSearch();
    void splitsAuthors();
    void joinsAuthors();
    void checksIsbns();
    void checksDois();
    void replacesEntities();
    void cleansCharacters();
};

void CalibreTextTest::initTestCase()
{
    QVERIFY2(!oracle().isEmpty(), "tests/data/oracle/text.json is missing; run make oracle");
    QCOMPARE(oracle().value(QLatin1String("interface_language")).toString(), QStringLiteral("fr"));
}

void CalibreTextTest::titleSort_data()
{
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("order");
    QTest::addColumn<QString>("expected");
    for (const QJsonValue &value : cases("title_sort")) {
        const QJsonObject test = value.toObject();
        const QString language = test.value(QLatin1String("lang")).toString();
        QTest::addRow("%s [%s]%s", qPrintable(test.value(QLatin1String("title")).toString()),
                      language.isEmpty() ? "interface" : qPrintable(language),
                      test.contains(QLatin1String("order")) ? " strict" : "")
            << test.value(QLatin1String("title")).toString() << language
            << test.value(QLatin1String("order")).toString()
            << test.value(QLatin1String("expected")).toString();
    }
}

void CalibreTextTest::titleSort()
{
    QFETCH(QString, title);
    QFETCH(QString, language);
    QFETCH(QString, order);
    QFETCH(QString, expected);
    const CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    // The oracle ran with a French interface: a book without a language
    // sorts with French articles, as Calibre's SQL triggers sort it.
    const QString interface = oracle().value(QLatin1String("interface_language")).toString();
    const QString actual = order.isEmpty() ? CalibreText::titleSort(title, language, tweaks, interface)
                                           : CalibreText::titleSort(title, language, tweaks, interface, order);
    QCOMPARE(actual, expected);
}

void CalibreTextTest::authorSort_data()
{
    QTest::addColumn<QString>("author");
    QTest::addColumn<QString>("method");
    QTest::addColumn<bool>("surnamePrefixes");
    QTest::addColumn<QString>("expected");
    for (const QJsonValue &value : cases("author_sort")) {
        const QJsonObject test = value.toObject();
        const bool prefixes = test.value(QLatin1String("surname_prefixes")).toBool();
        QTest::addRow("%s [%s%s]", qPrintable(test.value(QLatin1String("author")).toString()),
                      qPrintable(test.value(QLatin1String("method")).toString()), prefixes ? ", prefixes" : "")
            << test.value(QLatin1String("author")).toString() << test.value(QLatin1String("method")).toString()
            << prefixes << test.value(QLatin1String("expected")).toString();
    }
}

void CalibreTextTest::authorSort()
{
    QFETCH(QString, author);
    QFETCH(QString, method);
    QFETCH(bool, surnamePrefixes);
    QFETCH(QString, expected);
    CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    tweaks.authorUseSurnamePrefixes = surnamePrefixes;
    QCOMPARE(CalibreText::authorToAuthorSort(author, tweaks, method), expected);
}

void CalibreTextTest::sqlAuthorSort()
{
    const CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    const QJsonArray tests = cases("sql_author_sort");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        QCOMPARE(CalibreText::sqlAuthorToAuthorSort(test.value(QLatin1String("author")).toString(), tweaks),
                 test.value(QLatin1String("expected")).toString());
    }
}

void CalibreTextTest::canonicalizesLanguages()
{
    const QJsonArray tests = cases("canonicalize_lang");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        const QString raw = test.value(QLatin1String("raw")).toString();
        QCOMPARE(CalibreText::canonicalizeLanguage(raw), test.value(QLatin1String("expected")).toString());
    }
    QCOMPARE(CalibreText::canonicalizeLanguage(QStringLiteral("French")), QStringLiteral("fra"));
}

void CalibreTextTest::titleSortFollowsTweaks()
{
    CalibreText::Tweaks tweaks = CalibreText::Tweaks::defaults();
    const QString title = QStringLiteral("Le Petit Prince");
    QCOMPARE(CalibreText::titleSort(title, {}, tweaks, QStringLiteral("fr")), QStringLiteral("Petit Prince, Le"));
    // English articles for an English interface, unless the tweak says
    // otherwise.
    QCOMPARE(CalibreText::titleSort(title, {}, tweaks, QStringLiteral("en_GB")), title);
    tweaks.defaultLanguageForTitleSort = QStringLiteral("fra");
    QCOMPARE(CalibreText::titleSort(title, {}, tweaks, QStringLiteral("en_GB")), QStringLiteral("Petit Prince, Le"));
    tweaks.titleSeriesSorting = QStringLiteral("strictly_alphabetic");
    QCOMPARE(CalibreText::titleSort(title, {}, tweaks, QStringLiteral("fr")), title);
}

// Calibre's names, in its interface language, with its translations and
// their fallbacks; Qt's English ones without Calibre.
void CalibreTextTest::namesLanguages()
{
    CalibreLanguages::setResourcesFolder(QStringLiteral(COUSOIR_TEST_DATA_DIR "/calibre-resources"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("fra"), QStringLiteral("en")), QStringLiteral("French"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("eng"), QStringLiteral("en")), QStringLiteral("English"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("qqq"), QStringLiteral("en")), QStringLiteral("qqq"));
    // Calibre's English name, its translation from iso639.mo.
    QCOMPARE(CalibreText::languageName(QStringLiteral("grc"), QStringLiteral("en")),
             QStringLiteral("Greek, Ancient (to 1453)"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("grc"), QStringLiteral("fr")),
             QStringLiteral("Grec, ancien (jusqu'à 1453)"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("fra"), QStringLiteral("fr")), QStringLiteral("Français"));
    // messages.mo first, then a country's own translation by its language.
    QCOMPARE(CalibreText::languageName(QStringLiteral("eng"), QStringLiteral("fr_CA")), QStringLiteral("Anglais"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("deu"), QStringLiteral("de_DE.UTF-8@euro")), QStringLiteral("Deutsch"));
    // A language Calibre has no translation for keeps the English names.
    QCOMPARE(CalibreText::languageName(QStringLiteral("deu"), QStringLiteral("eo")), QStringLiteral("German"));
    CalibreLanguages::setResourcesFolder(QStringLiteral(""));
    QCOMPARE(CalibreText::languageName(QStringLiteral("fra"), QStringLiteral("fr")), QStringLiteral("French"));
    QCOMPARE(CalibreText::languageName(QStringLiteral("qqq"), QStringLiteral("fr")), QStringLiteral("qqq"));
    CalibreLanguages::setResourcesFolder(QStringLiteral(COUSOIR_TEST_DATA_DIR "/calibre-resources"));
}

void CalibreTextTest::foldsForSearch()
{
    QCOMPARE(CalibreText::foldForSearch(QStringLiteral("L'Écume des Jours")), QStringLiteral("l'ecume des jours"));
    QCOMPARE(CalibreText::foldForSearch(QStringLiteral("Bérénice Œuvre STRASSE ß")),
             QStringLiteral("berenice oeuvre strasse ss"));
}

void CalibreTextTest::splitsAuthors()
{
    const QJsonArray tests = cases("string_to_authors");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        QCOMPARE(CalibreText::stringToAuthors(test.value(QLatin1String("text")).toString()),
                 strings(test.value(QLatin1String("expected"))));
    }
}

void CalibreTextTest::joinsAuthors()
{
    const QJsonArray tests = cases("authors_to_string");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        QCOMPARE(CalibreText::authorsToString(strings(test.value(QLatin1String("authors")))),
                 test.value(QLatin1String("expected")).toString());
    }
}

void CalibreTextTest::checksIsbns()
{
    const QJsonArray tests = cases("check_isbn");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        const QString text = test.value(QLatin1String("text")).toString();
        QCOMPARE(CalibreText::checkIsbn(text), test.value(QLatin1String("expected")).toString());
        QCOMPARE(CalibreText::checkIsbn(text, true), test.value(QLatin1String("simple")).toString());
    }
}

void CalibreTextTest::checksDois()
{
    const QJsonArray tests = cases("check_doi");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        QCOMPARE(CalibreText::checkDoi(test.value(QLatin1String("text")).toString()),
                 test.value(QLatin1String("expected")).toString());
    }
}

void CalibreTextTest::replacesEntities()
{
    const QJsonArray tests = cases("replace_entities");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        QCOMPARE(CalibreText::replaceEntities(test.value(QLatin1String("text")).toString()),
                 test.value(QLatin1String("expected")).toString());
    }
}

void CalibreTextTest::cleansCharacters()
{
    const QJsonArray tests = cases("clean_chars");
    QVERIFY(!tests.isEmpty());
    for (const QJsonValue &value : tests) {
        const QJsonObject test = value.toObject();
        const QString text = test.value(QLatin1String("text")).toString();
        QCOMPARE(CalibreText::cleanAsciiChars(text), test.value(QLatin1String("ascii")).toString());
        QCOMPARE(CalibreText::cleanXmlChars(text), test.value(QLatin1String("xml")).toString());
    }
}

QTEST_GUILESS_MAIN(CalibreTextTest)
#include "calibre_text_test.moc"
