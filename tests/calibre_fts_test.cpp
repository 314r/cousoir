#include <QtTest>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "book_text_index.h"
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
    void extractsAsCalibre_data();
    void extractsAsCalibre();
    void keepsTheIndexAsCalibre();
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

// Each book file's text as Calibre's indexing workers extract it
// (tools/oracle/book_text_oracle.py): Cousoir's the same, or the file left
// to Calibre.
void CalibreFtsTest::extractsAsCalibre_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("expected");
    const QJsonObject texts = QJsonDocument::fromJson(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/oracle/book_text.json")))
                                  .object()
                                  .value(QLatin1String("texts"))
                                  .toObject();
    QVERIFY(!texts.isEmpty());
    for (auto it = texts.begin(); it != texts.end(); ++it)
        QTest::newRow(qPrintable(it.key())) << it.key() << it.value().toString();
}

void CalibreFtsTest::extractsAsCalibre()
{
    QFETCH(QString, file);
    QFETCH(QString, expected);
    const BookTextIndex::Extraction extraction
        = BookTextIndex::extract(QStringLiteral(COUSOIR_TEST_DATA_DIR "/") + file, QFileInfo(file).suffix());
    QVERIFY(extraction.supported);
    QCOMPARE(extraction.error, QString());
    QCOMPARE(extraction.text, expected);
}

// The index made, filled and kept in step as Calibre does: its rows, the
// formats waiting, and FTS5's own check of its tables.
void CalibreFtsTest::keepsTheIndexAsCalibre()
{
    QTemporaryDir library;
    QString error;
    const QString language = QStringLiteral("en");
    QVERIFY2(BookTextIndex::create(library.path(), { { 1, QStringLiteral("epub") }, { 2, QStringLiteral("AZW3") }, { 2, QStringLiteral("EPUB") } },
                                   language, &error),
             qPrintable(error));
    QVERIFY(!BookTextIndex::create(library.path(), {}, language, &error));
    const auto jobs = [&] {
        QStringList list;
        for (const BookTextIndex::Job &job : BookTextIndex::waiting(library.path(), nullptr))
            list.append(QString::number(job.book) + u' ' + job.format);
        return list;
    };
    QCOMPARE(jobs(), (QStringList{ QStringLiteral("1 EPUB"), QStringLiteral("2 AZW3"), QStringLiteral("2 EPUB") }));
    QCOMPARE(BookTextSearch::status(library.path()).pending, 3);

    const QString text = QStringLiteral("Colin aimait la musique de Duke Ellington. 𝄞");
    QVERIFY2(BookTextIndex::store(library.path(),
                                  { { 1, QStringLiteral("EPUB"), 1509, QStringLiteral("aaa"), text, {} },
                                    { 2, QStringLiteral("AZW3"), 11180, QStringLiteral("bbb"), {}, {} },
                                    { 2, QStringLiteral("EPUB"), 1483, QStringLiteral("ccc"), {}, QStringLiteral("Not a book") } },
                                  language, &error),
             qPrintable(error));
    QVERIFY(jobs().isEmpty());
    SqliteDb db;
    QVERIFY2(db.open(BookTextSearch::indexPath(library.path()), SqliteDb::Mode::ReadWrite, &error)
                 && CalibreFts::registerTokenizers(db.handle(), language, &error),
             qPrintable(error));
    const auto rows = [&db] {
        return db.rows(QStringLiteral("SELECT book, format, format_size, format_hash, searchable_text, text_size, text_hash, "
                                      "err_msg, typeof(timestamp) FROM books_text ORDER BY book, format"));
    };
    QList<QVariantList> stored = rows();
    QCOMPARE(stored.size(), 2);
    // Its length in code points, as Python counts them; SHA-1 of its UTF-8.
    QCOMPARE(stored.at(0), (QVariantList{ 1, QStringLiteral("EPUB"), 1509, QStringLiteral("aaa"), text, 44,
                                          QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex()),
                                          QString(), QStringLiteral("real") }));
    QCOMPARE(stored.at(1), (QVariantList{ 2, QStringLiteral("EPUB"), 1483, QStringLiteral("ccc"), QString(), 0, QString(),
                                          QStringLiteral("Not a book"), QStringLiteral("real") }));
    QVERIFY(BookTextIndex::current(library.path(), { 1, QStringLiteral("EPUB") }, 1509, QStringLiteral("aaa")));
    QVERIFY(!BookTextIndex::current(library.path(), { 1, QStringLiteral("EPUB") }, 1510, QStringLiteral("aaa")));
    QCOMPARE(BookTextSearch::find(library.path(), QStringLiteral("ellington"), true, language, 10, &error).size(), 1);

    // The same text again is not stored again, its file's hash with it.
    QVERIFY(BookTextIndex::follow(library.path(), { { BookTextIndex::FormatChange::Kind::FormatChanged, 1, QStringLiteral("EPUB") } },
                                  language, &error));
    QCOMPARE(jobs(), QStringList{ QStringLiteral("1 EPUB") });
    QVERIFY(BookTextIndex::store(library.path(), { { 1, QStringLiteral("EPUB"), 1600, QStringLiteral("ddd"), text, {} } }, language, &error));
    QVERIFY(jobs().isEmpty());
    QCOMPARE(rows().at(0).at(3).toString(), QStringLiteral("aaa"));
    // Another text replaces it, in the index too.
    QVERIFY(BookTextIndex::store(library.path(), { { 1, QStringLiteral("EPUB"), 1600, QStringLiteral("ddd"), QStringLiteral("Le pianocktail."), {} } },
                                 language, &error));
    QCOMPARE(rows().at(0).at(4).toString(), QStringLiteral("Le pianocktail."));
    QVERIFY(BookTextSearch::find(library.path(), QStringLiteral("ellington"), true, language, 10, &error).isEmpty());
    QCOMPARE(BookTextSearch::find(library.path(), QStringLiteral("pianocktail"), true, language, 10, &error).size(), 1);

    // As Calibre's triggers: a format gone takes its text, a book gone all
    // of its own, a format added or changed waits.
    QVERIFY2(BookTextIndex::follow(library.path(),
                                   { { BookTextIndex::FormatChange::Kind::FormatRemoved, 1, QStringLiteral("EPUB") },
                                     { BookTextIndex::FormatChange::Kind::FormatChanged, 3, QStringLiteral("PDF") },
                                     { BookTextIndex::FormatChange::Kind::FormatChanged, 2, QStringLiteral("AZW3") },
                                     { BookTextIndex::FormatChange::Kind::BookRemoved, 2, {} } },
                                   language, &error),
             qPrintable(error));
    QVERIFY(rows().isEmpty());
    QCOMPARE(jobs(), QStringList{ QStringLiteral("3 PDF") });
    QVERIFY(BookTextSearch::find(library.path(), QStringLiteral("pianocktail"), true, language, 10, &error).isEmpty());
    for (const char *table : { "books_fts", "books_fts_stemmed" })
        QVERIFY2(db.exec(QStringLiteral("INSERT INTO %1(%1) VALUES('integrity-check')").arg(QLatin1String(table)), &error),
                 qPrintable(error));
    QCOMPARE(db.value(QStringLiteral("PRAGMA user_version")).toInt(), 1);
    // Nothing to follow in a library without an index.
    QTemporaryDir other;
    QVERIFY(BookTextIndex::follow(other.path(), { { BookTextIndex::FormatChange::Kind::BookRemoved, 1, {} } }, language, &error));
    QVERIFY(!QFileInfo::exists(BookTextSearch::indexPath(other.path())));
}

QTEST_GUILESS_MAIN(CalibreFtsTest)

#include "calibre_fts_test.moc"
