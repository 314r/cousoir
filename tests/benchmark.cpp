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
// search, a virtual library, a book open or a folder standing for a Kobo
// plugged in, and saves the picture; the library is only read.
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
#include "metadata_downloader.h"
#include "cover_cache.h"
#include "device_library.h"
#include "highlight_browser.h"
#include "highlight_document.h"
#include "highlight_sync.h"
#ifdef COUSOIR_READER
#include "book_reader.h"
#endif
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

    // The command palette, letter by letter.
    slowest = 0;
    int hits = 0;
    for (qsizetype i = 1; i <= typed.size(); ++i) {
        QElapsedTimer key;
        key.start();
        hits = int(library.paletteSearch(typed.left(i), {}).size());
        slowest = std::max(slowest, key.nsecsElapsed());
    }
    report(QStringLiteral("palette, slowest letter (%1 shown)").arg(hits), slowest);

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
    qmlRegisterType<MetadataDownloader>("Cousoir", 1, 0, "MetadataDownloader");
    qmlRegisterType<DeviceLibrary>("Cousoir", 1, 0, "DeviceLibrary");
    qmlRegisterType<HighlightBrowser>("Cousoir", 1, 0, "HighlightBrowser");
    qmlRegisterType<HighlightDocument>("Cousoir", 1, 0, "HighlightDocument");
    qmlRegisterType<HighlightSync>("Cousoir", 1, 0, "HighlightSync");
#ifdef COUSOIR_READER
    qmlRegisterType<BookReader>("Cousoir", 1, 0, "BookReader");
#endif
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
    // The item of the section whose books show.
    int item = 0;
    QString search;
    QString virtualLibrary;
    int book = 0;
    int selected = 0;
    QStringList clicks;
    // Milliseconds waited after each click, for what it starts.
    int pause = 0;
    // A folder that stands for a Kobo plugged in.
    QString kobo;
    // Keys pressed, then text typed, after the clicks.
    QStringList keys;
    QString typed;
    // The book read in Cousoir's reader, drawn instead, the keys pressed
    // there; the reader's size, and its settings ("Reader/" keys).
    bool read = false;
    QSize readerSize;
    QStringList settings;
    // Controls of the reader clicked, by name, after the keys.
    QStringList readerClicks;
};

QQuickItem *visualDescendant(QQuickItem *item, const QString &name)
{
    for (QQuickItem *child : item->childItems()) {
        if (child->objectName() == name)
            return child;
        if (QQuickItem *found = visualDescendant(child, name))
            return found;
    }
    return nullptr;
}

int screenshot(const QString &path, const Shot &shot)
{
    if (!shot.theme.isEmpty())
        QSettings().setValue(QStringLiteral("Appearance/colorMode"), shot.theme);
    for (const QString &setting : shot.settings) {
        const qsizetype equals = setting.indexOf(u'=');
        if (equals > 0)
            QSettings().setValue(QStringLiteral("Reader/") + setting.left(equals), setting.mid(equals + 1));
    }
    QQmlApplicationEngine engine;
    QQuickWindow *window = openWindow(engine, path, shot.size);
    if (!window)
        return 1;
    QObject *library = window->property("library").value<QObject *>();
    if (!shot.kobo.isEmpty()) {
        auto *devices = window->property("devices").value<DeviceLibrary *>();
        const QString kobo = QDir(shot.kobo).absolutePath();
        devices->setVolumeProvider([kobo] { return QList<Volume>{ { kobo, QString() } }; }, 100);
        devices->refresh();
        if (!QTest::qWaitFor([devices] { return !devices->devices().isEmpty() && !devices->loading(); }, 10000))
            out() << "the Kobo was not read" << Qt::endl;
    }
    if (!shot.virtualLibrary.isEmpty())
        library->setProperty("virtualLibrary", shot.virtualLibrary);
    if (!shot.section.isEmpty())
        window->setProperty("section", QString(shot.section).replace(QStringLiteral("@KOBO@"),
                                                                     QDir(shot.kobo).absolutePath()));
    if (shot.item > 0)
        QMetaObject::invokeMethod(library, "openFacetItem", Q_ARG(int, shot.item));
    if (!shot.search.isEmpty())
        library->setProperty("searchText", shot.search);
    if (shot.book > 0)
        library->setProperty("currentBookId", shot.book);
    // A few rows selected, for the pane that edits them together.
    if (shot.selected > 1) {
        QObject *books = library->property("books").value<QObject *>();
        for (int row = 0; row < shot.selected; ++row)
            QMetaObject::invokeMethod(books, "select", Q_ARG(int, row), Q_ARG(QString, row ? QStringLiteral("toggle")
                                                                                          : QStringLiteral("only")));
    }
    // Controls clicked, by name, in order.
    for (const QString &name : shot.clicks) {
        drawn(window);
        QQuickItem *item = window->findChild<QQuickItem *>(name);
        // A view's delegates are its items', not its children.
        if (!item)
            item = visualDescendant(window->contentItem(), name);
        if (!item) {
            out() << "no control " << name << Qt::endl;
            return 1;
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        if (shot.pause > 0)
            QTest::qWait(shot.pause);
    }
    if (shot.read) {
        QMetaObject::invokeMethod(window, "readBook");
        QQuickWindow *reader = nullptr;
        (void)QTest::qWaitFor([&] {
            for (QObject *root : engine.rootObjects()) {
                if ((reader = root->findChild<QQuickWindow *>(QStringLiteral("readerWindow")))
                    || (reader = root->findChild<QQuickWindow *>(QStringLiteral("pdfReaderWindow"))))
                    return true;
            }
            return false;
        }, 5000);
        if (!reader) {
            out() << "no reader" << Qt::endl;
            return 1;
        }
        window = reader;
        if (shot.readerSize.isValid())
            reader->resize(shot.readerSize);
        // The page laid out and its position told.
        QTest::qWait(2500);
    }
    for (const QString &key : shot.keys) {
        drawn(window);
        QTest::keySequence(window, QKeySequence(key));
        if (shot.read)
            QTest::qWait(500);
    }
    for (const QString &name : shot.readerClicks) {
        drawn(window);
        QQuickItem *item = window->findChild<QQuickItem *>(name);
        if (!item)
            item = visualDescendant(window->contentItem(), name);
        // A popup's items are the overlay's.
        if (!item && window->contentItem()->parentItem())
            item = visualDescendant(window->contentItem()->parentItem(), name);
        if (!item) {
            out() << "no control " << name << Qt::endl;
            return 1;
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QTest::qWait(500);
    }
    if (!shot.typed.isEmpty()) {
        drawn(window);
        for (const QChar character : shot.typed)
            QTest::keyClick(window, character.toLatin1());
    }
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
#ifdef COUSOIR_READER
    BookReader::initialize();
#endif
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
    parser.addOption({ QStringLiteral("item"), QStringLiteral("The section's item whose books show."), QStringLiteral("id") });
    parser.addOption({ QStringLiteral("search"), QStringLiteral("The search typed."), QStringLiteral("text") });
    parser.addOption({ QStringLiteral("vl"), QStringLiteral("The virtual library."), QStringLiteral("name") });
    parser.addOption({ QStringLiteral("book"), QStringLiteral("The book open."), QStringLiteral("id") });
    parser.addOption({ QStringLiteral("select"), QStringLiteral("How many of the first books to select."),
                       QStringLiteral("count") });
    parser.addOption({ QStringLiteral("click"), QStringLiteral("A control to click, by its objectName."),
                       QStringLiteral("name") });
    parser.addOption({ QStringLiteral("pause"), QStringLiteral("Milliseconds to wait after each click."),
                       QStringLiteral("ms") });
    parser.addOption({ QStringLiteral("kobo"), QStringLiteral("A folder standing for a Kobo plugged in."),
                       QStringLiteral("folder") });
    parser.addOption({ QStringLiteral("key"), QStringLiteral("Keys to press, Ctrl+K, after the clicks."),
                       QStringLiteral("sequence") });
    parser.addOption({ QStringLiteral("type"), QStringLiteral("Text to type, after the keys."), QStringLiteral("text") });
    parser.addOption({ QStringLiteral("read"), QStringLiteral("Read the book in Cousoir's reader, drawn instead.") });
    parser.addOption({ QStringLiteral("reader-size"), QStringLiteral("The reader's size."), QStringLiteral("WxH") });
    parser.addOption({ QStringLiteral("set"), QStringLiteral("A reader setting, key=value."), QStringLiteral("setting") });
    parser.addOption({ QStringLiteral("reader-click"), QStringLiteral("A control of the reader to click, after the keys."),
                       QStringLiteral("name") });
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
        shot.item = parser.value(QStringLiteral("item")).toInt();
        shot.search = parser.value(QStringLiteral("search"));
        shot.virtualLibrary = parser.value(QStringLiteral("vl"));
        shot.book = parser.value(QStringLiteral("book")).toInt();
        shot.selected = parser.value(QStringLiteral("select")).toInt();
        shot.clicks = parser.values(QStringLiteral("click"));
        shot.pause = parser.value(QStringLiteral("pause")).toInt();
        shot.kobo = parser.value(QStringLiteral("kobo"));
        shot.keys = parser.values(QStringLiteral("key"));
        shot.typed = parser.value(QStringLiteral("type"));
        shot.read = parser.isSet(QStringLiteral("read"));
        const QStringList readerSize = parser.value(QStringLiteral("reader-size")).split(u'x');
        if (readerSize.size() == 2)
            shot.readerSize = QSize(readerSize.at(0).toInt(), readerSize.at(1).toInt());
        shot.settings = parser.values(QStringLiteral("set"));
        shot.readerClicks = parser.values(QStringLiteral("reader-click"));
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
