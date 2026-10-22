#include <QtTest>

#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "calibre_config.h"
#include "calibre_db.h"
#include "calibre_languages.h"
#include "library_server.h"
#include "opds.h"
#include "test_support.h"

namespace {

CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

struct Answer
{
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;
};

// An Atom feed's entries: their titles and links by rel.
struct Entry
{
    QString title;
    QMultiHash<QString, QString> links;
};

struct Feed
{
    QString title;
    QList<Entry> entries;
    QMultiHash<QString, QString> links;
    int total = -1;
};

Feed parseFeed(const QByteArray &xml)
{
    Feed feed;
    QXmlStreamReader reader(xml);
    Entry *entry = nullptr;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("entry")) {
                feed.entries.append(Entry{});
                entry = &feed.entries.last();
            } else if (reader.name() == QLatin1String("title")) {
                (entry ? entry->title : feed.title) = reader.readElementText();
            } else if (reader.name() == QLatin1String("link")) {
                const QString rel = reader.attributes().value(QLatin1String("rel")).toString();
                const QString href = reader.attributes().value(QLatin1String("href")).toString();
                (entry ? entry->links : feed.links).insert(rel, href);
            } else if (reader.name() == QLatin1String("totalResults")) {
                feed.total = reader.readElementText().toInt();
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("entry")) {
            entry = nullptr;
        }
    }
    if (reader.hasError())
        qWarning() << reader.errorString();
    return feed;
}

QStringList titles(const Feed &feed)
{
    QStringList result;
    for (const Entry &entry : feed.entries)
        result.append(entry.title);
    return result;
}

} // namespace

class LibraryServerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void browsesTheCatalog();
    void searchesAsCalibre();
    void browsesCategories();
    void servesFilesAndCovers();
    void asksForThePassword();
    void showsAWebPage();
    void pagesLongFeeds();

private:
    Answer get(const QString &path, const QByteArray &verb = "GET", const QByteArray &authorization = {});

    QTemporaryDir m_cache;
    LibraryData m_data;
    std::unique_ptr<CalibreSearch::Index> m_index;
    std::unique_ptr<BookSort::Sorter> m_sorter;
    LibraryServer m_server;
    QNetworkAccessManager m_network;
};

void LibraryServerTest::initTestCase()
{
    CalibreLanguages::setResourcesFolder(QStringLiteral(COUSOIR_TEST_DATA_DIR "/calibre-resources"));
    QString error;
    QVERIFY2(CalibreDb::loadLibrary(fixtureLibrary(), m_cache.filePath(QStringLiteral("snapshot.db")), frenchConfig(),
                                    &m_data, &error),
             qPrintable(error));
    const CalibreConfig config = frenchConfig();
    m_index = std::make_unique<CalibreSearch::Index>(m_data, CalibreSearch::Preferences::fromRows(m_data.preferences),
                                                     config.tweaks, config.interfaceLanguage);
    m_sorter = std::make_unique<BookSort::Sorter>(m_data, config);
    LibraryServer::Catalog catalog;
    catalog.data = &m_data;
    catalog.find = [this](const QString &query, const QList<BookSort::Level> &sort, QString *error) {
        return LibraryServer::findBooks(m_data, *m_index, m_sorter.get(), query, sort, error);
    };
    catalog.name = QStringLiteral("Bibliothèque");
    catalog.interfaceLanguage = QStringLiteral("fr");
    catalog.thumbnailFolder = m_cache.filePath(QStringLiteral("thumbnails"));
    m_server.setCatalog(catalog);
    // Only this computer reaches it, on a port the system picks.
    QCOMPARE(m_server.listen(0, QHostAddress::LocalHost), QString());
    QVERIFY(m_server.port() > 0);
}

Answer LibraryServerTest::get(const QString &path, const QByteArray &verb, const QByteArray &authorization)
{
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.port()).arg(path)));
    if (!authorization.isEmpty())
        request.setRawHeader("Authorization", authorization);
    QNetworkReply *reply = m_network.sendCustomRequest(request, verb);
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished())
        finished.wait(10000);
    Answer answer;
    answer.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    answer.body = reply->readAll();
    for (const auto &[name, value] : reply->rawHeaderPairs())
        answer.headers.insert(name.toLower(), value);
    reply->deleteLater();
    return answer;
}

void LibraryServerTest::browsesTheCatalog()
{
    const Answer root = get(QStringLiteral("/opds"));
    QCOMPARE(root.status, 200);
    QVERIFY(root.headers.value("content-type").startsWith("application/atom+xml;profile=opds-catalog;kind=navigation"));
    const Feed start = parseFeed(root.body);
    QCOMPARE(start.title, QStringLiteral("Bibliothèque"));
    QCOMPARE(start.links.value(QStringLiteral("search")), QStringLiteral("/opds/search.xml"));
    QCOMPARE(start.entries.size(), 7);

    // The books by title, as Calibre sorts them: by their title sort.
    const QString byTitle = start.entries.at(1).links.value(QStringLiteral("subsection"));
    const Feed books = parseFeed(get(byTitle).body);
    QCOMPARE(books.total, 6);
    QCOMPARE(titles(books), (QStringList{ QStringLiteral("The Black Company"), QStringLiteral("L'Écume des jours"),
                                          QStringLiteral("Good Omens"), QStringLiteral("Manuel technique"),
                                          QStringLiteral("Le Petit Prince"), QStringLiteral("Shadows Linger") }));
    const Entry &blackCompany = books.entries.first();
    QCOMPARE(blackCompany.links.values(QStringLiteral("http://opds-spec.org/acquisition")).size(), 2);
    QVERIFY(blackCompany.links.values(QStringLiteral("http://opds-spec.org/acquisition"))
                .contains(QStringLiteral("/get/2/EPUB")));
    QCOMPARE(blackCompany.links.value(QStringLiteral("http://opds-spec.org/image")), QStringLiteral("/cover/2"));
    // A book without a cover has no link to one.
    QVERIFY(!books.entries.at(3).links.contains(QStringLiteral("http://opds-spec.org/image")));

    // The newest first.
    const Feed newest = parseFeed(get(start.entries.first().links.value(QStringLiteral("subsection"))).body);
    QCOMPARE(newest.entries.size(), 6);
    QDateTime previous;
    for (const QString &title : titles(newest)) {
        const auto book = std::find_if(m_data.books.cbegin(), m_data.books.cend(),
                                       [&](const Book &candidate) { return candidate.title == title; });
        QVERIFY(book != m_data.books.cend());
        QVERIFY(!previous.isValid() || book->timestamp <= previous);
        previous = book->timestamp;
    }
}

void LibraryServerTest::searchesAsCalibre()
{
    const Feed fantasy = parseFeed(get(QStringLiteral("/opds/books?q=tags%3A%22%3DFantasy%22&sort=title")).body);
    QCOMPARE(titles(fantasy), (QStringList{ QStringLiteral("The Black Company"), QStringLiteral("Good Omens"),
                                            QStringLiteral("Shadows Linger") }));
    // "+" for a space, as a form sends it.
    const Feed glen = parseFeed(get(QStringLiteral("/opds/books?q=authors:glen+and+not+shadows")).body);
    QCOMPARE(titles(glen), QStringList{ QStringLiteral("The Black Company") });
    // A search Calibre cannot read finds nothing.
    QCOMPARE(parseFeed(get(QStringLiteral("/opds/books?q=%28unbalanced")).body).entries.size(), 0);

    const QByteArray description = get(QStringLiteral("/opds/search.xml")).body;
    QVERIFY(description.contains("template=\"/opds/books?q={searchTerms}&amp;sort=title\""));
}

void LibraryServerTest::browsesCategories()
{
    const Feed authors = parseFeed(get(QStringLiteral("/opds/authors")).body);
    QVERIFY(titles(authors).contains(QStringLiteral("Glen Cook")));
    const auto glen = std::find_if(authors.entries.cbegin(), authors.entries.cend(),
                                   [](const Entry &entry) { return entry.title == QLatin1String("Glen Cook"); });
    QVERIFY(glen != authors.entries.cend());
    const Feed books = parseFeed(get(glen->links.value(QStringLiteral("subsection"))).body);
    QCOMPARE(books.title, QStringLiteral("Glen Cook"));
    QCOMPARE(titles(books), (QStringList{ QStringLiteral("The Black Company"), QStringLiteral("Shadows Linger") }));

    // Languages by their names, in the interface's language.
    const Feed languages = parseFeed(get(QStringLiteral("/opds/languages")).body);
    QVERIFY2(titles(languages).contains(QStringLiteral("Français")), qPrintable(titles(languages).join(u',')));
    const auto french = std::find_if(languages.entries.cbegin(), languages.entries.cend(),
                                     [](const Entry &entry) { return entry.title == QStringLiteral("Français"); });
    QVERIFY(french != languages.entries.cend());
    const Feed frenchBooks = parseFeed(get(french->links.value(QStringLiteral("subsection"))).body);
    QVERIFY(titles(frenchBooks).contains(QStringLiteral("L'Écume des jours")));

    // A series' books in its order.
    const Feed series = parseFeed(get(QStringLiteral("/opds/series")).body);
    QCOMPARE(titles(series), (QStringList{ QStringLiteral("Chronicles of the Black Company"), QStringLiteral("Contes") }));
    QCOMPARE(titles(parseFeed(get(series.entries.first().links.value(QStringLiteral("subsection"))).body)),
             (QStringList{ QStringLiteral("The Black Company"), QStringLiteral("Shadows Linger") }));

    QCOMPARE(get(QStringLiteral("/opds/nothing")).status, 404);
}

void LibraryServerTest::servesFilesAndCovers()
{
    const Book *book = m_data.book(1);
    QVERIFY(book);
    const Answer epub = get(QStringLiteral("/get/1/epub"));
    QCOMPARE(epub.status, 200);
    QCOMPARE(epub.headers.value("content-type"), QByteArray("application/epub+zip"));
    QCOMPARE(epub.body, readAll(book->formats.first().file));
    // Named as Calibre's content server names it.
    QCOMPARE(epub.headers.value("content-disposition"),
             QByteArray("attachment; filename=\"L'Ecume des jours - Boris Vian_1.epub\""));

    const Answer cover = get(QStringLiteral("/cover/1"));
    QCOMPARE(cover.status, 200);
    QCOMPARE(cover.body, readAll(book->coverFile));
    const Answer thumbnail = get(QStringLiteral("/thumb/1"));
    QCOMPARE(thumbnail.status, 200);
    const QImage image = QImage::fromData(thumbnail.body, "JPEG");
    QVERIFY(!image.isNull());
    QVERIFY(image.height() <= 300);
    // Kept for the next time.
    QCOMPARE(QDir(m_cache.filePath(QStringLiteral("thumbnails"))).entryList(QDir::Files).size(), 1);
    QCOMPARE(get(QStringLiteral("/thumb/1")).body, thumbnail.body);

    const Answer head = get(QStringLiteral("/get/1/EPUB"), "HEAD");
    QCOMPARE(head.status, 200);
    QCOMPARE(head.headers.value("content-length").toLongLong(), book->formats.first().size);
    QVERIFY(head.body.isEmpty());

    QCOMPARE(get(QStringLiteral("/get/1/PDF")).status, 404);
    QCOMPARE(get(QStringLiteral("/get/99/EPUB")).status, 404);
    QCOMPARE(get(QStringLiteral("/cover/5")).status, 404);
    QCOMPARE(get(QStringLiteral("/opds"), "POST").status, 405);
}

void LibraryServerTest::asksForThePassword()
{
    m_server.setPassword(QStringLiteral("sésame"));
    const Answer refused = get(QStringLiteral("/opds"));
    QCOMPARE(refused.status, 401);
    QVERIFY(refused.headers.value("www-authenticate").startsWith("Basic"));
    QCOMPARE(get(QStringLiteral("/opds"), "GET", "Basic " + QByteArray("reader:wrong").toBase64()).status, 401);
    QCOMPARE(get(QStringLiteral("/opds"), "GET", "Basic " + QStringLiteral("reader:sésame").toUtf8().toBase64()).status,
             200);
    m_server.setPassword({});
    QCOMPARE(get(QStringLiteral("/opds")).status, 200);
}

void LibraryServerTest::showsAWebPage()
{
    const Answer page = get(QStringLiteral("/"));
    QCOMPARE(page.status, 200);
    QCOMPARE(page.headers.value("content-type"), QByteArray("text/html;charset=utf-8"));
    const QString html = QString::fromUtf8(page.body);
    QVERIFY(html.contains(QStringLiteral("<title>Bibliothèque</title>")));
    QVERIFY(html.contains(QStringLiteral("href=\"/get/1/EPUB\"")));
    QVERIFY(html.contains(QStringLiteral("src=\"/thumb/1\"")));

    // What was searched shows in the box, as text.
    const QString found = QString::fromUtf8(get(QStringLiteral("/?q=%22%3E%3Cb%3Eglen")).body);
    QVERIFY(found.contains(QStringLiteral("value=\"&quot;&gt;&lt;b&gt;glen\"")));
    QVERIFY(!found.contains(QStringLiteral("<b>glen")));
    const QString glen = QString::fromUtf8(get(QStringLiteral("/?q=glen")).body);
    QVERIFY(glen.contains(QStringLiteral("Shadows Linger")));
    QVERIFY(!glen.contains(QStringLiteral("Good Omens")));
}

void LibraryServerTest::pagesLongFeeds()
{
    LibraryData data;
    data.uuid = QStringLiteral("library");
    QList<qsizetype> rows;
    for (int id = 1; id <= 120; ++id) {
        Book book;
        book.id = id;
        book.title = QStringLiteral("Book %1").arg(id);
        book.uuid = QStringLiteral("uuid-%1").arg(id);
        data.books.append(book);
        rows.append(id - 1);
    }
    const QString path = QStringLiteral("/opds/books?sort=title");
    const Feed first = parseFeed(Opds::booksFeed(data, rows, QStringLiteral("All"), path, 0));
    QCOMPARE(first.entries.size(), Opds::pageSize);
    QCOMPARE(first.total, 120);
    QCOMPARE(first.links.value(QStringLiteral("next")), QStringLiteral("/opds/books?sort=title&offset=50"));
    QVERIFY(!first.links.contains(QStringLiteral("previous")));
    const Feed last = parseFeed(Opds::booksFeed(data, rows, QStringLiteral("All"), path, 100));
    QCOMPARE(last.entries.size(), 20);
    QCOMPARE(last.entries.first().title, QStringLiteral("Book 101"));
    QCOMPARE(last.links.value(QStringLiteral("previous")), QStringLiteral("/opds/books?sort=title&offset=50"));
    QVERIFY(!last.links.contains(QStringLiteral("next")));

    // A "+" in a name stays one, not a space.
    data.tags.append({ 1, QStringLiteral("C++"), {}, 1, {} });
    data.books.first().tagIds = { 1 };
    data.books.first().tags = { QStringLiteral("C++") };
    const Feed tags = parseFeed(Opds::categoryFeed(data, QStringLiteral("tags"), QStringLiteral("en")));
    QCOMPARE(tags.entries.first().links.value(QStringLiteral("subsection")),
             QStringLiteral("/opds/books?q=tags%3A%22%3DC%2B%2B%22&sort=title&title=C%2B%2B"));
}

QTEST_GUILESS_MAIN(LibraryServerTest)

#include "library_server_test.moc"
