#include "sqlite.h"

namespace cutefish::store::detail {

ErrorCode errorCodeOf(int sqliteResult) noexcept
{
    switch (sqliteResult & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return ErrorCode::Conflict;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_CONSTRAINT:
    case SQLITE_MISMATCH:
        return ErrorCode::InvalidArgument;
    default:
        return ErrorCode::Internal;
    }
}

Result<Database> Database::open(const std::string& uri, int flags)
{
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(uri.c_str(), &db, flags | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        sqlite3_close_v2(db);
        return Error::notFound("cannot open " + uri + ": " + message);
    }
    sqlite3_extended_result_codes(db, 1);
    return Database(db);
}

void Database::close() noexcept
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

Result<void> Database::exec(const char* sql)
{
    char* message = nullptr;
    if (const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &message); rc != SQLITE_OK) {
        std::string text = message ? message : "unknown error";
        sqlite3_free(message);
        return Error(errorCodeOf(rc), "SQLite: " + text);
    }
    return {};
}

Error Database::error(std::string_view context, std::optional<ErrorCode> code) const
{
    const ErrorCode c = code ? *code : (m_db ? errorCodeOf(sqlite3_extended_errcode(m_db)) : ErrorCode::Internal);
    return Error(c, std::string(context) + ": " + (m_db ? sqlite3_errmsg(m_db) : "no database"));
}

Result<std::int64_t> Database::queryInt(const char* sql)
{
    auto stmt = Statement::prepare(*this, sql);
    if (!stmt)
        return std::move(stmt).error();
    const int rc = stmt->step();
    if (rc == SQLITE_ROW)
        return stmt->columnInt(0);
    if (rc == SQLITE_DONE)
        return Error::notFound(std::string("no row for: ") + sql);
    return error(sql);
}

Result<Statement> Statement::prepare(Database& db, const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
        return db.error(std::string("prepare: ") + sql);
    return Statement(stmt);
}

Result<void> Statement::run()
{
    const int rc = sqlite3_step(m_stmt);
    Result<void> result;
    if (rc != SQLITE_DONE) {
        const char* message = sqlite3_errmsg(sqlite3_db_handle(m_stmt));
        result = Error(errorCodeOf(rc), std::string("SQLite: ") + (message ? message : "step failed"));
    }
    reset();
    return result;
}

} // namespace cutefish::store::detail
