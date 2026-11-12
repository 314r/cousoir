#pragma once

#include <QCoreApplication>
#include <QFile>
#include <QString>

#include "translator.h"

// Cousoir's own words and formats in a test, whatever the computer's
// language: its catalogs with a language of the test's, English by default,
// written to a file in `folder` as Omarchy writes its own. English too needs
// it, for the plural forms of counted strings.
inline Translator *installTranslator(const QString &folder, const QString &language = QStringLiteral("en"))
{
    const QString path = folder + QStringLiteral("/language");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(language.toUtf8() + '\n');
    file.close();
    auto *translator = new Translator(path, QStringLiteral(":/i18n"), QCoreApplication::instance());
    QCoreApplication::installTranslator(translator);
    return translator;
}
