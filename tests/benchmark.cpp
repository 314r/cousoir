// cousoir-benchmark: how long Cousoir takes with a library, measured the
// way a user feels it. The model alone first: loading, checking, searching
// letter by letter, sorting, browsing, opening a book and saving an edit;
// then, with --ui, the whole window, each step until its frame is drawn.
//
//   cousoir-test-tool make-library build/sandbox/libs/50k 50000
//   tools/sandbox.sh build/cousoir-benchmark [--ui] build/sandbox/libs/50k
//
// The edit is saved into the library: give it a copy.
//
// With --screenshot, it draws the window instead, in a section, with a
// search, a virtual library or a book open, and saves the picture; the
// library is only read.
//
//   tools/sandbox.sh build/cousoir-benchmark --screenshot shot.png --search tag:sf LIBRARY

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QtQuickTest/quicktest.h>

#include <memory>

#include "book_library.h"
#include "calibre_config.h"
#include "concurrency_guard.h"
#include "conversion_service.h"
#include "cover_cache.h"
#include "device_library.h"
#include "omarchy_theme.h"
#include "system_appearance.h"

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

void report(const QString &step, qint64 nanoseconds)
{
    out() << qSetFieldWidth(44) << Qt::left << step << qSetFieldWidth(10) << Qt::right
          << QString::number(double(nanoseconds) / 1e6, 'f', 1) << qSetFieldWidth(0) << " ms" << Qt::endl;
}

// Waits for `signal` up to a minute; false when it did not come.
template<typename Object, typename Signal>
bool waitFor(Object *object, Signal signal)
{
    QSignalSpy spy(object, signal);
    return spy.wait(60000);
}

int benchmarkModel(const QString &path)
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    QElapsedTimer timer;
    timer.start();
    BookLibrary library(path, config);
    if (!waitFor(&library, &BookLibrary::libraryChanged) || !library.loaded()) {
        out() << "cannot load " << path << ": " << library.lastError() << Qt::endl;
        return 1;
    }
    report(QStringLiteral("load (%1 books)").arg(library.scopeCount()), timer.nsecsElapsed());
    timer.restart();
    if (library.checking() && !waitFor(&library, &BookLibrary::checkingChanged))
        out() << "the check did not end" << Qt::endl;
    report(QStringLiteral("check (%1 findings)").arg(library.check().value(QStringLiteral("count")).toInt()),
           timer.nsecsElapsed());

    const QString typed = QStringLiteral("montagne mer");
    qint64 slowest = 0;
    timer.restart();
    for (qsizetype i = 1; i <= typed.size(); ++i) {
        QElapsedTimer key;
        key.start();
        library.setSearchText(typed.left(i));
        slowest = std::max(slowest, key.nsecsElapsed());
    }
    report(QStringLiteral("search, per letter (%1 found)").arg(library.books()->count()),
           timer.nsecsElapsed() / typed.size());
    report(QStringLiteral("search, slowest letter"), slowest);
    timer.restart();
    library.setSearchText({});
    report(QStringLiteral("search cleared"), timer.nsecsElapsed());

    for (const char *option : { "authors", "series", "added", "rating", "titleDesc", "title" }) {
        timer.restart();
        library.setSortOption(QString::fromLatin1(option));
        report(QStringLiteral("sort by %1").arg(QLatin1String(option)), timer.nsecsElapsed());
    }

    for (const char *section : { "authors", "tags", "series" }) {
        timer.restart();
        library.setSection(QString::fromLatin1(section));
        report(QStringLiteral("section %1 (%2)").arg(QLatin1String(section)).arg(library.facetCount()),
               timer.nsecsElapsed());
    }
    const int firstSeries = library.facetRows().value(0).toMap().value(QStringLiteral("id")).toInt();
    timer.restart();
    library.openFacetItem(firstSeries);
    report(QStringLiteral("open a series (%1 books)").arg(library.books()->count()), timer.nsecsElapsed());
    timer.restart();
    library.setSection(QStringLiteral("books"));
    report(QStringLiteral("back to the books"), timer.nsecsElapsed());

    const int bookId = library.books()->idAt(library.books()->count() / 2);
    timer.restart();
    library.setCurrentBookId(bookId);
    report(QStringLiteral("open a book"), timer.nsecsElapsed());

    library.setWritable(true);
    timer.restart();
    QSignalSpy saved(&library, &BookLibrary::saved);
    QSignalSpy failed(&library, &BookLibrary::saveFailed);
    const QString error = library.updateBook(bookId, { { QStringLiteral("tags"), QStringLiteral("Benchmark") } });
    if (!error.isEmpty()) {
        out() << "cannot edit: " << error << Qt::endl;
        return 1;
    }
    const qint64 queued = timer.nsecsElapsed();
    if (!saved.wait(60000)) {
        out() << "the edit was not saved: " << failed.value(0).value(0).toString() << Qt::endl;
        return 1;
    }
    report(QStringLiteral("edit queued"), queued);
    report(QStringLiteral("edit saved"), timer.nsecsElapsed());
    if (library.loading() && !waitFor(&library, &BookLibrary::loadingChanged))
        out() << "the reload did not end" << Qt::endl;
    report(QStringLiteral("edit saved and reloaded"), timer.nsecsElapsed());
    return 0;
}

QString themeStateRoot;

class IsolatedTheme : public OmarchyTheme
{
public:
    explicit IsolatedTheme(QObject *parent = nullptr) : OmarchyTheme(themeStateRoot, parent) {}
};

// Waits for the window to be polished and draws a frame.
void drawn(QQuickWindow *window)
{
    QQuickTest::qWaitForPolish(window);
    window->grabWindow();
}

void registerTypes()
{
    qmlRegisterType<IsolatedTheme>("Cousoir", 1, 0, "OmarchyTheme");
    qmlRegisterType<BookLibrary>("Cousoir", 1, 0, "BookLibrary");
    qmlRegisterType<ConcurrencyGuard>("Cousoir", 1, 0, "ConcurrencyGuard");
    qmlRegisterType<ConversionService>("Cousoir", 1, 0, "ConversionService");
    qmlRegisterType<DeviceLibrary>("Cousoir", 1, 0, "DeviceLibrary");
    qmlRegisterType<SystemAppearance>("Cousoir", 1, 0, "SystemAppearance");
    // The interface's own types, from the resources as the UI tests have them.
    const QDir qml(QStringLiteral(":/qt/qml/Cousoir"));
    for (const QString &file : qml.entryList({ QStringLiteral("*.qml") })) {
        const QString type = file.chopped(4);
        if (type == QLatin1String("Theme"))
            qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/Theme.qml")), "Cousoir", 1, 0, "Theme");
        else if (type != QLatin1String("Main"))
            qmlRegisterType(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/%1").arg(file)), "Cousoir", 1, 0,
                            qPrintable(type));
    }

}

// The window on the library at `path`, once the library is loaded; null
// when it cannot load.
QQuickWindow *openWindow(QQmlApplicationEngine &engine, const QString &path, QSize size)
{
    qputenv("COUSOIR_LIBRARY_PATH", QFile::encodeName(path));
    qputenv("CALIBRE_OVERRIDE_LANG", "fr");
    engine.addImageProvider(QStringLiteral("covers"), new CoverCache);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Cousoir/Main.qml")));
    auto *window = engine.rootObjects().isEmpty() ? nullptr
                                                  : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) {
        out() << "cannot load the interface" << Qt::endl;
        return nullptr;
    }
    window->resize(size);
    QObject *library = window->property("library").value<QObject *>();
    if (!QTest::qWaitFor([library] { return library->property("loaded").toBool(); }, 60000)) {
        out() << "the library did not load" << Qt::endl;
        return nullptr;
    }
    return window;
}

int benchmarkUi(const QString &path)
{
    QElapsedTimer timer;
    timer.start();
    QQmlApplicationEngine engine;
    QQuickWindow *window = openWindow(engine, path, QSize(1240, 820));
    if (!window)
        return 1;
    QObject *library = window->property("library").value<QObject *>();
    drawn(window);
    report(QStringLiteral("ui: first frame with the books"), timer.nsecsElapsed());

    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    drawn(window);
    const QString typed = QStringLiteral("montagne mer");
    qint64 slowest = 0;
    timer.restart();
    for (const QChar character : typed) {
        QElapsedTimer key;
        key.start();
        QTest::keyClick(window, character.toLatin1());
        drawn(window);
        slowest = std::max(slowest, key.nsecsElapsed());
    }
    report(QStringLiteral("ui: search, per letter"), timer.nsecsElapsed() / typed.size());
    report(QStringLiteral("ui: search, slowest letter"), slowest);
    timer.restart();
    QTest::keyClick(window, Qt::Key_Escape);
    drawn(window);
    report(QStringLiteral("ui: search cleared"), timer.nsecsElapsed());

    for (const char *option : { "authors", "addedDesc", "title" }) {
        timer.restart();
        library->setProperty("sortOption", QString::fromLatin1(option));
        drawn(window);
        report(QStringLiteral("ui: sort by %1").arg(QLatin1String(option)), timer.nsecsElapsed());
    }

    timer.restart();
    QTest::keyClick(window, Qt::Key_G, Qt::ControlModifier);
    drawn(window);
    report(QStringLiteral("ui: covers"), timer.nsecsElapsed());
    timer.restart();
    for (int i = 0; i < 20; ++i) {
        QTest::keyClick(window, Qt::Key_PageDown);
        drawn(window);
    }
    report(QStringLiteral("ui: covers, page down"), timer.nsecsElapsed() / 20);
    timer.restart();
    QTest::keyClick(window, Qt::Key_G, Qt::ControlModifier);
    drawn(window);
    report(QStringLiteral("ui: table"), timer.nsecsElapsed());
    timer.restart();
    for (int i = 0; i < 20; ++i) {
        QTest::keyClick(window, Qt::Key_PageDown);
        drawn(window);
    }
    report(QStringLiteral("ui: table, page down"), timer.nsecsElapsed() / 20);

    for (const char *section : { "authors", "series", "books" }) {
        timer.restart();
        window->setProperty("section", QString::fromLatin1(section));
        drawn(window);
        report(QStringLiteral("ui: section %1").arg(QLatin1String(section)), timer.nsecsElapsed());
    }

    timer.restart();
    QTest::keyClick(window, Qt::Key_Return);
    drawn(window);
    report(QStringLiteral("ui: open a book"), timer.nsecsElapsed());
    timer.restart();
    QTest::keyClick(window, Qt::Key_Escape);
    drawn(window);
    report(QStringLiteral("ui: close it"), timer.nsecsElapsed());
    return 0;
}

struct Shot
{
    QString file;
    QSize size{ 1240, 820 };
    QString theme;
    QString section;
    QString search;
    QString virtualLibrary;
    int book = 0;
};

int screenshot(const QString &path, const Shot &shot)
{
    if (!shot.theme.isEmpty())
        QSettings().setValue(QStringLiteral("Appearance/colorMode"), shot.theme);
    QQmlApplicationEngine engine;
    QQuickWindow *window = openWindow(engine, path, shot.size);
    if (!window)
        return 1;
    QObject *library = window->property("library").value<QObject *>();
    if (!shot.virtualLibrary.isEmpty())
        library->setProperty("virtualLibrary", shot.virtualLibrary);
    if (!shot.section.isEmpty())
        window->setProperty("section", shot.section);
    if (!shot.search.isEmpty())
        library->setProperty("searchText", shot.search);
    if (shot.book > 0)
        library->setProperty("currentBookId", shot.book);
    // The covers load on their own threads.
    QTest::qWait(300);
    drawn(window);
    QTest::qWait(300);
    if (!window->grabWindow().save(shot.file)) {
        out() << "cannot write " << shot.file << Qt::endl;
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("cousoir-benchmark"));
    QCoreApplication::setApplicationName(QStringLiteral("cousoir-benchmark"));
    QSettings().clear();

    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("library"), QStringLiteral("The library to measure; edited."));
    parser.addOption({ QStringLiteral("ui"), QStringLiteral("Measure the whole window too.") });
    parser.addOption({ QStringLiteral("screenshot"), QStringLiteral("Draw the window to a PNG file instead."),
                       QStringLiteral("file") });
    parser.addOption({ QStringLiteral("size"), QStringLiteral("The window's size, 1240x820."), QStringLiteral("WxH") });
    parser.addOption({ QStringLiteral("theme"), QStringLiteral("light or dark."), QStringLiteral("theme") });
    parser.addOption({ QStringLiteral("section"), QStringLiteral("The section shown."), QStringLiteral("key") });
    parser.addOption({ QStringLiteral("search"), QStringLiteral("The search typed."), QStringLiteral("text") });
    parser.addOption({ QStringLiteral("vl"), QStringLiteral("The virtual library."), QStringLiteral("name") });
    parser.addOption({ QStringLiteral("book"), QStringLiteral("The book open."), QStringLiteral("id") });
    parser.process(app);
    if (parser.positionalArguments().size() != 1)
        parser.showHelp(2);
    const QString path = QDir(parser.positionalArguments().constFirst()).absolutePath();

    QTemporaryDir state;
    themeStateRoot = state.path();
    registerTypes();
    if (parser.isSet(QStringLiteral("screenshot"))) {
        Shot shot;
        shot.file = parser.value(QStringLiteral("screenshot"));
        const QStringList size = parser.value(QStringLiteral("size")).split(u'x');
        if (size.size() == 2)
            shot.size = QSize(size.at(0).toInt(), size.at(1).toInt());
        shot.theme = parser.value(QStringLiteral("theme"));
        shot.section = parser.value(QStringLiteral("section"));
        shot.search = parser.value(QStringLiteral("search"));
        shot.virtualLibrary = parser.value(QStringLiteral("vl"));
        shot.book = parser.value(QStringLiteral("book")).toInt();
        return screenshot(path, shot);
    }
    int status = benchmarkModel(path);
    if (status == 0 && parser.isSet(QStringLiteral("ui")))
        status = benchmarkUi(path);
    // The most memory the process held, from the kernel's count.
    QFile memory(QStringLiteral("/proc/self/status"));
    if (memory.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : memory.readAll().split('\n')) {
            if (line.startsWith("VmHWM:"))
                out() << "peak memory: " << line.mid(6).trimmed() << Qt::endl;
        }
    }
    return status;
}
