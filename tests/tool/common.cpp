#include "common.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <zip.h>

namespace Tool {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

QMap<QString, QString> files(const QString &root)
{
    QMap<QString, QString> found;
    const QDir base(QDir(root).absolutePath());
    QDirIterator it(base.path(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        found.insert(base.relativeFilePath(path), path);
    }
    return found;
}

std::optional<QByteArray> readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        err() << "Cannot write " << path << Qt::endl;
        return false;
    }
    return true;
}

std::optional<QJsonValue> readJson(const QString &path)
{
    const std::optional<QByteArray> data = readFile(path);
    if (!data) {
        err() << "Cannot read " << path << Qt::endl;
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(*data, &error);
    if (error.error != QJsonParseError::NoError) {
        err() << path << ": " << error.errorString() << Qt::endl;
        return std::nullopt;
    }
    return document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
}

QString sha256(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

std::optional<ZipEntries> readZip(const QString &path)
{
    int error = 0;
    zip_t *zip = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    if (!zip)
        return std::nullopt;
    ZipEntries entries;
    bool ok = true;
    const zip_int64_t count = zip_get_num_entries(zip, 0);
    for (zip_int64_t index = 0; ok && index < count; ++index) {
        zip_stat_t stat;
        zip_stat_init(&stat);
        ok = zip_stat_index(zip, zip_uint64_t(index), 0, &stat) == 0 && (stat.valid & ZIP_STAT_NAME)
             && (stat.valid & ZIP_STAT_SIZE);
        zip_file_t *file = ok ? zip_fopen_index(zip, zip_uint64_t(index), 0) : nullptr;
        if (!file) {
            ok = false;
            break;
        }
        QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
        const zip_int64_t read = zip_fread(file, data.data(), stat.size);
        zip_fclose(file);
        ok = read >= 0 && zip_uint64_t(read) == stat.size;
        entries.append({ QString::fromUtf8(stat.name), data });
    }
    zip_discard(zip);
    if (!ok)
        return std::nullopt;
    return entries;
}

bool writeZip(const QString &path, const ZipEntries &entries, const QStringList &stored)
{
    QFile::remove(path);
    int error = 0;
    zip_t *zip = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_EXCL, &error);
    if (!zip) {
        err() << "Cannot write " << path << Qt::endl;
        return false;
    }
    // 2026-01-01 00:00:00 in MS-DOS terms, the stamp of the fixtures.
    const zip_uint16_t date = ((2026 - 1980) << 9) | (1 << 5) | 1;
    bool ok = true;
    for (const auto &[name, data] : entries) {
        zip_source_t *source = zip_source_buffer(zip, data.constData(), zip_uint64_t(data.size()), 0);
        const zip_int64_t index = source ? zip_file_add(zip, name.toUtf8().constData(), source, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            if (source)
                zip_source_free(source);
            ok = false;
            break;
        }
        // Python's zipfile deflates at zlib's default level.
        const bool store = stored.contains(name);
        ok = zip_set_file_compression(zip, zip_uint64_t(index), store ? ZIP_CM_STORE : ZIP_CM_DEFLATE, store ? 0 : 6) == 0
             && zip_file_set_dostime(zip, zip_uint64_t(index), 0, date, 0) == 0;
        if (!ok)
            break;
    }
    if (ok && zip_close(zip) == 0)
        return true;
    err() << "Cannot write " << path << ": " << zip_strerror(zip) << Qt::endl;
    zip_discard(zip);
    QFile::remove(path);
    return false;
}

QByteArray substitute(const QByteArray &data, const QRegularExpression &pattern, const QString &replacement)
{
    return QString::fromLatin1(data).replace(pattern, replacement).toLatin1();
}

QJsonValue get(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
}

QString display(const QJsonValue &value)
{
    const QByteArray json = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString extension(const QString &path)
{
    const QString name = path.mid(path.lastIndexOf(u'/') + 1);
    const qsizetype dot = name.lastIndexOf(u'.');
    for (qsizetype i = 0; i < dot; ++i) {
        if (name.at(i) != u'.')
            return name.mid(dot);
    }
    return {};
}

} // namespace Tool
