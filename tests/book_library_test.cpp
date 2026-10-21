#include <QtTest>

#include <QDesktopServices>
#include <QDirIterator>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "book_library.h"
#include "calibre_db.h"
#include "conversion_service.h"
#include "device_library.h"
#include "sqlite_db.h"

namespace {

QString fixtureLibrary()
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/library");
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

// A folder made to look like a mounted Kobo: its version file and an empty
// database with the tables Calibre uses.
bool makeKobo(const QString &root)
{
    QDir().mkpath(QDir(root).filePath(QStringLiteral(".kobo")));
    QFile version(QDir(root).filePath(QStringLiteral(".kobo/version")));
    QFile schema(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo.sql"));
    QFile database(QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite")));
    if (!version.open(QIODevice::WriteOnly) || !schema.open(QIODevice::ReadOnly) || !database.open(QIODevice::WriteOnly))
        return false;
    version.write("N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390");
    database.close();
    SqliteDb db;
    QString error;
    return db.open(database.fileName(), SqliteDb::Mode::ReadWrite, &error) && db.exec(QString::fromUtf8(schema.readAll()), &error);
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
    void followsCalibreOnReload();
    void editsBooks();
    void followsSortsLikeCalibre();
    void refusesWhileReadOnly();
    void revertsCurrentBook();
    void addsBooks();
    void addsAndRemovesFormats();
    void movesBooksToTrash();
    void convertsBooks();
    void sendsToKobos();
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
    QCOMPARE(library->rows().size(), 6);
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
    QVERIFY(!ids(library->rows()).contains(4));
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

// A Kobo shows up while mounted, with its books matched with the library's;
// books go to it through Calibre, a stand-in here, and come off it; ejected,
// it stays out until plugged in again.
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
    devices.setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-send"));
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
    devices.setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-send"));
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
