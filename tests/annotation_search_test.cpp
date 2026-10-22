#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "annotation_search.h"
#include "book.h"
#include "calibre_fts.h"
#include "sqlite_db.h"
#include "test_support.h"

// The library's highlights and bookmarks as Calibre's "Browse annotations"
// finds them: the annotations Calibre's own API saved in a copy of the
// fixture's database (tools/oracle/annotations_oracle.py), all of them and
// those searches find, compared with what Calibre found; each book's in its
// order, and exported as Calibre exports them.

class AnnotationSearchTest : public QObject
{
    Q_OBJECT

private slots:
    void findsAsCalibre_data();
    void findsAsCalibre();
    void tellsWhatItCannotRead();
    void sortsAndExports();

private:
    bool openDatabase(SqliteDb *db);
};

bool AnnotationSearchTest::openDatabase(SqliteDb *db)
{
    QString error;
    const bool ok = db->open(QStringLiteral(COUSOIR_TEST_DATA_DIR "/annotations/metadata.db"), SqliteDb::Mode::Immutable, &error)
        && CalibreFts::registerTokenizers(db->handle(), QStringLiteral("fr"), &error);
    if (!ok)
        qWarning() << error;
    return ok;
}

void AnnotationSearchTest::findsAsCalibre_data()
{
    QTest::addColumn<QJsonObject>("query");
    QTest::addColumn<QJsonArray>("results");
    const QJsonArray cases = QJsonDocument::fromJson(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/annotations/expected.json"))).array();
    for (const QJsonValue &value : cases) {
        const QJsonObject query = value.toObject().value(QLatin1String("query")).toObject();
        const QString name = QStringLiteral("%1 %2 %3")
                                 .arg(query.value(QLatin1String("text")).toString(),
                                      query.value(QLatin1String("stemming")).toBool() ? QStringLiteral("stemmed") : QString(),
                                      query.value(QLatin1String("type")).toString());
        QTest::newRow(name.toUtf8().constData()) << query << value.toObject().value(QLatin1String("results")).toArray();
    }
}

void AnnotationSearchTest::findsAsCalibre()
{
    QFETCH(QJsonObject, query);
    QFETCH(QJsonArray, results);
    SqliteDb db;
    QVERIFY(openDatabase(&db));
    AnnotationSearch::Query search;
    search.text = query.value(QLatin1String("text")).toString();
    search.stemming = query.value(QLatin1String("stemming")).toBool();
    search.type = query.value(QLatin1String("type")).toString();
    QString error;
    const QList<AnnotationSearch::Hit> hits = AnnotationSearch::find(db, search, &error);
    QCOMPARE(error, QString());
    QJsonArray found;
    for (const AnnotationSearch::Hit &hit : hits) {
        found.append(QJsonArray{ hit.id, hit.bookId, hit.format, hit.userType, hit.user, hit.text,
                                 hit.annotation.text(QStringLiteral("uuid")) });
    }
    QCOMPARE(found, results);
}

void AnnotationSearchTest::tellsWhatItCannotRead()
{
    SqliteDb db;
    QVERIFY(openDatabase(&db));
    AnnotationSearch::Query search;
    search.text = QStringLiteral("AND OR (");
    QString error;
    QVERIFY(AnnotationSearch::find(db, search, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(AnnotationSearch::count(db), 7);
}

void AnnotationSearchTest::sortsAndExports()
{
    SqliteDb db;
    QVERIFY(openDatabase(&db));
    QString error;
    const QList<AnnotationSearch::Hit> hits = AnnotationSearch::find(db, {}, &error);
    LibraryData data;
    Book vian;
    vian.id = 1;
    vian.title = QStringLiteral("L'Écume des jours");
    vian.sort = QStringLiteral("Ecume des jours, L'");
    Book cook;
    cook.id = 2;
    cook.title = QStringLiteral("The Black Company");
    cook.sort = QStringLiteral("Black Company, The");
    Book omens;
    omens.id = 4;
    omens.title = QStringLiteral("Good Omens");
    omens.sort = QStringLiteral("Good Omens");
    data.books = { vian, cook, omens };

    // The books by their titles' sort; each one's in its order, a bookmark
    // by its position.
    const auto groups = AnnotationSearch::groupedByBook(hits, data);
    QCOMPARE(groups.size(), 3);
    QCOMPARE(groups.at(0).first, 2);
    QCOMPARE(groups.at(1).first, 1);
    QCOMPARE(groups.at(2).first, 4);
    QStringList order;
    for (const AnnotationSearch::Hit &hit : groups.at(1).second)
        order.append(hit.annotation.text(QStringLiteral("uuid")));
    QCOMPARE(order, (QStringList{ "a2", "a4", "a3", "a1" }));

    // As Calibre's export: the chapters as headings, "Unknown chapter" for
    // those that say none, a note after its highlight.
    QList<AnnotationSearch::Hit> book;
    for (const auto &group : groups) {
        if (group.first == 1)
            book = group.second;
    }
    const QString markdown = AnnotationSearch::exportText(book, data, true, QStringLiteral("en"));
    const QStringList lines = markdown.split(u'\n');
    QCOMPARE(lines.value(0), QStringLiteral("# L'Écume des jours"));
    QVERIFY(lines.contains(QStringLiteral("## Chapitre I")));
    QVERIFY(lines.contains(QStringLiteral("## Chapitre II")));
    QVERIFY(lines.contains(QStringLiteral("### Une partie")));
    QVERIFY(lines.contains(QStringLiteral("## Unknown chapter")));
    QVERIFY(lines.contains(QStringLiteral("Le jazz, toujours.")));
    QVERIFY(lines.contains(QStringLiteral("--------------------")));
    QVERIFY(lines.indexOf(QStringLiteral("### Une partie")) > lines.indexOf(QStringLiteral("L'amour est une chose qui ne se décrit pas.")));
    const QString text = AnnotationSearch::exportText(book, data, false, QStringLiteral("en"));
    QVERIFY(text.contains(QStringLiteral("───")));
    QVERIFY(!text.contains(QStringLiteral("*")));
}

QTEST_GUILESS_MAIN(AnnotationSearchTest)
#include "annotation_search_test.moc"
