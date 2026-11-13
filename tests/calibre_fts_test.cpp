#include <QtTest>

#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "book_text_search.h"
#include "calibre_fts.h"
#include "sqlite_db.h"
#include "test_support.h"

// Cousoir's port of Calibre's FTS5 tokenizer against the tokens Calibre's own
// makes of the same texts (tools/oracle/fts_oracle.py), as fts5vocab lists
// them: each term at its place in the text, the words without their
// diacritics at the same place.
class CalibreFtsTest : public QObject
{
    Q_OBJECT

private slots:
    void searchesTheBooksText();
    void tokenizesAsCalibre_data();
    void tokenizesAsCalibre();
};

void CalibreFtsTest::tokenizesAsCalibre_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("table");
    for (const char *language : { "en", "fr" }) {
        for (const char *table : { "plain", "stemmed", "calibre" })
            QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(QLatin1String(language), QLatin1String(table))))
                << QString::fromLatin1(language) << QString::fromLatin1(table);
    }
}

void CalibreFtsTest::tokenizesAsCalibre()
{
    QFETCH(QString, language);
    QFETCH(QString, table);
    const QJsonObject oracle = QJsonDocument::fromJson(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/oracle/fts.json"))).object();
    const QJsonArray texts = oracle.value(QLatin1String("texts")).toArray();
    const QJsonArray expected = oracle.value(QLatin1String("languages")).toObject().value(language).toObject().value(table).toArray();
    QCOMPARE(expected.size(), texts.size());

    SqliteDb db;
    QString error;
    QVERIFY2(db.open(QStringLiteral(":memory:"), SqliteDb::Mode::ReadWrite, &error), qPrintable(error));
    QVERIFY2(CalibreFts::registerTokenizers(db.handle(), language, &error), qPrintable(error));
    const QString tokenize = oracle.value(QLatin1String("tables")).toObject().value(table).toString();
    QVERIFY2(db.exec(QStringLiteral("CREATE VIRTUAL TABLE t USING fts5(text, tokenize = '%1');"
                                    "CREATE VIRTUAL TABLE v USING fts5vocab(t, 'instance');").arg(tokenize),
                     &error),
             qPrintable(error));
    for (qsizetype i = 0; i < texts.size(); ++i)
        QVERIFY2(db.run(QStringLiteral("INSERT INTO t(rowid, text) VALUES (?, ?)"), { i + 1, texts.at(i).toString() }, &error),
                 qPrintable(error));
    for (qsizetype i = 0; i < texts.size(); ++i) {
        QStringList ours;
        for (const QVariantList &row : db.rows(QStringLiteral("SELECT term, offset FROM v WHERE doc=? ORDER BY offset, term"), { i + 1 }))
            ours.append(row.value(0).toString() + u'@' + row.value(1).toString());
        QStringList theirs;
        for (const QJsonValue &instance : expected.at(i).toArray())
            theirs.append(instance.toArray().at(0).toString() + u'@' + QString::number(instance.toArray().at(1).toInt()));
        QCOMPARE(ours, theirs);
    }
}

// The books' text found in an index made as Calibre makes it, with its
// schema (resources/fts_sqlite.sql) and its tokenizer.
void CalibreFtsTest::searchesTheBooksText()
{
    QTemporaryDir library;
    QCOMPARE(BookTextSearch::status(library.path()).exists, false);
    QString error;
    QVERIFY(BookTextSearch::find(library.path(), QStringLiteral("annals"), true, QStringLiteral("en"), 10, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY2(makeCalibreTextIndex(library.path(),
                                  { { 2, QStringLiteral("EPUB"), QStringLiteral("The Black Company is running north. The Annals are kept by Croaker.") },
                                    { 1, QStringLiteral("epub"), QStringLiteral("Colin aimait la musique de Duke Ellington. Écume des jours.") },
                                    { 4, QStringLiteral("AZW3"), QStringLiteral("The world will end next Saturday, just before dinner.") } },
                                  { { 5, QStringLiteral("PDF") } }, QStringLiteral("en"), &error),
             qPrintable(error));
    const BookTextSearch::Status status = BookTextSearch::status(library.path());
    QVERIFY(status.exists);
    QCOMPARE(status.indexed, 3);
    QCOMPARE(status.pending, 1);

    // By the words' stems: "run" finds "running".
    QList<BookTextSearch::Hit> hits = BookTextSearch::find(library.path(), QStringLiteral("run"), true, QStringLiteral("en"), 10, &error);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().bookId, 2);
    QCOMPARE(hits.first().format, QStringLiteral("EPUB"));
    QVERIFY(hits.first().snippet.contains(QStringLiteral("\x1drunning\x1d")));
    QVERIFY(BookTextSearch::find(library.path(), QStringLiteral("run"), false, QStringLiteral("en"), 10, &error).isEmpty());
    // Stemmed in another language than Calibre's: as written.
    error.clear();
    hits = BookTextSearch::find(library.path(), QStringLiteral("running"), true, QStringLiteral("fr"), 10, &error);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().bookId, 2);
    QVERIFY(error.isEmpty());
    // Accents aside, as Calibre's tokenizer folds them.
    hits = BookTextSearch::find(library.path(), QStringLiteral("ecume"), true, QStringLiteral("fr"), 10, &error);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().bookId, 1);
    QCOMPARE(hits.first().format, QStringLiteral("EPUB"));
    // A query FTS5 cannot read says why.
    QVERIFY(BookTextSearch::find(library.path(), QStringLiteral("\"unclosed"), true, QStringLiteral("en"), 10, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(CalibreFtsTest)

#include "calibre_fts_test.moc"
