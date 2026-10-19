#include <QtTest>

#include <QDesktopServices>
#include <QDirIterator>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "book_library.h"
#include "calibre_db.h"
#include "sqlite_db.h"

namespace {

QString fixtureLibrary()
{
    return QStringLiteral(RELIURE_TEST_DATA_DIR "/library");
}

CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

QList<int> ids(const QVariantList &rows)
{
    QList<int> result;
    for (const QVariant &row : rows)
        result.append(row.toMap().value(QStringLiteral("id")).toInt());
    return result;
}

QStringList names(const QVariantList &rows)
{
    QStringList result;
    for (const QVariant &row : rows)
        result.append(row.toMap().value(QStringLiteral("name")).toString());
    return result;
}

bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(source));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(source, target))
            return false;
    }
    return true;
}

// Takes the URLs QDesktopServices would open.
class UrlRecorder : public QObject
{
    Q_OBJECT

public:
    QList<QUrl> urls;

public slots:
    void open(const QUrl &url) { urls.append(url); }
};

} // namespace

class BookLibraryTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void loadsLibrary();
    void sortsBooks_data();
    void sortsBooks();
    void searchesBooks_data();
    void searchesBooks();
    void browsesSections();
    void filtersByItem();
    void describesCurrentBook();
    void opensFiles();
    void reloadsWhenDatabaseChanges();
    void keepsBooksWhenReloadFails();
    void switchesLibrary();
    void reportsMissingLibrary();

private:
    std::unique_ptr<BookLibrary> open(const QString &path = fixtureLibrary());
};

void BookLibraryTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

std::unique_ptr<BookLibrary> BookLibraryTest::open(const QString &path)
{
    auto library = std::make_unique<BookLibrary>(path, frenchConfig());
    QSignalSpy loaded(library.get(), &BookLibrary::libraryChanged);
    if (!loaded.wait(10000))
        return nullptr;
    return library;
}

void BookLibraryTest::loadsLibrary()
{
    auto library = open();
    QVERIFY(library);
    QVERIFY(library->loaded());
    QVERIFY(!library->loading());
    QVERIFY(library->lastError().isEmpty());
    QVERIFY(library->schemaKnown());
    QCOMPARE(library->schemaVersion(), 28);
    QCOMPARE(library->counts().value(QStringLiteral("books")).toInt(), 6);
    QCOMPARE(library->counts().value(QStringLiteral("authors")).toInt(), 6);
    QCOMPARE(library->scopeCount(), 6);
    QCOMPARE(library->rows().size(), 6);
    // Its copy of the database stays out of the library.
    QVERIFY(QFileInfo::exists(BookLibrary::cacheFolder(library->libraryPath()) + QStringLiteral("/snapshot.db")));
    QVERIFY(!QFileInfo::exists(QDir(fixtureLibrary()).filePath(QStringLiteral("metadata.db-journal"))));

    const QVariantMap black = library->rows().first().toMap();
    QCOMPARE(black.value(QStringLiteral("title")).toString(), QStringLiteral("The Black Company"));
    QCOMPARE(black.value(QStringLiteral("authors")).toString(), QStringLiteral("Glen Cook"));
    QCOMPARE(black.value(QStringLiteral("series")).toString(), QStringLiteral("Chronicles of the Black Company [1]"));
    QCOMPARE(black.value(QStringLiteral("formats")).toString(), QStringLiteral("AZW3, EPUB"));
    QCOMPARE(black.value(QStringLiteral("rating")).toString(), QStringLiteral("★★★★★"));
    QVERIFY(black.value(QStringLiteral("cover")).toString().startsWith(QStringLiteral("image://covers/")));
    QCOMPARE(black.value(QStringLiteral("added")).toString().size(), 10);
}

void BookLibraryTest::sortsBooks_data()
{
    QTest::addColumn<QString>("option");
    QTest::addColumn<QList<int>>("expected");
    // Titles sort by Calibre's title sort: "Black Company, The".
    QTest::newRow("title") << QStringLiteral("title") << QList<int>{ 2, 1, 4, 5, 6, 3 };
    QTest::newRow("titleDesc") << QStringLiteral("titleDesc") << QList<int>{ 3, 6, 5, 4, 1, 2 };
    QTest::newRow("authors") << QStringLiteral("authors") << QList<int>{ 2, 3, 5, 4, 6, 1 };
    // In series order, books in no series last.
    QTest::newRow("series") << QStringLiteral("series") << QList<int>{ 2, 3, 6, 1, 4, 5 };
    QTest::newRow("added") << QStringLiteral("added") << QList<int>{ 1, 2, 3, 4, 5, 6 };
    QTest::newRow("addedDesc") << QStringLiteral("addedDesc") << QList<int>{ 6, 5, 4, 3, 2, 1 };
    QTest::newRow("rating") << QStringLiteral("rating") << QList<int>{ 5, 6, 3, 4, 1, 2 };
}

void BookLibraryTest::sortsBooks()
{
    QFETCH(QString, option);
    QFETCH(QList<int>, expected);
    auto library = open();
    QVERIFY(library);
    library->setSortOption(option);
    QCOMPARE(library->sortOption(), option);
    QCOMPARE(ids(library->rows()), expected);
    library->setSortOption(QStringLiteral("nonsense"));
    QCOMPARE(library->sortOption(), option);
}

void BookLibraryTest::searchesBooks_data()
{
    QTest::addColumn<QString>("search");
    QTest::addColumn<QList<int>>("expected");
    QTest::newRow("accents") << QStringLiteral("ecume") << QList<int>{ 1 };
    QTest::newRow("author") << QStringLiteral("GLEN") << QList<int>{ 2, 3 };
    // Each word anywhere: Shadows Linger is in the Black Company's series.
    QTest::newRow("words anywhere") << QStringLiteral("black cook") << QList<int>{ 2, 3 };
    QTest::newRow("all words") << QStringLiteral("omens neil") << QList<int>{ 4 };
    QTest::newRow("tag") << QStringLiteral("fantasy") << QList<int>{ 2, 4, 3 };
    QTest::newRow("series") << QStringLiteral("chronicles") << QList<int>{ 2, 3 };
    QTest::newRow("second author") << QStringLiteral("gaiman") << QList<int>{ 4 };
    QTest::newRow("none") << QStringLiteral("zzz") << QList<int>{};
    QTest::newRow("blank") << QStringLiteral("   ") << QList<int>{ 2, 1, 4, 5, 6, 3 };
}

void BookLibraryTest::searchesBooks()
{
    QFETCH(QString, search);
    QFETCH(QList<int>, expected);
    auto library = open();
    QVERIFY(library);
    library->setSearchText(search);
    QCOMPARE(ids(library->rows()), expected);
    QCOMPARE(library->scopeCount(), 6);
}

void BookLibraryTest::browsesSections()
{
    auto library = open();
    QVERIFY(library);
    library->setSearchText(QStringLiteral("glen"));

    // A browse section lists items, not books, and starts without a search.
    library->setSection(QStringLiteral("authors"));
    QVERIFY(library->searchText().isEmpty());
    QVERIFY(library->rows().isEmpty());
    QCOMPARE(library->facetCount(), 6);
    QCOMPARE(names(library->facetRows()),
             (QStringList{ QStringLiteral("Glen Cook"), QStringLiteral("Jean-Pierre Dupont"),
                           QStringLiteral("Neil Gaiman"), QStringLiteral("Terry Pratchett"),
                           QStringLiteral("Antoine de Saint-Exupéry"), QStringLiteral("Boris Vian") }));
    QCOMPARE(library->facetRows().first().toMap().value(QStringLiteral("count")).toInt(), 2);

    library->setFacetSortOption(QStringLiteral("countDesc"));
    QCOMPARE(names(library->facetRows()).first(), QStringLiteral("Glen Cook"));
    library->setFacetSortOption(QStringLiteral("nameDesc"));
    QCOMPARE(names(library->facetRows()).first(), QStringLiteral("Boris Vian"));
    library->setFacetSortOption(QStringLiteral("name"));

    library->setSearchText(QStringLiteral("saint exupery"));
    QCOMPARE(names(library->facetRows()), QStringList{ QStringLiteral("Antoine de Saint-Exupéry") });

    library->setSection(QStringLiteral("tags"));
    QCOMPARE(names(library->facetRows()),
             (QStringList{ QStringLiteral("Classique"), QStringLiteral("Fantasy"), QStringLiteral("Humour"),
                           QStringLiteral("Roman"), QStringLiteral("Technique") }));
    library->setSection(QStringLiteral("languages"));
    QCOMPARE(names(library->facetRows()), (QStringList{ QStringLiteral("English"), QStringLiteral("French") }));
    library->setSection(QStringLiteral("series"));
    QCOMPARE(names(library->facetRows()),
             (QStringList{ QStringLiteral("Chronicles of the Black Company"), QStringLiteral("Contes") }));
    library->setSection(QStringLiteral("publishers"));
    QCOMPARE(library->facetCount(), 3);
    library->setSection(QStringLiteral("nowhere"));
    QCOMPARE(library->section(), QStringLiteral("publishers"));
}

void BookLibraryTest::filtersByItem()
{
    auto library = open();
    QVERIFY(library);
    library->setSection(QStringLiteral("authors"));
    QSignalSpy filterChanged(library.get(), &BookLibrary::filterChanged);
    library->openFacetItem(2);
    QCOMPARE(filterChanged.size(), 1);
    QCOMPARE(library->filter().value(QStringLiteral("kind")).toString(), QStringLiteral("authors"));
    QCOMPARE(library->filter().value(QStringLiteral("name")).toString(), QStringLiteral("Glen Cook"));
    QCOMPARE(ids(library->rows()), (QList<int>{ 2, 3 }));
    QCOMPARE(library->scopeCount(), 2);

    library->setSearchText(QStringLiteral("shadows"));
    QCOMPARE(ids(library->rows()), QList<int>{ 3 });
    QCOMPARE(library->scopeCount(), 2);

    library->clearFilter();
    QVERIFY(library->filter().isEmpty());
    QVERIFY(library->searchText().isEmpty());
    QVERIFY(library->rows().isEmpty());

    // An id the section does not have changes nothing.
    library->openFacetItem(999);
    QVERIFY(library->filter().isEmpty());

    library->setSection(QStringLiteral("languages"));
    const QVariantList languages = library->facetRows();
    library->openFacetItem(languages.last().toMap().value(QStringLiteral("id")).toInt());
    QCOMPARE(ids(library->rows()), (QList<int>{ 1, 5, 6 }));
    library->setSection(QStringLiteral("books"));
    QVERIFY(library->filter().isEmpty());
    QCOMPARE(library->rows().size(), 6);
}

void BookLibraryTest::describesCurrentBook()
{
    auto library = open();
    QVERIFY(library);
    library->setCurrentBookId(1);
    QVariantMap book = library->currentBook();
    QCOMPARE(book.value(QStringLiteral("title")).toString(), QStringLiteral("L'Écume des jours"));
    QCOMPARE(book.value(QStringLiteral("sort")).toString(), QStringLiteral("Écume des jours, L'"));
    QCOMPARE(book.value(QStringLiteral("authors")).toStringList(), QStringList{ QStringLiteral("Boris Vian") });
    QCOMPARE(book.value(QStringLiteral("rating")).toString(), QStringLiteral("★★★★"));
    QCOMPARE(book.value(QStringLiteral("published")).toString(), QStringLiteral("Mar 1947"));
    QCOMPARE(book.value(QStringLiteral("languages")).toStringList(), QStringList{ QStringLiteral("French") });
    QCOMPARE(book.value(QStringLiteral("publisher")).toString(), QStringLiteral("Gallimard"));
    // The description keeps its structure and loses the editor's styling.
    QCOMPARE(book.value(QStringLiteral("description")).toString(),
             QStringLiteral("<div><p>Un roman d’amour fou.</p><p>Paris, <em>1946</em>.</p></div>"));
    const QVariantList identifiers = book.value(QStringLiteral("identifiers")).toList();
    QCOMPARE(identifiers.size(), 1);
    QCOMPARE(identifiers.first().toMap().value(QStringLiteral("label")).toString(), QStringLiteral("ISBN"));
    const QVariantList formats = book.value(QStringLiteral("formats")).toList();
    QCOMPARE(formats.size(), 1);
    QCOMPARE(formats.first().toMap().value(QStringLiteral("format")).toString(), QStringLiteral("EPUB"));
    QVERIFY(!formats.first().toMap().value(QStringLiteral("missing")).toBool());
    QCOMPARE(book.value(QStringLiteral("missingFiles")).toInt(), 0);

    library->setCurrentBookId(4);
    book = library->currentBook();
    QCOMPARE(book.value(QStringLiteral("rating")).toString(), QStringLiteral("★★★½"));
    QCOMPARE(book.value(QStringLiteral("description")).toString(),
             QStringLiteral("<p>The world will end next Saturday.</p>"));
    const QVariantMap goodreads = book.value(QStringLiteral("identifiers")).toList().first().toMap();
    QCOMPARE(goodreads.value(QStringLiteral("label")).toString(), QStringLiteral("Goodreads"));
    QCOMPARE(goodreads.value(QStringLiteral("url")).toString(), QStringLiteral("https://www.goodreads.com/book/show/12067"));

    library->setCurrentBookId(6);
    book = library->currentBook();
    QCOMPARE(book.value(QStringLiteral("seriesText")).toString(), QStringLiteral("Book 1.5 of Contes"));
    QVERIFY(book.value(QStringLiteral("published")).toString().isEmpty());
    QVERIFY(book.value(QStringLiteral("formats")).toList().isEmpty());

    library->setCurrentBookId(42);
    QCOMPARE(library->currentBookId(), 0);
    QVERIFY(library->currentBook().isEmpty());
}

void BookLibraryTest::opensFiles()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    const QDir black(copy.filePath(QStringLiteral("Glen Cook/The Black Company (2)")));
    QVERIFY(QFile::remove(black.filePath(QStringLiteral("The Black Company - Glen Cook.epub"))));
    auto library = open(copy.path());
    QVERIFY(library);
    UrlRecorder recorder;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &recorder, "open");

    QCOMPARE(library->openFormat(1), QString());
    QCOMPARE(recorder.urls.size(), 1);
    QVERIFY(recorder.urls.last().toLocalFile().endsWith(QStringLiteral(".epub")));
    // EPUB first, but a missing EPUB gives way to the AZW3.
    QCOMPARE(library->openFormat(2), QString());
    QVERIFY(recorder.urls.last().toLocalFile().endsWith(QStringLiteral(".azw3")));
    QCOMPARE(library->openFormat(2, QStringLiteral("epub")), QStringLiteral("The EPUB file is missing from the library."));
    QCOMPARE(library->openFormat(6), QStringLiteral("This book has no file to open."));
    QCOMPARE(library->openFormat(99), QStringLiteral("This book is no longer in the library."));
    QCOMPARE(library->openFolder(5), QString());
    QVERIFY(recorder.urls.last().toLocalFile().endsWith(QStringLiteral("Manuel technique (5)")));
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
}

void BookLibraryTest::reloadsWhenDatabaseChanges()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    auto library = open(copy.path());
    QVERIFY(library);
    library->setCurrentBookId(5);

    // Calibre renaming a book, as its own connection would.
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(copy.filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY2(CalibreDb::prepareConnection(db, CalibreText::Tweaks::defaults(), QStringLiteral("fr"), &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QStringLiteral("UPDATE books SET title='Le Manuel' WHERE id=5"), &error), qPrintable(error));
        // books_update_trg sorted it with title_sort().
        QCOMPARE(db.value(QStringLiteral("SELECT sort FROM books WHERE id=5")).toString(), QStringLiteral("Manuel, Le"));
    }
    QTRY_COMPARE_WITH_TIMEOUT(library->currentBook().value(QStringLiteral("title")).toString(),
                              QStringLiteral("Le Manuel"), 10000);
    QCOMPARE(library->currentBookId(), 5);
}

void BookLibraryTest::keepsBooksWhenReloadFails()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    auto library = open(copy.path());
    QVERIFY(library);
    QFile journal(copy.filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();

    QSignalSpy failed(library.get(), &BookLibrary::loadFailed);
    library->reload();
    QVERIFY(failed.wait(10000));
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("Calibre is writing")));
    QVERIFY(library->loaded());
    QCOMPARE(library->rows().size(), 6);
}

void BookLibraryTest::switchesLibrary()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(copy.filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        QVERIFY(db.exec(QStringLiteral("DELETE FROM books_authors_link WHERE book=6; DELETE FROM books WHERE id=6"), &error));
    }
    auto library = open();
    QVERIFY(library);
    library->setCurrentBookId(1);
    QSignalSpy changed(library.get(), &BookLibrary::libraryChanged);
    library->setLibraryPath(copy.path());
    QCOMPARE(library->libraryPath(), QDir::cleanPath(copy.path()));
    QVERIFY(!library->loaded());
    QCOMPARE(library->currentBookId(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(library->loaded(), 10000);
    QCOMPARE(library->rows().size(), 5);
}

void BookLibraryTest::reportsMissingLibrary()
{
    QTemporaryDir empty;
    auto library = std::make_unique<BookLibrary>(empty.path(), frenchConfig());
    QSignalSpy changed(library.get(), &BookLibrary::libraryChanged);
    QVERIFY(changed.wait(10000));
    QVERIFY(!library->loaded());
    QVERIFY(library->lastError().contains(QStringLiteral("no Calibre library")));
    QVERIFY(library->rows().isEmpty());
}

QTEST_MAIN(BookLibraryTest)
#include "book_library_test.moc"
