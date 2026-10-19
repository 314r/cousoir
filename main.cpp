#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>

#include "book_library.h"
#include "concurrency_guard.h"
#include "cover_cache.h"
#include "omarchy_theme.h"
#include "system_appearance.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    app.setOrganizationName(QStringLiteral("reliure"));
    app.setApplicationName(QStringLiteral("reliure"));
    app.setApplicationDisplayName(QStringLiteral("Reliure"));
    app.setApplicationVersion(QStringLiteral(RELIURE_VERSION));
    app.setOrganizationDomain(QStringLiteral("314r.github.io"));
    app.setDesktopFileName(QStringLiteral("io.github._314r.reliure"));
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Reliure/assets/icons/hicolor/reliure-256.png")));

    qmlRegisterType<BookLibrary>("Reliure", 1, 0, "BookLibrary");
    qmlRegisterType<ConcurrencyGuard>("Reliure", 1, 0, "ConcurrencyGuard");
    qmlRegisterType<OmarchyTheme>("Reliure", 1, 0, "OmarchyTheme");
    qmlRegisterType<SystemAppearance>("Reliure", 1, 0, "SystemAppearance");

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverCache);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("Reliure", "Main");

    return app.exec();
}
