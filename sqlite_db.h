#pragma once

#include <QString>
#include <QVariant>

struct sqlite3;
struct sqlite3_stmt;

// A connection to a SQLite file through the C API, which Calibre's SQL
// functions and temporary triggers need. Closed when destroyed.
class SqliteDb
{
public:
    enum class Mode {
        // Read-only with SQLite's immutable flag: no lock is taken and
        // nothing is written beside the file, which must not change while
        // the connection is open.
        Immutable,
        ReadWrite,
    };

    SqliteDb() = default;
    ~SqliteDb();
    SqliteDb(const SqliteDb &) = delete;
    SqliteDb &operator=(const SqliteDb &) = delete;

    bool open(const QString &path, Mode mode, QString *error);
    void close();
    bool isOpen() const;
    sqlite3 *handle() const;

    // Runs statements that return no rows.
    bool exec(const QString &sql, QString *error);
    // The first column of the first row of `sql`, or an invalid QVariant.
    QVariant value(const QString &sql, QString *error = nullptr);
    QString lastError() const;
    // The rowid of the last row an INSERT added.
    qint64 lastInsertRowId() const;

private:
    sqlite3 *m_db = nullptr;
};

// A prepared statement; step() walks its rows.
class SqliteStatement
{
public:
    SqliteStatement(SqliteDb &db, const QString &sql);
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement &) = delete;
    SqliteStatement &operator=(const SqliteStatement &) = delete;

    bool isValid() const;
    // The preparation's or the last step's error, empty when there was none.
    QString error() const;

    void bind(int index, const QString &value);
    void bind(int index, qint64 value);
    void bind(int index, double value);
    void bindNull(int index);

    // True while there is a row; false at the end or on an error.
    bool step();
    void reset();

    int columnCount() const;
    bool isNull(int column) const;
    qint64 integer(int column) const;
    double real(int column) const;
    // Text as UTF-8; a NULL is an empty string.
    QString text(int column) const;
    QVariant value(int column) const;

private:
    sqlite3 *m_db = nullptr;
    sqlite3_stmt *m_statement = nullptr;
    QString m_error;
};
