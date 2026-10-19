#include "calibre_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QStandardPaths>

namespace {

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QStringList strings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        result.append(item.toString());
    return result;
}

// Overrides a tweak when tweaks.json holds a value of the right kind.
void readTweaks(const QJsonObject &json, CalibreText::Tweaks *tweaks)
{
    const auto text = [&json](const char *key, QString *target) {
        const QJsonValue value = json.value(QLatin1String(key));
        if (value.isString())
            *target = value.toString();
        else if (value.isNull() && json.contains(QLatin1String(key)))
            target->clear();
    };
    const auto list = [&json](const char *key, QStringList *target) {
        const QJsonValue value = json.value(QLatin1String(key));
        if (value.isArray())
            *target = strings(value);
    };

    text("title_series_sorting", &tweaks->titleSeriesSorting);
    text("default_language_for_title_sort", &tweaks->defaultLanguageForTitleSort);
    text("author_sort_copy_method", &tweaks->authorSortCopyMethod);
    list("author_name_suffixes", &tweaks->authorNameSuffixes);
    list("author_name_prefixes", &tweaks->authorNamePrefixes);
    list("author_name_copywords", &tweaks->authorNameCopywords);
    list("author_surname_prefixes", &tweaks->authorSurnamePrefixes);
    const QJsonValue useSurnamePrefixes = json.value(QLatin1String("author_use_surname_prefixes"));
    if (useSurnamePrefixes.isBool())
        tweaks->authorUseSurnamePrefixes = useSurnamePrefixes.toBool();
    const QJsonValue increment = json.value(QLatin1String("series_index_auto_increment"));
    if (increment.isString()) {
        tweaks->seriesIndexAutoIncrement = increment.toString();
        tweaks->seriesIndexAutoIncrementNumber.reset();
    } else if (increment.isDouble()) {
        tweaks->seriesIndexAutoIncrement.clear();
        tweaks->seriesIndexAutoIncrementNumber = increment.toDouble();
    }
    const QJsonValue articles = json.value(QLatin1String("per_language_title_sort_articles"));
    if (articles.isObject()) {
        tweaks->perLanguageTitleSortArticles.clear();
        const QJsonObject languages = articles.toObject();
        for (auto it = languages.constBegin(); it != languages.constEnd(); ++it)
            tweaks->perLanguageTitleSortArticles.insert(it.key(), strings(it.value()));
    }
}

} // namespace

QString CalibreConfig::configDirectory()
{
    const QString overridden = qEnvironmentVariable("CALIBRE_CONFIG_DIRECTORY");
    if (!overridden.isEmpty())
        return QDir(overridden).absolutePath();
#if defined(Q_OS_WIN)
    return QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA")) + QStringLiteral("/calibre");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Preferences/calibre");
#else
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/calibre");
#endif
}

QString CalibreConfig::defaultLibraryPath()
{
    const QString overridePath = qEnvironmentVariable("RELIURE_LIBRARY_PATH");
    const QString path = !overridePath.isEmpty() ? overridePath : load().libraryPath;
    return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

CalibreConfig CalibreConfig::load(const QString &directory)
{
    CalibreConfig config;
    const QDir dir(directory);
    const QJsonObject global = readObject(dir.filePath(QStringLiteral("global.py.json")));
    config.libraryPath = global.value(QLatin1String("library_path")).toString();

    config.interfaceLanguage = qEnvironmentVariable("CALIBRE_OVERRIDE_LANG");
    if (config.interfaceLanguage.isEmpty())
        config.interfaceLanguage = global.value(QLatin1String("language")).toString();
    if (config.interfaceLanguage.isEmpty())
        config.interfaceLanguage = QLocale::system().name();

    if (global.value(QLatin1String("filename_pattern")).isString())
        config.filenamePattern = global.value(QLatin1String("filename_pattern")).toString();
    if (global.value(QLatin1String("read_file_metadata")).isBool())
        config.readFileMetadata = global.value(QLatin1String("read_file_metadata")).toBool();
    if (global.value(QLatin1String("swap_author_names")).isBool())
        config.swapAuthorNames = global.value(QLatin1String("swap_author_names")).toBool();
    config.newBookTags = strings(global.value(QLatin1String("new_book_tags")));

    readTweaks(readObject(dir.filePath(QStringLiteral("tweaks.json"))), &config.tweaks);
    return config;
}
