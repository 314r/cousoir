#pragma once

#include <QDateTime>
#include <QString>

#include "book.h"
#include "calibre_config.h"

class SqliteDb;

// Reading a Calibre library's metadata.db. Nothing here writes inside the
// library: the database is copied out and read from the copy.
namespace CalibreDb {

// The schema version, PRAGMA user_version, of Calibre 6 to 9.
inline constexpr int SchemaVersion = 28;

FileIdentity identify(const QString &path);

// Copies <library>/metadata.db to `snapshotPath` as one consistent file. The
// copy is retried while Calibre's journal is there, while the database
// changes during the copy, or when the copy fails SQLite's quick check.
// `source` gets the database as it was copied, `sha256` the copy's hash.
bool takeSnapshot(const QString &libraryPath, const QString &snapshotPath, FileIdentity *source,
                  QByteArray *sha256, QString *error);

// Sets a connection up like Calibre's Connection class in db/backend.py: its
// SQL functions and aggregates, the temporary author triggers, foreign keys
// and a 10 s busy timeout. title_sort uses `tweaks` and `interfaceLanguage`.
bool prepareConnection(SqliteDb &db, const CalibreText::Tweaks &tweaks, const QString &interfaceLanguage,
                       QString *error);

// A hash of the tables with their columns and the names of the indexes,
// triggers and views, leaving custom columns out. It changes when Calibre
// changes its schema, not when an upgraded library spells its SQL
// differently from a new one.
QString schemaFingerprint(SqliteDb &db);
bool isKnownSchema(int userVersion, const QString &fingerprint);

// Reads the books, the items they link to and the library's id.
bool readLibrary(SqliteDb &db, LibraryData *data, QString *error);
// Finds each book's cover and format files in its folder, as calibredb list
// does: a format whose file has another name is found by its extension.
void findFiles(LibraryData *data);

// Everything above: the snapshot, a prepared immutable connection to it, the
// read and the file check.
bool loadLibrary(const QString &libraryPath, const QString &snapshotPath, const CalibreConfig &config,
                 LibraryData *data, QString *error);

// Calibre's dates: "2015-07-07 18:41:43.605980+00:00" and its variants, in UTC
// to the second. NULL and text that is no date are Calibre's undefined date.
QDateTime parseDate(const QVariant &value);
QDateTime undefinedDate();
bool isUndefinedDate(const QDateTime &date);
// "2015-07-07T18:41:43+00:00", Python's isoformat() of the UTC date.
QString isoFormat(const QDateTime &date);

} // namespace CalibreDb
