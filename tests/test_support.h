#pragma once

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <zip.h>

#include "calibre_config.h"
#include "sqlite_db.h"

// What the tests share: the fixture library, Calibre in French, reading and
// writing a file whole, copying and listing a folder, the text of an EPUB, a
// book's formats, a JSON array of strings and a fake Kobo.

inline QString fixtureLibrary()
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/library");
}

inline CalibreConfig frenchConfig()
{
    CalibreConfig config;
    config.interfaceLanguage = QStringLiteral("fr");
    return config;
}

inline QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Writes `data` to `path`, making its folders first.
inline bool writeAll(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

inline bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = QDir(to).filePath(QDir(from).relativeFilePath(source));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(source, target))
            return false;
    }
    return true;
}

// The files under `root`, hidden ones too, by their path relative to it,
// sorted.
inline QStringList filesUnder(const QString &root)
{
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(QDir(root).relativeFilePath(it.next()));
    files.sort();
    return files;
}

// The text of every HTML file of an EPUB.
inline QByteArray epubText(const QString &path)
{
    int error = 0;
    zip_t *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    if (!archive)
        return {};
    QByteArray text;
    for (zip_int64_t index = 0; index < zip_get_num_entries(archive, 0); ++index) {
        const QString name = QString::fromUtf8(zip_get_name(archive, index, 0));
        zip_stat_t stat;
        if (!name.contains(QStringLiteral("htm")) || zip_stat_index(archive, index, 0, &stat) != 0)
            continue;
        zip_file_t *file = zip_fopen_index(archive, index, 0);
        QByteArray data(qsizetype(stat.size), Qt::Uninitialized);
        if (file && zip_fread(file, data.data(), stat.size) == zip_int64_t(stat.size))
            text += data;
        if (file)
            zip_fclose(file);
    }
    zip_close(archive);
    return text;
}

// The formats of a book, as the library's maps of a book list them.
inline QStringList formatsOf(const QVariantMap &book)
{
    QStringList formats;
    for (const QVariant &format : book.value(QStringLiteral("formats")).toList())
        formats.append(format.toMap().value(QStringLiteral("format")).toString());
    return formats;
}

// A JSON array of strings, as a list.
inline QStringList strings(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        result.append(item.toString());
    return result;
}

// A folder made to look like a mounted Kobo Libra 2 with firmware 4.38: its
// version file and an empty database with the tables Calibre uses.
inline bool makeKobo(const QString &root, QString *error = nullptr)
{
    const QString database = QDir(root).filePath(QStringLiteral(".kobo/KoboReader.sqlite"));
    SqliteDb db;
    return writeAll(QDir(root).filePath(QStringLiteral(".kobo/version")),
                    "N418000000000,4.1.15,4.38.21908,4.1.15,4.1.15,00000000-0000-0000-0000-000000000390")
        && writeAll(database, {}) && db.open(database, SqliteDb::Mode::ReadWrite, error)
        && db.exec(QString::fromUtf8(readAll(QStringLiteral(COUSOIR_TEST_DATA_DIR "/kobo.sql"))), error);
}
