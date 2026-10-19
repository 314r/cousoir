#include <QtTest>

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

private:
    bool loadUi();
    QQuickItem *control(const QString &name) const;
    QQuickItem *control(const char *name) const { return control(QString::fromLatin1(name)); }
    QObject *library() const;
    QObject *guard() const;
    QString text(const char *name) const;
    void typeText(const QString &text);
    void click(const char *name);

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
    qmlRegisterType<SystemAppearance>("Reliure", 1, 0, "SystemAppearance");
    for (const char *type : { "ActionMenu", "AppSession", "AppearanceSettingsPage", "BookDetailsPane", "BookPage",
                              "BooksPage", "ChoiceBox", "CoverGrid", "Divider", "FacetPage", "FlatButton", "FormField",
                              "HeadingLabel", "LibrarySettingsPage", "ListTable", "MetaRow", "SearchField",
                              "SettingsWindow", "Sidebar", "SidebarItem", "StatTile", "ThinScrollBar" }) {
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

// Clicks a control once the layouts placed it: an item that just appeared
// sits at 0, 0 until then, over whatever is there.
void LibraryUiTest::click(const char *name)
{
    QVERIFY(QQuickTest::qWaitForPolish(m_window));
    QQuickItem *item = control(name);
    QVERIFY2(item, name);
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, center(item));
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
    QTRY_COMPARE(control("bookCover")->childItems().first()->property("status").toInt(), 1);

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

#include "library_ui_test.moc"
