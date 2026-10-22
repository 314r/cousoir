// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/utils/localization.py: calibre_langcode_to_name(),
//   translator_for_lang() and its fallbacks, get_lc_messages_path(),
//   locale_fallbacks(), available_translations()
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include "calibre_languages.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QStandardPaths>
#include <QVariant>

#include <cstring>
#include <optional>

#include <zip.h>

namespace CalibreLanguages {

namespace {

// msgpack, as Calibre writes its tables: maps, arrays, texts, numbers;
// what else comes is skipped.
class MsgpackReader
{
public:
    explicit MsgpackReader(const QByteArray &data) : m_data(data) {}

    std::optional<QVariant> read()
    {
        if (m_at >= m_data.size())
            return std::nullopt;
        const quint8 type = quint8(m_data.at(m_at++));
        if (type <= 0x7f)
            return QVariant(qint64(type));
        if (type >= 0xe0)
            return QVariant(qint64(qint8(type)));
        if ((type & 0xf0) == 0x80)
            return map(type & 0x0f);
        if ((type & 0xf0) == 0x90)
            return array(type & 0x0f);
        if ((type & 0xe0) == 0xa0)
            return text(type & 0x1f);
        switch (type) {
        case 0xc0:
            return QVariant();
        case 0xc2:
            return QVariant(false);
        case 0xc3:
            return QVariant(true);
        case 0xc4:
        case 0xc5:
        case 0xc6: {
            const std::optional<quint64> size = number(type == 0xc4 ? 1 : type == 0xc5 ? 2 : 4);
            return size && skip(*size) ? std::optional(QVariant()) : std::nullopt;
        }
        case 0xc7:
        case 0xc8:
        case 0xc9: {
            const std::optional<quint64> size = number(type == 0xc7 ? 1 : type == 0xc8 ? 2 : 4);
            return size && skip(*size + 1) ? std::optional(QVariant()) : std::nullopt;
        }
        case 0xca: {
            const std::optional<quint64> bits = number(4);
            if (!bits)
                return std::nullopt;
            const quint32 value = quint32(*bits);
            float result;
            std::memcpy(&result, &value, sizeof result);
            return QVariant(double(result));
        }
        case 0xcb: {
            const std::optional<quint64> bits = number(8);
            if (!bits)
                return std::nullopt;
            double result;
            std::memcpy(&result, &*bits, sizeof result);
            return QVariant(result);
        }
        case 0xcc:
        case 0xcd:
        case 0xce:
        case 0xcf: {
            const std::optional<quint64> value = number(1 << (type - 0xcc));
            return value ? std::optional(QVariant(qint64(*value))) : std::nullopt;
        }
        case 0xd0:
        case 0xd1:
        case 0xd2:
        case 0xd3: {
            const int size = 1 << (type - 0xd0);
            const std::optional<quint64> value = number(size);
            if (!value)
                return std::nullopt;
            const qint64 shift = 64 - 8 * size;
            return QVariant(qint64(*value << shift) >> shift);
        }
        case 0xd4:
        case 0xd5:
        case 0xd6:
        case 0xd7:
        case 0xd8:
            return skip(1 + (1 << (type - 0xd4))) ? std::optional(QVariant()) : std::nullopt;
        case 0xd9:
        case 0xda:
        case 0xdb: {
            const std::optional<quint64> size = number(type == 0xd9 ? 1 : type == 0xda ? 2 : 4);
            return size ? text(*size) : std::nullopt;
        }
        case 0xdc:
        case 0xdd: {
            const std::optional<quint64> size = number(type == 0xdc ? 2 : 4);
            return size ? array(*size) : std::nullopt;
        }
        case 0xde:
        case 0xdf: {
            const std::optional<quint64> size = number(type == 0xde ? 2 : 4);
            return size ? map(*size) : std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

private:
    // A big-endian unsigned number of `size` bytes.
    std::optional<quint64> number(int size)
    {
        if (m_at + size > m_data.size())
            return std::nullopt;
        quint64 value = 0;
        for (int i = 0; i < size; ++i)
            value = (value << 8) | quint8(m_data.at(m_at++));
        return value;
    }

    bool skip(quint64 size)
    {
        if (m_at + qint64(size) > m_data.size())
            return false;
        m_at += qint64(size);
        return true;
    }

    std::optional<QVariant> text(quint64 size)
    {
        if (m_at + qint64(size) > m_data.size())
            return std::nullopt;
        const QString value = QString::fromUtf8(m_data.constData() + m_at, qsizetype(size));
        m_at += qint64(size);
        return QVariant(value);
    }

    std::optional<QVariant> array(quint64 size)
    {
        QVariantList values;
        for (quint64 i = 0; i < size; ++i) {
            const std::optional<QVariant> value = read();
            if (!value)
                return std::nullopt;
            values.append(*value);
        }
        return QVariant(values);
    }

    // A map; keys that are no text are kept as their text.
    std::optional<QVariant> map(quint64 size)
    {
        QVariantMap values;
        for (quint64 i = 0; i < size; ++i) {
            const std::optional<QVariant> key = read();
            const std::optional<QVariant> value = key ? read() : std::nullopt;
            if (!value)
                return std::nullopt;
            values.insert(key->toString(), *value);
        }
        return QVariant(values);
    }

    const QByteArray &m_data;
    qint64 m_at = 0;
};

QVariant readMsgpack(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    return MsgpackReader(data).read().value_or(QVariant());
}

// A file of a zip archive.
std::optional<QByteArray> readZipEntry(zip_t *zip, const QString &name)
{
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat(zip, name.toUtf8().constData(), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE))
        return std::nullopt;
    zip_file_t *file = zip_fopen(zip, name.toUtf8().constData(), 0);
    if (!file)
        return std::nullopt;
    QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
    const zip_int64_t read = zip_fread(file, data.data(), stat.size);
    zip_fclose(file);
    if (read != zip_int64_t(stat.size))
        return std::nullopt;
    return data;
}

// locale_fallbacks(): the names a language's translations are looked for
// under, most precise first.
QStringList localeFallbacks(const QString &language)
{
    const QString base = language.section(u'@', 0, 0);
    const QString modifier = language.contains(u'@') && !language.section(u'@', 1).isEmpty()
        ? u'@' + language.section(u'@', 1) : QString();
    const QString primary = base.section(u'_', 0, 0).toLower();
    QStringList result;
    for (const QString &candidate : { language, primary + modifier, base, primary }) {
        if (!candidate.isEmpty() && !result.contains(candidate))
            result.append(candidate);
    }
    return result;
}

QMutex lock;
std::optional<QString> overriddenFolder;
QHash<QString, std::shared_ptr<const Names>> cache;
std::optional<std::shared_ptr<const Iso639>> iso639Cache;

QHash<QString, QString> textMap(const QVariant &value)
{
    QHash<QString, QString> result;
    const QVariantMap map = value.toMap();
    result.reserve(map.size());
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        result.insert(it.key(), it.value().toString());
    return result;
}

// translator_for_lang(): the catalogs of the interface language, Calibre's
// messages then its languages and countries, those of the first of its
// locale names Calibre has translations for, a tenth translated at least.
QList<QHash<QString, QString>> catalogs(const QString &localization, const QString &interfaceLanguage)
{
    QList<QHash<QString, QString>> result;
    const QVariantMap stats = readMsgpack(QDir(localization).filePath(QStringLiteral("stats.calibre_msgpack"))).toMap();
    QString folder;
    for (const QString &candidate : localeFallbacks(interfaceLanguage)) {
        if (stats.value(candidate).toDouble() > 0.1) {
            folder = candidate;
            break;
        }
    }
    if (folder.isEmpty())
        return result;
    int error = 0;
    zip_t *zip = zip_open(QFile::encodeName(QDir(localization).filePath(QStringLiteral("locales.zip"))).constData(),
                          ZIP_RDONLY, &error);
    if (!zip)
        return result;
    // Low German's languages and countries are German's.
    const QString isoFolder = folder == QLatin1String("nds") ? QStringLiteral("de") : folder;
    if (const std::optional<QByteArray> messages = readZipEntry(zip, folder + QStringLiteral("/messages.mo"))) {
        result.append(readMo(*messages));
        for (const QString &name : { QStringLiteral("iso639.mo"), QStringLiteral("iso3166.mo") }) {
            if (const std::optional<QByteArray> catalog = readZipEntry(zip, isoFolder + u'/' + name))
                result.append(readMo(*catalog));
        }
    }
    zip_close(zip);
    return result;
}

} // namespace

QHash<QString, QString> readMo(const QByteArray &data)
{
    QHash<QString, QString> result;
    if (data.size() < 20)
        return result;
    const auto word = [&data](qsizetype at, bool little) -> quint32 {
        if (at < 0 || at + 4 > data.size())
            return 0;
        const auto byte = [&](int i) { return quint32(quint8(data.at(at + i))); };
        return little ? byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24
                      : byte(3) | byte(2) << 8 | byte(1) << 16 | byte(0) << 24;
    };
    const bool little = word(0, true) == 0x950412de;
    if (!little && word(0, false) != 0x950412de)
        return result;
    const quint32 count = word(8, little);
    const quint32 originals = word(12, little);
    const quint32 translations = word(16, little);
    const auto string = [&](quint32 table, quint32 index) -> std::optional<QByteArray> {
        const quint32 length = word(table + 8 * index, little);
        const quint32 offset = word(table + 8 * index + 4, little);
        if (qsizetype(offset) + qsizetype(length) > data.size())
            return std::nullopt;
        return data.mid(offset, length);
    };
    for (quint32 i = 0; i < count; ++i) {
        const std::optional<QByteArray> original = string(originals, i);
        const std::optional<QByteArray> translation = string(translations, i);
        // A message with a context is another message; a plural's first
        // form stands for it.
        if (!original || !translation || original->isEmpty() || original->contains('\x04'))
            continue;
        const auto first = [](const QByteArray &text) { return text.left(text.indexOf('\0') < 0 ? text.size() : text.indexOf('\0')); };
        result.insert(QString::fromUtf8(first(*original)), QString::fromUtf8(first(*translation)));
    }
    return result;
}

std::shared_ptr<const Iso639> iso639()
{
    {
        const QMutexLocker locker(&lock);
        if (iso639Cache)
            return *iso639Cache;
    }
    std::shared_ptr<const Iso639> table;
    const QString resources = resourcesFolder();
    if (!resources.isEmpty()) {
        const QVariantMap raw = readMsgpack(QDir(resources).filePath(QStringLiteral("localization/iso639.calibre_msgpack")))
                                    .toMap();
        auto made = std::make_shared<Iso639>();
        made->by3 = textMap(raw.value(raw.contains(QStringLiteral("by_3")) ? QStringLiteral("by_3") : QStringLiteral("by_3t")));
        made->twoToThree = textMap(raw.value(QStringLiteral("2to3")));
        made->nameMap = textMap(raw.value(QStringLiteral("name_map")));
        if (!made->by3.isEmpty())
            table = made;
    }
    const QMutexLocker locker(&lock);
    iso639Cache = table;
    return table;
}

std::optional<QString> canonicalize(const QString &raw)
{
    const std::shared_ptr<const Iso639> table = iso639();
    if (!table || table->twoToThree.isEmpty())
        return std::nullopt;
    QString code = raw.toLower().trimmed();
    code.replace(u'_', u'-');
    code = code.section(u'-', 0, 0).section(u'@', 0, 0).trimmed();
    if (code.isEmpty())
        return QString();
    if (code.size() == 2) {
        if (const auto found = table->twoToThree.constFind(code); found != table->twoToThree.cend())
            return *found;
    } else if (code.size() == 3 && table->by3.contains(code)) {
        return code;
    }
    return table->nameMap.value(code);
}

Names::Names(const QString &resources, const QString &interfaceLanguage)
{
    if (resources.isEmpty())
        return;
    m_iso639 = iso639();
    if (!m_iso639)
        return;
    m_catalogs = catalogs(QDir(resources).filePath(QStringLiteral("localization")), interfaceLanguage);
    // reverse_lang_map: by lower case name, the code.
    m_codesByName.reserve(m_iso639->by3.size());
    for (auto it = m_iso639->by3.cbegin(); it != m_iso639->by3.cend(); ++it)
        m_codesByName.insert(name(it.key()).toLower(), it.key());
}

QString Names::name(const QString &code) const
{
    if (!m_iso639) {
        const QLocale::Language language = QLocale::codeToLanguage(code, QLocale::AnyLanguageCode);
        return language == QLocale::AnyLanguage ? code : QLocale::languageToString(language);
    }
    const auto english = m_iso639->by3.constFind(code);
    if (english == m_iso639->by3.cend())
        return code;
    for (const QHash<QString, QString> &catalog : m_catalogs) {
        const auto translated = catalog.constFind(*english);
        if (translated != catalog.cend() && !translated->isEmpty())
            return *translated;
    }
    return *english;
}

QString Names::codeOfName(const QString &name) const
{
    return m_codesByName.value(name);
}

bool Names::fromCalibre() const
{
    return bool(m_iso639);
}

QString resourcesFolder()
{
    {
        const QMutexLocker locker(&lock);
        if (overriddenFolder)
            return *overriddenFolder;
    }
    if (qEnvironmentVariableIsSet("COUSOIR_CALIBRE_RESOURCES"))
        return qEnvironmentVariable("COUSOIR_CALIBRE_RESOURCES");
    QStringList candidates;
    // Calibre's own build: resources beside calibre-debug, or in the
    // application bundle on macOS.
    const QString debug = QStandardPaths::findExecutable(QStringLiteral("calibre-debug"));
    if (!debug.isEmpty()) {
        const QDir folder = QFileInfo(QFileInfo(debug).canonicalFilePath()).dir();
        candidates << folder.filePath(QStringLiteral("resources"))
                   << folder.filePath(QStringLiteral("../Resources/resources"))
                   << folder.filePath(QStringLiteral("app/resources"));
    }
    candidates << QStringLiteral("/opt/calibre/resources") << QStringLiteral("/usr/share/calibre")
               << QStringLiteral("/usr/local/share/calibre")
               << QStringLiteral("/Applications/calibre.app/Contents/Resources/resources")
               << QStringLiteral("C:/Program Files/Calibre2/app/resources");
    for (const QString &candidate : std::as_const(candidates)) {
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("localization/iso639.calibre_msgpack"))))
            return QDir::cleanPath(candidate);
    }
    return {};
}

void setResourcesFolder(const QString &folder)
{
    const QMutexLocker locker(&lock);
    if (folder.isNull())
        overriddenFolder.reset();
    else
        overriddenFolder = folder;
    cache.clear();
    iso639Cache.reset();
}

std::shared_ptr<const Names> names(const QString &interfaceLanguage)
{
    {
        const QMutexLocker locker(&lock);
        if (const auto found = cache.constFind(interfaceLanguage); found != cache.cend())
            return *found;
    }
    auto made = std::make_shared<const Names>(resourcesFolder(), interfaceLanguage);
    const QMutexLocker locker(&lock);
    cache.insert(interfaceLanguage, made);
    return made;
}

} // namespace CalibreLanguages
