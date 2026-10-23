#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "fake_open_library.h"
#include "open_library.h"
#include "test_support.h"

namespace {

QString data(const QString &name)
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/") + name;
}

QJsonObject oracle()
{
    return QJsonDocument::fromJson(readAll(data(QStringLiteral("oracle/source.json")))).object();
}

MetadataDownload::Query query(const QString &title, const QStringList &authors,
                              const QList<QPair<QString, QString>> &identifiers = {})
{
    MetadataDownload::Query result;
    result.title = title;
    result.authors = authors;
    result.identifiers = identifiers;
    result.timeout = 10;
    return result;
}

} // namespace

class MetadataWebTest : public QObject
{
    Q_OBJECT

private slots:
    void searchesByCalibresWords();
    void readsTheSearch();
    void readsSeriesAndSkipsDuplicates();
    void parsesDates_data();
    void parsesDates();
    void ranksAsCalibre();
    void identifiesByIsbn();
    void fallsBackToTheTitle();
    void downloadsCovers();
    void answersLater();
    void saysWhenOffline();
};

void MetadataWebTest::searchesByCalibresWords()
{
    const QJsonObject expected = oracle();
    for (const QJsonValue &row : expected.value(QLatin1String("title_tokens")).toArray()) {
        const QString title = row.toArray().at(0).toString();
        QStringList tokens;
        for (const QJsonValue &token : row.toArray().at(1).toArray())
            tokens.append(token.toString());
        QCOMPARE(OpenLibrary::titleTokens(title), tokens);
    }
    for (const QJsonValue &row : expected.value(QLatin1String("author_tokens")).toArray()) {
        QStringList authors;
        for (const QJsonValue &author : row.toArray().at(0).toArray())
            authors.append(author.toString());
        QStringList tokens;
        for (const QJsonValue &token : row.toArray().at(1).toArray())
            tokens.append(token.toString());
        QCOMPARE(OpenLibrary::authorTokens(authors), tokens);
    }
    for (const QJsonValue &row : expected.value(QLatin1String("cleanup_title")).toArray())
        QCOMPARE(OpenLibrary::cleanupTitle(row.toArray().at(0).toString()), row.toArray().at(1).toString());

    const OpenLibrary::Site site{ QStringLiteral("http://ol"), QStringLiteral("http://covers") };
    const QUrl byTitle = OpenLibrary::searchUrl(site, query(QStringLiteral("The Black Company"), { QStringLiteral("Cook, Glen") }),
                                                false, QStringLiteral("eng"));
    const QUrlQuery parameters(byTitle);
    QCOMPARE(byTitle.path(), QStringLiteral("/search.json"));
    QCOMPARE(parameters.queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded), QStringLiteral("Black Company"));
    QCOMPARE(parameters.queryItemValue(QStringLiteral("author"), QUrl::FullyDecoded), QStringLiteral("Glen Cook"));
    QCOMPARE(parameters.queryItemValue(QStringLiteral("lang")), QStringLiteral("en"));
    const QUrl byIsbn = OpenLibrary::searchUrl(
        site, query({}, {}, { { QStringLiteral("isbn"), QStringLiteral("978-2-253-14087-0") } }), true, QStringLiteral("fra"));
    QCOMPARE(QUrlQuery(byIsbn).queryItemValue(QStringLiteral("isbn")), QStringLiteral("9782253140870"));
    QCOMPARE(QUrlQuery(byIsbn).queryItemValue(QStringLiteral("lang")), QStringLiteral("fr"));
    QVERIFY(OpenLibrary::searchUrl(site, query({}, {}), false, {}).isEmpty());
}

void MetadataWebTest::readsTheSearch()
{
    QString error;
    const auto wanted = query(QStringLiteral("L'Écume des jours"), { QStringLiteral("Boris Vian") },
                              { { QStringLiteral("isbn"), QStringLiteral("9782253140870") } });
    QList<OpenLibrary::Candidate> candidates =
        OpenLibrary::parseSearch(readAll(data(QStringLiteral("openlibrary/search-isbn.json"))), wanted, 20, &error);
    QCOMPARE(error, QString());
    QCOMPARE(candidates.size(), 1);
    OpenLibrary::Candidate &ecume = candidates.first();
    // The work's title, accented, for the edition's "L'Ecume Des Jours".
    QCOMPARE(ecume.found.title, QStringLiteral("L'Écume des jours"));
    QCOMPARE(ecume.found.authors, QStringList{ QStringLiteral("Boris Vian") });
    QCOMPARE(ecume.found.publisher, QStringLiteral("Librairie Generale Francaise"));
    QCOMPARE(ecume.found.pubdate, std::optional<QDate>(QDate(1999, 12, 15)));
    QCOMPARE(ecume.found.languages, QStringList{ QStringLiteral("fra") });
    QCOMPARE(ecume.found.identifiers, (QList<QPair<QString, QString>>{ { QStringLiteral("isbn"), QStringLiteral("9782253140870") } }));
    QVERIFY(ecume.found.tags.contains(QStringLiteral("Jazz")));
    QVERIFY(!ecume.found.tags.contains(QStringLiteral("General")));
    QVERIFY(ecume.found.comments.isEmpty());
    QCOMPARE(ecume.covers, (QList<qint64>{ 7395866, 3169856 }));
    QCOMPARE(ecume.work, QStringLiteral("/works/OL708597W"));

    OpenLibrary::addWork(&ecume, readAll(data(QStringLiteral("openlibrary/works/OL708597W.json"))));
    QVERIFY(ecume.found.comments.startsWith(QStringLiteral("<div><p>Chick, Alise, Chloé et Colin")));
    // Its Markdown, as HTML.
    QVERIFY(ecume.found.comments.contains(QStringLiteral("<em>L'Écume des jours</em>")));
    QVERIFY(ecume.covers.size() > 2);
    QCOMPARE(ecume.covers.first(), 7395866);

    // Tags cut to Calibre's max_tags.
    QCOMPARE(OpenLibrary::parseSearch(readAll(data(QStringLiteral("openlibrary/search-isbn.json"))), wanted, 3, &error)
                 .first().found.tags.size(),
             3);
    QVERIFY(OpenLibrary::parseSearch("not json", wanted, 20, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void MetadataWebTest::readsSeriesAndSkipsDuplicates()
{
    QString error;
    const QList<OpenLibrary::Candidate> candidates =
        OpenLibrary::parseSearch(readAll(data(QStringLiteral("openlibrary/search-title.json"))),
                                 query(QStringLiteral("Shadows Linger"), { QStringLiteral("Glen Cook") }), 20, &error);
    QVERIFY(!candidates.isEmpty());
    for (const OpenLibrary::Candidate &candidate : candidates)
        QVERIFY(!candidate.found.title.contains(QStringLiteral("Duplicate")));
    const MetadataDownload::Found &first = candidates.first().found;
    QCOMPARE(first.title, QStringLiteral("Shadows Linger"));
    QCOMPARE(first.series, QStringLiteral("The Black Company"));
    QCOMPARE(first.tags, (QStringList{ QStringLiteral("Dark fantasy"), QStringLiteral("High fantasy") }));
    QCOMPARE(first.publisher, QStringLiteral("Tor Fantasy"));
    QCOMPARE(first.pubdate, std::optional<QDate>(QDate(1990, 4, 15)));
    QCOMPARE(first.identifiers.value(0).second, QStringLiteral("9780812508420"));
}

void MetadataWebTest::parsesDates_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QDate>("date");
    // As Calibre's parse_only_date(): the 15th of this month for what is
    // missing, never the first or last day of a month.
    QTest::newRow("year") << QStringLiteral("1984") << QDate(1984, QDateTime::currentDateTimeUtc().date().month(), 15);
    QTest::newRow("month") << QStringLiteral("1990-04") << QDate(1990, 4, 15);
    QTest::newRow("day") << QStringLiteral("2001-05-03") << QDate(2001, 5, 3);
    QTest::newRow("first") << QStringLiteral("2001-05-01") << QDate(2001, 5, 2);
    QTest::newRow("last") << QStringLiteral("2001-04-30") << QDate(2001, 4, 29);
    QTest::newRow("words") << QStringLiteral("December 1999") << QDate(1999, 12, 15);
    QTest::newRow("american") << QStringLiteral("May 3, 2001") << QDate(2001, 5, 3);
    QTest::newRow("british") << QStringLiteral("3rd May 2001") << QDate(2001, 5, 3);
    QTest::newRow("abbreviated") << QStringLiteral("Sept. 2010") << QDate(2010, 9, 15);
    QTest::newRow("printed") << QStringLiteral("c1995") << QDate();
    QTest::newRow("none") << QStringLiteral("n.d.") << QDate();
}

void MetadataWebTest::parsesDates()
{
    QFETCH(QString, text);
    QFETCH(QDate, date);
    const std::optional<QDate> parsed = OpenLibrary::parseDate(text);
    QCOMPARE(parsed.value_or(QDate()), date);
}

void MetadataWebTest::ranksAsCalibre()
{
    const auto candidate = [](const QString &title, bool cover, const QString &comments, const QString &isbn = {}) {
        OpenLibrary::Candidate result;
        result.found.title = title;
        result.found.authors = { QStringLiteral("Glen Cook") };
        result.found.hasCover = cover;
        result.found.comments = comments;
        if (!isbn.isEmpty())
            result.found.identifiers = { { QStringLiteral("isbn"), isbn } };
        return result;
    };
    QList<OpenLibrary::Candidate> candidates{
        candidate(QStringLiteral("Shadows Linger"), false, {}),
        candidate(QStringLiteral("Shadows Linger: The Second Chronicle"), true, QStringLiteral("short")),
        candidate(QStringLiteral("The Shadows Linger"), true, QStringLiteral("a much longer description than the other")),
        candidate(QStringLiteral("Shadows Linger"), true, QStringLiteral("short")),
        candidate(QStringLiteral("Other"), false, {}, QStringLiteral("9780812508420")),
    };
    OpenLibrary::rank(&candidates, query(QStringLiteral("Shadows Linger"), { QStringLiteral("Glen Cook") },
                                         { { QStringLiteral("isbn"), QStringLiteral("9780812508420") } }),
                      QStringLiteral("en"));
    QStringList order;
    for (const OpenLibrary::Candidate &result : std::as_const(candidates))
        order.append(result.found.title + (result.found.hasCover ? QStringLiteral(" +") : QString()));
    // The same ISBN, then a cover, then the same title, "The" left out,
    // then a description over 10 % longer, then the order found.
    QCOMPARE(order, (QStringList{ QStringLiteral("Other"), QStringLiteral("The Shadows Linger +"),
                                  QStringLiteral("Shadows Linger +"),
                                  QStringLiteral("Shadows Linger: The Second Chronicle +"), QStringLiteral("Shadows Linger") }));
}

void MetadataWebTest::identifiesByIsbn()
{
    FakeHttpServer server;
    server.handler = fakeOpenLibrary;
    OpenLibrary::Client client;
    client.setSite({ server.url(), server.url() });
    QSignalSpy identified(&client, &OpenLibrary::Client::identified);
    client.identify(query(QStringLiteral("L'Écume des jours"), { QStringLiteral("Boris Vian") },
                          { { QStringLiteral("isbn"), QStringLiteral("9782253140870") } }),
                    QStringLiteral("fra"), QStringLiteral("fr"), {});
    QVERIFY(identified.wait(10000));
    const auto books = identified.first().at(0).value<QList<MetadataDownload::Found>>();
    QCOMPARE(identified.first().at(1).toString(), QString());
    QCOMPARE(books.size(), 1);
    QCOMPARE(books.first().title, QStringLiteral("L'Écume des jours"));
    QVERIFY(books.first().comments.contains(QStringLiteral("Duke Ellington")));
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests.first().contains(QStringLiteral("isbn=9782253140870")));
    QVERIFY(server.requests.first().contains(QStringLiteral("lang=fr")));
    QCOMPARE(server.requests.at(1), QStringLiteral("/works/OL708597W.json"));
}

void MetadataWebTest::fallsBackToTheTitle()
{
    FakeHttpServer server;
    server.handler = fakeOpenLibrary;
    OpenLibrary::Client client;
    client.setSite({ server.url(), server.url() });
    QSignalSpy identified(&client, &OpenLibrary::Client::identified);
    // An ISBN Open Library does not know.
    client.identify(query(QStringLiteral("Shadows Linger"), { QStringLiteral("Glen Cook") },
                          { { QStringLiteral("isbn"), QStringLiteral("9780306406157") } }),
                    QStringLiteral("eng"), QStringLiteral("en"), {});
    QVERIFY(identified.wait(10000));
    const auto books = identified.first().at(0).value<QList<MetadataDownload::Found>>();
    QVERIFY(!books.isEmpty());
    QCOMPARE(books.first().title, QStringLiteral("Shadows Linger"));
    QVERIFY(books.first().comments.contains(QStringLiteral("White Rose")));
    QVERIFY(server.requests.at(1).contains(QStringLiteral("title=Shadows")));
}

void MetadataWebTest::downloadsCovers()
{
    FakeHttpServer server;
    server.handler = fakeOpenLibrary;
    OpenLibrary::Client client;
    client.setSite({ server.url(), server.url() });
    QSignalSpy identified(&client, &OpenLibrary::Client::identified);
    client.identify(query(QStringLiteral("Shadows Linger"), { QStringLiteral("Glen Cook") }), QStringLiteral("eng"),
                    QStringLiteral("en"), {});
    QVERIFY(identified.wait(10000));

    QTemporaryDir folder;
    QSignalSpy found(&client, &OpenLibrary::Client::coverFound);
    QSignalSpy finished(&client, &OpenLibrary::Client::coversFinished);
    client.covers(0, folder.path());
    QVERIFY(finished.wait(10000));
    // The work's two covers, one of which Open Library no longer has.
    QCOMPARE(finished.first().at(0).toString(), QString());
    QCOMPARE(found.size(), 1);
    const auto cover = found.first().at(0).value<MetadataDownload::Cover>();
    QCOMPARE(cover.source, QStringLiteral("Open Library"));
    QCOMPARE(cover.width, 300 + 6874911 % 100);
    QCOMPARE(cover.height, 450);
    QVERIFY(QFileInfo::exists(cover.file));
    QVERIFY(cover.file.startsWith(folder.path()));
}

void MetadataWebTest::answersLater()
{
    // Even what is known at once comes once the call has returned, for a
    // caller that waits on an event loop then.
    OpenLibrary::Client client;
    QSignalSpy identified(&client, &OpenLibrary::Client::identified);
    client.identify(query({}, {}), {}, QStringLiteral("en"), {});
    QCOMPARE(identified.size(), 0);
    QVERIFY(identified.wait(1000));
    QVERIFY(!identified.first().at(1).toString().isEmpty());

    QSignalSpy finished(&client, &OpenLibrary::Client::coversFinished);
    client.covers(0, {});
    QCOMPARE(finished.size(), 0);
    QVERIFY(finished.wait(1000));
    // Stopped first, never.
    client.covers(0, {});
    client.stop();
    QVERIFY(!finished.wait(200));
}

void MetadataWebTest::saysWhenOffline()
{
    OpenLibrary::Client client;
    // Nothing listens there.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const QString url = QStringLiteral("http://127.0.0.1:%1").arg(closed.serverPort());
    closed.close();
    client.setSite({ url, url });
    QSignalSpy identified(&client, &OpenLibrary::Client::identified);
    client.identify(query(QStringLiteral("Shadows Linger"), { QStringLiteral("Glen Cook") }), {}, QStringLiteral("en"), {});
    QVERIFY(identified.wait(10000));
    QVERIFY(identified.first().at(0).value<QList<MetadataDownload::Found>>().isEmpty());
    QVERIFY(identified.first().at(1).toString().startsWith(QStringLiteral("Open Library cannot be reached")));
}

QTEST_GUILESS_MAIN(MetadataWebTest)

#include "metadata_web_test.moc"
