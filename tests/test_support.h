#pragma once

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include "sqlite_db.h"

// What the tests share: the fixture library, reading and writing a file
// whole, copying a folder and a fake Kobo.

inline QString fixtureLibrary()
{
    return QStringLiteral(COUSOIR_TEST_DATA_DIR "/library");
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
