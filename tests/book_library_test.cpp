#include <QtTest>

#include <QDesktopServices>
#include <QDirIterator>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "calibre_languages.h"
#include "book_library.h"
#include "calibre_db.h"
#include "conversion_service.h"
#include "metadata_downloader.h"
#include "device_library.h"
#include "fake_open_library.h"
#include "sqlite_db.h"
#include "test_support.h"

namespace {

CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

// The rows the book list shows, as maps.
QVariantList bookRows(BookLibrary &library)
{
    QVariantList result;
    for (int row = 0; row < library.books()->count(); ++row)
        result.append(library.books()->get(row));
    return result;
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
    void init();
    void loadsLibrary();
    void sortsBooks_data();
    void sortsBooks();
    void keepsColumnsAndSort();
    void searchesBooks_data();
    void searchesBooks();
    void keepsRowsInPlace();
    void keepsVirtualLibraryChoice();
    void undoesOnlyBulkEdits();
    void browsesSections();
    void filtersByItem();
    void describesCurrentBook();
    void opensFiles();
    void reloadsWhenDatabaseChanges();
    void keepsBooksWhenReloadFails();
    void switchesLibrary();
    void reportsMissingLibrary();
    void followsCalibreOnReload();
    void editsBooks();
    void followsSortsLikeCalibre();
    void refusesWhileReadOnly();
    void revertsCurrentBook();
    void addsBooks();
    void addsAndRemovesFormats();
    void movesBooksToTrash();
    void convertsBooks();
    void convertsWithoutCalibre();
    void downloadsMetadata();
    void downloadsFromOpenLibrary();
    void sendsToKobos();
    void sendsToKobosWithoutCalibre();
    void sendsToKindles();
    void checksLibrary();
    void mergesAuthorVariants();
    void keepsOneCopy();
    void removesMissingEntries();

private:
    std::unique_ptr<BookLibrary> open(const QString &path = fixtureLibrary());
};

void BookLibraryTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("cousoir-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("book-library"));
    QSettings().clear();
    // Calibre's names of languages from a part of its resources, whatever
    // Calibre is installed.
    CalibreLanguages::setResourcesFolder(QStringLiteral(COUSOIR_TEST_DATA_DIR "/calibre-resources"));
}

// Each test starts with no columns or sort kept: the fixture's copies are
// all the same library.
void BookLibraryTest::init()
{
    QSettings().clear();
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
    QCOMPARE(bookRows(*library).size(), 6);
    // Its copy of the database stays out of the library.
    QVERIFY(QFileInfo::exists(BookLibrary::cacheFolder(library->libraryPath()) + QStringLiteral("/snapshot.db")));
    QVERIFY(!QFileInfo::exists(QDir(fixtureLibrary()).filePath(QStringLiteral("metadata.db-journal"))));

    const QVariantMap black = bookRows(*library).first().toMap();
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
    // As Calibre sorts them: books in no series first, by title as the list
    // was sorted before; the series in the order of their numbers.
    QTest::newRow("series") << QStringLiteral("series") << QList<int>{ 1, 4, 5, 2, 3, 6 };
    QTest::newRow("seriesDesc") << QStringLiteral("seriesDesc") << QList<int>{ 6, 3, 2, 1, 4, 5 };
    QTest::newRow("timestamp") << QStringLiteral("timestamp") << QList<int>{ 1, 2, 3, 4, 5, 6 };
    // Reversed, books added the same second keep their order.
    QTest::newRow("timestampDesc") << QStringLiteral("timestampDesc") << QList<int>{ 6, 4, 5, 3, 2, 1 };
    QTest::newRow("rating") << QStringLiteral("rating") << QList<int>{ 5, 6, 3, 4, 1, 2 };
    // An undefined date is Calibre's year 101.
    QTest::newRow("pubdate") << QStringLiteral("pubdate") << QList<int>{ 4, 5, 6, 3, 1, 2 };
}

void BookLibraryTest::sortsBooks()
{
    QFETCH(QString, option);
    QFETCH(QList<int>, expected);
    auto library = open();
    QVERIFY(library);
    library->setSortOption(option);
    QCOMPARE(library->sortOption(), option);
    QCOMPARE(ids(bookRows(*library)), expected);
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
    QCOMPARE(ids(bookRows(*library)), expected);
    QCOMPARE(library->scopeCount(), 6);
}

void BookLibraryTest::keepsRowsInPlace()
{
    auto library = open();
    QVERIFY(library);
    BookListModel *books = library->books();
    QCOMPARE(books->count(), 6);
    QCOMPARE(books->idAt(0), 2);
    QCOMPARE(books->indexOf(2), 0);
    QCOMPARE(books->indexOf(3), 5);
    QCOMPARE(books->indexOf(99), -1);
    QCOMPARE(books->idAt(6), 0);
    QVERIFY(books->get(-1).isEmpty());
    QCOMPARE(books->data(books->index(1), Qt::UserRole).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("L'Écume des jours"));

    // The same books read again only change their values; another order
    // starts the list again.
    QSignalSpy resets(books, &QAbstractItemModel::modelReset);
    QSignalSpy changes(books, &QAbstractItemModel::dataChanged);
    QSignalSpy loaded(library.get(), &BookLibrary::libraryChanged);
    library->reload();
    QVERIFY(loaded.wait(10000));
    QCOMPARE(resets.count(), 0);
    QCOMPARE(changes.count(), 1);
    library->setSortOption(QStringLiteral("added"));
    QCOMPARE(resets.count(), 1);
    QCOMPARE(books->ids(), (QList<int>{ 1, 2, 3, 4, 5, 6 }));
    // A search narrows the sorted list without sorting it again.
    library->setSearchText(QStringLiteral("glen"));
    QCOMPARE(books->ids(), (QList<int>{ 2, 3 }));
    QCOMPARE(books->indexOf(3), 1);
    library->setSortOption(QStringLiteral("addedDesc"));
    QCOMPARE(books->ids(), (QList<int>{ 3, 2 }));
}

void BookLibraryTest::browsesSections()
{
    auto library = open();
    QVERIFY(library);
    library->setSearchText(QStringLiteral("glen"));

    // A browse section lists items, not books, and starts without a search.
    library->setSection(QStringLiteral("authors"));
    QVERIFY(library->searchText().isEmpty());
    QVERIFY(bookRows(*library).isEmpty());
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
    QCOMPARE(names(library->facetRows()), (QStringList{ QStringLiteral("Anglais"), QStringLiteral("Français") }));
    library->setSection(QStringLiteral("series"));
    QCOMPARE(names(library->facetRows()),
             (QStringList{ QStringLiteral("Chronicles of the Black Company"), QStringLiteral("Contes") }));
    library->setSection(QStringLiteral("publishers"));
    QCOMPARE(library->facetCount(), 3);
    library->setSection(QStringLiteral("nowhere"));
    QCOMPARE(library->section(), QStringLiteral("publishers"));
}

void BookLibraryTest::keepsVirtualLibraryChoice()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    {
        SqliteDb db;
        QString error;
        QVERIFY(db.open(QDir(copy.path()).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
        // Broken's search misses its closing parenthesis.
        QVERIFY2(db.exec(QStringLiteral(
            "INSERT OR REPLACE INTO preferences(key, val) VALUES "
            "('virtual_libraries', '{\"Fantasy\": \"tags:fantasy\", \"Broken\": \"(tags:x\"}'),"
            "('virtual_lib_on_startup', '\"Fantasy\"'),"
            "('saved_searches', '{\"Cook\": \"author:cook\", \"Read\": \"rating:many\"}');"), &error),
                 qPrintable(error));
    }
    QSettings().remove(QStringLiteral("Library/virtualLibrary"));

    // Calibre's choice at the library's start, until one is made here.
    auto library = open(copy.path());
    QVERIFY(library);
    QCOMPARE(library->virtualLibraries(), (QStringList{ QStringLiteral("Broken"), QStringLiteral("Fantasy") }));
    QCOMPARE(library->virtualLibrary(), QStringLiteral("Fantasy"));
    QCOMPARE(library->counts().value(QStringLiteral("books")).toInt(), 3);
    // Fantasy and Humour.
    QCOMPARE(library->counts().value(QStringLiteral("tags")).toInt(), 2);
    QCOMPARE(ids(bookRows(*library)), (QList<int>{ 2, 4, 3 }));
    QCOMPARE(library->savedSearches().value(0).toMap().value(QStringLiteral("count")).toInt(), 2);

    // A virtual library Cousoir cannot search leaves the books whole, and
    // says why.
    library->setVirtualLibrary(QStringLiteral("Broken"));
    QCOMPARE(bookRows(*library).size(), 6);
    QCOMPARE(library->virtualLibraryError(), QStringLiteral("missing )"));
    library->setVirtualLibrary(QString());
    QVERIFY(library->virtualLibraryError().isEmpty());
    QCOMPARE(bookRows(*library).size(), 6);
    QCOMPARE(library->savedSearches().value(0).toMap().value(QStringLiteral("count")).toInt(), 2);
    // A saved search Cousoir cannot run has no count, but its reason.
    const QVariantMap read = library->savedSearches().value(1).toMap();
    QCOMPARE(read.value(QStringLiteral("count")).toString(), QString());
    QVERIFY(!read.value(QStringLiteral("error")).toString().isEmpty());
    library->setSection(QStringLiteral("search:Read"));
    QVERIFY(bookRows(*library).isEmpty());
    QCOMPARE(library->sectionError(), read.value(QStringLiteral("error")).toString());
    library.reset();

    // None is a choice too.
    library = open(copy.path());
    QVERIFY(library);
    QCOMPARE(library->virtualLibrary(), QString());
    QCOMPARE(library->counts().value(QStringLiteral("books")).toInt(), 6);
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
    QCOMPARE(ids(bookRows(*library)), (QList<int>{ 2, 3 }));
    QCOMPARE(library->scopeCount(), 2);

    library->setSearchText(QStringLiteral("shadows"));
    QCOMPARE(ids(bookRows(*library)), QList<int>{ 3 });
    QCOMPARE(library->scopeCount(), 2);

    library->clearFilter();
    QVERIFY(library->filter().isEmpty());
    QVERIFY(library->searchText().isEmpty());
    QVERIFY(bookRows(*library).isEmpty());

    // An id the section does not have changes nothing.
    library->openFacetItem(999);
    QVERIFY(library->filter().isEmpty());

    library->setSection(QStringLiteral("languages"));
    const QVariantList languages = library->facetRows();
    library->openFacetItem(languages.last().toMap().value(QStringLiteral("id")).toInt());
    QCOMPARE(ids(bookRows(*library)), (QList<int>{ 1, 5, 6 }));
    library->setSection(QStringLiteral("books"));
    QVERIFY(library->filter().isEmpty());
    QCOMPARE(bookRows(*library).size(), 6);
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
    QCOMPARE(book.value(QStringLiteral("languages")).toStringList(), QStringList{ QStringLiteral("Français") });
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
    QCOMPARE(bookRows(*library).size(), 6);
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
    QCOMPARE(bookRows(*library).size(), 5);
}

void BookLibraryTest::reportsMissingLibrary()
{
    QTemporaryDir empty;
    auto library = std::make_unique<BookLibrary>(empty.path(), frenchConfig());
    QSignalSpy changed(library.get(), &BookLibrary::libraryChanged);
    QVERIFY(changed.wait(10000));
    QVERIFY(!library->loaded());
    QVERIFY(library->lastError().contains(QStringLiteral("no Calibre library")));
    QVERIFY(bookRows(*library).isEmpty());
}

void BookLibraryTest::followsCalibreOnReload()
{
    QTemporaryDir config;
    qunsetenv("COUSOIR_LIBRARY_PATH");
    qputenv("CALIBRE_CONFIG_DIRECTORY", QFile::encodeName(config.path()));
    const auto restore = qScopeGuard([] { qunsetenv("CALIBRE_CONFIG_DIRECTORY"); });
    auto library = std::make_unique<BookLibrary>(QString(), frenchConfig());
    QSignalSpy changed(library.get(), &BookLibrary::libraryChanged);
    QVERIFY(changed.wait(10000));
    QVERIFY(library->libraryPath().isEmpty());
    QVERIFY(!library->loaded());

    // Calibre's welcome wizard made a library meanwhile.
    QFile global(config.filePath(QStringLiteral("global.py.json")));
    QVERIFY(global.open(QIODevice::WriteOnly));
    global.write(QJsonDocument(QJsonObject{ { QStringLiteral("library_path"), fixtureLibrary() } }).toJson());
    global.close();
    library->reload();
    QCOMPARE(library->libraryPath(), QDir::cleanPath(fixtureLibrary()));
    QTRY_VERIFY_WITH_TIMEOUT(library->loaded(), 10000);
    QCOMPARE(bookRows(*library).size(), 6);
}

namespace {

// A copy of the fixture library the tests may write, old enough for that.
std::unique_ptr<QTemporaryDir> writableCopy()
{
    auto copy = std::make_unique<QTemporaryDir>();
    if (!copyTree(fixtureLibrary(), copy->path()))
        return nullptr;
    return copy;
}

bool waitForSave(BookLibrary *library)
{
    QSignalSpy saved(library, &BookLibrary::saved);
    QSignalSpy failed(library, &BookLibrary::saveFailed);
    if (!library->saving())
        return true;
    return saved.wait(15000) || !failed.isEmpty();
}

QString title(BookLibrary *library)
{
    return library->currentBook().value(QStringLiteral("title")).toString();
}

} // namespace

void BookLibraryTest::undoesOnlyBulkEdits()
{
    QTemporaryDir copy;
    QVERIFY(copyTree(fixtureLibrary(), copy.path()));
    auto library = open(copy.path());
    QVERIFY(library);
    library->setWritable(true);
    const auto field = [&library](int id, const char *key) {
        library->setCurrentBookId(id);
        const QVariant value = library->currentBook().value(QLatin1String(key));
        library->setCurrentBookId(0);
        return value;
    };
    const QVariant publisher2 = field(2, "publisher");

    QCOMPARE(library->updateBooks({ 1, 2 }, { { QStringLiteral("publisher"), QStringLiteral("Folio") },
                                              { QStringLiteral("rating"), 6 } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->bulkUndoCount(), 2);
    QCOMPARE(field(1, "publisher").toString(), QStringLiteral("Folio"));
    // A field edited since stays as it is edited.
    QCOMPARE(library->updateBook(1, { { QStringLiteral("rating"), 2 } }), QString());
    QVERIFY(waitForSave(library.get()));
    library->undoBulkEdit();
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(field(1, "publisher").toString(), QStringLiteral("Gallimard"));
    QCOMPARE(field(1, "rating").toString(), QStringLiteral("★"));
    QCOMPARE(field(2, "publisher"), publisher2);
    QCOMPARE(library->bulkUndoCount(), 0);

    // Two in a row: the undo puts back the second only.
    QCOMPARE(library->updateBooks({ 3 }, { { QStringLiteral("publisher"), QStringLiteral("A") } }), QString());
    QCOMPARE(library->updateBooks({ 3 }, { { QStringLiteral("publisher"), QStringLiteral("B") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_VERIFY_WITH_TIMEOUT(!library->saving(), 15000);
    QTRY_COMPARE(field(3, "publisher").toString(), QStringLiteral("B"));
    QCOMPARE(library->bulkUndoCount(), 1);
    library->undoBulkEdit();
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(field(3, "publisher").toString(), QStringLiteral("A"));
}

void BookLibraryTest::editsBooks()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    library->setCurrentBookId(2);
    QSignalSpy saved(library.get(), &BookLibrary::saved);
    QCOMPARE(library->updateBook(2, { { QStringLiteral("title"), QStringLiteral("The White Rose") } }), QString());
    QVERIFY(library->saving());
    // Edits made meanwhile wait their turn.
    QCOMPARE(library->updateBook(2, { { QStringLiteral("tags"), QStringLiteral("Fantasy, Military") } }), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!library->saving(), 15000);
    QVERIFY(saved.size() >= 1);
    QCOMPARE(title(library.get()), QStringLiteral("The White Rose"));
    QCOMPARE(library->currentBook().value(QStringLiteral("tags")).toStringList(),
             (QStringList{ QStringLiteral("Fantasy"), QStringLiteral("Military") }));
    QVERIFY(!library->publishedKey().isEmpty());
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Glen Cook/The White Rose (2)/The White Rose - Glen Cook.azw3"))));
    // The session's first change left a backup.
    QVERIFY(!QDir(library->backupFolder()).entryList({ QStringLiteral("metadata-*.db") }).isEmpty());

    // A value it refuses comes back at once.
    QVERIFY(!library->updateBook(2, { { QStringLiteral("pubdate"), QStringLiteral("someday") } }).isEmpty());
    QVERIFY(!library->updateBook(2, { { QStringLiteral("colour"), QStringLiteral("red") } }).isEmpty());
    QVERIFY(!library->updateBook(99, { { QStringLiteral("title"), QStringLiteral("X") } }).isEmpty());
}

// The list's columns and sort are kept for the library, Calibre's own at
// first; a sort keeps the earlier ones for its ties, as Calibre's list does.
void BookLibraryTest::keepsColumnsAndSort()
{
    auto copy = writableCopy();
    {
        auto library = open(copy->path());
        QVERIFY(library);
        QCOMPARE(library->columns(), (QStringList{ QStringLiteral("title"), QStringLiteral("authors"),
                                                   QStringLiteral("series"), QStringLiteral("formats"),
                                                   QStringLiteral("timestamp"), QStringLiteral("rating") }));
        // Known columns once, the title first.
        library->setColumns({ QStringLiteral("pubdate"), QStringLiteral("title"), QStringLiteral("nonsense"),
                              QStringLiteral("pubdate"), QStringLiteral("tags") });
        QCOMPARE(library->columns(), (QStringList{ QStringLiteral("title"), QStringLiteral("pubdate"), QStringLiteral("tags") }));
        const QVariantList available = library->availableColumns();
        QCOMPARE(available.at(1).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("pubdate"));
        QVERIFY(available.at(2).toMap().value(QStringLiteral("shown")).toBool());
        QVERIFY(!available.at(3).toMap().value(QStringLiteral("shown")).toBool());
        // The rows have the text of each column.
        const QVariantMap row = library->books()->get(library->books()->indexOf(4));
        QCOMPARE(row.value(QStringLiteral("tags")).toString(), QStringLiteral("Fantasy, Humour"));
        QCOMPARE(library->books()->get(library->books()->indexOf(1)).value(QStringLiteral("pubdate")).toString(),
                 QStringLiteral("Mar 1947"));

        // Sorted by rating, the books without one by title, as before.
        library->sortBy(QStringLiteral("rating"));
        QCOMPARE(library->sortOption(), QStringLiteral("rating"));
        library->sortBy(QStringLiteral("rating"));
        QCOMPARE(library->sortOption(), QStringLiteral("ratingDesc"));
        QCOMPARE(ids(bookRows(*library)), (QList<int>{ 2, 1, 4, 5, 6, 3 }));
        // Cousoir's old name.
        library->setSortOption(QStringLiteral("addedDesc"));
        QCOMPARE(library->sortOption(), QStringLiteral("timestampDesc"));
    }
    {
        // Kept for the library.
        auto library = open(copy->path());
        QVERIFY(library);
        QCOMPARE(library->columns(), (QStringList{ QStringLiteral("title"), QStringLiteral("pubdate"), QStringLiteral("tags") }));
        QCOMPARE(library->sortOption(), QStringLiteral("timestampDesc"));
        // Its ties by rating, then title: 4 before 5, added the same second.
        QCOMPARE(ids(bookRows(*library)), (QList<int>{ 6, 4, 5, 3, 2, 1 }));
    }

    // Calibre's list as it left it, before Cousoir kept anything.
    QSettings().clear();
    SqliteDb db;
    QString error;
    QVERIFY(db.open(copy->filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
    QVERIFY(db.exec(QStringLiteral(
        "INSERT INTO preferences(key, val) VALUES ('library_view books view state', "
        "'{\"hidden_columns\": [\"tags\", \"size\"], \"column_positions\": {\"ondevice\": 0, \"authors\": 1, "
        "\"title\": 2, \"tags\": 3, \"size\": 4, \"rating\": 5, \"publisher\": 6}, "
        "\"sort_history\": [[\"rating\", false], [\"date\", true], [\"title\", true], [\"authors\", true]]}')"),
                    &error));
    db.close();
    auto library = open(copy->path());
    QVERIFY(library);
    QCOMPARE(library->columns(), (QStringList{ QStringLiteral("title"), QStringLiteral("authors"),
                                               QStringLiteral("rating"), QStringLiteral("publisher") }));
    QCOMPARE(library->sortOption(), QStringLiteral("ratingDesc"));
    // Calibre's three last sorts: the books without a rating by date added.
    QCOMPARE(ids(bookRows(*library)), (QList<int>{ 2, 1, 4, 3, 5, 6 }));
}

void BookLibraryTest::followsSortsLikeCalibre()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    library->setCurrentBookId(2);
    // A sort that followed its title follows it, with the book's English
    // articles.
    QCOMPARE(library->updateBook(2, { { QStringLiteral("title"), QStringLiteral("The Silver Spike") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("sort")).toString(), QStringLiteral("Silver Spike, The"));
    // A sort set by hand stays.
    QCOMPARE(library->updateBook(2, { { QStringLiteral("sort"), QStringLiteral("Spike") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QCOMPARE(library->updateBook(2, { { QStringLiteral("title"), QStringLiteral("The Silver Spike (2)") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(title(library.get()), QStringLiteral("The Silver Spike (2)"));
    QCOMPARE(library->currentBook().value(QStringLiteral("sort")).toString(), QStringLiteral("Spike"));

    // The same for the author sort.
    QCOMPARE(library->updateBook(2, { { QStringLiteral("authors"), QStringLiteral("Glen Cook & Neil Gaiman") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("authorSort")).toString(), QStringLiteral("Cook, Glen & Gaiman, Neil"));
    QCOMPARE(library->updateBook(2, { { QStringLiteral("authorSort"), QStringLiteral("Cook") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QCOMPARE(library->updateBook(2, { { QStringLiteral("authors"), QStringLiteral("Glen Cook") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("authors")).toStringList(), QStringList{ QStringLiteral("Glen Cook") });
    QCOMPARE(library->currentBook().value(QStringLiteral("authorSort")).toString(), QStringLiteral("Cook"));
}

void BookLibraryTest::refusesWhileReadOnly()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    const QString error = library->updateBook(1, { { QStringLiteral("title"), QStringLiteral("X") } });
    QVERIFY(error.contains(QStringLiteral("cannot be changed")));
    QVERIFY(!library->saving());
    QVERIFY(!library->pasteCover(1).isEmpty());
}

void BookLibraryTest::revertsCurrentBook()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    library->setCurrentBookId(1);
    const QByteArray cover = [&] {
        QFile file(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/cover.jpg")));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    QVERIFY(!library->canRevert());
    QCOMPARE(library->updateBook(1, { { QStringLiteral("title"), QStringLiteral("Mood Indigo") },
                                      { QStringLiteral("pubdate"), QString() },
                                      { QStringLiteral("rating"), 3 } }), QString());
    QVERIFY(library->canRevert());
    QVERIFY(waitForSave(library.get()));
    QCOMPARE(library->removeCover(1), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(title(library.get()), QStringLiteral("Mood Indigo"));
    QVERIFY(!library->currentBook().value(QStringLiteral("hasCover")).toBool());

    library->revertCurrentBook();
    QVERIFY(!library->canRevert());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(title(library.get()), QStringLiteral("L'Écume des jours"));
    const QVariantMap book = library->currentBook();
    QCOMPARE(book.value(QStringLiteral("sort")).toString(), QStringLiteral("Écume des jours, L'"));
    QCOMPARE(book.value(QStringLiteral("rating")).toString(), QStringLiteral("★★★★"));
    QCOMPARE(book.value(QStringLiteral("pubdateText")).toString(), QStringLiteral("1947-03-20"));
    QFile restored(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/cover.jpg")));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), cover);
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/L'Ecume des jours - Boris Vian.epub"))));
}

namespace {

QString importFile(const QString &name)
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/import/") + name;
}

QStringList formatsOf(const QVariantMap &book)
{
    QStringList formats;
    for (const QVariant &format : book.value(QStringLiteral("formats")).toList())
        formats.append(format.toMap().value(QStringLiteral("format")).toString());
    return formats;
}

QStringList trashKeys(BookLibrary *library)
{
    QStringList keys;
    for (const QVariant &entry : library->trashEntries())
        keys.append(entry.toMap().value(QStringLiteral("key")).toString());
    return keys;
}

} // namespace

void BookLibraryTest::addsBooks()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    QVERIFY(!library->addBooks({}).isEmpty());
    QVERIFY(!library->addBooks({ QUrl::fromLocalFile(copy->filePath(QStringLiteral("nothing.epub"))) }).isEmpty());

    // The fixture's own EPUB is a book the library has: Calibre would ask.
    QSignalSpy duplicates(library.get(), &BookLibrary::duplicatesFound);
    QSignalSpy added(library.get(), &BookLibrary::booksAdded);
    const QString existing = copy->filePath(QStringLiteral("Glen Cook/The Black Company (2)/The Black Company - Glen Cook.epub"));
    QCOMPARE(library->addBooks({ QUrl::fromLocalFile(importFile(QStringLiteral("Le Vicomte - Inconnu.epub"))),
                                 QUrl::fromLocalFile(existing) }), QString());
    QVERIFY(library->importing());
    QVERIFY(duplicates.wait(15000));
    QCOMPARE(duplicates.first().first().toStringList(), QStringList{ QStringLiteral("The Black Company") });
    QCOMPARE(duplicates.first().at(1).toInt(), 2);
    QVERIFY(!library->addBooks({ QUrl::fromLocalFile(existing) }).isEmpty());
    library->addDuplicates(false);
    QVERIFY(added.wait(15000));
    QCOMPARE(added.first().first().toList(), QVariantList{ 7 });
    QCOMPARE(library->counts().value(QStringLiteral("books")).toInt(), 7);

    library->setCurrentBookId(7);
    const QVariantMap book = library->currentBook();
    QCOMPARE(book.value(QStringLiteral("title")).toString(), QStringLiteral("Le Vicomte de Bragelonne: Tome premier"));
    QCOMPARE(book.value(QStringLiteral("authors")).toStringList(),
             (QStringList{ QStringLiteral("Auguste Maquet"), QStringLiteral("Alexandre Dumas") }));
    QCOMPARE(book.value(QStringLiteral("series")).toString(), QStringLiteral("Les Mousquetaires"));
    QVERIFY(!book.value(QStringLiteral("cover")).toString().isEmpty());
    QCOMPARE(formatsOf(book), QStringList{ QStringLiteral("EPUB") });
    const QString folder = copy->filePath(QStringLiteral("Auguste Maquet/Le Vicomte de Bragelonne_ Tome premier (7)"));
    QVERIFY(QFileInfo::exists(folder + QStringLiteral("/Le Vicomte de Bragelonne_ Tome premier - Auguste Maquet.epub")));
    QVERIFY(QFileInfo::exists(folder + QStringLiteral("/metadata.opf")));

    // A folder adds its book files.
    QTemporaryDir drop;
    QVERIFY(QFile::copy(importFile(QStringLiteral("manga.epub")), drop.filePath(QStringLiteral("manga.epub"))));
    QVERIFY(QFile::copy(importFile(QStringLiteral("data.xyz")), drop.filePath(QStringLiteral("data.xyz"))));
    added.clear();
    QCOMPARE(library->addBooks({ QUrl::fromLocalFile(drop.path()) }), QString());
    QVERIFY(added.wait(15000));
    QCOMPARE(added.first().first().toList(), QVariantList{ 8 });
}

void BookLibraryTest::addsAndRemovesFormats()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    library->setCurrentBookId(1);
    QVERIFY(!library->addFormat(1, copy->filePath(QStringLiteral("missing.mobi"))).isEmpty());
    QVERIFY(!library->addFormat(99, importFile(QStringLiteral("Vicomte.mobi"))).isEmpty());

    QCOMPARE(library->addFormat(1, QUrl::fromLocalFile(importFile(QStringLiteral("Vicomte.mobi"))).toString()), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(formatsOf(library->currentBook()), (QStringList{ QStringLiteral("EPUB"), QStringLiteral("MOBI") }));
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/L'Ecume des jours - Boris Vian.mobi"))));

    QCOMPARE(library->removeFormat(1, QStringLiteral("MOBI")), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(formatsOf(library->currentBook()), QStringList{ QStringLiteral("EPUB") });
    QCOMPARE(trashKeys(library.get()), QStringList{ QStringLiteral("f-1") });
    const QVariantMap entry = library->trashEntries().first().toMap();
    QCOMPARE(entry.value(QStringLiteral("formats")).toStringList(), QStringList{ QStringLiteral("MOBI") });
    QCOMPARE(entry.value(QStringLiteral("title")).toString(), QStringLiteral("L'Écume des jours"));
    QVERIFY(entry.value(QStringLiteral("restorable")).toBool());
    QCOMPARE(entry.value(QStringLiteral("daysLeft")).toInt(), 14);
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral(".caltrash/f/1/mobi"))));

    QCOMPARE(library->restoreFromTrash(1, { QStringLiteral("MOBI") }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(formatsOf(library->currentBook()), (QStringList{ QStringLiteral("EPUB"), QStringLiteral("MOBI") }));
    QVERIFY(library->trashEntries().isEmpty());
    QVERIFY(!QFileInfo::exists(copy->filePath(QStringLiteral(".caltrash/f/1"))));
}

void BookLibraryTest::movesBooksToTrash()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    QCOMPARE(library->trashExpiryDays(), 14);
    QVERIFY(!library->moveToTrash({ 99 }).isEmpty());

    QCOMPARE(library->moveToTrash({ 4 }), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->counts().value(QStringLiteral("books")).toInt(), 5);
    QVERIFY(!ids(bookRows(*library)).contains(4));
    QCOMPARE(trashKeys(library.get()), QStringList{ QStringLiteral("b-4") });
    QCOMPARE(library->counts().value(QStringLiteral("trash")).toInt(), 1);
    const QVariantMap entry = library->trashEntries().first().toMap();
    QCOMPARE(entry.value(QStringLiteral("title")).toString(), QStringLiteral("Good Omens"));
    QCOMPARE(entry.value(QStringLiteral("author")).toString(), QStringLiteral("Terry Pratchett"));
    QVERIFY(!entry.value(QStringLiteral("cover")).toString().isEmpty());
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral(".caltrash/b/4/Good Omens - Terry Pratchett.epub"))));
    QVERIFY(!QFileInfo::exists(copy->filePath(QStringLiteral("Terry Pratchett"))));

    QCOMPARE(library->restoreFromTrash(4), QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->counts().value(QStringLiteral("books")).toInt(), 6);
    library->setCurrentBookId(4);
    QCOMPARE(title(library.get()), QStringLiteral("Good Omens"));
    QCOMPARE(library->currentBook().value(QStringLiteral("authors")).toStringList(),
             (QStringList{ QStringLiteral("Terry Pratchett"), QStringLiteral("Neil Gaiman") }));
    QCOMPARE(formatsOf(library->currentBook()), QStringList{ QStringLiteral("EPUB") });
    QVERIFY(library->trashEntries().isEmpty());
    // A book the library has is not restored over.
    QVERIFY(!library->restoreFromTrash(4).isEmpty());
}

// The conversions queue, report their progress, add their files and settings
// to the library, and stop when asked; Calibre is a stand-in here.
void BookLibraryTest::convertsBooks()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    ConversionService service;
    service.setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-debug"));
    service.setLibrary(library.get());
    QVERIFY(service.available());
    QCOMPARE(service.formatsFor(1), (QStringList{ QStringLiteral("KEPUB"), QStringLiteral("AZW3") }));
    QVERIFY(service.formatsFor(6).isEmpty());
    // Read-only, nothing is converted.
    QVERIFY(!service.convert(1, QStringLiteral("KEPUB")).isEmpty());
    library->setWritable(true);
    QVERIFY(!service.convert(6, QStringLiteral("KEPUB")).isEmpty());
    QVERIFY(!service.convert(1, QStringLiteral("PDF")).isEmpty());
    QVERIFY(!service.convert(99, QStringLiteral("KEPUB")).isEmpty());

    QSignalSpy converted(&service, &ConversionService::converted);
    double progress = 0;
    connect(&service, &ConversionService::jobsChanged, this, [&] {
        if (service.busy())
            progress = qMax(progress, service.jobs().first().toMap().value(QStringLiteral("progress")).toDouble());
    });
    library->setCurrentBookId(1);
    QCOMPARE(service.convert(1, QStringLiteral("kepub")), QString());
    // Asked twice, it converts once.
    QCOMPARE(service.convert(1, QStringLiteral("KEPUB")), QString());
    QCOMPARE(service.jobs().size(), 1);
    QVERIFY(service.busy());
    QVERIFY(converted.wait(10000));
    QCOMPARE(converted.first().at(1).toString(), QStringLiteral("L'Écume des jours"));
    QCOMPARE(converted.first().at(2).toString(), QStringLiteral("KEPUB"));
    QVERIFY(!service.busy());
    QCOMPARE(progress, 1.0);
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(formatsOf(library->currentBook()), (QStringList{ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }));
    QFile file(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/L'Ecume des jours - Boris Vian.kepub")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("converted to kepub"));
    SqliteDb db;
    QString error;
    QVERIFY(db.open(copy->filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error));
    QCOMPARE(db.value(QStringLiteral("SELECT data FROM conversion_options WHERE book=1")).toByteArray(),
             QByteArray("\x80\x02T\x07\x00\x00\x00json:{}.", 15));
    db.close();
    // The job's folder goes once its file is in.
    const QDir jobs(QDir(BookLibrary::cacheFolder(library->libraryPath())).filePath(QStringLiteral("conversions")));
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    // Stopped, a conversion adds nothing, and the next one runs.
    qputenv("COUSOIR_FAKE_DELAY", "20");
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(service.convert(3, QStringLiteral("AZW3")), QString());
    QCOMPARE(service.convert(4, QStringLiteral("AZW3")), QString());
    QTRY_VERIFY(service.jobs().first().toMap().value(QStringLiteral("running")).toBool());
    service.cancel(3);
    QCOMPARE(service.jobs().size(), 1);
    QCOMPARE(service.jobs().first().toMap().value(QStringLiteral("bookId")).toInt(), 4);
    service.cancel();
    QVERIFY(!service.busy());
    QVERIFY(timer.elapsed() < 10000);
    qunsetenv("COUSOIR_FAKE_DELAY");
    QVERIFY(!QFileInfo::exists(copy->filePath(QStringLiteral("Glen Cook/Shadows Linger (3)/Shadows Linger - Glen Cook.azw3"))));
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

// A metadata download lists what Calibre's sources found, compares it with
// the book, finds covers and saves the fields chosen and a cover as one
// edit, which Revert undoes; Calibre is a stand-in here.
void BookLibraryTest::downloadsFromOpenLibrary()
{
    // Without Calibre: Open Library, here a server of the test's own.
    FakeHttpServer server;
    server.handler = fakeOpenLibrary;
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    MetadataDownloader downloader;
    downloader.setNative(true);
    downloader.setSite({ server.url(), server.url() });
    downloader.setLibrary(library.get());
    QVERIFY(downloader.available());
    QCOMPARE(downloader.source(), QStringLiteral("openlibrary"));
    library->setCurrentBookId(3);
    QCOMPARE(downloader.search(3), QString());
    QCOMPARE(downloader.state(), QStringLiteral("searching"));
    QTRY_COMPARE(downloader.state(), QStringLiteral("found"));
    const QVariantMap first = downloader.results().first().toMap();
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Shadows Linger"));
    QCOMPARE(first.value(QStringLiteral("published")).toString(), QStringLiteral("1990"));
    QVERIFY(first.value(QStringLiteral("hasSummary")).toBool());

    QHash<QString, QVariantMap> fields;
    for (const QVariant &row : downloader.fields(0))
        fields.insert(row.toMap().value(QStringLiteral("key")).toString(), row.toMap());
    QCOMPARE(fields.value(QStringLiteral("publisher")).value(QStringLiteral("found")).toString(), QStringLiteral("Tor Fantasy"));
    QVERIFY(fields.value(QStringLiteral("comments")).value(QStringLiteral("chosen")).toBool());

    downloader.searchCovers(0);
    QTRY_VERIFY(!downloader.searchingCovers());
    const QVariantList covers = downloader.covers();
    QCOMPARE(covers.size(), 2);
    QCOMPARE(covers.at(1).toMap().value(QStringLiteral("source")).toString(), QStringLiteral("Open Library"));
    QVERIFY(downloader.coversError().isEmpty());

    QCOMPARE(downloader.apply(0, { QStringLiteral("comments"), QStringLiteral("publisher") },
                              covers.at(1).toMap().value(QStringLiteral("url")).toString()),
             QString());
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("publisher")).toString(), QStringLiteral("Tor Fantasy"));
    QVERIFY(library->currentBook().value(QStringLiteral("description")).toString().contains(QStringLiteral("White Rose")));
    QCOMPARE(QImageReader(copy->filePath(QStringLiteral("Glen Cook/Shadows Linger (3)/cover.jpg"))).size(),
             QSize(300 + 6874911 % 100, 450));
}

void BookLibraryTest::downloadsMetadata()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    MetadataDownloader downloader;
    downloader.setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-debug"));
    downloader.setLibrary(library.get());
    QVERIFY(downloader.available());
    QVERIFY(!downloader.search(99).isEmpty());
    library->setCurrentBookId(2);
    QCOMPARE(downloader.search(2), QString());
    QCOMPARE(downloader.state(), QStringLiteral("searching"));
    QTRY_COMPARE(downloader.state(), QStringLiteral("found"));
    QCOMPARE(downloader.bookId(), 2);
    const QVariantList results = downloader.results();
    QCOMPARE(results.size(), 4);
    const QVariantMap first = results.first().toMap();
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("The Black Company"));
    QCOMPARE(first.value(QStringLiteral("published")).toString(), QStringLiteral("1984"));
    QVERIFY(first.value(QStringLiteral("hasCover")).toBool());
    QVERIFY(first.value(QStringLiteral("hasSummary")).toBool());
    QVERIFY(!results.at(1).toMap().value(QStringLiteral("hasCover")).toBool());

    // Each field the book found has, beside the book's; chosen when it
    // differs.
    QHash<QString, QVariantMap> fields;
    for (const QVariant &row : downloader.fields(0))
        fields.insert(row.toMap().value(QStringLiteral("key")).toString(), row.toMap());
    QCOMPARE(fields.size(), 10);
    QCOMPARE(fields.value(QStringLiteral("title")).value(QStringLiteral("found")).toString(), QStringLiteral("The Black Company"));
    QVERIFY(!fields.value(QStringLiteral("title")).value(QStringLiteral("chosen")).toBool());
    QCOMPARE(fields.value(QStringLiteral("tags")).value(QStringLiteral("current")).toString(), QStringLiteral("Fantasy"));
    QCOMPARE(fields.value(QStringLiteral("tags")).value(QStringLiteral("found")).toString(),
             QStringLiteral("Dark fantasy, fiction, Fantasy"));
    QVERIFY(fields.value(QStringLiteral("tags")).value(QStringLiteral("chosen")).toBool());
    // 4.3 stars is four, as Calibre's editor has no half stars.
    QCOMPARE(fields.value(QStringLiteral("rating")).value(QStringLiteral("found")).toString(), QStringLiteral("★★★★"));
    QCOMPARE(fields.value(QStringLiteral("pubdate")).value(QStringLiteral("found")).toString(), QStringLiteral("1984-05-01"));
    QCOMPARE(fields.value(QStringLiteral("identifiers")).value(QStringLiteral("found")).toString(),
             QStringLiteral("google:xE4_AAAA, isbn:9780812521399"));
    QCOMPARE(fields.value(QStringLiteral("series")).value(QStringLiteral("found")).toString(),
             QStringLiteral("The Chronicles of the Black Company, book 1"));
    QVERIFY(downloader.fields(9).isEmpty());

    // The book's own cover, then those found, the largest first.
    downloader.searchCovers(0);
    QVERIFY(downloader.searchingCovers());
    QTRY_VERIFY(!downloader.searchingCovers());
    const QVariantList covers = downloader.covers();
    QCOMPARE(covers.size(), 3);
    QVERIFY(covers.at(0).toMap().value(QStringLiteral("own")).toBool());
    QCOMPARE(covers.at(1).toMap().value(QStringLiteral("source")).toString(), QStringLiteral("Google"));
    QCOMPARE(covers.at(1).toMap().value(QStringLiteral("width")).toInt(), 1200);
    QCOMPARE(covers.at(2).toMap().value(QStringLiteral("source")).toString(), QStringLiteral("Amazon.com"));
    QVERIFY(downloader.coversError().isEmpty());

    // Asked for again, for another book found and back, the covers are
    // those of the last asked; asked for while they come, too.
    downloader.searchCovers(1);
    QTRY_VERIFY(!downloader.searchingCovers());
    downloader.searchCovers(0);
    QTRY_VERIFY(!downloader.searchingCovers());
    QCOMPARE(downloader.covers().size(), 3);
    QVERIFY(downloader.coversError().isEmpty());
    qputenv("COUSOIR_FAKE_DELAY", "20");
    downloader.searchCovers(1);
    QVERIFY(downloader.searchingCovers());
    qunsetenv("COUSOIR_FAKE_DELAY");
    downloader.searchCovers(0);
    QTRY_VERIFY(!downloader.searchingCovers());
    QCOMPARE(downloader.covers().size(), 3);
    QVERIFY(downloader.coversError().isEmpty());

    // A cover by its address; one that is not there is refused.
    QVERIFY(!downloader.apply(0, { QStringLiteral("tags") }, QStringLiteral("file:///nowhere/cover.jpg")).isEmpty());
    QCOMPARE(downloader.apply(0, { QStringLiteral("tags"), QStringLiteral("pubdate"), QStringLiteral("series") },
                              downloader.covers().at(1).toMap().value(QStringLiteral("url")).toString()),
             QString());
    QCOMPARE(downloader.state(), QStringLiteral("idle"));
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("tags")).toStringList(),
                 (QStringList{ QStringLiteral("Dark fantasy"), QStringLiteral("fiction"), QStringLiteral("Fantasy") }));
    QCOMPARE(library->currentBook().value(QStringLiteral("series")).toString(),
             QStringLiteral("The Chronicles of the Black Company"));
    QCOMPARE(library->currentBook().value(QStringLiteral("title")).toString(), QStringLiteral("The Black Company"));
    const QString coverPath = copy->filePath(QStringLiteral("Glen Cook/The Black Company (2)/cover.jpg"));
    // Scaled to Calibre's largest cover, as Calibre saves one.
    QCOMPARE(QImageReader(coverPath).size(), QSize(1466, 2200));
    // The job's folder goes with the download.
    const QDir jobs(QDir(BookLibrary::cacheFolder(library->libraryPath())).filePath(QStringLiteral("downloads")));
    QVERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    // Revert puts the book back, cover included.
    QVERIFY(library->canRevert());
    library->revertCurrentBook();
    QVERIFY(waitForSave(library.get()));
    QTRY_COMPARE(library->currentBook().value(QStringLiteral("tags")).toStringList(), QStringList{ QStringLiteral("Fantasy") });
    QVERIFY(QImageReader(coverPath).size() != QSize(1466, 2200));

    // Covers that fail tell why, those saved before kept.
    QCOMPARE(downloader.search(2), QString());
    QTRY_COMPARE(downloader.state(), QStringLiteral("found"));
    qputenv("COUSOIR_FAKE_FAIL_COVERS", "1");
    downloader.searchCovers(0);
    QTRY_VERIFY(!downloader.searchingCovers());
    qunsetenv("COUSOIR_FAKE_FAIL_COVERS");
    QVERIFY(downloader.coversError().contains(QStringLiteral("Covers unreachable")));
    QCOMPARE(downloader.covers().size(), 2);

    // A field Calibre is set to leave aside is not ticked; a book without
    // a cover keeps having none unless one is picked.
    qputenv("COUSOIR_FAKE_IGNORE", "tags");
    QCOMPARE(downloader.search(5), QString());
    QTRY_COMPARE(downloader.state(), QStringLiteral("found"));
    qunsetenv("COUSOIR_FAKE_IGNORE");
    for (const QVariant &row : downloader.fields(0)) {
        if (row.toMap().value(QStringLiteral("key")) == QLatin1String("tags"))
            QVERIFY(!row.toMap().value(QStringLiteral("chosen")).toBool());
        if (row.toMap().value(QStringLiteral("key")) == QLatin1String("publisher"))
            QVERIFY(row.toMap().value(QStringLiteral("chosen")).toBool());
    }
    QCOMPARE(downloader.covers().size(), 1);
    QVERIFY(downloader.covers().first().toMap().value(QStringLiteral("own")).toBool());
    QVERIFY(downloader.covers().first().toMap().value(QStringLiteral("url")).toString().isEmpty());
    downloader.cancel();
    QVERIFY(downloader.covers().isEmpty());

    // A failure tells what Calibre logged.
    qputenv("COUSOIR_FAKE_FAIL", "1");
    QCOMPARE(downloader.search(2), QString());
    QTRY_COMPARE(downloader.state(), QStringLiteral("failed"));
    QVERIFY(downloader.error().contains(QStringLiteral("Sources unreachable")));
    qunsetenv("COUSOIR_FAKE_FAIL");

    // Stopped, it is over at once.
    qputenv("COUSOIR_FAKE_DELAY", "20");
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(downloader.search(3), QString());
    QCOMPARE(downloader.state(), QStringLiteral("searching"));
    downloader.cancel();
    QCOMPARE(downloader.state(), QStringLiteral("idle"));
    QVERIFY(timer.elapsed() < 10000);
    qunsetenv("COUSOIR_FAKE_DELAY");
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

// Without Calibre, a KEPUB made by Cousoir of the book's EPUB goes into the
// library as Calibre's would.
void BookLibraryTest::convertsWithoutCalibre()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    ConversionService service;
    service.setNative(true);
    service.setLibrary(library.get());
    QVERIFY(service.available());
    QVERIFY(!service.calibre());
    QCOMPARE(service.formatsFor(1), QStringList{ QStringLiteral("KEPUB") });
    // A PDF alone, nothing.
    QVERIFY(service.formatsFor(5).isEmpty());
    QVERIFY(!service.convert(1, QStringLiteral("AZW3")).isEmpty());
    QSignalSpy converted(&service, &ConversionService::converted);
    QCOMPARE(service.convert(1, QStringLiteral("KEPUB")), QString());
    QVERIFY(converted.wait(10000));
    QVERIFY(waitForSave(library.get()));
    library->setCurrentBookId(1);
    QTRY_COMPARE(formatsOf(library->currentBook()), (QStringList{ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }));
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Boris Vian/L'Ecume des jours (1)/L'Ecume des jours - Boris Vian.kepub"))));
}

// A Kobo shows up while mounted, with its books matched with the library's;
// books go to it through Calibre, a stand-in here, and come off it; ejected,
// it stays out until plugged in again.
// Without Calibre, Cousoir sends to a Kobo itself: the EPUB as a KEPUB at
// the driver's place, the book in metadata.calibre, and the Kobo's list
// with it, matched with the library's book.
void BookLibraryTest::sendsToKobosWithoutCalibre()
{
    auto library = open();
    QVERIFY(library);
    QTemporaryDir kobo;
    QVERIFY(makeKobo(kobo.path()));
    const QString root = QDir::cleanPath(kobo.path());
    QTemporaryDir config;
    QList<Volume> volumes{ { kobo.path(), QStringLiteral("/dev/sdz") } };
    DeviceLibrary devices;
    devices.setNative(true);
    devices.setConfigDirectory(config.path());
    devices.setVolumeProvider([&volumes] { return volumes; }, 20);
    devices.setLibrary(library.get());
    QTRY_COMPARE(devices.devices().size(), 1);
    devices.setCurrentRoot(root);
    QTRY_VERIFY(!devices.loading());
    QSignalSpy sent(&devices, &DeviceLibrary::sent);
    QCOMPARE(devices.send({ 1, 5 }, root), QString());
    QVERIFY(sent.wait(10000));
    const QString kepub = QStringLiteral("Vian, Boris/Ecume des jours, L_ - Boris Vian.kepub.epub");
    QVERIFY(QFileInfo::exists(QDir(root).filePath(kepub)));
    QVERIFY(QFileInfo::exists(QDir(root).filePath(QStringLiteral("Dupont, Jean-Pierre/Manuel technique - Jean-Pierre Dupont.pdf"))));
    QFile metadata(QDir(root).filePath(QStringLiteral("metadata.calibre")));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const QJsonArray books = QJsonDocument::fromJson(metadata.readAll()).array();
    QCOMPARE(books.size(), 2);
    QCOMPARE(books.first().toObject().value(QStringLiteral("lpath")).toString(), kepub);
    QCOMPARE(books.first().toObject().value(QStringLiteral("uuid")).toString(), library->data().book(1)->uuid);
    QVERIFY(QFileInfo::exists(QDir(root).filePath(QStringLiteral("driveinfo.calibre"))));
    // The Kobo's list has them, matched with the library's books.
    QTRY_COMPARE(devices.rows().size(), 2);
    QStringList matched;
    for (const QVariant &row : devices.rows())
        matched.append(QString::number(row.toMap().value(QStringLiteral("bookId")).toInt()));
    matched.sort();
    QCOMPARE(matched, (QStringList{ QStringLiteral("1"), QStringLiteral("5") }));
}

void BookLibraryTest::sendsToKobos()
{
    auto library = open();
    QVERIFY(library);
    // The test's cache outlives it: none left from an earlier run.
    QDir jobs(QDir(BookLibrary::cacheFolder(library->libraryPath())).filePath(QStringLiteral("sends")));
    QVERIFY(jobs.removeRecursively());
    QTemporaryDir kobo;
    QVERIFY(makeKobo(kobo.path()));
    const QString root = QDir::cleanPath(kobo.path());
    QTemporaryDir config;
    QList<Volume> volumes;
    DeviceLibrary devices;
    // Run as calibre-debug, the test tool is a fake sending job.
    devices.setProgram(QStringLiteral(COUSOIR_TEST_TOOL));
    devices.setConfigDirectory(config.path());
    QString ejected;
    devices.setEjector([&ejected](const Kobo::Device &device, std::function<void(const QString &)> done) {
        ejected = device.root;
        done({});
    });
    devices.setVolumeProvider([&volumes] { return volumes; }, 20);
    devices.setLibrary(library.get());
    QVERIFY(devices.devices().isEmpty());
    QVERIFY(devices.available());
    QVERIFY(devices.writable());

    // Any folder with a .kobo in it.
    QTemporaryDir other;
    volumes = { { other.path(), QStringLiteral("/dev/sdy") }, { kobo.path(), QStringLiteral("/dev/sdz") } };
    QTRY_COMPARE(devices.devices().size(), 1);
    const QVariantMap device = devices.devices().first().toMap();
    QCOMPARE(device.value(QStringLiteral("root")).toString(), root);
    QCOMPARE(device.value(QStringLiteral("name")).toString(), QStringLiteral("Kobo eReader"));
    devices.setCurrentRoot(root);
    QCOMPARE(devices.current().value(QStringLiteral("root")).toString(), root);
    QTRY_VERIFY(!devices.loading());
    QVERIFY(devices.rows().isEmpty());

    QCOMPARE(devices.sendFormat(1, root), QStringLiteral("EPUB"));
    QCOMPARE(devices.sendFormat(5, root), QStringLiteral("PDF"));
    QCOMPARE(devices.sendFormat(6, root), QString());
    QCOMPARE(devices.convertFormat(root), QStringLiteral("KEPUB"));
    QVERIFY(devices.send({ 6 }, root).contains(QStringLiteral("none of the formats")));
    QVERIFY(!devices.send({ 1 }, QStringLiteral("/nowhere")).isEmpty());
    QVERIFY(!devices.busy());

    QSignalSpy sent(&devices, &DeviceLibrary::sent);
    double progress = 0;
    connect(&devices, &DeviceLibrary::jobsChanged, this, [&] {
        if (devices.busy())
            progress = qMax(progress, devices.jobs().first().toMap().value(QStringLiteral("progress")).toDouble());
    });
    QCOMPARE(devices.send({ 1, 2 }, root), QString());
    QVERIFY(devices.busy());
    QVERIFY(sent.wait(10000));
    QCOMPARE(sent.first().at(0).toStringList(), (QStringList{ QStringLiteral("L'Écume des jours"), QStringLiteral("The Black Company") }));
    QCOMPARE(sent.first().at(1).toString(), QStringLiteral("Kobo eReader"));
    QVERIFY(!devices.busy());
    QCOMPARE(progress, 1.0);
    const QString ecume = QStringLiteral("Vian, Boris/L'Écume des jours - Boris Vian.kepub.epub");
    QVERIFY(QFileInfo::exists(QDir(root).filePath(ecume)));
    QTRY_COMPARE(devices.rows().size(), 2);
    QCOMPARE(devices.rows().first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("L'Écume des jours"));
    QCOMPARE(devices.rows().first().toMap().value(QStringLiteral("bookId")).toInt(), 1);
    QCOMPARE(devices.rows().first().toMap().value(QStringLiteral("format")).toString(), QStringLiteral("KEPUB"));
    QCOMPARE(devices.onDevice().value(QStringLiteral("2")).toStringList(), QStringList{ QStringLiteral("Kobo eReader") });
    QVERIFY(!devices.onDevice().contains(QStringLiteral("3")));
    // The job's folder goes once its files are on the Kobo.
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    QCOMPARE(devices.remove({ ecume }), QString());
    QVERIFY(!QFileInfo::exists(QDir(root).filePath(ecume)));
    QTRY_COMPARE(devices.rows().size(), 1);
    QCOMPARE(Kobo::readMetadata(root).size(), 1);
    QVERIFY(!devices.onDevice().contains(QStringLiteral("1")));

    // Stopped, nothing is sent, and the next batch goes.
    qputenv("COUSOIR_FAKE_DELAY", "20");
    QElapsedTimer timer;
    timer.start();
    QCOMPARE(devices.send({ 3 }, root), QString());
    QCOMPARE(devices.send({ 4 }, root), QString());
    QTRY_VERIFY(devices.jobs().first().toMap().value(QStringLiteral("running")).toBool());
    QVERIFY(!devices.remove({ ecume }).isEmpty());
    QVERIFY(!devices.eject(root).isEmpty());
    devices.cancel(3);
    QCOMPARE(devices.jobs().size(), 1);
    devices.cancel();
    QVERIFY(!devices.busy());
    QVERIFY(timer.elapsed() < 10000);
    qunsetenv("COUSOIR_FAKE_DELAY");
    QCOMPARE(Kobo::readMetadata(root).size(), 1);
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    // Ejected, it goes at once, and comes back once plugged in again.
    QSignalSpy ejectedSpy(&devices, &DeviceLibrary::ejected);
    QCOMPARE(devices.eject(root), QString());
    QCOMPARE(ejected, root);
    QCOMPARE(ejectedSpy.size(), 1);
    QVERIFY(devices.devices().isEmpty());
    QVERIFY(devices.rows().isEmpty());
    QTest::qWait(100);
    QVERIFY(devices.devices().isEmpty());
    volumes = {};
    QTest::qWait(100);
    volumes = { { kobo.path(), QStringLiteral("/dev/sdz") } };
    QTRY_COMPARE(devices.devices().size(), 1);
}

// A Kindle over MTP, here a folder standing for one: its covers that Amazon
// blanked come back as it is read, books go to it and come off it, and it
// needs no ejecting.
void BookLibraryTest::sendsToKindles()
{
    auto library = open();
    QVERIFY(library);
    QDir jobs(QDir(BookLibrary::cacheFolder(library->libraryPath())).filePath(QStringLiteral("sends")));
    QVERIFY(jobs.removeRecursively());
    QTemporaryDir kindle;
    const QDir root(kindle.path());
    QVERIFY(root.mkpath(QStringLiteral("documents")) && root.mkpath(QStringLiteral("system/thumbnails"))
            && root.mkpath(QStringLiteral("amazon-cover-bug")));
    const auto write = [&root](const QString &path, const QByteArray &data) {
        QFile file(root.filePath(path));
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    };
    QVERIFY(write(QStringLiteral("system/thumbnails/thumbnail_a_EBOK_portrait.jpg"), "blank"));
    QVERIFY(write(QStringLiteral("amazon-cover-bug/thumbnail_a_EBOK_portrait.jpg"), "the cover"));
    QTemporaryDir config;
    QList<KindleSlot> kindles;
    DeviceLibrary devices;
    devices.setProgram(QStringLiteral(COUSOIR_TEST_TOOL));
    devices.setConfigDirectory(config.path());
    devices.setVolumeProvider([] { return QList<Volume>(); }, 20);
    devices.setKindleProvider([&kindles] { return kindles; });
    devices.setLibrary(library.get());

    kindles = { { 0, 0, 0x9981, kindle.path() } };
    QTRY_COMPARE(devices.devices().size(), 1);
    const QVariantMap device = devices.devices().first().toMap();
    const QString key = device.value(QStringLiteral("root")).toString();
    QCOMPARE(device.value(QStringLiteral("kind")).toString(), QStringLiteral("kindle"));
    QVERIFY(!device.value(QStringLiteral("ejectable")).toBool());
    devices.setCurrentRoot(key);
    QTRY_VERIFY(!devices.loading());
    QCOMPARE(devices.current().value(QStringLiteral("name")).toString(), QStringLiteral("Kindle"));
    QFile restored(root.filePath(QStringLiteral("system/thumbnails/thumbnail_a_EBOK_portrait.jpg")));
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("the cover"));

    QCOMPARE(devices.sendFormat(2, key), QStringLiteral("AZW3"));
    QCOMPARE(devices.sendFormat(1, key), QString());
    QCOMPARE(devices.convertFormat(key), QStringLiteral("AZW3"));
    QVERIFY(devices.send({ 1 }, key).contains(QStringLiteral("none of the formats")));
    QSignalSpy sent(&devices, &DeviceLibrary::sent);
    QCOMPARE(devices.send({ 2 }, key), QString());
    QVERIFY(sent.wait(10000));
    QCOMPARE(sent.first().at(0).toStringList(), QStringList{ QStringLiteral("The Black Company") });
    const QString company = QStringLiteral("documents/The Black Company - Glen Cook.azw3");
    QVERIFY(QFileInfo::exists(root.filePath(company)));
    QTRY_COMPARE(devices.rows().size(), 1);
    QCOMPARE(devices.rows().first().toMap().value(QStringLiteral("bookId")).toInt(), 2);
    QCOMPARE(devices.onDevice().value(QStringLiteral("2")).toStringList(), QStringList{ QStringLiteral("Kindle") });
    QTRY_VERIFY(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    QVERIFY(!devices.eject(key).isEmpty());
    QCOMPARE(devices.remove({ company }), QString());
    QTRY_VERIFY(!QFileInfo::exists(root.filePath(company)));
    QTRY_VERIFY(devices.rows().isEmpty());
    QVERIFY(!devices.onDevice().contains(QStringLiteral("2")));

    kindles = {};
    QTRY_VERIFY(devices.devices().isEmpty());
}

namespace {

// The check's rows of `kind`.
QVariantList checkRows(BookLibrary *library, const QString &kind)
{
    QVariantList rows;
    for (const QVariant &row : library->check().value(QStringLiteral("rows")).toList()) {
        if (row.toMap().value(QStringLiteral("kind")).toString() == kind)
            rows.append(row);
    }
    return rows;
}

// Waits for the check of the library as it now is.
bool waitForCheck(BookLibrary *library)
{
    QTest::qWait(0);
    return QTest::qWaitFor([library] { return !library->checking() && !library->saving(); }, 15000);
}

} // namespace

void BookLibraryTest::checksLibrary()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    QVERIFY(waitForCheck(library.get()));
    // Le Petit Prince has no file.
    QCOMPARE(library->check().value(QStringLiteral("count")).toInt(), 1);
    const QVariantMap missing = checkRows(library.get(), QStringLiteral("missing")).value(0).toMap();
    QCOMPARE(missing.value(QStringLiteral("bookId")).toInt(), 6);
    QVERIFY(missing.value(QStringLiteral("noFormats")).toBool());
    QVERIFY(!library->check().value(QStringLiteral("checked")).toString().isEmpty());

    // Ignored, until asked for again.
    library->ignoreFinding(missing.value(QStringLiteral("key")).toString());
    QCOMPARE(library->check().value(QStringLiteral("count")).toInt(), 0);
    QCOMPARE(library->check().value(QStringLiteral("ignored")).toInt(), 1);
    library->showIgnored();
    QCOMPARE(library->check().value(QStringLiteral("count")).toInt(), 1);

    // A file dropped in the library's folder, seen on opening the section.
    QFile stray(copy->filePath(QStringLiteral("stray.epub")));
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.close();
    library->setSection(QStringLiteral("check"));
    QVERIFY(waitForCheck(library.get()));
    QCOMPARE(checkRows(library.get(), QStringLiteral("outside")).value(0).toMap().value(QStringLiteral("path")).toString(),
             QStringLiteral("stray.epub"));
    QVERIFY(!library->showInFolder(QStringLiteral("../elsewhere")).isEmpty());

    // The fixes wait for the library to be writable.
    QVERIFY(!library->keepDuplicate(1, { 2 }).isEmpty());
    QVERIFY(!library->mergeAuthors({ 1, 2 }, 1).isEmpty());
    QVERIFY(!library->removeMissingFormats(6).isEmpty());
}

void BookLibraryTest::mergesAuthorVariants()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    QCOMPARE(library->updateBook(3, { { QStringLiteral("authors"), QStringLiteral("Cook, Glen") } }), QString());
    QVERIFY(waitForSave(library.get()));
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Cook, Glen/Shadows Linger (3)"))));
    QVERIFY(waitForCheck(library.get()));
    const QVariantMap group = checkRows(library.get(), QStringLiteral("authors")).value(0).toMap();
    QCOMPARE(group.value(QStringLiteral("ids")).toList().size(), 2);
    QVERIFY(!group.value(QStringLiteral("initials")).toBool());

    QCOMPARE(library->mergeAuthors(group.value(QStringLiteral("ids")).toList(), 2), QString());
    QVERIFY(waitForSave(library.get()));
    QVERIFY(QFileInfo::exists(copy->filePath(QStringLiteral("Glen Cook/Shadows Linger (3)"))));
    QVERIFY(!QFileInfo::exists(copy->filePath(QStringLiteral("Cook, Glen"))));
    QCOMPARE(library->counts().value(QStringLiteral("authors")).toInt(), 6);
    QVERIFY(waitForCheck(library.get()));
    QVERIFY(checkRows(library.get(), QStringLiteral("authors")).isEmpty());
}

void BookLibraryTest::keepsOneCopy()
{
    auto copy = writableCopy();
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    // Book 2's EPUB added again: book 7.
    QSignalSpy added(library.get(), &BookLibrary::booksAdded);
    QCOMPARE(library->addBooks({ QUrl::fromLocalFile(
                 copy->filePath(QStringLiteral("Glen Cook/The Black Company (2)/The Black Company - Glen Cook.epub"))) }),
             QString());
    QSignalSpy duplicates(library.get(), &BookLibrary::duplicatesFound);
    QVERIFY(duplicates.wait(15000));
    library->addDuplicates(true);
    QVERIFY(added.wait(15000));
    QVERIFY(waitForCheck(library.get()));
    const QVariantMap group = checkRows(library.get(), QStringLiteral("duplicates")).value(0).toMap();
    QVERIFY(group.value(QStringLiteral("sameTitle")).toBool());
    const QVariantList books = group.value(QStringLiteral("books")).toList();
    QCOMPARE(books.size(), 2);
    // The copy with both files first; keeping the other brings it the AZW3.
    QCOMPARE(books.at(0).toMap().value(QStringLiteral("id")).toInt(), 2);
    QCOMPARE(books.at(1).toMap().value(QStringLiteral("gains")).toStringList(), QStringList{ QStringLiteral("AZW3") });

    QCOMPARE(library->keepDuplicate(7, { 2, 7 }), QString());
    QVERIFY(waitForSave(library.get()));
    library->setCurrentBookId(7);
    QCOMPARE(formatsOf(library->currentBook()), (QStringList{ QStringLiteral("AZW3"), QStringLiteral("EPUB") }));
    QCOMPARE(library->trashEntries().size(), 1);
    QVERIFY(waitForCheck(library.get()));
    QVERIFY(checkRows(library.get(), QStringLiteral("duplicates")).isEmpty());

    // Back from the trash, the two copies are found again.
    QCOMPARE(library->restoreFromTrash(2), QString());
    QVERIFY(waitForSave(library.get()));
    QVERIFY(waitForCheck(library.get()));
    QCOMPARE(checkRows(library.get(), QStringLiteral("duplicates")).size(), 1);
}

void BookLibraryTest::removesMissingEntries()
{
    auto copy = writableCopy();
    QVERIFY(QFile::remove(copy->filePath(QStringLiteral("Glen Cook/The Black Company (2)/The Black Company - Glen Cook.azw3"))));
    auto library = open(copy->path());
    QVERIFY(library);
    library->setWritable(true);
    QVERIFY(waitForCheck(library.get()));
    QCOMPARE(library->removeMissingFormats(1), QStringLiteral("None of the book's files is missing."));
    QCOMPARE(library->removeMissingFormats(2), QString());
    QVERIFY(waitForSave(library.get()));
    library->setCurrentBookId(2);
    QCOMPARE(formatsOf(library->currentBook()), QStringList{ QStringLiteral("EPUB") });
    // Nothing went to the trash: there was nothing to put there.
    QVERIFY(!QFileInfo::exists(copy->filePath(QStringLiteral(".caltrash/f/2"))));
    QVERIFY(waitForCheck(library.get()));
    QCOMPARE(checkRows(library.get(), QStringLiteral("missing")).size(), 1);
}

QTEST_MAIN(BookLibraryTest)
#include "book_library_test.moc"
