#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "calibre_db.h"
#include "calibre_search.h"
#include "test_support.h"

namespace {

// Whether Calibre 9.16's collators find each query in each text, written by
// tools/oracle/search_oracle.py.
QJsonObject oracle()
{
    static const QJsonObject data = [] {
        QFile file(QStringLiteral(COUSOIR_TEST_DATA_DIR "/oracle/search.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

QList<int> ids(const CalibreSearch::Index &index, const QBitArray &rows)
{
    QList<int> result;
    for (qsizetype row = 0; row < rows.size(); ++row) {
        if (rows.testBit(row))
            result.append(index.data().books.at(row).id);
    }
    return result;
}

} // namespace

class CalibreSearchTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void foldsAsCalibre();
    void foldsCharacters_data();
    void foldsCharacters();
    void searchesFixture_data();
    void searchesFixture();
    void refusesBadQueries_data();
    void refusesBadQueries();
    void usesLibraryPreferences();

private:
    CalibreSearch::Index fixtureIndex(const QHash<QString, QString> &preferences = {});
};

void CalibreSearchTest::initTestCase()
{
    QVERIFY2(!oracle().isEmpty(), "tests/data/oracle/search.json is missing; run make oracle");
    QCOMPARE(oracle().value(QLatin1String("interface_language")).toString(), QStringLiteral("fr"));
}

void CalibreSearchTest::foldsAsCalibre()
{
    QStringList texts;
    for (const QJsonValue &text : oracle().value(QLatin1String("texts")).toArray())
        texts.append(text.toString());
    QList<CalibreSearch::Folded> folded[2];
    for (const QString &text : std::as_const(texts)) {
        folded[0].append(CalibreSearch::fold(text));
        folded[1].append(CalibreSearch::fold(text, true));
    }
    int checked = 0;
    QStringList wrong;
    for (const QJsonValue &value : oracle().value(QLatin1String("cases")).toArray()) {
        const QJsonObject test = value.toObject();
        const QString query = test.value(QLatin1String("query")).toString();
        const QString expected[2] = { test.value(QLatin1String("contains")).toString(),
                                      test.value(QLatin1String("accents")).toString() };
        for (int keep = 0; keep < 2; ++keep) {
            const CalibreSearch::Folded q = CalibreSearch::fold(query, keep == 1);
            for (qsizetype i = 0; i < texts.size(); ++i) {
                ++checked;
                if (CalibreSearch::contains(folded[keep].at(i), q) != (expected[keep].at(i) == u'1'))
                    wrong.append(QStringLiteral("%1 %2 in %3").arg(keep ? u'^' : u' ').arg(query, texts.at(i)));
            }
        }
    }
    QVERIFY(checked > 100000);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.mid(0, 20).join(u'\n')));
}

void CalibreSearchTest::foldsCharacters_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("folded");
    QTest::addColumn<QString>("kept");
    QTest::newRow("accents") << QStringLiteral("L'Écume") << QStringLiteral("lecume") << QStringLiteral("l'ecume");
    QTest::newRow("ligature") << QStringLiteral("Œuvre") << QStringLiteral("oeuvre") << QStringLiteral("oeuvre");
    QTest::newRow("sharp s") << QStringLiteral("Straße") << QStringLiteral("strasse") << QStringLiteral("strasse");
    QTest::newRow("eth") << QStringLiteral("Ðe") << QStringLiteral("de") << QStringLiteral("de");
    QTest::newRow("short i") << QStringLiteral("Йога") << QStringLiteral("йога") << QStringLiteral("йога");
    QTest::newRow("katakana") << QStringLiteral("パン") << QStringLiteral("はん") << QStringLiteral("はん");
    QTest::newRow("symbols stay") << QStringLiteral("C++ & R&D") << QStringLiteral("c++rd") << QStringLiteral("c++ & r&d");
    QTest::newRow("decomposed") << QStringLiteral("café") << QStringLiteral("cafe") << QStringLiteral("cafe");
}

void CalibreSearchTest::foldsCharacters()
{
    QFETCH(QString, text);
    QFETCH(QString, folded);
    QFETCH(QString, kept);
    QCOMPARE(QString::fromUtf8(CalibreSearch::fold(text).text), folded);
    QCOMPARE(QString::fromUtf8(CalibreSearch::fold(text, true).text), kept);
}

CalibreSearch::Index CalibreSearchTest::fixtureIndex(const QHash<QString, QString> &preferences)
{
    LibraryData data;
    QString error;
    QTemporaryDir cache;
    if (!CalibreDb::loadLibrary(fixtureLibrary(), cache.filePath(QStringLiteral("snapshot.db")), frenchConfig(),
                                &data, &error)) {
        qWarning() << error;
        return {};
    }
    for (auto it = preferences.cbegin(); it != preferences.cend(); ++it)
        data.preferences.insert(it.key(), it.value());
    return CalibreSearch::Index(data, CalibreSearch::Preferences::fromRows(data.preferences),
                                CalibreText::Tweaks::defaults(), QStringLiteral("fr"));
}

// The fixture's books: 1 L'Écume des jours (Boris Vian), 2 The Black
// Company and 3 Shadows Linger (Glen Cook, a series), 4 Good Omens (Terry
// Pratchett and Neil Gaiman), 5 a PDF, 6 Le Petit Prince.
void CalibreSearchTest::searchesFixture_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<QList<int>>("expected");
    QTest::newRow("word") << QStringLiteral("ecume") << QList<int>{ 1 };
    // Shadows Linger is in the Black Company's series.
    QTest::newRow("words") << QStringLiteral("glen black") << QList<int>{ 2, 3 };
    QTest::newRow("or") << QStringLiteral("vian or gaiman") << QList<int>{ 1, 4 };
    QTest::newRow("not") << QStringLiteral("not author:cook") << QList<int>{ 1, 4, 5, 6 };
    QTest::newRow("parentheses") << QStringLiteral("(author:cook or author:vian) and not series:true")
                                 << QList<int>{ 1 };
    QTest::newRow("location") << QStringLiteral("title:prince") << QList<int>{ 6 };
    QTest::newRow("quoted") << QStringLiteral("title:\"petit prince\"") << QList<int>{ 6 };
    QTest::newRow("equals") << QStringLiteral("author:\"=Glen Cook\"") << QList<int>{ 2, 3 };
    QTest::newRow("regexp") << QStringLiteral("title:~^the") << QList<int>{ 2 };
    QTest::newRow("rating") << QStringLiteral("rating:>=4") << QList<int>{ 1, 2 };
    QTest::newRow("formats") << QStringLiteral("formats:pdf") << QList<int>{ 5 };
    QTest::newRow("format count") << QStringLiteral("formats:#>1") << QList<int>{ 2 };
    QTest::newRow("no tags") << QStringLiteral("tags:false") << QList<int>{ 6 };
    QTest::newRow("id") << QStringLiteral("id:<3") << QList<int>{ 1, 2 };
    QTest::newRow("unknown location") << QStringLiteral("nowhere:vian") << QList<int>{};
}

void CalibreSearchTest::searchesFixture()
{
    QFETCH(QString, query);
    QFETCH(QList<int>, expected);
    const CalibreSearch::Index index = fixtureIndex();
    QCOMPARE(index.size(), 6);
    const CalibreSearch::Result result = CalibreSearch::search(index, query);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QList<int> found = ids(index, result.rows);
    std::sort(found.begin(), found.end());
    std::sort(expected.begin(), expected.end());
    QCOMPARE(found, expected);
}

void CalibreSearchTest::refusesBadQueries_data()
{
    QTest::addColumn<QString>("query");
    QTest::newRow("missing parenthesis") << QStringLiteral("(vian or cook");
    QTest::newRow("stray parenthesis") << QStringLiteral("vian)");
    QTest::newRow("not alone") << QStringLiteral("not");
    QTest::newRow("number") << QStringLiteral("rating:many");
    QTest::newRow("date") << QStringLiteral("pubdate:someday");
    QTest::newRow("regexp") << QStringLiteral("title:~(");
    QTest::newRow("saved search") << QStringLiteral("search:nothing");
    QTest::newRow("virtual library") << QStringLiteral("vl:nothing");
}

void CalibreSearchTest::refusesBadQueries()
{
    QFETCH(QString, query);
    const CalibreSearch::Result result = CalibreSearch::search(fixtureIndex(), query);
    QVERIFY(!result.ok());
    QCOMPARE(result.rows.count(true), 0);
}

void CalibreSearchTest::usesLibraryPreferences()
{
    const CalibreSearch::Index index = fixtureIndex({
        { QStringLiteral("saved_searches"), QStringLiteral(R"({"Cook": "author:cook", "Loop": "search:loop"})") },
        { QStringLiteral("virtual_libraries"), QStringLiteral(R"({"Fantasy": "tags:fantasy", "French": "languages:fra"})") },
        { QStringLiteral("grouped_search_terms"), QStringLiteral(R"({"people": ["authors", "publisher"]})") },
        { QStringLiteral("user_categories"), QStringLiteral(R"({"Favourites": [["Boris Vian", "authors", 0]]})") },
        { QStringLiteral("virtual_lib_on_startup"), QStringLiteral(R"("Fantasy")") },
    });
    QCOMPARE(index.preferences().virtualLibraryOnStartup, QStringLiteral("Fantasy"));
    const auto found = [&index](const QString &query, const QString &restriction = {}) {
        const CalibreSearch::Result result = CalibreSearch::search(index, query, restriction);
        QList<int> books = ids(index, result.rows);
        std::sort(books.begin(), books.end());
        return result.ok() ? books : QList<int>{ -1 };
    };
    QCOMPARE(found(QStringLiteral("search:cook")), (QList<int>{ 2, 3 }));
    QCOMPARE(found(QStringLiteral("search:\"=Cook\"")), (QList<int>{ 2, 3 }));
    QCOMPARE(found(QStringLiteral("search:loop")), QList<int>{ -1 });
    QCOMPARE(found(QStringLiteral("vl:Fantasy")), (QList<int>{ 2, 3, 4 }));
    // A virtual library restricts the search, and "not" with it.
    QCOMPARE(found(QStringLiteral("not author:cook"), QStringLiteral("tags:fantasy")), QList<int>{ 4 });
    QCOMPARE(found(QString(), QStringLiteral("languages:fra")), (QList<int>{ 1, 5, 6 }));
    QCOMPARE(found(QStringLiteral("people:gaiman")), QList<int>{ 4 });
    QCOMPARE(found(QStringLiteral("@favourites:true")), QList<int>{ 1 });
    QCOMPARE(found(QStringLiteral("@favourites:false")), (QList<int>{ 2, 3, 4, 5, 6 }));
}

QTEST_GUILESS_MAIN(CalibreSearchTest)
#include "calibre_search_test.moc"
