#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QQuickWindow>

#include <cstdio>

#include "app_instance.h"
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
#include "portal_file_chooser.h"
#include "system_appearance.h"
#include "translator.h"

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
    // Omarchy can make its windows see-through (Appearance > Windows > Window
    // Opacity), which a window drawn without an alpha channel cannot be.
    // Before any window is made.
    if (OmarchyTheme::present())
        QQuickWindow::setDefaultAlphaBuffer(true);

    app.setOrganizationName(QStringLiteral("cousoir"));
    app.setApplicationName(QStringLiteral("cousoir"));
    app.setApplicationDisplayName(QStringLiteral("Cousoir"));
    app.setApplicationVersion(QStringLiteral(COUSOIR_VERSION));
    app.setOrganizationDomain(QStringLiteral("314r.github.io"));
    app.setDesktopFileName(QStringLiteral("io.github._314r.cousoir"));
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Cousoir/assets/icons/hicolor/cousoir-256.png")));

    // One Cousoir a session: a second launch hands what it was asked to the
    // first, whose window comes forward, and ends.
    AppInstance::Invocation invocation;
    QString error;
    if (!AppInstance::parse(app.arguments(), &invocation, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    AppInstance instance;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const AppInstance::Forward forwarded = AppInstance::forward(invocation);
        if (forwarded == AppInstance::Forward::Taken)
            return 0;
        if (forwarded == AppInstance::Forward::NotAnswering) {
            std::fprintf(stderr, "%s\n", qPrintable(QCoreApplication::translate(
                                     "main", "Cousoir is running but does not answer; close it and try again.")));
            return 1;
        }
        // Two launches at once: the one that could not listen hands over.
        if (instance.listen())
            break;
    }
    if (!invocation.library.isEmpty())
        qputenv("COUSOIR_LIBRARY_PATH", QFile::encodeName(invocation.library));

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
    qmlRegisterType<PortalFileChooser>("Cousoir", 1, 0, "PortalFileChooser");
    qmlRegisterType<SystemAppearance>("Cousoir", 1, 0, "SystemAppearance");

    // Installed before anything is translated; every qsTr() is read again
    // when Omarchy's language changes.
    Translator translator;
    app.installTranslator(&translator);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appInstance"), &instance);
    engine.rootContext()->setContextProperty(QStringLiteral("startupInvocation"), invocation.toMap());
    QObject::connect(&translator, &Translator::changed, &engine, [&engine] {
        engine.retranslate();
        // What the library worded or formatted is read again.
        for (QObject *window : engine.rootObjects())
            QMetaObject::invokeMethod(window, "relocalize");
    });
    engine.addImageProvider(QStringLiteral("covers"), new CoverCache);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("Cousoir", "Main");

    return app.exec();
}
