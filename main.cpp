#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlExtensionPlugin>

#include "book_library.h"
#include "concurrency_guard.h"
#include "conversion_service.h"
#include "metadata_downloader.h"
#include "device_library.h"
#include "highlight_browser.h"
#include "highlight_document.h"
#include "highlight_sync.h"
#include "cover_cache.h"
#include "omarchy_theme.h"
#include "system_appearance.h"

#ifdef COUSOIR_READER
#include "book_reader.h"
#endif

// The interface's QML module, a static library's.
Q_IMPORT_QML_PLUGIN(CousoirPlugin)

int main(int argc, char *argv[])
{
#ifdef COUSOIR_READER
    // Before the application: the books' addresses, and WebEngine.
    BookReader::initialize();
#endif
    QGuiApplication app(argc, argv);

    app.setOrganizationName(QStringLiteral("cousoir"));
    app.setApplicationName(QStringLiteral("cousoir"));
    app.setApplicationDisplayName(QStringLiteral("Cousoir"));
    app.setApplicationVersion(QStringLiteral(COUSOIR_VERSION));
    app.setOrganizationDomain(QStringLiteral("314r.github.io"));
    app.setDesktopFileName(QStringLiteral("io.github._314r.cousoir"));
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Cousoir/assets/icons/hicolor/cousoir-256.png")));

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
    qmlRegisterType<OmarchyTheme>("Cousoir", 1, 0, "OmarchyTheme");
    qmlRegisterType<SystemAppearance>("Cousoir", 1, 0, "SystemAppearance");

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverCache);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("Cousoir", "Main");

    return app.exec();
}
