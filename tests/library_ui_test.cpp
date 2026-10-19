#include <QtTest>

#include <QClipboard>
#include <QDesktopServices>
#include <QDirIterator>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

#include <memory>

#include "book_library.h"
#include "concurrency_guard.h"
#include "conversion_service.h"
#include "device_library.h"
#include "sqlite_db.h"
#include "cover_cache.h"
#include "omarchy_theme.h"
#include "system_appearance.h"

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

class UrlRecorder : public QObject
{
    Q_OBJECT

public:
    QList<QUrl> urls;

public slots:
    void open(const QUrl &url) { urls.append(url); }
};
// A folder made to look like a mounted Kobo: its version file and an empty
// database with the tables Calibre uses.
bool makeKobo(const QString &root)
{
    QDir().mkpath(QDir(root).filePath(QStringLiteral(".kobo")));
    QFile version(QDir(root).filePath(QStringLiteral(".kobo/version")));
    QFile schema(QStringLiteral(RELIURE_TEST_DATA_DIR "/kobo.sql"));
    QFile database(QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite")));
    if (!version.open(QIODevice::WriteOnly) || !schema.open(QIODevice::ReadOnly) || !database.open(QIODevice::WriteOnly))
        return false;
    version.write("N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390");
    database.close();
    SqliteDb db;
    QString error;
    return db.open(database.fileName(), SqliteDb::Mode::ReadWrite, &error) && db.exec(QString::fromUtf8(schema.readAll()), &error);
}

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
    void opensInReader();
    void showsWriteState();
    void picksLibraryInSettings();
    void missingLibraryShowsError();
    void editsDetailsInPlace();
    void revertsBookChanges();
    void refusesInvalidValues();
    void changesRating();
    void changesCovers();
    void movesBooksToTrashAndBack();
    void addsAndRemovesFiles();
    void asksAboutDuplicates();
    void convertsBooks();
    void sendsToKobo();
    void sendsToKindle();
    void keepsLongNamesShort();

private:
    bool loadUi();
    QQuickItem *control(const QString &name) const;
    QQuickItem *control(const char *name) const { return control(QString::fromLatin1(name)); }
    // A popup or another object that is no item.
    QObject *object(const char *name) const { return m_window->findChild<QObject *>(QString::fromLatin1(name)); }
    QObject *library() const;
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
    qmlRegisterType<IsolatedTheme>("Reliure", 1, 0, "OmarchyTheme");
    qmlRegisterType<BookLibrary>("Reliure", 1, 0, "BookLibrary");
    qmlRegisterType<ConcurrencyGuard>("Reliure", 1, 0, "ConcurrencyGuard");
    qmlRegisterType<ConversionService>("Reliure", 1, 0, "ConversionService");
    qmlRegisterType<DeviceLibrary>("Reliure", 1, 0, "DeviceLibrary");
    qmlRegisterType<SystemAppearance>("Reliure", 1, 0, "SystemAppearance");
    for (const char *type : { "ActionMenu", "AppSession", "AppearanceSettingsPage", "BookDetailsPane", "BookPage",
                              "BooksPage", "ChoiceBox", "ConfirmDialog", "CoverGrid", "Divider", "FacetPage", "FlatButton", "FormField",
                              "HeadingLabel", "LibrarySettingsPage", "ListTable", "MetaRow", "SearchField",
                              "SettingsWindow", "Sidebar", "SidebarItem", "StatTile", "ThinScrollBar", "TrashPage" }) {
        qmlRegisterType(QUrl(QStringLiteral("qrc:/qt/qml/Reliure/%1.qml").arg(QLatin1String(type))),
                        "Reliure", 1, 0, type);
    }
    qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/qt/qml/Reliure/Theme.qml")), "Reliure", 1, 0, "Theme");
}

void LibraryUiTest::init()
{
    QSettings().clear();
    m_libraryDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_libraryDir->isValid());
    m_libraryPath = m_libraryDir->filePath(QStringLiteral("library"));
    QVERIFY(copyTree(QStringLiteral(RELIURE_TEST_DATA_DIR "/library"), m_libraryPath));
    qputenv("RELIURE_LIBRARY_PATH", QFile::encodeName(m_libraryPath));
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
    m_engine->load(QUrl(QStringLiteral("qrc:/qt/qml/Reliure/Main.qml")));
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
    qunsetenv("RELIURE_LIBRARY_PATH");
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

    // A header click sorts, a second one reverses.
    QQuickItem *added = control("bookAddedColumn");
    QVERIFY(added);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(added));
    QTRY_COMPARE(library()->property("sortOption").toString(), QStringLiteral("added"));
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(added));
    QTRY_COMPARE(library()->property("sortOption").toString(), QStringLiteral("addedDesc"));
    QCOMPARE(library()->property("rows").toList().first().toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Le Petit Prince"));
    // The order is remembered.
    QTRY_COMPARE(QSettings().value(QStringLiteral("Layout/sortOption")).toString(), QStringLiteral("addedDesc"));
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
    QCOMPARE(m_window->title(), QStringLiteral("L'Écume des jours — Reliure"));
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
    // A copy just made changed a moment ago: read-only, metadata as text.
    QVERIFY(!control("bookFields")->isVisible());
    QVERIFY(!control("coverButton")->isVisible());

    // Ctrl+I hides the pane; its facts move under the title.
    QVERIFY(!control("factsLine")->isVisible());
    QTest::keyClick(m_window, Qt::Key_I, Qt::ControlModifier);
    QTRY_VERIFY(!pane->isVisible());
    QVERIFY(control("factsLine")->isVisible());
    QCOMPARE(text("factsLine"), QStringLiteral("Mar 1947  ·  French  ·  EPUB"));
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
    QCOMPARE(m_window->title(), QStringLiteral("Glen Cook — Reliure"));
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
    // The copied library just changed and is in no Dropbox folder.
    QTRY_COMPARE(guard()->property("state").toString(), QStringLiteral("recent"));
    QQuickItem *state = control("writeState");
    QVERIFY(state && state->isVisible());
    QVERIFY(state->property("text").toString().startsWith(QStringLiteral("Read-only: library changed")));

    // Calibre's journal takes over, and goes.
    QFile journal(QDir(m_libraryPath).filePath(QStringLiteral("metadata.db-journal")));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    guard()->metaObject()->invokeMethod(guard(), "refresh");
    QTRY_COMPARE(state->property("text").toString(), QStringLiteral("Read-only: Calibre is writing to the library"));

    // A click shows why in the settings.
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(state));
    QQuickWindow *settings = m_window->findChild<QQuickWindow *>(QStringLiteral("settingsWindow"));
    QVERIFY(settings);
    QTRY_VERIFY(settings->isVisible());
    QCOMPARE(settings->property("currentSectionIndex").toInt(), 1);
    settings->close();
}

void LibraryUiTest::picksLibraryInSettings()
{
    QTemporaryDir other;
    QVERIFY(copyTree(QStringLiteral(RELIURE_TEST_DATA_DIR "/library"), other.path()));
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

void LibraryUiTest::missingLibraryShowsError()
{
    m_engine.reset();
    QTemporaryDir empty;
    qputenv("RELIURE_LIBRARY_PATH", QFile::encodeName(empty.path()));
    QVERIFY(loadUi());
    QTRY_COMPARE_WITH_TIMEOUT(text("emptyTitle"), QStringLiteral("Cannot read the library"), 10000);
    QVERIFY(text("emptyMessage").contains(QStringLiteral("no Calibre library")));
    QVERIFY(control("reloadButton")->isVisible());
    QVERIFY(control("chooseLibraryButton")->isVisible());
    QCOMPARE(text("bookCount"), QString());
    QVERIFY(!control("writeState")->isVisible());
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
    const QVariantList rows = library()->property("rows").toList();
    QVERIFY(std::any_of(rows.cbegin(), rows.cend(), [](const QVariant &row) {
        return row.toMap().value(QStringLiteral("title")).toString() == QLatin1String("Mood Indigo");
    }));
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

void LibraryUiTest::changesCovers()
{
    openWritable(5);
    QVERIFY(!library()->property("currentBook").toMap().value(QStringLiteral("hasCover")).toBool());
    QCOMPARE(QMetaObject::invokeMethod(library(), "setCover", Q_ARG(int, 5),
                                       Q_ARG(QString, QStringLiteral(RELIURE_TEST_DATA_DIR "/edits/transparent.png"))),
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
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid())
        return 1;
    themeStateRoot = settingsDir.filePath(QStringLiteral("omarchy"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    app.setOrganizationName(QStringLiteral("reliure-tests"));
    app.setApplicationName(QStringLiteral("library-ui"));
    LibraryUiTest test;
    return QTest::qExec(&test, argc, argv);
}


namespace {

QStringList formatsOf(const QVariantMap &book)
{
    QStringList formats;
    for (const QVariant &format : book.value(QStringLiteral("formats")).toList())
        formats.append(format.toMap().value(QStringLiteral("format")).toString());
    return formats;
}

} // namespace

void LibraryUiTest::movesBooksToTrashAndBack()
{
    makeWritable();
    // Del on the list asks before moving the current book.
    QMetaObject::invokeMethod(m_window, "focusCurrentList");
    QTRY_VERIFY(m_window->activeFocusItem());
    const QVariantList rows = library()->property("rows").toList();
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
    QTRY_COMPARE(library()->property("rows").toList().size(), rows.size() - 1);

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
    const QUrl mobi = QUrl::fromLocalFile(QStringLiteral(RELIURE_TEST_DATA_DIR "/import/Vicomte.mobi"));
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

// The window's conversions, with a stand-in for Calibre: from the Files
// section, asking before replacing a file, and stopped from there.
void LibraryUiTest::convertsBooks()
{
    auto *conversions = qobject_cast<ConversionService *>(m_window->property("conversions").value<QObject *>());
    QVERIFY(conversions);
    conversions->setProgram(QStringLiteral(RELIURE_TEST_DATA_DIR "/fake-calibre-debug"));
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

    qputenv("RELIURE_FAKE_DELAY", "20");
    click("convert_AZW3");
    QTRY_VERIFY(control("conversionState")->isVisible());
    QTRY_COMPARE(text("conversionState"), QStringLiteral("Converting to AZW3 · 50%"));
    QVERIFY(text("keyHints").startsWith(QStringLiteral("Converting “L'Écume des jours” to AZW3")));
    QVERIFY(control("stopConversions")->isVisible());
    click("stopConversion");
    QTRY_VERIFY(!conversions->busy());
    qunsetenv("RELIURE_FAKE_DELAY");
    QTRY_VERIFY(!control("conversionState")->isVisible());
    QCOMPARE(formatsOf(library()->property("currentBook").toMap()),
             (QStringList{ QStringLiteral("EPUB"), QStringLiteral("KEPUB") }));
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
    devices->setProgram(QStringLiteral(RELIURE_TEST_DATA_DIR "/fake-calibre-send"));
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
    devices->setProgram(QStringLiteral(RELIURE_TEST_DATA_DIR "/fake-calibre-send"));
    conversions->setProgram(QStringLiteral(RELIURE_TEST_DATA_DIR "/fake-calibre-debug"));
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
