#include <QtTest>

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

#include <memory>

#include "calibre_db.h"
#include "calibre_fts.h"
#include "calibre_languages.h"
#include "book_library.h"
#include "concurrency_guard.h"
#include "conversion_service.h"
#include "metadata_downloader.h"
#include "device_library.h"
#include "epub_book.h"
#include "epub_cfi.h"
#include "highlight_browser.h"
#ifdef COUSOIR_READER
#include "book_reader.h"
#endif
#include "sqlite_db.h"
#include "cover_cache.h"
#include "omarchy_theme.h"
#include "system_appearance.h"
#include "test_support.h"

namespace {
QString themeStateRoot;

// Keep the user's Omarchy configuration out of the test.
class IsolatedTheme : public OmarchyTheme
{
public:
    explicit IsolatedTheme(QObject *parent = nullptr)
        : OmarchyTheme(themeStateRoot, parent) {}
};

QList<QQuickItem *> visualDescendants(QQuickItem *parent, const QString &name)
{
    QList<QQuickItem *> result;
    for (QQuickItem *child : parent->childItems()) {
        if (child->objectName() == name)
            result.append(child);
        result.append(visualDescendants(child, name));
    }
    return result;
}

QPoint center(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

class UrlRecorder : public QObject
{
    Q_OBJECT

public:
    QList<QUrl> urls;

public slots:
    void open(const QUrl &url) { urls.append(url); }
};
} // namespace

class LibraryUiTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void listsSearchesAndSortsBooks();
    void opensAndClosesBook();
    void showsBookDetails();
    void switchesToCovers();
    void browsesByAuthor();
    void searchesAsCalibre();
    void findsWithPalette();
    void opensInReader();
    void showsWriteState();
    void picksLibraryInSettings();
    void creditsCalibreInSettings();
    void readsInCousoir();
    void encodesCfisAsTheReader();
    void readsPdfs();
    void usesOmarchyColorsByDefault();
    void missingLibraryShowsError();
    void welcomesWithoutLibrary();
    void editsDetailsInPlace();
    void revertsBookChanges();
    void keepsWhatIsTyped();
    void refusesInvalidValues();
    void changesRating();
    void editsDescription();
    void keepsNotes();
    void browsesHighlights();
    void editsCustomColumns();
    void changesCovers();
    void movesBooksToTrashAndBack();
    void editsManyBooks();
    void addsAndRemovesFiles();
    void asksAboutDuplicates();
    void checksLibrary();
    void convertsBooks();
    void downloadsMetadata();
    void sendsToKobo();
    void showsReading();
    void sendsToKindle();
    void keepsLongNamesShort();

private:
    bool loadUi();
    QQuickItem *control(const QString &name) const;
    QQuickItem *control(const char *name) const { return control(QString::fromLatin1(name)); }
    // A popup or another object that is no item.
    QObject *object(const char *name) const { return m_window->findChild<QObject *>(QString::fromLatin1(name)); }
    QObject *library() const;
    // The rows the book list shows, as maps.
    QVariantList bookRows() const;
    QObject *guard() const;
    QString text(const char *name) const;
    void typeText(const QString &text);
    void click(const char *name);
    // Clicks the visible one of the controls named `name`: each dialog has
    // its own buttons of the same names.
    void clickVisible(const char *name);
    // Makes the copy old enough to be written.
    void makeWritable();
    // Makes the copy writable and opens `bookId`.
    void openWritable(int bookId);
    // Clicks a field of the details pane and types over its text.
    void retype(const char *field, const QString &text);
    void waitForSave();

    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;
    // Each test reads its own copy of the sample library.
    std::unique_ptr<QTemporaryDir> m_libraryDir;
    QString m_libraryPath;
};

void LibraryUiTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // Calibre's names of languages from a part of its resources, whatever
    // Calibre is installed.
    CalibreLanguages::setResourcesFolder(QStringLiteral(COUSOIR_TEST_DATA_DIR "/calibre-resources"));
    qmlRegisterType<IsolatedTheme>("Cousoir", 1, 0, "OmarchyTheme");
    qmlRegisterType<BookLibrary>("Cousoir", 1, 0, "BookLibrary");
    qmlRegisterType<ConcurrencyGuard>("Cousoir", 1, 0, "ConcurrencyGuard");
    qmlRegisterType<ConversionService>("Cousoir", 1, 0, "ConversionService");
    qmlRegisterType<MetadataDownloader>("Cousoir", 1, 0, "MetadataDownloader");
    qmlRegisterType<DeviceLibrary>("Cousoir", 1, 0, "DeviceLibrary");
    qmlRegisterType<HighlightBrowser>("Cousoir", 1, 0, "HighlightBrowser");
#ifdef COUSOIR_READER
    qmlRegisterType<BookReader>("Cousoir", 1, 0, "BookReader");
#endif
    qmlRegisterType<SystemAppearance>("Cousoir", 1, 0, "SystemAppearance");
    for (const char *type : { "AboutSettingsPage", "ActionMenu", "AppSession", "AppearanceSettingsPage", "BookDetailsPane", "BookPage",
                              "BooksPage", "BulkEditPane", "CheckPage", "ChoiceBox", "ConfirmDialog", "CoverGrid", "Divider", "FacetPage", "FlatButton", "FormField",
                              "HeadingLabel", "LibrarySettingsPage", "ListTable", "MetaRow", "SearchField",
                              "SettingsWindow", "Sidebar", "SidebarItem", "ThemedWindow", "ThinScrollBar", "TrashPage" }) {
        qmlRegisterType(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/%1.qml").arg(QLatin1String(type))),
                        "Cousoir", 1, 0, type);
    }
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/Theme.qml")), "Cousoir", 1, 0, "Theme");
}

void LibraryUiTest::init()
{
    QSettings().clear();
    m_libraryDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_libraryDir->isValid());
    m_libraryPath = m_libraryDir->filePath(QStringLiteral("library"));
    QVERIFY(copyTree(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library"), m_libraryPath));
    qputenv("COUSOIR_LIBRARY_PATH", QFile::encodeName(m_libraryPath));
    qputenv("CALIBRE_OVERRIDE_LANG", "fr");
    m_warnings.clear();
    QVERIFY2(loadUi(), qPrintable(m_warnings.join('\n')));
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool(), 10000);
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
}

bool LibraryUiTest::loadUi()
{
    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_engine->addImageProvider(QStringLiteral("covers"), new CoverCache);
    connect(m_engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors)
            m_warnings.append(error.toString());
    });
    m_engine->load(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/Main.qml")));
    if (m_engine->rootObjects().isEmpty())
        return false;
    m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
    if (!m_window)
        return false;
    m_window->resize(1240, 820);
    QTest::qWait(30);
    return true;
}

void LibraryUiTest::cleanup()
{
    m_engine.reset();
    m_window = nullptr;
    m_libraryDir.reset();
    qunsetenv("COUSOIR_LIBRARY_PATH");
    qunsetenv("CALIBRE_OVERRIDE_LANG");
    QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join('\n')));
}

QQuickItem *LibraryUiTest::control(const QString &name) const
{
    if (QQuickItem *item = m_window->findChild<QQuickItem *>(name))
        return item;
    const QList<QQuickItem *> items = visualDescendants(m_window->contentItem(), name);
    return items.isEmpty() ? nullptr : items.constFirst();
}

QObject *LibraryUiTest::library() const
{
    return m_window->property("library").value<QObject *>();
}

QVariantList LibraryUiTest::bookRows() const
{
    auto *books = library()->property("books").value<BookListModel *>();
    QVariantList rows;
    for (int row = 0; books && row < books->count(); ++row)
        rows.append(books->get(row));
    return rows;
}

QObject *LibraryUiTest::guard() const
{
    return m_window->property("guard").value<QObject *>();
}

QString LibraryUiTest::text(const char *name) const
{
    QQuickItem *item = control(name);
    return item ? item->property("text").toString() : QString();
}

void LibraryUiTest::makeWritable()
{
    QFile database(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db")));
    QVERIFY(database.open(QIODevice::ReadWrite));
    QVERIFY(database.setFileTime(QDateTime::currentDateTime().addSecs(-600), QFileDevice::FileModificationTime));
    database.close();
    QMetaObject::invokeMethod(guard(), "refresh");
    QTRY_VERIFY2(guard()->property("writable").toBool(), qPrintable(guard()->property("readOnlyReason").toString()));
    QTRY_VERIFY(library()->property("writable").toBool());
}

void LibraryUiTest::openWritable(int bookId)
{
    makeWritable();
    library()->setProperty("currentBookId", bookId);
    QTRY_VERIFY(control("bookFields") && control("bookFields")->isVisible());
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
}

void LibraryUiTest::retype(const char *field, const QString &text)
{
    click(field);
    QQuickItem *item = control(field);
    QTRY_VERIFY(item->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    if (text.isEmpty())
        QTest::keyClick(m_window, Qt::Key_Backspace);
    for (const QChar character : text)
        QTest::keyClick(m_window, character.toLatin1());
}

void LibraryUiTest::waitForSave()
{
    QTRY_VERIFY_WITH_TIMEOUT(!library()->property("saving").toBool(), 15000);
}

// Clicks a control once the layouts placed it: an item that just appeared
// sits at 0, 0 until then, over whatever is there.
void LibraryUiTest::click(const char *name)
{
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QQuickItem *item = control(name);
    QVERIFY2(item, name);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(item));
}

void LibraryUiTest::clickVisible(const char *name)
{
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QQuickItem *found = nullptr;
    QTRY_VERIFY([&] {
        const QList<QQuickItem *> items = m_window->findChildren<QQuickItem *>(QString::fromLatin1(name));
        const auto visible = std::find_if(items.cbegin(), items.cend(), [](QQuickItem *item) {
            return item->isVisible() && item->window() && item->opacity() > 0.99;
        });
        found = visible != items.cend() ? *visible : nullptr;
        return found != nullptr;
    }());
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(found));
}

void LibraryUiTest::typeText(const QString &text)
{
    for (const QChar character : text)
        QTest::keyClick(m_window, character.toLatin1());
}

void LibraryUiTest::listsSearchesAndSortsBooks()
{
    QCOMPARE(text("bookCount"), QStringLiteral("6 books"));
    QCOMPARE(text("pageTitle"), QStringLiteral("Books"));
    QQuickItem *list = control("bookList");
    QVERIFY(list);
    QCOMPARE(list->property("count").toInt(), 6);
    QVERIFY(list->hasActiveFocus());
    QVERIFY(control("bookRow0"));

    // Typing filters the list, accents or not; Esc clears it.
    QTest::keyClick(m_window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(control("searchField")->hasActiveFocus());
    typeText(QStringLiteral("ecume"));
    QTRY_COMPARE(list->property("count").toInt(), 1);
    QCOMPARE(text("bookCount"), QStringLiteral("1 of 6 books"));
    typeText(QStringLiteral("zz"));
    QTRY_COMPARE(text("emptyTitle"), QStringLiteral("No book matches “ecumezz”"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(list->property("count").toInt(), 6);

    // A header click sorts, a second one reverses. The header shows again
    // with the books: once it is laid out.
    click("bookTimestampColumn");
    QTRY_COMPARE(library()->property("sortOption").toString(), QStringLiteral("timestamp"));
    click("bookTimestampColumn");
    QTRY_COMPARE(library()->property("sortOption").toString(), QStringLiteral("timestampDesc"));
    QCOMPARE(bookRows().first().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Le Petit Prince"));
    // The order is remembered for the library, the sorts before it too.
    const QString uuid = library()->property("libraryUuid").toString();
    QTRY_COMPARE(QSettings().value(QStringLiteral("Library/sortHistory/%1").arg(uuid)).toString(),
                 QStringLiteral(R"([["timestamp",false],["title",true]])"));

    // A right click on a header shows its menu: a column hidden, another
    // shown beside it.
    QQuickItem *authors = control("bookAuthorsColumn");
    QVERIFY(authors);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, authors->mapToScene(QPointF(4, 4)).toPoint());
    QTRY_VERIFY(object("columnMenu")->property("opened").toBool());
    clickVisible("columnMenu_hide");
    QTRY_COMPARE(library()->property("columns").toStringList(),
                 (QStringList{ QStringLiteral("title"), QStringLiteral("series"), QStringLiteral("formats"),
                               QStringLiteral("timestamp"), QStringLiteral("rating") }));
    QTRY_VERIFY(!control("bookAuthorsColumn"));
    QQuickItem *series = control("bookSeriesColumn");
    QVERIFY(series);
    QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, series->mapToScene(QPointF(4, 4)).toPoint());
    QTRY_VERIFY(object("columnMenu")->property("opened").toBool());
    clickVisible("columnMenu_show:tags");
    QTRY_COMPARE(library()->property("columns").toStringList(),
                 (QStringList{ QStringLiteral("title"), QStringLiteral("series"), QStringLiteral("tags"),
                               QStringLiteral("formats"), QStringLiteral("timestamp"), QStringLiteral("rating") }));
    QTRY_VERIFY(control("bookTagsColumn") && control("bookTagsColumn")->isVisible());
}

void LibraryUiTest::opensAndClosesBook()
{
    QQuickItem *row = control("bookRow1");
    QVERIFY(row);
    // A click selects, a double click opens.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(row));
    QCOMPARE(control("bookList")->property("currentIndex").toInt(), 1);
    QVERIFY(!library()->property("currentBookId").toInt());
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, center(row));
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 1);
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("L'Écume des jours"));
    QCOMPARE(text("rootCrumb"), QStringLiteral("Books"));
    QCOMPARE(m_window->title(), QStringLiteral("L'Écume des jours — Cousoir"));
    QVERIFY(!control("sidebar")->isVisible());
    QCOMPARE(text("bookCount"), QStringLiteral("EPUB"));

    // Esc goes back to the same row.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    QCOMPARE(control("bookList")->property("currentIndex").toInt(), 1);
    QVERIFY(control("bookList")->hasActiveFocus());

    // Keys open the next one; the back button closes it.
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 4);
    click("backButton");
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
}

void LibraryUiTest::showsBookDetails()
{
    // Shared with other computers, the copy just made is read-only for now.
    guard()->setProperty("sharing", QStringLiteral("shared"));
    QTRY_COMPARE(guard()->property("state").toString(), QStringLiteral("recent"));
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, center(control("bookRow1")));
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 1);
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QCOMPARE(text("bookTitle"), QStringLiteral("L'Écume des jours"));
    QCOMPARE(text("bookAuthors"), QStringLiteral("Boris Vian"));
    QCOMPARE(text("bookRating"), QStringLiteral("★★★★"));
    QVERIFY(text("bookDescription").contains(QStringLiteral("<em>1946</em>")));
    QQuickItem *pane = control("detailsPane");
    QVERIFY(pane && pane->isVisible());
    QCOMPARE(control("fact_publisher")->property("value").toString(), QStringLiteral("Gallimard"));
    QCOMPARE(control("fact_sort")->property("value").toString(), QStringLiteral("Écume des jours, L'"));
    QVERIFY(!control("fact_series")->isVisible());
    QCOMPARE(control("identifier_isbn")->property("value").toString(), QStringLiteral("9782253140870"));
    QVERIFY(control("format_EPUB")->isEnabled());
    QTRY_COMPARE(control("bookCoverImage")->property("status").toInt(), 1);
    // Read-only: metadata as text.
    QVERIFY(!control("bookFields")->isVisible());
    QVERIFY(!control("coverButton")->isVisible());

    // Ctrl+I hides the pane; its facts move under the title.
    QVERIFY(!control("factsLine")->isVisible());
    QTest::keyClick(m_window, Qt::Key_I, Qt::ControlModifier);
    QTRY_VERIFY(!pane->isVisible());
    QVERIFY(control("factsLine")->isVisible());
    QCOMPARE(text("factsLine"), QStringLiteral("Mar 1947  ·  Français  ·  EPUB"));
    QTest::keyClick(m_window, Qt::Key_I, Qt::ControlModifier);
    QTRY_VERIFY(pane->isVisible());

    // A narrow window hides it unless the toggle forces it.
    m_window->resize(800, 600);
    QTRY_VERIFY(!pane->isVisible());
    click("detailsToggle");
    QTRY_VERIFY(pane->isVisible());
}

void LibraryUiTest::switchesToCovers()
{
    QQuickItem *list = control("bookList");
    QTest::keyClick(m_window, Qt::Key_Down);
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_G, Qt::ControlModifier);
    QQuickItem *grid = control("coverGrid");
    QTRY_VERIFY(grid && grid->isVisible());
    QVERIFY(!list->isVisible());
    QTRY_VERIFY(grid->hasActiveFocus());
    // The same book stays current.
    QCOMPARE(grid->property("currentIndex").toInt(), 1);
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QQuickItem *cell = control("coverCell2");
    QVERIFY(cell);
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, center(cell));
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 4);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    QVERIFY(grid->hasActiveFocus());
    QTRY_VERIFY(QSettings().value(QStringLiteral("Layout/gridMode")).toBool());

    click("viewToggle");
    QTRY_VERIFY(list->isVisible());
    QCOMPARE(list->property("currentIndex").toInt(), 2);
}

void LibraryUiTest::browsesByAuthor()
{
    click("sidebar_authors");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Authors"));
    QCOMPARE(text("bookCount"), QStringLiteral("6 authors"));
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QQuickItem *facets = control("facetList");
    QVERIFY(facets && facets->isVisible());
    QCOMPARE(facets->property("count").toInt(), 6);

    // Enter on Glen Cook lists his books, with a crumb back.
    click("facetRow0");
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Glen Cook"));
    QCOMPARE(text("rootCrumb"), QStringLiteral("Authors"));
    QCOMPARE(text("bookCount"), QStringLiteral("2 books"));
    QCOMPARE(m_window->title(), QStringLiteral("Glen Cook — Cousoir"));
    QTRY_VERIFY(control("bookList")->hasActiveFocus());

    // A book opened from there shows all three levels.
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 2);
    QCOMPARE(text("filterCrumb"), QStringLiteral("Glen Cook"));
    click("filterCrumb");
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    QCOMPARE(text("pageTitle"), QStringLiteral("Glen Cook"));

    // Esc goes back to the authors, on the same one.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Authors"));
    QCOMPARE(facets->property("currentIndex").toInt(), 0);

    // The search filters the section's items.
    QTest::keyClick(m_window, Qt::Key_F, Qt::ControlModifier);
    typeText(QStringLiteral("vian"));
    QTRY_COMPARE(facets->property("count").toInt(), 1);
    QCOMPARE(text("bookCount"), QStringLiteral("1 of 6 authors"));

    click("sidebar_books");
    QTRY_COMPARE(text("bookCount"), QStringLiteral("6 books"));
    QVERIFY(library()->property("searchText").toString().isEmpty());
}

void LibraryUiTest::searchesAsCalibre()
{
    // The library's saved searches and virtual libraries, as Calibre keeps
    // them; the library reloads when its database changes.
    {
        SqliteDb db;
        QString error;
        QVERIFY2(db.open(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db")), SqliteDb::Mode::ReadWrite, &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QStringLiteral(R"SQL(
            INSERT OR REPLACE INTO preferences(key, val) VALUES ('saved_searches', '{"Cook": "author:cook", "Rated": "rating:>=4"}');
            INSERT OR REPLACE INTO preferences(key, val) VALUES ('virtual_libraries', '{"Fantasy": "tags:fantasy"}');
        )SQL"), &error), qPrintable(error));
    }
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY(control("sidebar_search:Cook"));
    QVERIFY(QQuickTest::qWaitForPolish(m_window));

    // A saved search is a section of its own, with its count.
    click("sidebar_search:Cook");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Cook"));
    QTRY_COMPARE(text("bookCount"), QStringLiteral("2 books"));
    QCOMPARE(m_window->title(), QStringLiteral("Cook — Cousoir"));

    // Calibre's search language, and what it cannot read.
    click("sidebar_books");
    QTRY_COMPARE(text("bookCount"), QStringLiteral("6 books"));
    QTest::keyClick(m_window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(control("searchField")->hasActiveFocus());
    typeText(QStringLiteral("rating:>3 not author:vian"));
    QTRY_COMPARE(text("bookCount"), QStringLiteral("1 of 6 books"));
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    typeText(QStringLiteral("(vian"));
    QTRY_COMPARE(text("emptyTitle"), QStringLiteral("Calibre's search cannot read “(vian”"));
    QCOMPARE(text("emptyMessage"), QStringLiteral("missing )"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(text("bookCount"), QStringLiteral("6 books"));

    // A virtual library narrows every section and their counts.
    QQuickItem *box = control("virtualLibraryBox");
    QTRY_VERIFY(box && box->isVisible());
    click("virtualLibraryBox");
    QTRY_VERIFY(box->property("popup").value<QObject *>()->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("virtualLibrary").toString(), QStringLiteral("Fantasy"));
    QTRY_COMPARE(text("bookCount"), QStringLiteral("3 books"));
    QCOMPARE(library()->property("counts").toMap().value(QStringLiteral("authors")).toInt(), 3);
    click("sidebar_search:Cook");
    QTRY_COMPARE(text("bookCount"), QStringLiteral("2 books"));
    // The choice stays with the library.
    QTRY_COMPARE(QSettings().value(QStringLiteral("Library/virtualLibrary/%1")
                                       .arg(library()->property("libraryUuid").toString())).toString(),
                 QStringLiteral("Fantasy"));
}

void LibraryUiTest::opensInReader()
{
    UrlRecorder recorder;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &recorder, "open");
    QTest::keyClick(m_window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE(recorder.urls.size(), 1);
    QVERIFY(recorder.urls.first().toLocalFile().endsWith(QStringLiteral("The Black Company - Glen Cook.epub")));

    // The PDF-only book opens its PDF from the toolbar.
    library()->setProperty("currentBookId", 5);
    QTRY_VERIFY(control("openInReaderButton")->isVisible());
    click("openInReaderButton");
    QTRY_COMPARE(recorder.urls.size(), 2);
    QVERIFY(recorder.urls.last().toLocalFile().endsWith(QStringLiteral(".pdf")));

    // A book without files says so.
    library()->setProperty("currentBookId", 6);
    QTRY_VERIFY(!control("openInReaderButton")->isEnabled());
    QTest::keyClick(m_window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE(text("keyHints"), QStringLiteral("This book has no file to open."));
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
}

void LibraryUiTest::showsWriteState()
{
    // The copied library just changed. When only this computer changes it,
    // that is no reason to wait.
    guard()->setProperty("sharing", QStringLiteral("local"));
    QTRY_COMPARE(guard()->property("state").toString(), QStringLiteral("writable"));
    QQuickItem *state = control("writeState");
    QVERIFY(state && !state->isVisible());

    // Shared with other computers, it waits for the change to settle.
    guard()->setProperty("sharing", QStringLiteral("shared"));
    QTRY_COMPARE(guard()->property("state").toString(), QStringLiteral("recent"));
    QTRY_VERIFY(state->isVisible());
    QVERIFY(state->property("text").toString().startsWith(QStringLiteral("Read-only: library changed")));

    // Calibre's journal takes over, and goes.
    QFile journal(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    guard()->metaObject()->invokeMethod(guard(), "refresh");
    QTRY_COMPARE(state->property("text").toString(), QStringLiteral("Read-only: Calibre is writing to the library"));

    // A click shows why in the settings, and how the library is shared.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(state));
    QQuickWindow *settings = m_window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    QVERIFY(settings);
    QTRY_VERIFY(settings->isVisible());
    QCOMPARE(settings->property("currentSectionIndex").toInt(), 1);
    const QList<QQuickItem *> shared = visualDescendants(settings->contentItem(), QStringLiteral("sharingOption_shared"));
    QCOMPARE(shared.size(), 1);
    QVERIFY(shared.constFirst()->property("selected").toBool());
    const QList<QQuickItem *> sharing = visualDescendants(settings->contentItem(), QStringLiteral("sharingLabel"));
    QCOMPARE(sharing.size(), 1);
    QCOMPARE(sharing.constFirst()->property("text").toString(), QStringLiteral("In effect: shared with other computers"));
    settings->close();
}

void LibraryUiTest::picksLibraryInSettings()
{
    QTemporaryDir other;
    QVERIFY(copyTree(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library"), other.path()));
    click("settingsButton");
    QQuickWindow *settings = m_window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    QTRY_VERIFY(settings && settings->isVisible());
    QMetaObject::invokeMethod(settings, "showSection", Q_ARG(QVariant, QStringLiteral("library")));
    QCOMPARE(settings->property("currentSectionIndex").toInt(), 1);

    // As the folder dialog would pick it.
    QMetaObject::invokeMethod(settings, "libraryPathSelected", Q_ARG(QString, other.path()));
    QTRY_COMPARE(library()->property("libraryPath").toString(), QDir::cleanPath(other.path()));
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool(), 10000);
    QTRY_COMPARE(QSettings().value(QStringLiteral("Library/path")).toString(), other.path());

    // Back to Calibre's own, which the test sets to the first copy.
    QMetaObject::invokeMethod(settings, "libraryPathSelected", Q_ARG(QString, QString()));
    QTRY_COMPARE(library()->property("libraryPath").toString(), QDir::cleanPath(m_libraryPath));
    settings->close();
}

void LibraryUiTest::creditsCalibreInSettings()
{
    click("settingsButton");
    QQuickWindow *settings = m_window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    QTRY_VERIFY(settings && settings->isVisible());
    QMetaObject::invokeMethod(settings, "showSection", Q_ARG(QVariant, QStringLiteral("about")));
    QCOMPARE(settings->property("currentSectionIndex").toInt(), 2);

    const QList<QQuickItem *> credit = visualDescendants(settings->contentItem(), QStringLiteral("calibreCreditLabel"));
    QCOMPARE(credit.size(), 1);
    QVERIFY(credit.constFirst()->isVisible());
    const QString text = credit.constFirst()->property("text").toString();
    QVERIFY(text.contains(QStringLiteral("Calibre")));
    QVERIFY(text.contains(QStringLiteral("Kovid Goyal")));
    const QList<QQuickItem *> version = visualDescendants(settings->contentItem(), QStringLiteral("versionLabel"));
    QCOMPARE(version.size(), 1);
    QVERIFY(version.constFirst()->property("text").toString().contains(QStringLiteral(COUSOIR_VERSION)));
    settings->close();
}

// A book read in Cousoir's reader: its pages turned, where reading stopped
// kept, and the book opened again there.
void LibraryUiTest::readsInCousoir()
{
#ifndef COUSOIR_READER
    QSKIP("Built without Qt WebEngine");
#else
    makeWritable();
    QString error;
    QMetaObject::invokeMethod(library(), "addBooks", Q_RETURN_ARG(QString, error),
                              Q_ARG(QVariantList, QVariantList{ QUrl::fromLocalFile(QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/voyage.epub")) }));
    QCOMPARE(error, QString());
    waitForSave();
    int bookId = 0;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const QVariant &row : bookRows()) {
            if (row.toMap().value(QStringLiteral("title")).toString() == QLatin1String("The Voyage"))
                bookId = row.toMap().value(QStringLiteral("id")).toInt();
        }
        return bookId > 0;
    }(), 15000);
    library()->setProperty("currentBookId", bookId);
    QVERIFY(library()->property("readerAvailable").toBool());

    const auto openReader = [this] {
        QMetaObject::invokeMethod(m_window, "readBook");
        return m_window->findChild<QQuickWindow *>(QStringLiteral("readerWindow"));
    };
    QQuickWindow *reader = openReader();
    QVERIFY(reader);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->property("position").toString().isEmpty(), 20000);
    // The cover, then the first chapter's pages.
    QCOMPARE(reader->property("spineIndex").toInt(), 0);
    for (int i = 0; i < 3; ++i) {
        const QString before = reader->property("position").toString();
        QTest::keyClick(reader, Qt::Key_Right);
        QTRY_VERIFY_WITH_TIMEOUT(reader->property("position").toString() != before, 10000);
    }
    QCOMPARE(reader->property("spineIndex").toInt(), 1);
    QCOMPARE(reader->property("page").toInt(), 2);
    const QString position = reader->property("position").toString();
    QVERIFY2(position.startsWith(QLatin1String("epubcfi(/4/2/4/")), qPrintable(position));
    QVERIFY(reader->property("bookFraction").toDouble() > 0);

    // Kept when the book is closed, and read from there again.
    reader->close();
    QTRY_VERIFY(!reader->isVisible());
    reader = openReader();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isVisible() && !reader->property("position").toString().isEmpty(), 20000);
    QTRY_COMPARE(reader->property("spineIndex").toInt(), 1);
    QTRY_COMPARE(reader->property("page").toInt(), 2);
    QCOMPARE(reader->property("position").toString(), position);

    // The table of contents: a section of the second chapter.
    QMetaObject::invokeMethod(reader, "load", Q_ARG(QVariant, 2), Q_ARG(QVariant, QStringLiteral("fragment:s2")));
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("spineIndex").toInt() == 2 && reader->property("page").toInt() > 0, 20000);
    const QString section = reader->property("position").toString();
    reader->close();

    // Kept in the library too, as Calibre keeps its viewer's.
    waitForSave();
    SqliteDb db;
    QString dbError;
    const QString database = QDir(m_libraryPath).filePath(QStringLiteral("metadata.db"));
    QVERIFY2(db.open(database, SqliteDb::Mode::ReadWrite, &dbError), qPrintable(dbError));
    QStringList rows;
    for (const QVariantList &row :
         db.rows(QStringLiteral("SELECT format, user, device, cfi FROM last_read_positions WHERE book=?"), { bookId })) {
        QStringList columns;
        for (const QVariant &column : row)
            columns.append(column.toString());
        rows.append(columns.join(u'|'));
    }
    QCOMPARE(rows, QStringList{ QStringLiteral("EPUB|local|cousoir|") + section });

    // Calibre's viewer read on since: where the book opens now.
    const QString calibreCfi = QStringLiteral("epubcfi(/8/2/4/2[top]/1:0)");
    QVERIFY(db.run(QStringLiteral("INSERT INTO last_read_positions(book,format,user,device,cfi,epoch,pos_frac) "
                                  "VALUES (?,'EPUB','local','calibre-desktop-viewer',?,?,0.8)"),
                   { bookId, calibreCfi, double(QDateTime::currentSecsSinceEpoch() + 60) }, &dbError));
    db.close();
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool() && !library()->property("loading").toBool(), 15000);
    reader = openReader();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isVisible() && !reader->property("position").toString().isEmpty(), 20000);
    QTRY_COMPARE(reader->property("spineIndex").toInt(), 3);

    // A highlight of the first paragraph's start, kept as Calibre's viewer
    // keeps its own.
    QMetaObject::invokeMethod(reader, "load", Q_ARG(QVariant, 1), Q_ARG(QVariant, QStringLiteral("start")));
    // The chapter loaded: a position in it told.
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("position").toString().startsWith(QLatin1String("epubcfi(/4/"))
                                 && reader->property("page").toInt() == 0,
                             20000);
    QObject *view = reader->findChild<QObject *>(QStringLiteral("readerView"));
    QVERIFY(view);
    QVERIFY(QMetaObject::invokeMethod(view, "runJavaScript", Q_ARG(QString, QStringLiteral(
        "const p = document.querySelector('p'); const r = document.createRange();"
        "r.setStart(p.firstChild, 0); r.setEnd(p.firstChild, 22);"
        "getSelection().removeAllRanges(); getSelection().addRange(r);"))));
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("hasSelection").toBool(), 10000);
    QMetaObject::invokeMethod(reader, "highlightSelection");
    QObject *bookReader = reader->property("reader").value<QObject *>();
    QTRY_COMPARE(bookReader->property("highlights").toList().size(), 1);
    const QVariantMap highlight = bookReader->property("highlights").toList().first().toMap();
    QCOMPARE(highlight.value(QStringLiteral("text")).toString(), QStringLiteral("Chapter 1, paragraph 0"));
    // body, then its third element: h1, h2, p.
    QCOMPARE(highlight.value(QStringLiteral("start")).toString(), QStringLiteral("/2/4/6/1:0"));
    QCOMPARE(highlight.value(QStringLiteral("end")).toString(), QStringLiteral("/2/4/6/1:22"));
    QTRY_COMPARE(reader->property("highlightsShown").toInt(), 1);
    // Written with the position when the book closes.
    reader->close();
    waitForSave();
    QVERIFY2(db.open(database, SqliteDb::Mode::ReadWrite, &dbError), qPrintable(dbError));
    const QList<QVariantList> annotations = db.rows(
        QStringLiteral("SELECT user_type, user, annot_type, annot_data, searchable_text FROM annotations WHERE book=?"),
        { bookId });
    db.close();
    QCOMPARE(annotations.size(), 1);
    QCOMPARE(annotations.first().value(0).toString(), QStringLiteral("local"));
    QCOMPARE(annotations.first().value(1).toString(), QStringLiteral("viewer"));
    QCOMPARE(annotations.first().value(2).toString(), QStringLiteral("highlight"));
    const QJsonObject data = QJsonDocument::fromJson(annotations.first().value(3).toString().toUtf8()).object();
    QCOMPARE(data.value(QLatin1String("spine_name")).toString(), QStringLiteral("OEBPS/text/chapter1.xhtml"));
    QCOMPARE(data.value(QLatin1String("style")).toObject().value(QLatin1String("which")).toString(), QStringLiteral("yellow"));
    QCOMPARE(annotations.first().value(4).toString(), QStringLiteral("Chapter 1, paragraph 0"));

    // Shown again from the library, and removed as Calibre's viewer removes one.
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool() && !library()->property("loading").toBool(), 15000);
    reader = openReader();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isVisible() && !reader->property("position").toString().isEmpty(), 20000);
    QMetaObject::invokeMethod(reader, "load", Q_ARG(QVariant, 1), Q_ARG(QVariant, QStringLiteral("start")));
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("spineIndex").toInt() == 1 && reader->property("highlightsShown").toInt() == 1,
                             20000);
    reader->setProperty("chosenHighlight", highlight.value(QStringLiteral("uuid")));
    QMetaObject::invokeMethod(reader, "removeChosenHighlight");
    QTRY_COMPARE(bookReader->property("highlights").toList().size(), 0);
    reader->close();
    waitForSave();
    QVERIFY2(db.open(database, SqliteDb::Mode::ReadWrite, &dbError), qPrintable(dbError));
    const QJsonObject removed = QJsonDocument::fromJson(
        db.value(QStringLiteral("SELECT annot_data FROM annotations WHERE book=%1").arg(bookId)).toString().toUtf8()).object();
    db.close();
    QVERIFY(removed.value(QLatin1String("removed")).toBool());
#endif
}

// The CFIs Cousoir works out from a page's XML are those the reader's own
// document gives, for the places of every text of pages made to differ.
void LibraryUiTest::encodesCfisAsTheReader()
{
#ifndef COUSOIR_READER
    QSKIP("Built without Qt WebEngine");
#else
    makeWritable();
    const QString path = QStringLiteral(COUSOIR_TEST_DATA_DIR "/reader/cfi.epub");
    QString error;
    QMetaObject::invokeMethod(library(), "addBooks", Q_RETURN_ARG(QString, error),
                              Q_ARG(QVariantList, QVariantList{ QUrl::fromLocalFile(path) }));
    QCOMPARE(error, QString());
    waitForSave();
    int bookId = 0;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const QVariant &row : bookRows()) {
            if (row.toMap().value(QStringLiteral("title")).toString() == QLatin1String("The Odd Pages"))
                bookId = row.toMap().value(QStringLiteral("id")).toInt();
        }
        return bookId > 0;
    }(), 15000);
    library()->setProperty("currentBookId", bookId);
    QMetaObject::invokeMethod(m_window, "readBook");
    QQuickWindow *reader = m_window->findChild<QQuickWindow *>(QStringLiteral("readerWindow"));
    QVERIFY(reader);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->property("position").toString().isEmpty(), 20000);
    QObject *view = reader->findChild<QObject *>(QStringLiteral("readerView"));
    QVERIFY(view);

    const std::unique_ptr<EpubBook> book = EpubBook::open(path, &error);
    QVERIFY2(book, qPrintable(error));
    for (int index = 0; index < book->spine().size(); ++index) {
        QMetaObject::invokeMethod(reader, "load", Q_ARG(QVariant, index), Q_ARG(QVariant, QStringLiteral("start")));
        QTRY_VERIFY_WITH_TIMEOUT(reader->property("spineIndex").toInt() == index
                                     && reader->property("position").toString().startsWith(
                                         QStringLiteral("epubcfi(/%1/").arg(2 * (index + 1))),
                                 20000);
        const std::unique_ptr<XmlTree::Node> root = XmlTree::parse(book->read(book->spine().at(index).name));
        QVERIFY(root);
        const EpubCfi::Document document(*root);
        const XmlTree::Node *body = XmlTree::childElement(XmlTree::documentElement(root.get()), "body");
        QVERIFY(body);
        // The texts under the body, as one: a place is an offset in it, the
        // text it is in the first that holds it, however each side splits
        // its texts.
        QList<const XmlTree::Node *> texts;
        const std::function<void(const XmlTree::Node &)> walk = [&](const XmlTree::Node &node) {
            for (const auto &child : node.children) {
                if (child->kind == XmlTree::Node::Kind::Text || child->kind == XmlTree::Node::Kind::Cdata)
                    texts.append(child.get());
                else
                    walk(*child);
            }
        };
        walk(*body);
        QList<qsizetype> starts;
        qsizetype total = 0;
        for (const XmlTree::Node *text : std::as_const(texts)) {
            starts.append(total);
            total += text->text.size();
        }
        QJsonArray table;
        for (qsizetype i = 0; i < texts.size(); ++i) {
            const qsizetype size = texts.at(i)->text.size();
            for (const qsizetype offset : { qsizetype(0), qMin<qsizetype>(1, size), size / 2, size }) {
                const qsizetype at = starts.at(i) + offset;
                qsizetype k = 0;
                while (!(starts.at(k) <= at && at <= starts.at(k) + texts.at(k)->text.size()))
                    ++k;
                table.append(QJsonArray{ double(at), document.encode(*texts.at(k), at - starts.at(k)) });
            }
        }
        const QString script = QStringLiteral(R"((function (table, total) {
            const texts = [];
            (function walk(node) {
                for (let child = node.firstChild; child; child = child.nextSibling) {
                    if (child.nodeType === Node.TEXT_NODE || child.nodeType === Node.CDATA_SECTION_NODE)
                        texts.push(child);
                    else
                        walk(child);
                }
            })(document.body);
            const starts = [];
            let length = 0;
            for (const text of texts) {
                starts.push(length);
                length += text.nodeValue.length;
            }
            let result = length === total ? 'cfi-ok ' + table.length : 'cfi-text ' + length;
            for (const [at, expected] of length === total ? table : []) {
                let k = 0;
                while (!(starts[k] <= at && at <= starts[k] + texts[k].nodeValue.length))
                    ++k;
                const got = window.cousoir.encode(texts[k], at - starts[k]);
                if (got !== expected) {
                    result = 'cfi-differs ' + at + ' ' + expected + ' ' + got;
                    break;
                }
            }
            document.title = result;
        })(%1, %2))").arg(QString::fromUtf8(QJsonDocument(table).toJson(QJsonDocument::Compact))).arg(total);
        QVERIFY(QMetaObject::invokeMethod(view, "runJavaScript", Q_ARG(QString, script)));
        QTRY_COMPARE_WITH_TIMEOUT(view->property("title").toString(), QStringLiteral("cfi-ok %1").arg(table.size()), 20000);
    }
    reader->close();
#endif
}

// A book with only a PDF read in Cousoir's PDF reader.
void LibraryUiTest::readsPdfs()
{
#ifndef COUSOIR_READER
    QSKIP("Built without Qt WebEngine");
#else
    library()->setProperty("currentBookId", 5);
    QString format;
    QMetaObject::invokeMethod(library(), "readableFormat", Q_RETURN_ARG(QString, format), Q_ARG(int, 5));
    QCOMPARE(format, QStringLiteral("PDF"));
    QMetaObject::invokeMethod(m_window, "readBook");
    QQuickWindow *reader = m_window->findChild<QQuickWindow *>(QStringLiteral("pdfReaderWindow"));
    QVERIFY(reader);
    QTRY_VERIFY(reader->isVisible());
    QTRY_COMPARE(reader->property("pages").toInt(), 1);
    QCOMPARE(reader->title(), QStringLiteral("Manuel technique"));
    reader->close();
    QTRY_VERIFY(!reader->isVisible());
#endif
}

void LibraryUiTest::usesOmarchyColorsByDefault()
{
    const auto settingsWindow = [this] {
        return m_window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    };
    QVERIFY(settingsWindow());
    QCOMPARE(settingsWindow()->property("currentThemeMode").toString(), QStringLiteral("system"));

    // On Omarchy, its colors until another mode is picked; the setting stays
    // empty until then.
    const auto removeTheme = qScopeGuard([] { QDir(themeStateRoot).removeRecursively(); });
    QVERIFY(QDir().mkpath(themeStateRoot + QStringLiteral("/theme")));
    QFile colors(themeStateRoot + QStringLiteral("/theme/colors.toml"));
    QVERIFY(colors.open(QIODevice::WriteOnly));
    colors.write("accent = \"#d35f5f\"\n");
    colors.close();
    m_engine.reset();
    QVERIFY(loadUi());
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool(), 10000);
    QCOMPARE(settingsWindow()->property("currentThemeMode").toString(), QStringLiteral("omarchy"));
    QVERIFY(QSettings().value(QStringLiteral("Appearance/colorMode")).toString().isEmpty());

    QMetaObject::invokeMethod(settingsWindow(), "themeModeSelected", Q_ARG(QString, QStringLiteral("dark")));
    QCOMPARE(settingsWindow()->property("currentThemeMode").toString(), QStringLiteral("dark"));
    QTRY_COMPARE(QSettings().value(QStringLiteral("Appearance/colorMode")).toString(), QStringLiteral("dark"));
    m_engine.reset();
    QVERIFY(loadUi());
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool(), 10000);
    QCOMPARE(settingsWindow()->property("currentThemeMode").toString(), QStringLiteral("dark"));
}

void LibraryUiTest::missingLibraryShowsError()
{
    m_engine.reset();
    QTemporaryDir empty;
    qputenv("COUSOIR_LIBRARY_PATH", QFile::encodeName(empty.path()));
    QVERIFY(loadUi());
    QTRY_COMPARE_WITH_TIMEOUT(text("emptyTitle"), QStringLiteral("Cannot read the library"), 10000);
    QVERIFY(text("emptyMessage").contains(QStringLiteral("no Calibre library")));
    QVERIFY(control("reloadButton")->isVisible());
    QVERIFY(control("chooseLibraryButton")->isVisible());
    QCOMPARE(control("chooseLibraryButton")->property("variant").toString(), QStringLiteral("outline"));
    QCOMPARE(text("bookCount"), QString());
    QVERIFY(!control("writeState")->isVisible());
}

void LibraryUiTest::welcomesWithoutLibrary()
{
    m_engine.reset();
    QTemporaryDir config;
    qunsetenv("COUSOIR_LIBRARY_PATH");
    qputenv("CALIBRE_CONFIG_DIRECTORY", QFile::encodeName(config.path()));
    const auto restore = qScopeGuard([] { qunsetenv("CALIBRE_CONFIG_DIRECTORY"); });
    QVERIFY(loadUi());
    QTRY_COMPARE_WITH_TIMEOUT(text("emptyTitle"), QStringLiteral("Welcome to Cousoir"), 10000);
    QVERIFY(text("emptyMessage").contains(QStringLiteral("metadata.db")));
    QVERIFY(control("chooseLibraryButton")->isVisible());
    QCOMPARE(control("chooseLibraryButton")->property("variant").toString(), QStringLiteral("primary"));
    QVERIFY(control("reloadButton")->isVisible());

    // Calibre's welcome wizard made a library meanwhile: Reload finds it.
    QFile global(config.filePath(QStringLiteral("global.py.json")));
    QVERIFY(global.open(QIODevice::WriteOnly));
    global.write(QJsonDocument(QJsonObject{ { QStringLiteral("library_path"), m_libraryPath } }).toJson());
    global.close();
    click("reloadButton");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool(), 10000);
    QCOMPARE(library()->property("libraryPath").toString(), QDir::cleanPath(m_libraryPath));
}

void LibraryUiTest::editsDetailsInPlace()
{
    openWritable(1);
    QCOMPARE(control("field_title")->property("text").toString(), QStringLiteral("L'Écume des jours"));
    QVERIFY(control("coverButton")->isVisible());

    // Enter saves; the book's folder follows its title.
    retype("field_title", QStringLiteral("Mood Indigo"));
    QCOMPARE(text("keyHints"), QStringLiteral("Enter save · Esc revert · Tab next field"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Mood Indigo"));
    QCOMPARE(text("pageTitle"), QStringLiteral("Mood Indigo"));
    QVERIFY(QFileInfo(QDir(m_libraryPath).filePath(QStringLiteral("Boris Vian/Mood Indigo (1)/Mood Indigo - Boris Vian.epub"))).isFile());
    // The title sort followed, with the book's French articles.
    QCOMPARE(library()->property("currentBook").toMap().value(QStringLiteral("sort")).toString(), QStringLiteral("Mood Indigo"));
    // Its own change keeps the library writable.
    QTRY_VERIFY(guard()->property("writable").toBool());

    // Tab saves too and moves on; the list shows the change.
    retype("field_tags", QStringLiteral("Roman, Jazz"));
    QTest::keyClick(m_window, Qt::Key_Tab);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("tags")).toStringList(),
                 (QStringList{ QStringLiteral("Roman"), QStringLiteral("Jazz") }));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    const QVariantList rows = bookRows();
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const QVariant &row) {
        return row.toMap().value(QStringLiteral("title")).toString() == QLatin1String("Mood Indigo");
    }));
}

void LibraryUiTest::keepsWhatIsTyped()
{
    openWritable(2);
    // Typing in the title sort while a change of the title, which it
    // follows, is saved.
    retype("field_sort", QStringLiteral("Mon tri"));
    const QVariantMap title{ { QStringLiteral("title"), QStringLiteral("The White Rose") } };
    QCOMPARE(QMetaObject::invokeMethod(library(), "updateBook", Q_ARG(int, 2), Q_ARG(QVariantMap, title)), true);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("sort")).toString(),
                 QStringLiteral("White Rose, The"));
    QCOMPARE(text("field_sort"), QStringLiteral("Mon tri"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("sort")).toString(),
                 QStringLiteral("Mon tri"));
    // The field shows what was saved, its focus kept.
    QTRY_COMPARE(text("field_sort"), QStringLiteral("Mon tri"));
    QVERIFY(control("field_sort")->hasActiveFocus());
}

void LibraryUiTest::revertsBookChanges()
{
    openWritable(2);
    QVERIFY(!control("revertChangesButton")->isVisible());
    retype("field_publisher", QStringLiteral("Gallimard"));
    QTest::keyClick(m_window, Qt::Key_Return);
    retype("field_series", QString());
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_VERIFY(control("revertChangesButton")->isVisible());
    QCOMPARE(library()->property("currentBook").toMap().value(QStringLiteral("series")).toString(), QString());

    // Asked first; Esc keeps the changes.
    click("revertChangesButton");
    QTRY_VERIFY(control("revertQuestion")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!control("revertQuestion")->isVisible());
    QCOMPARE(library()->property("currentBookId").toInt(), 2);

    click("revertChangesButton");
    click("confirmRevertButton");
    waitForSave();
    const QVariantMap book = library()->property("currentBook").toMap();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("publisher")).toString(), QStringLiteral("Tor"));
    QCOMPARE(library()->property("currentBook").toMap().value(QStringLiteral("series")).toString(),
             QStringLiteral("Chronicles of the Black Company"));
    QVERIFY(!control("revertChangesButton")->isVisible());
}

void LibraryUiTest::refusesInvalidValues()
{
    openWritable(3);
    // Esc puts the stored value back without saving.
    retype("field_title", QStringLiteral("Nope"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QCOMPARE(control("field_title")->property("text").toString(), QStringLiteral("Shadows Linger"));
    QCOMPARE(library()->property("currentBookId").toInt(), 3);
    QVERIFY(!library()->property("saving").toBool());

    // A date the field cannot take shows in red and is not saved.
    retype("field_pubdate", QStringLiteral("someday"));
    QVERIFY(control("field_pubdate")->property("invalid").toBool());
    QTest::keyClick(m_window, Qt::Key_Return);
    QVERIFY(!library()->property("saving").toBool());
    QCOMPARE(library()->property("currentBook").toMap().value(QStringLiteral("pubdateText")).toString(), QString());

    retype("field_pubdate", QStringLiteral("1989-6"));
    QVERIFY(!control("field_pubdate")->property("invalid").toBool());
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("pubdateText")).toString(),
                 QStringLiteral("1989-06-15"));

    // A title cannot be empty.
    retype("field_title", QString());
    QVERIFY(control("field_title")->property("invalid").toBool());
}

void LibraryUiTest::changesRating()
{
    openWritable(1);
    QCOMPARE(control("field_rating")->property("currentIndex").toInt(), 8);
    click("field_rating");
    QTRY_VERIFY(control("field_rating")->property("popup").value<QObject *>()->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("rating")).toString(), QStringLiteral("★★★★½"));
}

void LibraryUiTest::editsDescription()
{
    openWritable(1);
    QCOMPARE(library()->property("currentBook").toMap().value(QStringLiteral("descriptionText")).toString(),
             QStringLiteral("Un roman d’amour fou.\n\nParis, *1946*."));
    click("editDescriptionButton");
    QQuickItem *editor = control("descriptionEditor");
    QTRY_VERIFY(editor->hasActiveFocus());
    QCOMPARE(text("descriptionEditor"), QStringLiteral("Un roman d’amour fou.\n\nParis, *1946*."));
    QCOMPARE(text("keyHints"), QStringLiteral("Ctrl+Enter save · Esc cancel"));

    // Esc puts it back and keeps the book open.
    typeText(QStringLiteral(" ajout"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!control("descriptionEditor")->isVisible());
    QCOMPARE(library()->property("currentBookId").toInt(), 1);
    QVERIFY(!library()->property("saving").toBool());

    // Ctrl+Enter saves, in paragraphs, with the marks as HTML.
    click("editDescriptionButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    editor->setProperty("text", QStringLiteral("Un roman **fou**.\nParis, 1946.\n\n- jazz\n- amour"));
    QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
    waitForSave();
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("description")).toString(),
                 QStringLiteral("<div><p>Un roman <strong>fou</strong>.<br>Paris, 1946.</p>"
                                "<ul><li>jazz</li><li>amour</li></ul></div>"));
    QVERIFY(library()->property("canRevert").toBool());

    // Saving it unchanged writes nothing; closing the book saves an edit.
    click("editDescriptionButton");
    click("saveDescriptionButton");
    QVERIFY(!library()->property("saving").toBool());
    click("editDescriptionButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    editor->setProperty("text", QString());
    click("rootCrumb");
    waitForSave();
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    library()->setProperty("currentBookId", 1);
    QTRY_COMPARE(library()->property("currentBook").toMap().value(QStringLiteral("description")).toString(), QString());
}

void LibraryUiTest::keepsNotes()
{
    // The notes Calibre 7 keeps, on an author with an image and a series.
    QDir(CalibreDb::notesFolder(m_libraryPath)).removeRecursively();
    QVERIFY(copyTree(QStringLiteral(COUSOIR_TEST_DATA_DIR "/notes/.calnotes"), CalibreDb::notesFolder(m_libraryPath)));
    makeWritable();
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool() && !library()->property("loading").toBool(), 15000);
    const auto filter = [this] { return library()->property("filter").toMap(); };

    // Marked in their section, shown beside their books.
    click("sidebar_series");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Series"));
    const QVariantList series = library()->property("facetRows").toList();
    QCOMPARE(std::count_if(series.cbegin(), series.cend(), [](const QVariant &row) {
                 return row.toMap().value(QStringLiteral("hasNote")).toBool();
             }), 1);
    QMetaObject::invokeMethod(library(), "openFacetItem", Q_ARG(int, 2));
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Contes"));
    QTRY_VERIFY(control("notePane")->isVisible());
    QVERIFY(text("noteText").contains(QStringLiteral("Tales for children")));

    // Written as text, saved as Calibre saves a note.
    click("editNoteButton");
    QQuickItem *editor = control("noteEditor");
    QTRY_VERIFY(editor->hasActiveFocus());
    QCOMPARE(text("noteEditor"), QStringLiteral("Tales for children, *and others*."));
    editor->setProperty("text", QStringLiteral("Contes **pour** enfants."));
    QTest::keyClick(m_window, Qt::Key_Return, Qt::ControlModifier);
    waitForSave();
    QTRY_VERIFY(filter().value(QStringLiteral("note")).toString().contains(QStringLiteral("<strong>pour</strong>")));
    QVERIFY(!editor->isVisible());
    const QString notes = QDir(CalibreDb::notesFolder(m_libraryPath)).filePath(QStringLiteral("notes.db"));
    const auto searchable = [&notes] {
        SqliteDb db;
        QString error;
        return db.open(notes, SqliteDb::Mode::Immutable, &error)
            ? db.value(QStringLiteral("SELECT searchable_text FROM notes WHERE colname='series' AND item=2"))
            : QVariant();
    };
    QCOMPARE(searchable().toString(), QStringLiteral("Contes\nContes pour enfants."));

    // Saved unchanged, nothing is written.
    click("editNoteButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    click("saveNoteButton");
    QVERIFY(!library()->property("saving").toBool());
    QTRY_VERIFY(!editor->isVisible());

    // A save that fails gives the text back to finish it.
    QFile database(notes);
    QVERIFY(database.setPermissions(QFileDevice::ReadOwner));
    click("editNoteButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    editor->setProperty("text", QStringLiteral("Contes cruels."));
    click("saveNoteButton");
    QTRY_VERIFY_WITH_TIMEOUT(editor->isVisible(), 15000);
    QCOMPARE(text("noteEditor"), QStringLiteral("Contes cruels."));
    QVERIFY(database.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!editor->isVisible());

    // Emptied, it goes, kept aside for Calibre.
    click("editNoteButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    editor->setProperty("text", QString());
    click("saveNoteButton");
    waitForSave();
    QTRY_VERIFY(filter().value(QStringLiteral("note")).toString().isEmpty());
    QVERIFY(!searchable().isValid());
    QCOMPARE(text("editNoteButton"), QStringLiteral("Add a note"));

    // A note with images is read, and left to Calibre to change.
    click("sidebar_authors");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Authors"));
    QMetaObject::invokeMethod(library(), "openFacetItem", Q_ARG(int, 2));
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Glen Cook"));
    QTRY_VERIFY(text("noteText").contains(QStringLiteral("dark")));
    QVERIFY(!control("editNoteButton")->isVisible());
    QVERIFY(filter().value(QStringLiteral("note")).toString().contains(QStringLiteral("file://")));
    // Its image, 600 pixels wide, fits the pane.
    QVERIFY(filter().value(QStringLiteral("note")).toString().contains(QStringLiteral("width=\"600\"")));
    const QRegularExpressionMatch shown
        = QRegularExpression(QStringLiteral("width=\"(\\d+)\"")).match(text("noteText"));
    QVERIFY(shown.hasMatch());
    QVERIFY(shown.captured(1).toInt() <= control("notePane")->width());

    // A note Calibre formatted beyond what text keeps is left to Calibre.
    {
        SqliteDb db;
        QString error;
        QVERIFY2(db.open(notes, SqliteDb::Mode::ReadWrite, &error)
                     && CalibreFts::registerTokenizers(db.handle(), QStringLiteral("fr"), &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QStringLiteral("UPDATE notes SET doc='<p>Worlds <u>not</u> ours.</p>' "
                                        "WHERE colname='tags' AND item=3"), &error), qPrintable(error));
    }
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool() && !library()->property("loading").toBool(), 15000);
    click("sidebar_tags");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Tags"));
    QMetaObject::invokeMethod(library(), "openFacetItem", Q_ARG(int, 3));
    QTRY_VERIFY(text("noteText").contains(QStringLiteral("ours")));
    QVERIFY(filter().value(QStringLiteral("noteLosesFormatting")).toBool());
    QVERIFY(!control("editNoteButton")->isVisible());

    // A language's note is Calibre's on its code.
    click("sidebar_languages");
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Languages"));
    QMetaObject::invokeMethod(library(), "openFacetItem", Q_ARG(int, 2));
    QTRY_COMPARE(text("editNoteButton"), QStringLiteral("Add a note"));
    click("editNoteButton");
    QTRY_VERIFY(editor->hasActiveFocus());
    editor->setProperty("text", QStringLiteral("Read in the original."));
    click("saveNoteButton");
    waitForSave();
    QTRY_VERIFY(filter().value(QStringLiteral("note")).toString().contains(QStringLiteral("original")));
    {
        SqliteDb db;
        QString error;
        QVERIFY2(db.open(notes, SqliteDb::Mode::Immutable, &error), qPrintable(error));
        QCOMPARE(db.value(QStringLiteral("SELECT searchable_text FROM notes WHERE colname='languages' AND item=2")).toString(),
                 QStringLiteral("eng\nRead in the original."));
    }

    // A library Calibre 7 never opened has no notes to show.
    QDir(CalibreDb::notesFolder(m_libraryPath)).removeRecursively();
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY(!control("notePane")->isVisible());
}

void LibraryUiTest::browsesHighlights()
{
    // The library with the highlights and bookmarks Calibre saved
    // (tools/oracle/annotations_oracle.py).
    const QString database = QDir(m_libraryPath).filePath(QStringLiteral("metadata.db"));
    QVERIFY(QFile::remove(database));
    QVERIFY(QFile::copy(QStringLiteral(COUSOIR_TEST_DATA_DIR "/annotations/metadata.db"), database));
    QFile(database).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QMetaObject::invokeMethod(library(), "reload");
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool() && !library()->property("loading").toBool(), 15000);
    QTRY_COMPARE(library()->property("counts").toMap().value(QStringLiteral("highlights")).toInt(), 7);

    // By book, the books by their titles' sort.
    click("sidebar_highlights");
    QQuickItem *page = control("highlightsPage");
    QTRY_VERIFY(page && page->isVisible());
    auto *browser = page->property("browser").value<HighlightBrowser *>();
    QVERIFY(browser);
    QTRY_COMPARE(browser->count(), 7);
    QCOMPARE(browser->rows().size(), 10);
    QCOMPARE(browser->rows().constFirst().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("The Black Company"));
    QCOMPARE(text("bookCount"), QStringLiteral("7 highlights"));

    // A search of Calibre's index, the words found in bold; Ctrl+F goes to
    // it.
    QTest::keyClick(m_window, Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(control("highlightSearch")->hasActiveFocus());
    typeText(QStringLiteral("amour"));
    QTRY_COMPARE(browser->count(), 1);
    QVERIFY(browser->rows().value(1).toMap().value(QStringLiteral("html")).toString().startsWith(QStringLiteral("<b>L'amour</b> est")));
    QVERIFY(browser->exportText(true).startsWith(QStringLiteral("# L'Écume des jours\n\n## Chapitre II")));

    // Enter asks the reader for the book, where the highlight is.
    QSignalSpy read(page, SIGNAL(readRequested(int,int,QString)));
    browser->setQuery(QString());
    browser->setType(QStringLiteral("highlight"));
    QTRY_COMPARE(browser->count(), 6);
    QMetaObject::invokeMethod(page, "focusList");
    QTRY_VERIFY(control("highlightList")->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(read.size(), 1);
    QCOMPARE(read.constFirst().at(0).toInt(), 2);
    QCOMPARE(read.constFirst().at(1).toInt(), 2);
    QCOMPARE(read.constFirst().at(2).toString(), QStringLiteral("/4/2/4/1:0"));

    browser->setType(QStringLiteral("bookmark"));
    QTRY_COMPARE(browser->count(), 1);
}

void LibraryUiTest::editsCustomColumns()
{
    // The library with a custom column of each kind.
    m_libraryPath = m_libraryDir->filePath(QStringLiteral("custom"));
    QVERIFY(copyTree(QStringLiteral(COUSOIR_TEST_DATA_DIR "/library-custom"), m_libraryPath));
    library()->setProperty("libraryPath", m_libraryPath);
    QTRY_VERIFY_WITH_TIMEOUT(library()->property("loaded").toBool()
                                 && library()->property("libraryPath").toString() == m_libraryPath, 10000);
    // Tall enough for every field without scrolling.
    m_window->resize(1240, 1400);
    openWritable(1);
    const auto custom = [this](const QString &key) {
        for (const QVariant &field : library()->property("currentBook").toMap().value(QStringLiteral("custom")).toList()) {
            if (field.toMap().value(QStringLiteral("key")).toString() == key)
                return field.toMap();
        }
        return QVariantMap();
    };
    QCOMPARE(custom(QStringLiteral("#genre")).value(QStringLiteral("text")).toString(), QStringLiteral("SF, Classique"));
    QCOMPARE(custom(QStringLiteral("#review")).value(QStringLiteral("editText")).toString(), QStringLiteral("Bien"));
    // A composite shows as its template makes it, and cannot be edited.
    QCOMPARE(custom(QStringLiteral("#computed")).value(QStringLiteral("text")).toString(),
             QStringLiteral("L'Écume des jours by Boris Vian"));
    QVERIFY(!custom(QStringLiteral("#computed")).value(QStringLiteral("editable")).toBool());
    QCOMPARE(control("field_#computed")->property("value").toString(), QStringLiteral("L'Écume des jours by Boris Vian"));

    // A save keeps the other fields as they are, focus and typing with them.
    const QPointer<QQuickItem> price = control("field_#price");
    retype("field_#note", QStringLiteral("Autre note"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#note")).value(QStringLiteral("text")).toString(), QStringLiteral("Autre note"));
    QVERIFY(price && price == control("field_#price"));

    // A change the template reads computes it again.
    retype("field_title", QStringLiteral("L'Arrache-coeur"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#computed")).value(QStringLiteral("text")).toString(),
                 QStringLiteral("L'Arrache-coeur by Boris Vian"));

    // A number takes digits only.
    retype("field_#pagecount", QStringLiteral("abc"));
    QVERIFY(control("field_#pagecount")->property("invalid").toBool());
    QTest::keyClick(m_window, Qt::Key_Escape);
    retype("field_#price", QStringLiteral("9,5"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#price")).value(QStringLiteral("text")).toString(), QStringLiteral("9.5"));

    // Long text in HTML edits as the description does.
    retype("field_#review", QStringLiteral("**Tres** bien"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#review")).value(QStringLiteral("editText")).toString(),
                 QStringLiteral("**Tres** bien"));

    // Yes or no, and a list of values, choose.
    click("field_#read");
    QTRY_VERIFY(control("field_#read")->property("popup").value<QObject *>()->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#read")).value(QStringLiteral("text")).toString(), QStringLiteral("No"));
    click("field_#shelf");
    QTRY_VERIFY(control("field_#shelf")->property("popup").value<QObject *>()->property("visible").toBool());
    QTest::keyClick(m_window, Qt::Key_Down);
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#shelf")).value(QStringLiteral("text")).toString(), QStringLiteral("A"));

    // Revert puts them all back.
    click("revertChangesButton");
    QTRY_VERIFY(control("confirmRevertButton") && control("confirmRevertButton")->isVisible());
    click("confirmRevertButton");
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#note")).value(QStringLiteral("text")).toString(), QStringLiteral("Une note"));
    QCOMPARE(custom(QStringLiteral("#read")).value(QStringLiteral("text")).toString(), QStringLiteral("Yes"));
    QCOMPARE(custom(QStringLiteral("#shelf")).value(QStringLiteral("text")).toString(), QString());
    QCOMPARE(custom(QStringLiteral("#review")).value(QStringLiteral("editText")).toString(), QStringLiteral("Bien"));
    QCOMPARE(custom(QStringLiteral("#price")).value(QStringLiteral("text")).toString(), QString());

    const auto update = [this](const QVariantMap &changes) {
        QString error;
        QMetaObject::invokeMethod(library(), "updateBook", Q_RETURN_ARG(QString, error), Q_ARG(int, 1),
                                  Q_ARG(QVariantMap, changes));
        return error;
    };
    // A rating of half a star more than a whole-star column takes shows as
    // it is; a long text holding a table is Calibre's to edit.
    QCOMPARE(update({ { QStringLiteral("#myrating"), 7 },
                      { QStringLiteral("#review"), QStringLiteral("<table><tr><td>Bien</td></tr></table>") } }),
             QString());
    waitForSave();
    QTRY_COMPARE(control("field_#myrating")->property("currentText").toString(), QStringLiteral("★★★½"));
    QVERIFY(!control("field_#review")->isEnabled());
    QVERIFY(control("formattingKept_#review")->isVisible());

    // A date cleared, then set and reverted: cleared again.
    retype("field_#started", QString());
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#started")).value(QStringLiteral("text")).toString(), QString());
    QMetaObject::invokeMethod(m_window, "closeBook");
    QTRY_VERIFY(control("booksPage")->isVisible());
    openWritable(1);
    retype("field_#started", QStringLiteral("2020-01-01"));
    QTest::keyClick(m_window, Qt::Key_Return);
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#started")).value(QStringLiteral("text")).toString(), QStringLiteral("2020-01-01"));
    click("revertChangesButton");
    QTRY_VERIFY(control("confirmRevertButton") && control("confirmRevertButton")->isVisible());
    click("confirmRevertButton");
    waitForSave();
    QTRY_COMPARE(custom(QStringLiteral("#started")).value(QStringLiteral("text")).toString(), QString());
}

// Ctrl+K: a book, an author's books and a section, each a few letters and
// Enter away; Esc closes it.
void LibraryUiTest::findsWithPalette()
{
    QObject *palette = object("commandPalette");
    QVERIFY(palette);
    // The first hit for what is typed; none when the palette does not open.
    const auto find = [&](const QString &typed) -> QVariantMap {
        QTest::keySequence(m_window, QKeySequence(QStringLiteral("Ctrl+K")));
        if (!QTest::qWaitFor([&] { return palette->property("opened").toBool()
                                          && control("paletteField")->hasActiveFocus(); }))
            return {};
        typeText(typed);
        if (!QTest::qWaitFor([&] { return !palette->property("hits").toList().isEmpty(); }))
            return {};
        return palette->property("hits").toList().value(0).toMap();
    };

    QCOMPARE(find(QStringLiteral("omens")).value(QStringLiteral("title")).toString(), QStringLiteral("Good Omens"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!palette->property("opened").toBool());
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 4);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);

    QCOMPARE(find(QStringLiteral("pratch")).value(QStringLiteral("kind")).toString(), QStringLiteral("authors"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("section").toString(), QStringLiteral("authors"));
    QTRY_COMPARE(library()->property("filter").toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Terry Pratchett"));
    QCOMPARE(bookRows().size(), 1);

    QCOMPARE(find(QStringLiteral("trash")).value(QStringLiteral("kind")).toString(), QStringLiteral("section"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(control("trashPage")->isVisible());

    // From a book whose description is being edited: the text is saved,
    // the book closes, and the author's books show.
    QTest::keyClick(m_window, Qt::Key_Escape);
    openWritable(1);
    click("editDescriptionButton");
    QTRY_VERIFY(control("descriptionEditor")->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_End, Qt::ControlModifier);
    typeText(QStringLiteral(" Saved"));
    QCOMPARE(find(QStringLiteral("pratch")).value(QStringLiteral("kind")).toString(), QStringLiteral("authors"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    QTRY_COMPARE(library()->property("filter").toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Terry Pratchett"));
    waitForSave();
    QTRY_VERIFY(library()->property("books").value<BookListModel *>() && bookRows().size() == 1);
    // The section shown, picked again, goes back to its list.
    QCOMPARE(find(QStringLiteral("authors")).value(QStringLiteral("key")).toString(), QStringLiteral("authors"));
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(library()->property("filter").toMap().isEmpty());
    QTRY_VERIFY(control("facetPage")->isVisible());
    library()->setProperty("currentBookId", 1);
    QTRY_VERIFY(library()->property("currentBook").toMap().value(QStringLiteral("description")).toString()
                    .contains(QStringLiteral("Saved")));
    QMetaObject::invokeMethod(m_window, "closeBook");

    QTest::keySequence(m_window, QKeySequence(QStringLiteral("Ctrl+K")));
    QTRY_VERIFY(palette->property("opened").toBool());
    QTRY_VERIFY(control("paletteField")->hasActiveFocus());
    typeText(QStringLiteral("xyzzy"));
    QTRY_VERIFY(control("paletteEmpty")->isVisible());
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!palette->property("opened").toBool());
}

void LibraryUiTest::changesCovers()
{
    openWritable(5);
    QVERIFY(!library()->property("currentBook").toMap().value(QStringLiteral("hasCover")).toBool());
    QCOMPARE(QMetaObject::invokeMethod(library(), "setCover", Q_ARG(int, 5),
                                       Q_ARG(QString, QStringLiteral(COUSOIR_TEST_DATA_DIR "/edits/transparent.png"))),
             true);
    waitForSave();
    QTRY_VERIFY(library()->property("currentBook").toMap().value(QStringLiteral("hasCover")).toBool());
    QTRY_COMPARE(control("bookCoverImage")->property("status").toInt(), 1);
    const QString cover = QDir(m_libraryPath).filePath(QStringLiteral("Jean-Pierre Dupont/Manuel technique (5)/cover.jpg"));
    QCOMPARE(QImage(cover).size(), QSize(400, 600));

    // Ctrl+V pastes the clipboard's image.
    QImage pasted(120, 180, QImage::Format_RGB32);
    pasted.fill(Qt::darkGreen);
    QGuiApplication::clipboard()->setImage(pasted);
    QTest::keyClick(m_window, Qt::Key_V, Qt::ControlModifier);
    waitForSave();
    QTRY_COMPARE(QImage(cover).size(), QSize(120, 180));

    // The Cover menu removes it.
    click("coverButton");
    QQuickItem *remove = control("coverMenu_remove");
    QTRY_VERIFY(remove && remove->isVisible());
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(remove));
    waitForSave();
    QTRY_VERIFY(!library()->property("currentBook").toMap().value(QStringLiteral("hasCover")).toBool());
    QVERIFY(!QFileInfo::exists(cover));
}

int main(int argc, char **argv)
{
#ifdef COUSOIR_READER
    BookReader::initialize();
#endif
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid())
        return 1;
    themeStateRoot = settingsDir.filePath(QStringLiteral("omarchy"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("cousoir-tests"));
    app.setApplicationName(QStringLiteral("library-ui"));
    app.setApplicationVersion(QStringLiteral(COUSOIR_VERSION));
    LibraryUiTest test;
    return QTest::qExec(&test, argc, argv);
}

void LibraryUiTest::movesBooksToTrashAndBack()
{
    makeWritable();
    // Del on the list asks before moving the current book.
    QMetaObject::invokeMethod(m_window, "focusCurrentList");
    QTRY_VERIFY(m_window->activeFocusItem());
    const QVariantList rows = bookRows();
    QVERIFY(!rows.isEmpty());
    const QVariantMap first = rows.first().toMap();
    QVERIFY(text("keyHints").endsWith(QStringLiteral("Del trash")));
    QTest::keyClick(m_window, Qt::Key_Delete);
    QVERIFY(object("trashDialog"));
    QTRY_VERIFY(object("trashDialog")->property("opened").toBool());
    QCOMPARE(object("trashDialog")->property("title").toString(),
             QStringLiteral("Move “%1” to the trash?").arg(first.value(QStringLiteral("title")).toString()));
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_COMPARE(bookRows().size(), rows.size() - 1);

    // The Trash section lists it, and Restore puts it back.
    click("sidebar_trash");
    QTRY_VERIFY(control("trashPage")->isVisible());
    const QString key = QStringLiteral("b-%1").arg(first.value(QStringLiteral("id")).toInt());
    QTRY_VERIFY(control(QStringLiteral("trash_") + key));
    QCOMPARE(text("trashExpiry"), QStringLiteral("Calibre deletes books from the trash after 14 days."));
    QCOMPARE(text("bookCount"), QStringLiteral("1 item"));
    QVERIFY(!control("trashEmpty")->isVisible());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(control(QStringLiteral("restore_") + key)));
    waitForSave();
    QTRY_VERIFY(control("trashEmpty")->isVisible());
    QCOMPARE(library()->property("counts").toMap().value(QStringLiteral("books")).toInt(), rows.size());

    // From the book page too.
    click("sidebar_books");
    library()->setProperty("currentBookId", 2);
    QTRY_VERIFY(control("trashBookButton") && control("trashBookButton")->isVisible());
    click("trashBookButton");
    QTRY_VERIFY(object("trashDialog")->property("opened").toBool());
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 0);
    QTRY_COMPARE(library()->property("counts").toMap().value(QStringLiteral("trash")).toInt(), 1);
}

void LibraryUiTest::editsManyBooks()
{
    makeWritable();
    QQuickItem *list = control("bookList");
    QObject *books = library()->property("books").value<QObject *>();
    // By title: The Black Company (2), L'Écume des jours (1), Good Omens (4),
    // Manuel technique (5), Le Petit Prince (6), Shadows Linger (3).
    click("bookRow0");
    QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
    QTRY_COMPARE(books->property("selectedCount").toInt(), 3);
    QCOMPARE(list->property("currentIndex").toInt(), 2);
    QCOMPARE(text("bookCount"), QStringLiteral("3 selected · 6 books"));

    // The pane beside the list edits them together.
    QTRY_VERIFY(control("bulkPane") && control("bulkPane")->isVisible());
    QCOMPARE(text("bulkTitle"), QStringLiteral("3 books"));
    click("bulk_addTags");
    typeText(QStringLiteral("Lu"));
    click("bulk_publisher");
    typeText(QStringLiteral("Folio"));
    click("bulkApplyButton");
    waitForSave();
    const auto book = [this](int id) {
        for (const QVariant &row : bookRows()) {
            if (row.toMap().value(QStringLiteral("id")).toInt() == id)
                return row.toMap();
        }
        return QVariantMap();
    };
    QTRY_VERIFY(library()->property("bulkUndoCount").toInt() == 3);
    // The tags as saved, in their order.
    const QString database = QDir(m_libraryPath).filePath(QStringLiteral("metadata.db"));
    const auto tagsOf = [database](int id) {
        SqliteDb db;
        QString error;
        QStringList tags;
        if (!db.open(database, SqliteDb::Mode::ReadWrite, &error))
            return tags;
        for (const QVariantList &row : db.rows(QStringLiteral("SELECT name FROM tags JOIN books_tags_link l ON l.tag=tags.id "
                                                              "WHERE l.book=? ORDER BY l.id"), { id }))
            tags.append(row.value(0).toString());
        return tags;
    };
    QCOMPARE(tagsOf(2), (QStringList{ QStringLiteral("Fantasy"), QStringLiteral("Lu") }));
    QCOMPARE(tagsOf(1), (QStringList{ QStringLiteral("Roman"), QStringLiteral("Classique"), QStringLiteral("Lu") }));
    QCOMPARE(tagsOf(5), QStringList{ QStringLiteral("Technique") });
    QCOMPARE(book(4).value(QStringLiteral("id")).toInt(), 4);
    // The selection stays through the save.
    QCOMPARE(books->property("selectedCount").toInt(), 3);

    // Undo puts them back.
    click("bulkUndoButton");
    waitForSave();
    QTRY_COMPARE(tagsOf(2), QStringList{ QStringLiteral("Fantasy") });
    QCOMPARE(tagsOf(4), (QStringList{ QStringLiteral("Fantasy"), QStringLiteral("Humour") }));
    QCOMPARE(library()->property("bulkUndoCount").toInt(), 0);

    // Ctrl takes a book out, or adds one; Ctrl+A takes them all, Esc goes
    // back to one.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ControlModifier, center(control("bookRow1")));
    QTRY_COMPARE(books->property("selectedCount").toInt(), 2);
    // The current book, taken out, is not what Del acts on.
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(object("trashDialog")->property("opened").toBool());
    QCOMPARE(object("trashDialog")->property("title").toString(), QStringLiteral("Move 2 books to the trash?"));
    QCOMPARE(object("trashDialog")->property("bookIds").toList(), (QVariantList{ 2, 4 }));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!object("trashDialog")->property("opened").toBool());
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::ControlModifier, center(control("bookRow5")));
    QTRY_COMPARE(books->property("selectedCount").toInt(), 3);
    QCOMPARE(books->property("selectedIds").toList(),
             (QVariantList{ 2, 4, 3 }));
    QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
    QTRY_COMPARE(books->property("selectedCount").toInt(), 6);
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_COMPARE(books->property("selectedCount").toInt(), 1);
    QVERIFY(!control("bulkPane")->isVisible());

    // Delete asks about all of them.
    QTest::keyClick(m_window, Qt::Key_Home);
    QTest::keyClick(m_window, Qt::Key_End, Qt::ShiftModifier);
    QTRY_COMPARE(books->property("selectedCount").toInt(), 6);
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(object("trashDialog")->property("opened").toBool());
    QCOMPARE(object("trashDialog")->property("title").toString(), QStringLiteral("Move 6 books to the trash?"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!object("trashDialog")->property("opened").toBool());
    QCOMPARE(bookRows().size(), 6);
}

void LibraryUiTest::addsAndRemovesFiles()
{
    openWritable(1);
    // A file goes to the trash after a question.
    click("removeFormat_EPUB");
    QTRY_VERIFY(object("removeFormatDialog")->property("opened").toBool());
    QCOMPARE(object("removeFormatDialog")->property("title").toString(), QStringLiteral("Move the EPUB file to the trash?"));
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_VERIFY(formatsOf(library()->property("currentBook").toMap()).isEmpty());
    QVERIFY(QFileInfo::exists(QDir(m_libraryPath).filePath(QStringLiteral(".caltrash/f/1/epub"))));

    // A file added; the same format again asks before replacing it.
    const QUrl mobi = QUrl::fromLocalFile(QStringLiteral(COUSOIR_TEST_DATA_DIR "/import/Vicomte.mobi"));
    QMetaObject::invokeMethod(control("detailsPane"), "addFormat", Q_ARG(QVariant, mobi));
    waitForSave();
    QTRY_COMPARE(formatsOf(library()->property("currentBook").toMap()), QStringList{ QStringLiteral("MOBI") });
    QMetaObject::invokeMethod(control("detailsPane"), "addFormat", Q_ARG(QVariant, mobi));
    QTRY_VERIFY(object("replaceFormatDialog")->property("opened").toBool());
    QCOMPARE(object("replaceFormatDialog")->property("title").toString(), QStringLiteral("Replace the book's MOBI file?"));
    clickVisible("confirmAccept");
    waitForSave();
    QCOMPARE(formatsOf(library()->property("currentBook").toMap()), QStringList{ QStringLiteral("MOBI") });
}

void LibraryUiTest::asksAboutDuplicates()
{
    makeWritable();
    const int books = library()->property("counts").toMap().value(QStringLiteral("books")).toInt();
    const QString existing = QDir(m_libraryPath).filePath(QStringLiteral("Glen Cook/Shadows Linger (3)/Shadows Linger - Glen Cook.epub"));
    QString error;
    QMetaObject::invokeMethod(library(), "addBooks", Q_RETURN_ARG(QString, error),
                              Q_ARG(QVariantList, QVariantList{ QUrl::fromLocalFile(existing) }));
    QCOMPARE(error, QString());
    QTRY_VERIFY_WITH_TIMEOUT(object("duplicatesDialog")->property("opened").toBool(), 15000);
    QCOMPARE(object("duplicatesDialog")->property("title").toString(),
             QStringLiteral("“Shadows Linger” is already in the library"));
    QCOMPARE(object("duplicatesDialog")->property("message").toString(), QString());
    QCOMPARE(object("duplicatesDialog")->property("rejectText").toString(), QStringLiteral("Skip it"));
    clickVisible("confirmReject");
    QTRY_VERIFY(!object("duplicatesDialog")->property("opened").toBool());
    QTest::qWait(200);
    QVERIFY(!library()->property("saving").toBool());
    QCOMPARE(library()->property("counts").toMap().value(QStringLiteral("books")).toInt(), books);

    // Added anyway, it is a new book.
    QMetaObject::invokeMethod(library(), "addBooks", Q_RETURN_ARG(QString, error),
                              Q_ARG(QVariantList, QVariantList{ QUrl::fromLocalFile(existing) }));
    QTRY_VERIFY_WITH_TIMEOUT(object("duplicatesDialog")->property("opened").toBool(), 15000);
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_COMPARE(library()->property("counts").toMap().value(QStringLiteral("books")).toInt(), books + 1);
    QTRY_COMPARE(text("keyHints"), QStringLiteral("Added 1 book."));
}

void LibraryUiTest::checksLibrary()
{
    makeWritable();
    // Le Petit Prince has no file.
    QTRY_COMPARE_WITH_TIMEOUT(library()->property("check").toMap().value(QStringLiteral("count")).toInt(), 1, 10000);
    click("sidebar_check");
    QTRY_VERIFY(control("checkPage") && control("checkPage")->isVisible());
    QTRY_VERIFY(control("check_missing:6:"));
    // makeWritable() touched metadata.db: the library may be read and
    // checked again meanwhile.
    QTRY_COMPARE(text("bookCount"), QStringLiteral("1 thing to check"));

    // Book 3's EPUB added again: two copies of it.
    const QString existing = QDir(m_libraryPath).filePath(QStringLiteral("Glen Cook/Shadows Linger (3)/Shadows Linger - Glen Cook.epub"));
    QString error;
    QMetaObject::invokeMethod(library(), "addBooks", Q_RETURN_ARG(QString, error),
                              Q_ARG(QVariantList, QVariantList{ QUrl::fromLocalFile(existing) }));
    QCOMPARE(error, QString());
    QTRY_VERIFY_WITH_TIMEOUT(object("duplicatesDialog")->property("opened").toBool(), 15000);
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_VERIFY_WITH_TIMEOUT(control("check_duplicates:3,7") && control("keep_7"), 10000);

    // Calibre writing: the fixes wait, and say why.
    QFile journal(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    QMetaObject::invokeMethod(guard(), "refresh");
    QTRY_VERIFY(control("checkReadOnly")->isVisible());
    QVERIFY(!control("keep_7")->isEnabled());
    QVERIFY(journal.remove());
    QMetaObject::invokeMethod(guard(), "refresh");
    QTRY_VERIFY(control("keep_7")->isEnabled());

    click("keep_7");
    QTRY_VERIFY(object("keepDialog")->property("opened").toBool());
    QCOMPARE(object("keepDialog")->property("title").toString(),
             QStringLiteral("Keep “Shadows Linger” and move the other copy to the Trash?"));
    clickVisible("confirmAccept");
    waitForSave();
    QTRY_VERIFY_WITH_TIMEOUT(!control("check_duplicates:3,7"), 10000);
    QCOMPARE(library()->property("trashEntries").toList().size(), 1);

    // Ignored, it is out of the way until asked for.
    click("ignore_missing:6:");
    QTRY_COMPARE(text("checkEmpty"), QStringLiteral("Nothing to fix"));
    QVERIFY(control("checkEmpty")->isVisible());
    click("showIgnoredButton");
    QTRY_VERIFY(control("check_missing:6:"));
}

// The window's conversions, with a stand-in for Calibre: from the Files
// section, asking before replacing a file, and stopped from there.
void LibraryUiTest::convertsBooks()
{
    auto *conversions = qobject_cast<ConversionService *>(m_window->property("conversions").value<QObject *>());
    QVERIFY(conversions);
    conversions->setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-debug"));
    openWritable(1);
    click("convert_KEPUB");
    waitForSave();
    QTRY_COMPARE(formatsOf(library()->property("currentBook").toMap()),
                 (QStringList{ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }));
    QTRY_COMPARE(text("keyHints"), QStringLiteral("“L'Écume des jours” converted to KEPUB."));

    click("convert_KEPUB");
    QTRY_VERIFY(object("convertDialog")->property("opened").toBool());
    QCOMPARE(object("convertDialog")->property("title").toString(),
             QStringLiteral("Convert “L'Écume des jours” to KEPUB again?"));
    clickVisible("confirmReject");
    QTRY_VERIFY(!object("convertDialog")->property("opened").toBool());
    QVERIFY(!conversions->busy());

    qputenv("COUSOIR_FAKE_DELAY", "20");
    click("convert_AZW3");
    QTRY_VERIFY(control("conversionState")->isVisible());
    QTRY_COMPARE(text("conversionState"), QStringLiteral("Converting to AZW3 · 50%"));
    QVERIFY(text("keyHints").startsWith(QStringLiteral("Converting “L'Écume des jours” to AZW3")));
    QVERIFY(control("stopConversions")->isVisible());
    click("stopConversion");
    QTRY_VERIFY(!conversions->busy());
    qunsetenv("COUSOIR_FAKE_DELAY");
    QTRY_VERIFY(!control("conversionState")->isVisible());
    QCOMPARE(formatsOf(library()->property("currentBook").toMap()),
             (QStringList{ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }));
}

// Metadata from the web, with a stand-in for Calibre: the books found, a
// field left as it is, a cover picked, saved as one change that Revert
// undoes.
void LibraryUiTest::downloadsMetadata()
{
    auto *downloads = qobject_cast<MetadataDownloader *>(m_window->property("downloads").value<QObject *>());
    QVERIFY(downloads);
    downloads->setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-debug"));
    openWritable(2);
    click("downloadMetadataButton");
    QObject *dialog = object("metadataDownloadDialog");
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(downloads->state(), QStringLiteral("found"));
    QTRY_VERIFY(control("downloadField_tags"));
    QVERIFY(dialog->property("ticked").toMap().value(QStringLiteral("publisher")).toBool());
    click("downloadField_publisher");
    QVERIFY(!dialog->property("ticked").toMap().value(QStringLiteral("publisher")).toBool());
    click("downloadCoversButton");
    QTRY_VERIFY(!downloads->searchingCovers());
    QTRY_VERIFY(control("downloadCover1"));
    click("downloadCover1");
    QVERIFY(dialog->property("coverPicked").toBool());
    // Back to the fields, the cover picked still goes with them.
    click("downloadBack");
    QTRY_COMPARE(dialog->property("page").toString(), QStringLiteral("fields"));
    const QString coverBefore = library()->property("currentBook").toMap().value(QStringLiteral("cover")).toString();
    click("downloadSave");
    QTRY_VERIFY(!dialog->property("opened").toBool());
    waitForSave();
    const auto book = [this] { return library()->property("currentBook").toMap(); };
    QTRY_COMPARE(book().value(QStringLiteral("tags")).toStringList(),
                 (QStringList{ QStringLiteral("Dark fantasy"), QStringLiteral("fiction"), QStringLiteral("Fantasy") }));
    QCOMPARE(book().value(QStringLiteral("publisher")).toString(), QStringLiteral("Tor"));
    QTRY_COMPARE(text("keyHints"), QStringLiteral("“The Black Company” updated from the web; Revert changes undoes it."));
    QTRY_VERIFY(control("revertChangesButton")->isVisible());
    QCOMPARE(downloads->state(), QStringLiteral("idle"));
    QVERIFY(book().value(QStringLiteral("cover")).toString() != coverBefore);

    // Space ticks the field it is on; Enter on Cancel cancels.
    click("downloadMetadataButton");
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(downloads->state(), QStringLiteral("found"));
    QTRY_VERIFY(control("downloadField_title"));
    QVERIFY(!dialog->property("ticked").toMap().value(QStringLiteral("title")).toBool());
    control("downloadFields")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Space);
    QTRY_VERIFY(dialog->property("ticked").toMap().value(QStringLiteral("title")).toBool());
    control("downloadCancel")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(book().value(QStringLiteral("title")).toString(), QStringLiteral("The Black Company"));

    // Closed, the download stops.
    qputenv("COUSOIR_FAKE_DELAY", "20");
    click("downloadMetadataButton");
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(downloads->state(), QStringLiteral("searching"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(downloads->state(), QStringLiteral("idle"));
    qunsetenv("COUSOIR_FAKE_DELAY");
}

// A Kobo plugged in: a book sent to it from the book's Files, then the
// Kobo's page with the book, which opens the library's, the book removed
// after asking, and the Kobo ejected, which takes its page with it.
void LibraryUiTest::sendsToKobo()
{
    auto *devices = qobject_cast<DeviceLibrary *>(m_window->property("devices").value<QObject *>());
    QVERIFY(devices);
    QTemporaryDir kobo;
    QVERIFY(makeKobo(kobo.path()));
    const QString root = QDir::cleanPath(kobo.path());
    QTemporaryDir config;
    devices->setConfigDirectory(config.path());
    // Run as calibre-debug, the test tool is a fake sending job.
    devices->setProgram(QStringLiteral(COUSOIR_TEST_TOOL));
    devices->setEjector([](const Kobo::Device &, std::function<void(const QString &)> done) { done({}); });
    QList<Volume> volumes{ { kobo.path(), QStringLiteral("/dev/sdz") } };
    devices->setVolumeProvider([&volumes] { return volumes; }, 20);
    const QString entry = QStringLiteral("sidebar_device:") + root;
    QTRY_VERIFY(control(entry) && control(entry)->isVisible());
    QCOMPARE(control(entry)->property("text").toString(), QStringLiteral("Kobo eReader"));

    library()->setProperty("currentBookId", 1);
    QTRY_VERIFY(control("sendTo_0") && control("sendTo_0")->isVisible());
    QTRY_COMPARE(text("sendTo_0"), QStringLiteral("Kobo eReader"));
    QVERIFY(!control("onDevice")->isVisible());
    // Low in the pane, which may need scrolling to it.
    QMetaObject::invokeMethod(control("sendTo_0"), "clicked");
    QTRY_COMPARE(text("keyHints"), QStringLiteral("“L'Écume des jours” sent to Kobo eReader."));
    QTRY_VERIFY(control("onDevice")->isVisible());
    QCOMPARE(text("onDevice"), QStringLiteral("On Kobo eReader"));

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(control(entry)->isVisible());
    click(entry.toLatin1().constData());
    QTRY_VERIFY(control("devicePage")->isVisible());
    QCOMPARE(text("pageTitle"), QStringLiteral("Kobo eReader"));
    QTRY_VERIFY(control("deviceRow0"));
    QVERIFY(text("bookCount").startsWith(QStringLiteral("1 book · ")));
    QVERIFY(!control("searchField")->isVisible());
    control("deviceList")->forceActiveFocus();
    QTest::keyClick(m_window, Qt::Key_Return);
    QTRY_COMPARE(library()->property("currentBookId").toInt(), 1);
    QCOMPARE(text("rootCrumb"), QStringLiteral("Kobo eReader"));
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(control("devicePage")->isVisible());

    QTRY_VERIFY(control("deviceList")->hasActiveFocus());
    QTest::keyClick(m_window, Qt::Key_Delete);
    QTRY_VERIFY(object("removeFromDeviceDialog")->property("opened").toBool());
    QCOMPARE(object("removeFromDeviceDialog")->property("title").toString(),
             QStringLiteral("Remove “L'Écume des jours” from Kobo eReader?"));
    clickVisible("confirmAccept");
    QTRY_VERIFY(control("deviceEmpty") && control("deviceEmpty")->isVisible());
    QCOMPARE(text("deviceEmpty"), QStringLiteral("No books on Kobo eReader"));

    click("ejectButton");
    QTRY_COMPARE(text("keyHints"), QStringLiteral("Kobo eReader can be unplugged."));
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Books"));
    QVERIFY(!control(entry) || !control(entry)->isVisible());
    devices->setVolumeProvider([] { return QList<Volume>(); }, 2000);
}

// A Kobo that has read a book of the library: how far, and its highlights
// and notes, on the book's page and the Kobo's, copied as Markdown; as last
// seen once the Kobo is unplugged.
void LibraryUiTest::showsReading()
{
    auto *devices = qobject_cast<DeviceLibrary *>(m_window->property("devices").value<QObject *>());
    QVERIFY(devices);
    QTemporaryDir kobo;
    QVERIFY(makeKobo(kobo.path()));
    {
        SqliteDb db;
        QString error;
        QVERIFY2(db.open(QDir(kobo.path()).filePath(QStringLiteral(".kobo/KoboReader.sqlite")),
                         SqliteDb::Mode::ReadWrite, &error),
                 qPrintable(error));
        QVERIFY2(db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo-reading.sql"))), &error),
                 qPrintable(error));
    }
    QVERIFY(writeAll(QDir(kobo.path()).filePath(QStringLiteral("Vian, Boris/Ecume.kepub.epub")), "kepub"));
    QList<Volume> volumes{ { kobo.path(), QStringLiteral("/dev/sdz") } };
    devices->setVolumeProvider([&volumes] { return volumes; }, 20);
    const QString entry = QStringLiteral("sidebar_device:") + QDir::cleanPath(kobo.path());
    QTRY_VERIFY(control(entry) && control(entry)->isVisible());

    library()->setProperty("currentBookId", 1);
    QTRY_VERIFY(control("readingState") && control("readingState")->isVisible());
    QVERIFY2(text("readingState").startsWith(QStringLiteral("43 % read  ·  last read 2026-10-2")),
             qPrintable(text("readingState")));
    QVERIFY(text("readingState").endsWith(QStringLiteral("1 h 30 min  ·  6 notes")));
    QStringList annotations;
    for (QQuickItem *item : visualDescendants(m_window->contentItem(), QStringLiteral("noteAnnotation"))) {
        if (item->isVisible())
            annotations.append(item->property("text").toString());
    }
    annotations.sort();
    QCOMPARE(annotations, QStringList({ QStringLiteral("Le nénuphar, déjà ?"), QStringLiteral("Seulement une note") }));
    QMetaObject::invokeMethod(control("copyNotesButton"), "clicked");
    QTRY_COMPARE(text("keyHints"), QStringLiteral("Highlights and notes copied as Markdown"));
    QVERIFY2(QGuiApplication::clipboard()->text().startsWith(
                 QStringLiteral("# L'Écume des jours\n\nBoris Vian\n\n> perdu\n\n## Un\n\n> le pianocktail")),
             qPrintable(QGuiApplication::clipboard()->text()));

    // The Kobo's page: how far each book is read and how many notes it has.
    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(control("booksPage") && control("booksPage")->isVisible());
    QTRY_VERIFY(control(entry)->isVisible());
    click(entry.toLatin1().constData());
    QTRY_VERIFY(control("devicePage")->isVisible());
    QTRY_VERIFY(control("deviceRow0"));
    QVariantMap ecume;
    for (const QVariant &row : devices->rows()) {
        if (row.toMap().value(QStringLiteral("bookId")).toInt() == 1)
            ecume = row.toMap();
    }
    QCOMPARE(ecume.value(QStringLiteral("reading")).toString(), QStringLiteral("43 %"));
    QCOMPARE(ecume.value(QStringLiteral("notes")).toInt(), 6);

    // Unplugged, the book's page tells what the Kobo told when last seen.
    volumes.clear();
    QTRY_VERIFY(!control(entry) || !control(entry)->isVisible());
    library()->setProperty("currentBookId", 1);
    QTRY_VERIFY(control("bookPage") && control("bookPage")->isVisible());
    QTRY_VERIFY(control("readingState") && control("readingState")->isVisible());
    QVERIFY2(text("readingState").startsWith(QStringLiteral("Seen %1  ·  43 % read")
                                                 .arg(QDate::currentDate().toString(Qt::ISODate))),
             qPrintable(text("readingState")));
    devices->setVolumeProvider([] { return QList<Volume>(); }, 2000);
}

// A Kindle, here a folder standing for one: a book it reads sent from its
// Files, one it does not converted to AZW3 first, after asking, then sent;
// its page has no Eject.
void LibraryUiTest::sendsToKindle()
{
    auto *devices = qobject_cast<DeviceLibrary *>(m_window->property("devices").value<QObject *>());
    auto *conversions = qobject_cast<ConversionService *>(m_window->property("conversions").value<QObject *>());
    QVERIFY(devices && conversions);
    QTemporaryDir kindle;
    QVERIFY(QDir(kindle.path()).mkpath(QStringLiteral("documents")));
    QTemporaryDir config;
    devices->setConfigDirectory(config.path());
    devices->setProgram(QStringLiteral(COUSOIR_TEST_TOOL));
    conversions->setProgram(QStringLiteral(COUSOIR_TEST_DATA_DIR "/fake-calibre-debug"));
    QList<KindleSlot> kindles{ { 0, 0, 0x9981, kindle.path() } };
    devices->setKindleProvider([&kindles] { return kindles; });
    const QString entry = QStringLiteral("sidebar_device:mtpdir:") + QDir::cleanPath(kindle.path());
    QTRY_VERIFY(control(entry) && control(entry)->isVisible());
    QTRY_COMPARE(control(entry)->property("text").toString(), QStringLiteral("Kindle"));

    openWritable(2);
    QTRY_VERIFY(control("sendTo_0") && control("sendTo_0")->isVisible());
    QMetaObject::invokeMethod(control("sendTo_0"), "clicked");
    QTRY_COMPARE(text("keyHints"), QStringLiteral("“The Black Company” sent to Kindle."));
    QVERIFY(QFileInfo::exists(QDir(kindle.path()).filePath(QStringLiteral("documents/The Black Company - Glen Cook.azw3"))));

    // An EPUB only: converted first.
    library()->setProperty("currentBookId", 1);
    QTRY_VERIFY(control("sendTo_0") && control("sendTo_0")->isVisible() && control("sendTo_0")->isEnabled());
    QMetaObject::invokeMethod(control("sendTo_0"), "clicked");
    QTRY_VERIFY(object("convertAndSendDialog")->property("opened").toBool());
    QCOMPARE(object("convertAndSendDialog")->property("title").toString(),
             QStringLiteral("Convert “L'Écume des jours” to AZW3 for Kindle?"));
    clickVisible("confirmAccept");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(kindle.path()).filePath(QStringLiteral("documents/L'Écume des jours - Boris Vian.azw3"))),
                             15000);
    QTRY_COMPARE(text("keyHints"), QStringLiteral("“L'Écume des jours” sent to Kindle."));
    QCOMPARE(formatsOf(library()->property("currentBook").toMap()),
             (QStringList{ QStringLiteral("AZW3"), QStringLiteral("EPUB") }));

    QTest::keyClick(m_window, Qt::Key_Escape);
    QTRY_VERIFY(control(entry)->isVisible());
    click(entry.toLatin1().constData());
    QTRY_VERIFY(control("devicePage")->isVisible());
    QTRY_VERIFY(control("deviceRow1"));
    QVERIFY(!control("ejectButton")->isVisible());
    devices->setKindleProvider([] { return QList<KindleSlot>(); });
    QTRY_COMPARE(text("pageTitle"), QStringLiteral("Books"));
}

// A reader with a long name: the sidebar keeps its width, and the name is
// cut short there and on the Send to button, which fits in the details.
void LibraryUiTest::keepsLongNamesShort()
{
    auto *devices = qobject_cast<DeviceLibrary *>(m_window->property("devices").value<QObject *>());
    QVERIFY(devices);
    QTemporaryDir kindle;
    QVERIFY(QDir(kindle.path()).mkpath(QStringLiteral("documents")));
    const QString name = QStringLiteral("Kindle Paperwhite Signature Edition, the one on the shelf by the window");
    QFile driveInfo(QDir(kindle.path()).filePath(QStringLiteral("driveinfo.calibre")));
    QVERIFY(driveInfo.open(QIODevice::WriteOnly));
    driveInfo.write(QJsonDocument(QJsonObject{ { QStringLiteral("device_name"), name } }).toJson());
    driveInfo.close();
    QTemporaryDir config;
    devices->setConfigDirectory(config.path());
    QTRY_VERIFY(control("sidebar") && control("sidebar")->width() > 0);
    const qreal width = control("sidebar")->width();

    QList<KindleSlot> kindles{ { 0, 0, 0x9981, kindle.path() } };
    devices->setKindleProvider([&kindles] { return kindles; });
    const QString entry = QStringLiteral("sidebar_device:mtpdir:") + QDir::cleanPath(kindle.path());
    QTRY_VERIFY(control(entry) && control(entry)->isVisible());
    QTRY_COMPARE(control(entry)->property("text").toString(), name);
    QCOMPARE(control("sidebar")->width(), width);
    auto *label = control(entry)->findChild<QQuickItem *>(QStringLiteral("sidebarItemLabel"));
    QVERIFY(label);
    QTRY_VERIFY(label->property("truncated").toBool());

    openWritable(2);
    QTRY_VERIFY(control("sendTo_0") && control("sendTo_0")->isVisible());
    QTRY_VERIFY(text("sendTo_0").endsWith(QChar(0x2026)) && text("sendTo_0").size() > 1);
    QVERIFY(control("sendTo_0")->x() + control("sendTo_0")->width() <= control("sendTo_0")->parentItem()->width());
    QTest::keyClick(m_window, Qt::Key_Escape);
    devices->setKindleProvider([] { return QList<KindleSlot>(); });
    QTRY_VERIFY(!control(entry));
}

#include "library_ui_test.moc"
