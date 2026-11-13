#pragma once

#include <QByteArray>
#include <QCoreApplication>
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
#include "calibre_fts.h"
#include "calibre_lock.h"
#include "sqlite_db.h"

// What the tests share: the fixture library, Calibre in French, reading and
// writing a file whole, copying and listing a folder, the text of an EPUB, a
// book's formats, a JSON array of strings and a fake Kobo.

// The tests change copies of a library under a lock of their own, one for
// each test program: with Calibre's, a Calibre open on the computer would
// refuse every one of those changes.
inline const bool ownCalibreLock = [] {
    CalibreLock::setNameForTests("cousoir-tests-" + QByteArray::number(QCoreApplication::applicationPid()));
    return true;
}();

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

// The full text index Calibre keeps beside a library, with its schema
// (resources/fts_sqlite.sql) and its tokenizer in `language`, Calibre's
// interface language: `texts` holds { book, format, text } rows, `pending`
// { book, format } ones Calibre has still to index.
inline bool makeCalibreTextIndex(const QString &library, const QList<QVariantList> &texts,
                                 const QList<QVariantList> &pending, const QString &language, QString *error)
{
    const QString path = QDir(library).filePath(QStringLiteral("full-text-search.db"));
    SqliteDb db;
    if (!writeAll(path, {}) || !db.open(path, SqliteDb::Mode::ReadWrite, error)
        || !CalibreFts::registerTokenizers(db.handle(), language, error))
        return false;
    if (!db.exec(QStringLiteral(
            "CREATE TABLE dirtied_formats (id INTEGER PRIMARY KEY, book INTEGER NOT NULL, format TEXT NOT NULL COLLATE NOCASE,"
            " in_progress INTEGER NOT NULL DEFAULT FALSE, UNIQUE(book, format));"
            "CREATE TABLE books_text (id INTEGER PRIMARY KEY, book INTEGER NOT NULL, timestamp REAL NOT NULL,"
            " format TEXT NOT NULL COLLATE NOCASE, format_hash TEXT NOT NULL COLLATE NOCASE, format_size INTEGER NOT NULL DEFAULT 0,"
            " searchable_text TEXT NOT NULL DEFAULT '', text_size INTEGER NOT NULL DEFAULT 0,"
            " text_hash TEXT NOT NULL COLLATE NOCASE DEFAULT '', err_msg TEXT DEFAULT '', UNIQUE(book, format));"
            "CREATE VIRTUAL TABLE books_fts USING fts5(searchable_text, content = 'books_text', content_rowid = 'id',"
            " tokenize = 'calibre remove_diacritics 2');"
            "CREATE VIRTUAL TABLE books_fts_stemmed USING fts5(searchable_text, content = 'books_text', content_rowid = 'id',"
            " tokenize = 'porter calibre remove_diacritics 2');"
            "CREATE TRIGGER books_fts_insert_trg AFTER INSERT ON books_text BEGIN"
            " INSERT INTO books_fts(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);"
            " INSERT INTO books_fts_stemmed(rowid, searchable_text) VALUES (NEW.id, NEW.searchable_text);"
            " DELETE FROM dirtied_formats WHERE book=NEW.book AND format=NEW.format; END;"),
                 error))
        return false;
    for (const QVariantList &text : texts) {
        if (!db.run(QStringLiteral("INSERT INTO books_text(book, timestamp, format, format_hash, searchable_text)"
                                   " VALUES (?, 0, ?, 'hash', ?)"),
                    text, error))
            return false;
    }
    for (const QVariantList &format : pending) {
        if (!db.run(QStringLiteral("INSERT INTO dirtied_formats(book, format) VALUES (?, ?)"), format, error))
            return false;
    }
    return true;
}
