// Ported from Calibre 9.17, src/calibre/db/fts/connect.py, search(),
// Copyright (C) Kovid Goyal, GPL v3.
#include "book_text_search.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QObject>

#include "calibre_fts.h"
#include "sqlite_db.h"

namespace BookTextSearch {

QString indexPath(const QString &libraryPath)
{
    return QDir(libraryPath).filePath(QStringLiteral("full-text-search.db"));
}

Status status(const QString &libraryPath)
{
    Status result;
    const QString path = indexPath(libraryPath);
    if (!QFileInfo::exists(path))
        return result;
    SqliteDb db;
    if (!db.open(path, SqliteDb::Mode::ReadOnly, nullptr))
        return result;
    result.exists = true;
    result.indexed = db.value(QStringLiteral("SELECT count(*) FROM books_text WHERE searchable_text != ''")).toInt();
    result.pending = db.value(QStringLiteral("SELECT count(*) FROM dirtied_formats")).toInt();
    return result;
}

QList<Hit> find(const QString &libraryPath, const QString &query, bool stemming, const QString &interfaceLanguage,
                int limit, QString *error)
{
    QList<Hit> hits;
    const QString path = indexPath(libraryPath);
    if (!QFileInfo::exists(path)) {
        if (error)
            *error = QObject::tr("Calibre has not indexed the text of this library's books.");
        return hits;
    }
    SqliteDb db;
    if (!db.open(path, SqliteDb::Mode::ReadOnly, error)
        || !CalibreFts::registerTokenizers(db.handle(), interfaceLanguage, error))
        return hits;
    // unicode_normalize(): the query composed, as the text was indexed.
    const QString normalized = query.normalized(QString::NormalizationForm_C);
    const auto search = [&](const QString &table) {
        SqliteStatement statement(
            db, QStringLiteral("SELECT books_text.book, books_text.format, snippet(\"%1\", 0, ?, ?, '…', 32) "
                               "FROM books_text JOIN \"%1\" ON books_text.id = \"%1\".rowid "
                               "WHERE \"%1\" MATCH ? ORDER BY \"%1\".rank LIMIT ?")
                    .arg(table));
        statement.bind({ QStringLiteral("\x1d"), QStringLiteral("\x1d"), normalized, qint64(std::max(1, limit)) });
        while (statement.step() && hits.size() < std::max(1, limit)) {
            const Hit hit{ int(statement.integer(0)), statement.text(1).toUpper(), statement.text(2) };
            const bool known = std::any_of(hits.cbegin(), hits.cend(), [&hit](const Hit &other) {
                return other.bookId == hit.bookId && other.format == hit.format;
            });
            if (!known)
                hits.append(hit);
        }
        if (!statement.error().isEmpty() && error)
            *error = statement.error();
        return statement.error().isEmpty();
    };
    // The stems are the language's Calibre indexed in: one changed since
    // stems the words otherwise, so they are also looked for as written.
    if (!stemming || search(QStringLiteral("books_fts_stemmed")))
        search(QStringLiteral("books_fts"));
    return hits;
}

} // namespace BookTextSearch
