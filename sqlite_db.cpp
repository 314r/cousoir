#include "sqlite_db.h"

#include <QUrl>

#include <sqlite3.h>

namespace {

QString errorOf(sqlite3 *db)
{
    return db ? QString::fromUtf8(sqlite3_errmsg(db)) : QStringLiteral("out of memory");
}

} // namespace

SqliteDb::~SqliteDb()
{
    close();
}

bool SqliteDb::open(const QString &path, Mode mode, QString *error)
{
    close();
    // A file: URI keeps the path's own characters out of the parameters.
    QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    int flags = SQLITE_OPEN_URI;
    if (mode == Mode::Immutable) {
        uri += "?mode=ro&immutable=1";
        flags |= SQLITE_OPEN_READONLY;
    } else {
        flags |= SQLITE_OPEN_READWRITE;
    }
    if (sqlite3_open_v2(uri.constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        if (error)
            *error = errorOf(m_db);
        close();
        return false;
    }
    sqlite3_extended_result_codes(m_db, 1);
    return true;
}

void SqliteDb::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool SqliteDb::isOpen() const
{
    return m_db != nullptr;
}

sqlite3 *SqliteDb::handle() const
{
    return m_db;
}

bool SqliteDb::exec(const QString &sql, QString *error)
{
    char *message = nullptr;
    if (sqlite3_exec(m_db, sql.toUtf8().constData(), nullptr, nullptr, &message) != SQLITE_OK) {
        if (error)
            *error = message ? QString::fromUtf8(message) : errorOf(m_db);
        sqlite3_free(message);
        return false;
    }
    return true;
}

QVariant SqliteDb::value(const QString &sql, QString *error)
{
    SqliteStatement statement(*this, sql);
    if (statement.step())
        return statement.value(0);
    if (error)
        *error = statement.error();
    return {};
}

QString SqliteDb::lastError() const
{
    return errorOf(m_db);
}

qint64 SqliteDb::lastInsertRowId() const
{
    return sqlite3_last_insert_rowid(m_db);
}

SqliteStatement::SqliteStatement(SqliteDb &db, const QString &sql)
    : m_db(db.handle())
{
    if (!m_db) {
        m_error = QStringLiteral("The database is not open.");
        return;
    }
    const QByteArray utf8 = sql.toUtf8();
    if (sqlite3_prepare_v2(m_db, utf8.constData(), int(utf8.size()), &m_statement, nullptr) != SQLITE_OK)
        m_error = errorOf(m_db);
}

SqliteStatement::~SqliteStatement()
{
    sqlite3_finalize(m_statement);
}

bool SqliteStatement::isValid() const
{
    return m_statement != nullptr;
}

QString SqliteStatement::error() const
{
    return m_error;
}

void SqliteStatement::bind(int index, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    sqlite3_bind_text(m_statement, index, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
}

void SqliteStatement::bind(int index, qint64 value)
{
    sqlite3_bind_int64(m_statement, index, value);
}

void SqliteStatement::bind(int index, double value)
{
    sqlite3_bind_double(m_statement, index, value);
}

void SqliteStatement::bindNull(int index)
{
    sqlite3_bind_null(m_statement, index);
}

bool SqliteStatement::step()
{
    if (!m_statement)
        return false;
    const int result = sqlite3_step(m_statement);
    if (result == SQLITE_ROW)
        return true;
    if (result != SQLITE_DONE)
        m_error = errorOf(m_db);
    return false;
}

void SqliteStatement::reset()
{
    sqlite3_reset(m_statement);
    sqlite3_clear_bindings(m_statement);
    m_error.clear();
}

int SqliteStatement::columnCount() const
{
    return sqlite3_column_count(m_statement);
}

bool SqliteStatement::isNull(int column) const
{
    return sqlite3_column_type(m_statement, column) == SQLITE_NULL;
}

qint64 SqliteStatement::integer(int column) const
{
    return sqlite3_column_int64(m_statement, column);
}

double SqliteStatement::real(int column) const
{
    return sqlite3_column_double(m_statement, column);
}

QString SqliteStatement::text(int column) const
{
    const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(m_statement, column));
    return data ? QString::fromUtf8(data, sqlite3_column_bytes(m_statement, column)) : QString();
}

QVariant SqliteStatement::value(int column) const
{
    switch (sqlite3_column_type(m_statement, column)) {
    case SQLITE_INTEGER:
        return integer(column);
    case SQLITE_FLOAT:
        return real(column);
    case SQLITE_NULL:
        return {};
    default:
        return text(column);
    }
}
