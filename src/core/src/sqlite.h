#pragma once

// Minimal RAII wrappers around the SQLite C API, internal to store-core.
// Each Database and its Statements belong to one thread.

#include <cutefish/store/result.h>

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cutefish::store::detail {

/// The ErrorCode of an SQLite result code (H.2), so that callers can tell
/// what may go away from what will not:
/// - SQLITE_BUSY, SQLITE_LOCKED (another connection holds the file, a
///   shared-cache table lock): Conflict, temporary;
/// - SQLITE_CORRUPT, SQLITE_NOTADB (a damaged file), SQLITE_CONSTRAINT,
///   SQLITE_MISMATCH (data that breaks the schema): InvalidArgument,
///   permanent until the file or the data change;
/// - anything else (I/O, full disk, read-only file, misuse): Internal;
/// - except SQLITE_CONSTRAINT_COMMITHOOK, a commit refused by the
///   connection's own commit hook: Cancelled.
ErrorCode errorCodeOf(int sqliteResult) noexcept;

class Database
{
public:
    Database() = default;
    ~Database() { close(); }
    Database(Database&& other) noexcept : m_db(std::exchange(other.m_db, nullptr)) {}
    Database& operator=(Database&& other) noexcept
    {
        if (this != &other) {
            close();
            m_db = std::exchange(other.m_db, nullptr);
        }
        return *this;
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// `uri` is passed with SQLITE_OPEN_URI; flags are the usual
    /// SQLITE_OPEN_* values. NOMUTEX is always added: one thread per
    /// connection.
    static Result<Database> open(const std::string& uri, int flags);

    sqlite3* handle() const noexcept { return m_db; }
    explicit operator bool() const noexcept { return m_db != nullptr; }

    /// Runs one or more statements without results.
    Result<void> exec(const char* sql);
    /// Error with the last SQLite message and the given context. Its code is
    /// `code` when given, errorCodeOf() the last result otherwise.
    Error error(std::string_view context, std::optional<ErrorCode> code = std::nullopt) const;
    /// First column of the first row as an integer.
    Result<std::int64_t> queryInt(const char* sql);

    void close() noexcept;

private:
    explicit Database(sqlite3* db) : m_db(db) {}
    sqlite3* m_db = nullptr;
};

class Statement
{
public:
    Statement() = default;
    ~Statement() { sqlite3_finalize(m_stmt); }
    Statement(Statement&& other) noexcept : m_stmt(std::exchange(other.m_stmt, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept
    {
        if (this != &other) {
            sqlite3_finalize(m_stmt);
            m_stmt = std::exchange(other.m_stmt, nullptr);
        }
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    static Result<Statement> prepare(Database& db, const char* sql);

    /// Parameters are 1-based. Text is bound with SQLITE_STATIC: the caller
    /// keeps it alive until the statement is reset. An empty text is bound
    /// as '' even when its pointer is null (SQLite would bind NULL).
    Statement& bind(int index, std::string_view text) noexcept
    {
        sqlite3_bind_text(m_stmt, index, text.data() ? text.data() : "", static_cast<int>(text.size()), SQLITE_STATIC);
        return *this;
    }
    Statement& bind(int index, std::int64_t value) noexcept
    {
        sqlite3_bind_int64(m_stmt, index, value);
        return *this;
    }
    Statement& bindNull(int index) noexcept
    {
        sqlite3_bind_null(m_stmt, index);
        return *this;
    }

    /// SQLITE_ROW, SQLITE_DONE or an error code.
    int step() noexcept { return sqlite3_step(m_stmt); }
    /// Steps once, expecting SQLITE_DONE, then resets.
    Result<void> run();
    void reset() noexcept
    {
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
    }

    std::int64_t columnInt(int index) const noexcept { return sqlite3_column_int64(m_stmt, index); }
    std::string_view columnText(int index) const noexcept
    {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, index));
        return text ? std::string_view(text, static_cast<std::size_t>(sqlite3_column_bytes(m_stmt, index)))
                    : std::string_view();
    }
    bool columnIsNull(int index) const noexcept { return sqlite3_column_type(m_stmt, index) == SQLITE_NULL; }
    double columnDouble(int index) const noexcept { return sqlite3_column_double(m_stmt, index); }

    sqlite3_stmt* handle() const noexcept { return m_stmt; }

private:
    explicit Statement(sqlite3_stmt* stmt) : m_stmt(stmt) {}
    sqlite3_stmt* m_stmt = nullptr;
};

} // namespace cutefish::store::detail
