#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

// The interface's language: the one picked under Omarchy's Setup > Language,
// followed live, as Omarchy's own apps follow it (the Translator of Omacut
// and Monologue, MIT, David Heinemeier Hansson). Away from Omarchy, the
// system's language.
//
// Catalogs are flat JSON objects from the English text to its translation,
// one per Omarchy language code (i18n/fr.json, i18n/pt-BR.json), so QML keeps
// plain qsTr("Save") and C++ plain tr("Save"), and a string without a
// translation shows in English. A counted string, qsTr("%n books", "", count),
// maps to an object of plural forms keyed by CLDR category
// ({"one": "%n livre", "other": "%n livres"}); English has its own in
// i18n/en.json.
class Translator : public QTranslator
{
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = QStringLiteral(":/i18n"),
                        QObject *parent = nullptr);

    QString language() const { return m_language; }
    bool isEmpty() const override { return m_strings.isEmpty(); }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    // The code on the file's first line; empty without a file or a code.
    static QString readLanguage(const QString &path);
    // The first of the system's languages Cousoir has a catalog for, "en"
    // without one.
    static QString systemLanguage(const QStringList &uiLanguages, const QString &catalogDirectory);
    static QString pluralCategory(const QString &language, long long count);
    static QLocale formatLocale(const QString &language);

signals:
    void changed();

private:
    void reload();

    QString m_languageFile;
    QString m_catalogDirectory;
    QString m_language;
    // Whether a language was picked in Omarchy, whose formats then apply.
    bool m_picked = false;
    bool m_loaded = false;
    QHash<QString, QJsonValue> m_strings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
