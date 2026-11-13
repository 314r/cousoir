#include <QtTest>

#include <QGuiApplication>
#include <QTemporaryDir>

#include <optional>
#include <thread>

#include "app_instance.h"
#include "omarchy_theme.h"
#include "portal_file_chooser.h"
#include "test_support.h"
#include "translator.h"

namespace {
// Whether the test runs on a session bus of its own.
bool ownBus = false;
}

#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>

// Stands in for the desktop's portal on a bus of the test's own: keeps what
// was asked and answers with the files it is told to.
class FakePortal : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.FileChooser")

public:
    QString method;
    QString title;
    QVariantMap options;
    // The filters asked for: "name: pattern pattern".
    QStringList filters;
    QStringList answer;
    uint response = 0;

public slots:
    QDBusObjectPath OpenFile(const QString &parent, const QString &title, const QVariantMap &options,
                             const QDBusMessage &message)
    {
        return ask(QStringLiteral("OpenFile"), parent, title, options, message);
    }

    QDBusObjectPath SaveFile(const QString &parent, const QString &title, const QVariantMap &options,
                             const QDBusMessage &message)
    {
        return ask(QStringLiteral("SaveFile"), parent, title, options, message);
    }

private:
    QDBusObjectPath ask(const QString &name, const QString &, const QString &asked, const QVariantMap &given,
                        const QDBusMessage &message)
    {
        method = name;
        title = asked;
        options = given;
        filters.clear();
        const QDBusArgument argument = given.value(QStringLiteral("filters")).value<QDBusArgument>();
        if (given.contains(QStringLiteral("filters"))) {
            argument.beginArray();
            while (!argument.atEnd()) {
                QString filter;
                QStringList patterns;
                argument.beginStructure();
                argument >> filter;
                argument.beginArray();
                while (!argument.atEnd()) {
                    uint type = 0;
                    QString pattern;
                    argument.beginStructure();
                    argument >> type >> pattern;
                    argument.endStructure();
                    patterns.append(pattern);
                }
                argument.endArray();
                argument.endStructure();
                filters.append(filter + QStringLiteral(": ") + patterns.join(u' '));
            }
            argument.endArray();
        }
        // The answer comes on the request's path, made of the asker's name
        // and its token, as the portal makes it.
        QString sender = message.service().mid(1);
        sender.replace(u'.', u'_');
        const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                 .arg(sender, given.value(QStringLiteral("handle_token")).toString());
        QTimer::singleShot(0, this, [this, path] {
            QDBusMessage signal = QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.portal.Request"),
                                                             QStringLiteral("Response"));
            signal << response << QVariantMap{ { QStringLiteral("uris"), answer } };
            QDBusConnection::sessionBus().send(signal);
        });
        return QDBusObjectPath(path);
    }
};
#endif

// What Cousoir follows of Omarchy besides its colours: the language picked
// under Setup > Language, Hyprland's window rounding, and the desktop's file
// chooser.
class OmarchyTest : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();
    void readsQtFilters();
    void readsTheCommandLine();
    void handsALaunchToTheFirst();
    void asksThePortalForFiles();
    void speaksEnglishUntilALanguageIsPicked();
    void followsTheLanguageFile();
    void tellsMeaningsApart();
    void followsALanguageChange();
    void picksPluralForms();
    void followsTheSystemAwayFromOmarchy();
    void mirrorsOnlyALanguageItSpeaks();
    void shipsCatalogsThatFitTheirSources();
    void followsHyprlandRounding();
    void followsTheWindowOpacity();
    void keepsSquareCornersWithoutHyprland();
};

void OmarchyTest::cleanup()
{
    QLocale::setDefault(QLocale::system());
    QGuiApplication::setLayoutDirection(Qt::LeftToRight);
}

void OmarchyTest::readsTheCommandLine()
{
    AppInstance::Invocation invocation;
    QString error;
    QVERIFY(AppInstance::parse({ QStringLiteral("cousoir"), QStringLiteral("a.epub"),
                                 QStringLiteral("file:///books/Mood%20Indigo.epub") },
                               &invocation, &error));
    QCOMPARE(invocation.files, (QStringList{ QDir::current().absoluteFilePath(QStringLiteral("a.epub")),
                                            QStringLiteral("/books/Mood Indigo.epub") }));
    QVERIFY(!invocation.addAsked);

    QVERIFY(AppInstance::parse({ QStringLiteral("cousoir"), QStringLiteral("--add") }, &invocation, &error));
    QVERIFY(invocation.addAsked);
    QVERIFY(invocation.add.isEmpty());
    QVERIFY(AppInstance::parse({ QStringLiteral("cousoir"), QStringLiteral("--library"), QStringLiteral("/lib/"),
                                 QStringLiteral("--show"), QStringLiteral("12") },
                               &invocation, &error));
    QCOMPARE(invocation.library, QStringLiteral("/lib"));
    QCOMPARE(invocation.show, 12);
    QVERIFY(!AppInstance::parse({ QStringLiteral("cousoir"), QStringLiteral("--colour") }, &invocation, &error));
    QVERIFY(!error.isEmpty());

    const AppInstance::Invocation decoded = AppInstance::Invocation::decode(invocation.encode());
    QCOMPARE(decoded.toMap(), invocation.toMap());
}

// A second launch hands its command line to the first, which takes it.
void OmarchyTest::handsALaunchToTheFirst()
{
    const QString name = QStringLiteral("cousoir-test-%1").arg(QCoreApplication::applicationPid());
    AppInstance::Invocation invocation;
    invocation.files = { QStringLiteral("/books/a.epub") };
    QCOMPARE(AppInstance::forward(invocation, name), AppInstance::Forward::NoInstance);

    AppInstance first;
    QVERIFY(first.listen(name));
    AppInstance second;
    QVERIFY(!second.listen(name));
    QSignalSpy invoked(&first, &AppInstance::invoked);
    // The second waits for the first's answer, which its event loop gives.
    std::optional<AppInstance::Forward> forwarded;
    std::thread launch([&] { forwarded = AppInstance::forward(invocation, name); });
    QTRY_COMPARE(invoked.count(), 1);
    launch.join();
    QCOMPARE(forwarded, AppInstance::Forward::Taken);
    QCOMPARE(invoked.first().first().toMap().value(QStringLiteral("files")).toStringList(),
             QStringList{ QStringLiteral("/books/a.epub") });
}

void OmarchyTest::readsQtFilters()
{
    const PortalFileChooser::Filter books = PortalFileChooser::parseFilter(QStringLiteral("Books (*.epub *.kepub  *.azw3)"));
    QCOMPARE(books.name, QStringLiteral("Books"));
    QCOMPARE(books.patterns, (QStringList{ QStringLiteral("*.epub"), QStringLiteral("*.kepub"), QStringLiteral("*.azw3") }));
    const PortalFileChooser::Filter kobo = PortalFileChooser::parseFilter(QStringLiteral("Kobo database (KoboReader.sqlite)"));
    QCOMPARE(kobo.patterns, QStringList{ QStringLiteral("KoboReader.sqlite") });
    QCOMPARE(PortalFileChooser::parseFilter(QStringLiteral("All files (*)")).patterns, QStringList{ QStringLiteral("*") });
    QCOMPARE(PortalFileChooser::parseFilter(QStringLiteral("Anything")).patterns, QStringList{ QStringLiteral("*") });
}

// The portal asked as the desktop's would be, on a bus of the test's own so
// that no chooser shows on the desktop.
void OmarchyTest::asksThePortalForFiles()
{
#ifdef Q_OS_LINUX
    if (!ownBus)
        QSKIP("dbus-daemon is not installed");
    FakePortal portal;
    QVERIFY(QDBusConnection::sessionBus().isConnected());
    QVERIFY(QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &portal,
                                                         QDBusConnection::ExportAllSlots));
    QVERIFY(QDBusConnection::sessionBus().registerService(QStringLiteral("org.freedesktop.portal.Desktop")));

    PortalFileChooser chooser;
    QVERIFY(chooser.available());
    QSignalSpy accepted(&chooser, &PortalFileChooser::accepted);
    QSignalSpy rejected(&chooser, &PortalFileChooser::rejected);
    QSignalSpy failed(&chooser, &PortalFileChooser::failed);

    // Several books, from a folder, by Qt's filters.
    portal.answer = { QStringLiteral("file:///books/Mood%20Indigo.epub"), QStringLiteral("file:///books/b.pdf") };
    chooser.open(PortalFileChooser::OpenFiles, QStringLiteral("Add books"),
                 { QStringLiteral("Books (*.epub *.pdf)"), QStringLiteral("All files (*)") },
                 QUrl::fromLocalFile(QStringLiteral("/books")));
    QTRY_COMPARE(accepted.count(), 1);
    QCOMPARE(portal.method, QStringLiteral("OpenFile"));
    QCOMPARE(portal.title, QStringLiteral("Add books"));
    QCOMPARE(portal.options.value(QStringLiteral("multiple")).toBool(), true);
    QVERIFY(!portal.options.contains(QStringLiteral("directory")));
    QCOMPARE(portal.options.value(QStringLiteral("current_folder")).toByteArray(), QByteArray("/books\0", 7));
    QCOMPARE(portal.filters, (QStringList{ QStringLiteral("Books: *.epub *.pdf"), QStringLiteral("All files: *") }));
    const QList<QUrl> files = accepted.first().first().value<QList<QUrl>>();
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.first().toLocalFile(), QStringLiteral("/books/Mood Indigo.epub"));

    // A folder, a library's.
    portal.answer = { QStringLiteral("file:///home/reader/Calibre%20Library") };
    chooser.open(PortalFileChooser::OpenFolder, QStringLiteral("Choose a Calibre library"));
    QTRY_COMPARE(accepted.count(), 2);
    QCOMPARE(portal.options.value(QStringLiteral("directory")).toBool(), true);
    QVERIFY(portal.filters.isEmpty());

    // A file to save, under a name proposed.
    portal.answer = { QStringLiteral("file:///notes/Mood%20Indigo.md") };
    chooser.open(PortalFileChooser::SaveFile, QStringLiteral("Save"), { QStringLiteral("Markdown (*.md)") },
                 QUrl::fromLocalFile(QStringLiteral("/notes")), QStringLiteral("Mood Indigo.md"));
    QTRY_COMPARE(accepted.count(), 3);
    QCOMPARE(portal.method, QStringLiteral("SaveFile"));
    QCOMPARE(portal.options.value(QStringLiteral("current_name")).toString(), QStringLiteral("Mood Indigo.md"));

    // The chooser closed without a file: nothing picked, nothing wrong.
    portal.response = 1;
    portal.answer.clear();
    chooser.open(PortalFileChooser::OpenFile, QStringLiteral("Choose a cover"));
    QTRY_COMPARE(rejected.count(), 1);
    QCOMPARE(accepted.count(), 3);
    QCOMPARE(failed.count(), 0);
#else
    QVERIFY(!PortalFileChooser().available());
#endif
}

void OmarchyTest::speaksEnglishUntilALanguageIsPicked()
{
    QTemporaryDir dir;
    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.language(), QStringLiteral("en"));
    QVERIFY(translator.translate("Main", "Save").isNull());
    // No language picked: the system's formats stay.
    QCOMPARE(QLocale(), QLocale::system());
}

void OmarchyTest::followsTheLanguageFile()
{
    QTemporaryDir dir;
    QVERIFY(writeAll(dir.filePath(QStringLiteral("language")), "pt-BR\n"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("pt.json")), R"({"Save": "Guardar", "Open": "Abrir"})"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("pt-BR.json")), R"({"Save": "Salvar", "Quit": ""})"));

    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.language(), QStringLiteral("pt-BR"));
    // The regional catalog, then the language's, then English.
    QCOMPARE(translator.translate("Main", "Save"), QStringLiteral("Salvar"));
    QCOMPARE(translator.translate("Main", "Open"), QStringLiteral("Abrir"));
    QVERIFY(translator.translate("Main", "Quit").isNull());
    QCOMPARE(QLocale().language(), QLocale::Portuguese);
}

// A word of two meanings has a key for each, the word alone serving the
// meanings a catalog does not tell apart.
void OmarchyTest::tellsMeaningsApart()
{
    QTemporaryDir dir;
    QVERIFY(writeAll(dir.filePath(QStringLiteral("language")), "fr\n"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("fr.json")),
                     R"({"Read": "Lire", "Read|column": "Lu", "Series": "Séries", "%n books|status": {"one": "%n livre", "other": "%n livres"}})"));
    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.translate("Main", "Read"), QStringLiteral("Lire"));
    QCOMPARE(translator.translate("DevicePage", "Read", "column"), QStringLiteral("Lu"));
    QCOMPARE(translator.translate("BookDetailsPane", "Series", "field"), QStringLiteral("Séries"));
    QCOMPARE(translator.translate("Main", "%n books", "status", 2), QStringLiteral("%n livres"));
    QVERIFY(translator.translate("Main", "Quit", "menu").isNull());
}

void OmarchyTest::followsALanguageChange()
{
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("language"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("fr.json")), R"({"Save": "Enregistrer"})"));

    Translator translator(file, dir.path());
    QSignalSpy changed(&translator, &Translator::changed);
    QVERIFY(translator.translate("Main", "Save").isNull());
    // A first pick creates the file; omarchy-language then replaces it.
    QVERIFY(writeAll(file, "fr\n"));
    QTRY_COMPARE(translator.language(), QStringLiteral("fr"));
    QCOMPARE(translator.translate("Main", "Save"), QStringLiteral("Enregistrer"));
    QCOMPARE(QLocale().language(), QLocale::French);
    QVERIFY(QFile::remove(file));
    QVERIFY(writeAll(file, "en\n"));
    QTRY_COMPARE(translator.language(), QStringLiteral("en"));
    QVERIFY(translator.translate("Main", "Save").isNull());
    QCOMPARE(changed.count(), 2);
}

void OmarchyTest::picksPluralForms()
{
    QTemporaryDir dir;
    QVERIFY(writeAll(dir.filePath(QStringLiteral("language")), "ru\n"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("en.json")), R"({"%n books": {"one": "%n book", "other": "%n books"}})"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("ru.json")),
                     R"({"%n books": {"one": "%n книга", "few": "%n книги", "many": "%n книг", "other": "%n книги"}})"));

    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 21), QStringLiteral("%n книга"));
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 22), QStringLiteral("%n книги"));
    QCOMPARE(translator.translate("Main", "%n books", nullptr, 25), QStringLiteral("%n книг"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("ar"), 11), QStringLiteral("many"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("ja"), 1), QStringLiteral("other"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("fr"), 0), QStringLiteral("one"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("en"), 0), QStringLiteral("other"));

    // English has its own forms, which Qt fills the count into.
    QVERIFY(writeAll(dir.filePath(QStringLiteral("language")), "en\n"));
    QTRY_COMPARE(translator.language(), QStringLiteral("en"));
    QCoreApplication::installTranslator(&translator);
    QCOMPARE(QCoreApplication::translate("Main", "%n books", nullptr, 1), QStringLiteral("1 book"));
    QCOMPARE(QCoreApplication::translate("Main", "%n books", nullptr, 3), QStringLiteral("3 books"));
    QCoreApplication::removeTranslator(&translator);
}

void OmarchyTest::followsTheSystemAwayFromOmarchy()
{
    QTemporaryDir dir;
    QVERIFY(writeAll(dir.filePath(QStringLiteral("fr.json")), R"({"Save": "Enregistrer"})"));
    QVERIFY(writeAll(dir.filePath(QStringLiteral("pt-BR.json")), R"({"Save": "Salvar"})"));
    QCOMPARE(Translator::systemLanguage({ QStringLiteral("fr-FR"), QStringLiteral("en-US") }, dir.path()), QStringLiteral("fr"));
    QCOMPARE(Translator::systemLanguage({ QStringLiteral("pt-Latn-BR") }, dir.path()), QStringLiteral("pt-BR"));
    // A language Cousoir has no words for leaves the next one its turn, and
    // English is never translated.
    QCOMPARE(Translator::systemLanguage({ QStringLiteral("de-DE"), QStringLiteral("fr") }, dir.path()), QStringLiteral("fr"));
    QCOMPARE(Translator::systemLanguage({ QStringLiteral("en-GB"), QStringLiteral("fr") }, dir.path()), QStringLiteral("en"));
    QCOMPARE(Translator::systemLanguage({ QStringLiteral("de-DE") }, dir.path()), QStringLiteral("en"));
}

void OmarchyTest::mirrorsOnlyALanguageItSpeaks()
{
    QTemporaryDir dir;
    QVERIFY(writeAll(dir.filePath(QStringLiteral("language")), "he\n"));
    {
        // Hebrew picked, but Cousoir's words are English: left to right.
        Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
        QCOMPARE(translator.language(), QStringLiteral("he"));
        QCOMPARE(QGuiApplication::layoutDirection(), Qt::LeftToRight);
    }
    QVERIFY(writeAll(dir.filePath(QStringLiteral("he.json")), R"({"Save": "שמירה"})"));
    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(QGuiApplication::layoutDirection(), Qt::RightToLeft);
}

// Every catalog Cousoir ships: a translation keeps the placeholders of its
// English, and a counted string has the form every count falls back on.
void OmarchyTest::shipsCatalogsThatFitTheirSources()
{
    static const QRegularExpression placeholder(QStringLiteral("%(L?\\d+|n)"));
    auto placeholders = [](const QString &text) {
        QStringList found;
        auto it = placeholder.globalMatch(text);
        while (it.hasNext())
            found.append(it.next().captured(0));
        found.sort();
        return found;
    };
    const QStringList catalogs = QDir(QStringLiteral(":/i18n")).entryList({ QStringLiteral("*.json") });
    QVERIFY(catalogs.contains(QStringLiteral("en.json")));
    for (const QString &name : catalogs) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(readAll(QStringLiteral(":/i18n/") + name), &error);
        QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(name + u": " + error.errorString()));
        const QJsonObject catalog = document.object();
        for (auto it = catalog.begin(); it != catalog.end(); ++it) {
            const QByteArray where = (name + u": " + it.key()).toUtf8();
            if (it.value().isObject()) {
                const QJsonObject forms = it.value().toObject();
                QVERIFY2(it.key().contains(QLatin1String("%n")), where.constData());
                QVERIFY2(!forms.value(QLatin1String("other")).toString().isEmpty(), where.constData());
                // A form may leave the count out ("The other book"), never
                // another placeholder.
                QStringList wanted = placeholders(it.key());
                wanted.removeAll(QStringLiteral("%n"));
                for (auto form = forms.begin(); form != forms.end(); ++form) {
                    QStringList has = placeholders(form.value().toString());
                    has.removeAll(QStringLiteral("%n"));
                    QVERIFY2(has == wanted, where.constData());
                }
            } else {
                QVERIFY2(it.value().isString(), where.constData());
                QVERIFY2(!it.key().contains(QLatin1String("%n")), where.constData());
                QVERIFY2(placeholders(it.value().toString()) == placeholders(it.key()), where.constData());
            }
        }
    }
}

void OmarchyTest::followsHyprlandRounding()
{
    QTemporaryDir dir;
    const QString response = dir.filePath(QStringLiteral("response.json"));
    const QString command = dir.filePath(QStringLiteral("hyprctl"));
    QVERIFY(writeAll(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
    QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(writeAll(response, R"({"option": "decoration:rounding", "int": 6, "set": true})"));

    OmarchyTheme theme(dir.path(), nullptr, command);
    QTRY_COMPARE(theme.rounding(), 6);
    // Rounded again, or squared, while Cousoir runs.
    QSignalSpy changed(&theme, &OmarchyTheme::roundingChanged);
    QVERIFY(writeAll(response, R"({"option": "decoration:rounding", "int": 0, "set": true})"));
    QTRY_COMPARE_WITH_TIMEOUT(theme.rounding(), 0, 5000);
    QCOMPARE(changed.count(), 1);
    // An answer that is no rounding keeps the last one.
    QVERIFY(writeAll(response, "no such option"));
    QTest::qWait(1500);
    QCOMPARE(theme.rounding(), 0);
}

// As omarchy-hyprland-window-transparency leaves it: a toggle file beside
// the current theme's folder, none for the default.
void OmarchyTest::followsTheWindowOpacity()
{
    QTemporaryDir dir;
    const QString current = dir.filePath(QStringLiteral("omarchy/current"));
    const QString toggles = dir.filePath(QStringLiteral("omarchy/toggles/hypr"));
    QVERIFY(QDir().mkpath(current + QStringLiteral("/theme")));
    OmarchyTheme theme(current);
    QCOMPARE(theme.windowOpacity(), QStringLiteral("transparent"));

    QSignalSpy changed(&theme, &OmarchyTheme::windowOpacityChanged);
    QVERIFY(writeAll(toggles + QStringLiteral("/window-glassy.lua"), "-- glassy\n"));
    theme.reload();
    QCOMPARE(theme.windowOpacity(), QStringLiteral("glassy"));
    QVERIFY(QFile::remove(toggles + QStringLiteral("/window-glassy.lua")));
    QVERIFY(writeAll(toggles + QStringLiteral("/window-opaque.lua"), "-- opaque\n"));
    theme.reload();
    QCOMPARE(theme.windowOpacity(), QStringLiteral("opaque"));
    theme.reload();
    QCOMPARE(changed.count(), 2);
}

void OmarchyTest::keepsSquareCornersWithoutHyprland()
{
    QTemporaryDir dir;
    OmarchyTheme theme(dir.path());
    QTest::qWait(100);
    QCOMPARE(theme.rounding(), 0);
}

// A session bus of the test's own, started before the application: Qt's
// platform takes the session bus as it starts, and the desktop's real
// portal must neither be asked nor replaced.
int main(int argc, char **argv)
{
    QProcess bus;
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    if (!daemon.isEmpty()) {
        bus.start(daemon, { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address") });
        if (bus.waitForStarted() && bus.waitForReadyRead() && bus.canReadLine()) {
            qputenv("DBUS_SESSION_BUS_ADDRESS", bus.readLine().trimmed());
            ownBus = true;
        }
    }
    int status = 0;
    {
        QGuiApplication app(argc, argv);
        OmarchyTest test;
        status = QTest::qExec(&test, argc, argv);
    }
    if (bus.state() != QProcess::NotRunning) {
        bus.kill();
        bus.waitForFinished();
    }
    return status;
}
#include "omarchy_test.moc"
