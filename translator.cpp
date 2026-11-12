// Following Omarchy's language as Omacut and Monologue do (MIT, David
// Heinemeier Hansson).
#include "translator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

QJsonObject readCatalog(const QString &directory, const QString &code)
{
    QFile catalog(directory + u'/' + code + QStringLiteral(".json"));
    if (!catalog.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(catalog.readAll()).object();
}

bool hasCatalog(const QString &directory, const QString &code)
{
    return QFileInfo::exists(directory + u'/' + code + QStringLiteral(".json"));
}

} // namespace

Translator::Translator(const QString &languageFile, const QString &catalogDirectory, QObject *parent)
    : QTranslator(parent)
    , m_languageFile(languageFile.isEmpty() ? QDir::homePath() + QStringLiteral("/.config/omarchy/language") : languageFile)
    , m_catalogDirectory(catalogDirectory)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Translator::reload);
    reload();
}

QString Translator::readLanguage(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString code = QString::fromUtf8(file.readLine()).trimmed();
    static const QRegularExpression valid(QStringLiteral("^[a-z]{2,3}(-[A-Za-z]{2})?$"));
    return valid.match(code).hasMatch() ? code : QString();
}

QString Translator::systemLanguage(const QStringList &uiLanguages, const QString &catalogDirectory)
{
    // "pt-BR", "pt-Latn-BR" or "pt": the regional catalog, then the
    // language's.
    for (const QString &name : uiLanguages) {
        const QStringList parts = name.split(u'-');
        const QString base = parts.first().toLower();
        if (base == QLatin1String("en") || base == QLatin1String("c"))
            return QStringLiteral("en");
        const QString region = parts.size() > 1 && parts.last().size() == 2 ? parts.last().toUpper() : QString();
        if (!region.isEmpty() && hasCatalog(catalogDirectory, base + u'-' + region))
            return base + u'-' + region;
        if (hasCatalog(catalogDirectory, base))
            return base;
    }
    return QStringLiteral("en");
}

// CLDR's cardinal plural categories for whole counts, for Omarchy's
// languages (the rules of Omarchy's shell/Commons/Plural.js).
QString Translator::pluralCategory(const QString &language, long long count)
{
    const long long n = count < 0 ? -count : count, n10 = n % 10, n100 = n % 100;
    const bool millions = n != 0 && n % 1000000 == 0;
    const bool teens = n100 >= 12 && n100 <= 14;
    auto in = [&](std::initializer_list<const char *> codes) {
        for (const char *code : codes) {
            if (language == QLatin1String(code))
                return true;
        }
        return false;
    };
    auto category = [](const char *name) { return QString::fromLatin1(name); };
    if (in({ "ja", "ko", "zh-CN", "zh-TW", "th", "vi", "id", "ms" }))
        return category("other");
    if (in({ "hi", "bn", "fa", "si" }))
        return category(n <= 1 ? "one" : "other");
    if (in({ "fr", "pt-BR" }))
        return category(n <= 1 ? "one" : millions ? "many" : "other");
    if (in({ "ca", "es-MX", "it", "pt" }))
        return category(n == 1 ? "one" : millions ? "many" : "other");
    if (language == QLatin1String("fil"))
        return category(n10 == 4 || n10 == 6 || n10 == 9 ? "other" : "one");
    if (language == QLatin1String("is"))
        return category(n10 == 1 && n100 != 11 ? "one" : "other");
    if (in({ "cs", "sk" }))
        return category(n == 1 ? "one" : n >= 2 && n <= 4 ? "few" : "other");
    if (language == QLatin1String("pl"))
        return category(n == 1 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many");
    if (in({ "ru", "uk" }))
        return category(n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "many");
    if (in({ "hr", "sr" }))
        return category(n10 == 1 && n100 != 11 ? "one" : n10 >= 2 && n10 <= 4 && !teens ? "few" : "other");
    if (language == QLatin1String("sl"))
        return category(n100 == 1 ? "one" : n100 == 2 ? "two" : n100 >= 3 && n100 <= 4 ? "few" : "other");
    if (language == QLatin1String("lt"))
        return category(n100 >= 11 && n100 <= 19 ? "other" : n10 == 1 ? "one" : n10 >= 2 ? "few" : "other");
    if (language == QLatin1String("lv"))
        return category(n10 == 0 || (n100 >= 11 && n100 <= 19) ? "zero" : n10 == 1 ? "one" : "other");
    if (language == QLatin1String("ro"))
        return category(n == 1 ? "one" : n == 0 || (n100 >= 1 && n100 <= 19) ? "few" : "other");
    if (language == QLatin1String("ar")) {
        if (n <= 2)
            return category(n == 0 ? "zero" : n == 1 ? "one" : "two");
        return category(n100 >= 3 && n100 <= 10 ? "few" : n100 >= 11 ? "many" : "other");
    }
    if (language == QLatin1String("he"))
        return category(n == 1 ? "one" : n == 2 ? "two" : "other");
    if (language == QLatin1String("ga"))
        return category(n == 1 ? "one" : n == 2 ? "two" : n >= 3 && n <= 6 ? "few" : n >= 7 && n <= 10 ? "many" : "other");
    return category(n == 1 ? "one" : "other");
}

// The locale Omarchy formats this language with, from its registry, so
// Cousoir's numbers and dates match the desktop's (Arabic there keeps Western
// digits); without the registry, Qt's own locale for the code.
QLocale Translator::formatLocale(const QString &language)
{
    QFile registry(qEnvironmentVariable("OMARCHY_PATH") + QStringLiteral("/default/i18n/locales.json"));
    if (registry.open(QIODevice::ReadOnly)) {
        const QString name = QJsonDocument::fromJson(registry.readAll()).object()
                                 .value(language).toObject().value(QStringLiteral("formatLocale")).toString();
        if (!name.isEmpty())
            return QLocale(name);
    }
    return QLocale(language);
}

QString Translator::translate(const char *, const char *sourceText, const char *, int n) const
{
    // A null string tells Qt there is no translation: it shows the English.
    const QJsonValue value = m_strings.value(QString::fromUtf8(sourceText));
    if (value.isString())
        return value.toString();
    if (value.isObject()) {
        const QJsonObject forms = value.toObject();
        const QString form = forms.value(n < 0 ? QStringLiteral("other") : pluralCategory(m_language, n)).toString();
        return form.isEmpty() ? forms.value(QStringLiteral("other")).toString() : form;
    }
    return {};
}

void Translator::reload()
{
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);

    // omarchy-language replaces the file, which drops it from the watch, and
    // a first pick creates it: watch its folder too, and watch again after
    // every change.
    const QString directory = QFileInfo(m_languageFile).absolutePath();
    const bool omarchy = QFileInfo::exists(directory);
    if (omarchy)
        m_watcher.addPath(directory);
    if (QFileInfo::exists(m_languageFile))
        m_watcher.addPath(m_languageFile);

    // On Omarchy, its language, English until one is picked; elsewhere the
    // system's.
    const QString chosen = readLanguage(m_languageFile);
    const bool picked = !chosen.isEmpty();
    const QString language = picked ? chosen
        : omarchy ? QStringLiteral("en")
        : systemLanguage(QLocale::system().uiLanguages(), m_catalogDirectory);
    // The catalogs are compiled in: only another language needs a reload.
    if (m_loaded && language == m_language && picked == m_picked)
        return;

    // English first (it has the plural forms of counted strings), then the
    // language and its regional catalog over it (pt, then pt-BR), so a
    // missing string falls back toward English a step at a time.
    QHash<QString, QJsonValue> strings;
    QStringList codes{ QStringLiteral("en"), language.section(u'-', 0, 0), language };
    codes.removeDuplicates();
    bool translated = false;
    for (const QString &code : std::as_const(codes)) {
        const QJsonObject catalog = readCatalog(m_catalogDirectory, code);
        translated = translated || (code != QLatin1String("en") && !catalog.isEmpty());
        for (auto it = catalog.begin(); it != catalog.end(); ++it) {
            if ((it.value().isString() && !it.value().toString().isEmpty()) || it.value().isObject())
                strings.insert(it.key(), it.value());
        }
    }

    m_loaded = true;
    m_language = language;
    m_picked = picked;
    m_strings = strings;
    // Dates, numbers and Qt's own names of days and months follow a language
    // picked in Omarchy; without one they keep the system's formats. A window
    // is mirrored for a right-to-left language only once Cousoir speaks it:
    // English reads left to right.
    const QLocale locale = picked ? formatLocale(language) : QLocale::system();
    QLocale::setDefault(locale);
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        QGuiApplication::setLayoutDirection(translated ? locale.textDirection() : Qt::LeftToRight);
    emit changed();
}
