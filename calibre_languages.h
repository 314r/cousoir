#pragma once

#include <QHash>
#include <QString>

#include <memory>
#include <optional>

// Calibre's names of languages, as calibre_langcode_to_name() gives them:
// the English name of its ISO 639 table, through the translations of
// Calibre's interface language. Both are read from the Calibre installed,
// its resources folder; without it, the names are Qt's English ones.
namespace CalibreLanguages {

// Calibre's ISO 639 table, iso639.calibre_msgpack: English names by code,
// two letter codes' three letter ones, codes by lower case English name.
struct Iso639
{
    QHash<QString, QString> by3;
    QHash<QString, QString> twoToThree;
    QHash<QString, QString> threeToTwo;
    QHash<QString, QString> nameMap;
};

// The table of Calibre's resources, read once; none without Calibre.
std::shared_ptr<const Iso639> iso639();

// canonicalize_lang(): the code of a language given by its code, two or
// three letters, or English name; empty for none. Nothing without Calibre's
// table, which the caller then does without.
std::optional<QString> canonicalize(const QString &raw);

class Names
{
public:
    // The names in `interfaceLanguage`, from the resources in `resources`.
    Names(const QString &resources, const QString &interfaceLanguage);

    // The name of an ISO 639-2 code; the code itself for one unknown.
    QString name(const QString &code) const;
    // The code of a language by its name in the interface language, as
    // typed in lower case; empty for none (reverse lang_map()).
    QString codeOfName(const QString &name) const;
    // Whether Calibre's table was read.
    bool fromCalibre() const;

private:
    std::shared_ptr<const Iso639> m_iso639;
    // messages.mo, then iso639.mo and iso3166.mo: Calibre's translator and
    // its fallbacks.
    QList<QHash<QString, QString>> m_catalogs;
    QHash<QString, QString> m_codesByName;
};

// Calibre's resources folder: beside the real calibre-debug, or a
// distribution's; COUSOIR_CALIBRE_RESOURCES when set, none when empty.
QString resourcesFolder();
// For the tests: the resources folder to read, empty for none; a null text
// finds Calibre's again.
void setResourcesFolder(const QString &folder);

// The names for the interface language, read once and kept; safe from any
// thread.
std::shared_ptr<const Names> names(const QString &interfaceLanguage);

// The translations of gettext's .mo file: by message, the first form.
QHash<QString, QString> readMo(const QByteArray &data);

} // namespace CalibreLanguages
