#pragma once

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include <atomic>
#include <functional>

class SqliteDb;

// Calibre's full text index of a library's books, full-text-search.db, kept
// as Calibre keeps it (src/calibre/db/fts): in step with the library's
// formats as its triggers keep it, made as it makes it, and filled with the
// books' text as its indexing workers extract it, for the formats Cousoir
// reads. Calibre and Cousoir then each find what the other indexed.
namespace BookTextIndex {

// What an indexing worker makes of a book file (db/fts/text.py): its text,
// or why there is none. Not `supported` for a file Cousoir cannot read,
// which is left to Calibre.
struct Extraction
{
    bool supported = false;
    QString text;
    QString error;
};

// The text of the book file at `path`, of `format`: an EPUB's or a Kindle
// book's spine as Calibre's html_to_text() reads each page, a PDF's from
// pdftotext as Calibre runs it.
Extraction extract(const QString &path, const QString &format);

// What Calibre's triggers on its connection (fts_triggers.sql) would do to
// the index for a change to metadata.db.
struct FormatChange
{
    enum class Kind { BookRemoved, FormatRemoved, FormatChanged };
    Kind kind = Kind::FormatChanged;
    int book = 0;
    QString format;
};

// Records the format changes made through `db`, as Calibre's triggers see
// them, until formatChanges() reads them.
bool trackFormats(SqliteDb &db, QString *error);
QList<FormatChange> formatChanges(SqliteDb &db);
// Whether full text search is on for the library of `db`: Calibre's
// fts_enabled preference.
bool enabled(SqliteDb &db);

// The index brought in step with `changes`; nothing when the library has
// none.
bool follow(const QString &libraryPath, const QList<FormatChange> &changes, const QString &interfaceLanguage,
            QString *error);

struct Job
{
    int book = 0;
    QString format;
};

// A new index, empty, as Calibre makes one (fts_sqlite.sql), with `formats`
// waiting to be indexed, as Calibre's dirty_existing() leaves them.
bool create(const QString &libraryPath, const QList<Job> &formats, const QString &interfaceLanguage, QString *error);

// The formats waiting to be indexed, first queued first.
QList<Job> waiting(const QString &libraryPath, QString *error);

// A format's text found, or why there is none, for store().
struct Result
{
    int book = 0;
    QString format;
    qint64 formatSize = 0;
    // SHA-1 of the file, as Calibre hashes it.
    QString formatHash;
    QString text;
    QString errorMessage;
};

// The file's size and hash, as Calibre takes them before indexing it.
bool hashFile(const QString &path, qint64 *size, QString *hash);
// Whether the index already has that file's text: queue_job()'s test, the
// format then no longer waiting once stored.
bool current(const QString &libraryPath, const Job &job, qint64 size, const QString &hash);

// The results kept, as commit_result() keeps them: the text with its hash
// unless the same text is there already, or the error; each format no
// longer waiting. To be called under Calibre's lock.
bool store(const QString &libraryPath, const QList<Result> &results, const QString &interfaceLanguage, QString *error);

// Whether Cousoir reads the text of that format.
bool reads(const QString &format);
// Whether Calibre does (is_fmt_extractable()); a format it does not read
// no longer waits.
bool calibreReads(const QString &format);

struct Task
{
    int book = 0;
    QString format;
    // Empty for a format whose file is gone: it no longer waits, as
    // Calibre leaves it.
    QString path;
};

struct Run
{
    // Formats whose text went in, or that no longer wait.
    int stored = 0;
    // Formats Cousoir could not read after all, a Kindle book with DRM.
    QSet<QPair<int, QString>> leftToCalibre;
    // A Calibre program took the lock: its own workers index the rest.
    bool calibreRunning = false;
};

// Indexes `tasks` as Calibre's workers would, their text kept a few at a
// time under Calibre's lock; formats Cousoir does not read left waiting.
// `cancel`, when set, stops it after the format being read; `progress`
// hears how many tasks were done.
Run index(const QString &libraryPath, const QString &interfaceLanguage, const QList<Task> &tasks,
          const std::atomic_bool *cancel = nullptr, const std::function<void(int)> &progress = {});

} // namespace BookTextIndex
