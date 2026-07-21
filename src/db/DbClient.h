#pragma once
// DbClient — thin wrapper around a single SQLite3 connection.
//
// Replaces Acadia-Server's Drogon/PostgreSQL `DbClient` (which exposed a Drogon
// ORM `DbClientPtr` pool). Per master CLAUDE.md §4 the *-Server stack is
// "gRPC + SQLite" (not PostgreSQL); this is the host-buildable SQLite port. The
// deviation from Acadia-Server's PostgreSQL is flagged in server_update/10_arches_port.md.
//
// Threading model: a single `sqlite3*` connection is shared across all server
// threads (gRPC threads + the result-writer thread). Every call that touches the
// connection acquires a process-wide mutex, so each individual sqlite3 call is
// atomic w.r.t. other threads. A `PreparedStatement` does NOT hold the lock for
// its lifetime — only during each bind/step/reset call — because the prepared
// statement's bound-parameter state lives in the `sqlite3_stmt*`, not in the
// shared connection, so interleaving with another thread's operation on the same
// connection cannot corrupt this statement. This keeps lock hold time minimal
// and avoids deadlocks, and is correct regardless of SQLite's compile-time
// threading mode.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace arches::server::db {

// Initialise the global SQLite connection. Call once before any repository or
// MigrationRunner is constructed. Opens the file (creating it if absent),
// enables WAL + a 5s busy timeout, and sets PRAGMA foreign_keys=ON so the
// ON DELETE CASCADE constraints in the migrations actually fire. Throws
// std::runtime_error on failure.
void init_db_client(const std::string& db_path);

// Close the global connection. Optional; called at shutdown. Safe to call even
// if init was never done.
void shutdown_db_client();

// Returns the shared SQLite connection. Throws if init_db_client() was not
// called. Acquires the connection mutex internally for every call.
sqlite3* get_db();

// Execute one or more SQL statements (no parameters, no results). Acquires the
// mutex and runs sqlite3_exec. Throws std::runtime_error on error.
void exec(const std::string& sql);

// RAII prepared statement with typed binders + column accessors. Repositories
// build one of these from a SQL string, bind parameters, then call step() in a
// loop. The lock is taken per bind/step/reset call (see the threading note
// above), so a long-running SELECT iteration does not starve other threads.
class PreparedStatement {
public:
    // Compiles `sql` against the shared connection. Throws on compile error.
    explicit PreparedStatement(const std::string& sql);
    ~PreparedStatement();

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    PreparedStatement(PreparedStatement&&) noexcept;
    PreparedStatement& operator=(PreparedStatement&&) noexcept;

    // Bind by 1-based index.
    void bind(int idx, std::nullptr_t);
    void bind(int idx, int value);
    void bind(int idx, int64_t value);
    void bind(int idx, double value);
    void bind(int idx, const std::string& value);
    void bind(int idx, const char* value);
    void bind_text_or_null(int idx, const std::optional<std::string>& value);
    void bind_int64_or_null(int idx, const std::optional<int64_t>& value);
    void bind_blob(int idx, const void* data, std::size_t len);
    void bind_blob(int idx, const std::vector<std::uint8_t>& blob);

    // Advance to the next row. Returns true if a row is available, false if the
    // statement is exhausted (no more rows). Throws on error.
    bool step();

    // Reset to before the first row and clear all bindings, for reuse in a loop.
    void reset();

    // Column accessors (0-based column index). Call only after step() returned true.
    int column_int(int idx) const;
    int64_t column_int64(int idx) const;
    double column_double(int idx) const;
    std::string column_text(int idx) const;
    std::vector<std::uint8_t> column_blob(int idx) const;
    bool column_is_null(int idx) const;

    // Number of columns in the result row.
    int column_count() const;

private:
    void check(int rc, const char* context) const;

    sqlite3_stmt* stmt_{nullptr};
};

// Convenience: a scoped lock on the connection mutex. Repositories rarely need
// this (per-call locking suffices); it is exposed for the rare case that needs
// to make several calls atomic as a unit (e.g. read-then-write without racing).
class [[nodiscard]] DbLock {
public:
    DbLock();
    ~DbLock();
    DbLock(const DbLock&) = delete;
    DbLock& operator=(const DbLock&) = delete;
private:
    std::unique_lock<std::mutex> lock_;
};

}  // namespace arches::server::db