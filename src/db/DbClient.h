#pragma once
// DbClient — thin wrapper around a single SQLite3 connection.
//
// Replaces the prior reference implementation's Drogon/PostgreSQL `DbClient` (which exposed a Drogon
// ORM `DbClientPtr` pool). Per master CLAUDE.md §4 the *-Server stack is
// "gRPC + SQLite" (not PostgreSQL); this is the host-buildable SQLite port. The
// deviation from the prior reference implementation's PostgreSQL is flagged in server_update/10_arches_port.md.
//
// Threading model: a single `sqlite3*` connection is shared across all server
// threads (gRPC read-loop thread + the db-writer thread + the test/main thread).
// The connection is opened with SQLITE_OPEN_NOMUTEX, so SQLite provides NO
// internal serialization — we are responsible for it.
//
// A `PreparedStatement` HOLDS the connection mutex (`g_db_mutex`, a
// `std::recursive_mutex`) for its ENTIRE lifetime: acquired in the constructor
// (around `sqlite3_prepare_v2`) and released in the destructor (after
// `sqlite3_finalize`). bind/step/reset/column accessors run under that already-
// held lock and do NOT re-lock. This makes each PreparedStatement object one
// fully-serialized critical section, so two statements on the shared connection
// can NEVER interleave their prepare/bind/step/reset call sequences.
//
// WHY lifetime locking (not per-call): `sqlite3_prepare_v2`, `sqlite3_step`, and
// the column accessors touch the CONNECTION's shared internal state (the SQL
// parser, the vdbe memory pool, the lookaside allocator, the per-connection
// error state) — not just the `sqlite3_stmt*`. An earlier per-call-locking
// design let thread A's `insert` step interleave with thread B's `set_run_status`
// prepare on the same connection, which corrupted SQLite's internal memory pool
// and surfaced as an intermittent SEGV deep inside `sqlite3_prepare_v2`
// (`sqlite3DbMallocRawNN`) — caught deterministically by ASan. Lifetime locking
// eliminates the interleave.
//
// The mutex is RECURSIVE because `ResultRepository::insert_recursive` opens a
// statement, then recurses into subtests opening further statements on the SAME
// thread (and `upsert_run_summary` opens one inside `insert`); same-thread
// re-acquisition must not self-deadlock. Cross-thread serialization is preserved:
// a second thread's PreparedStatement blocks at its constructor until the first
// thread's statement is destroyed.
//
// Tradeoff: a long SELECT iteration holds the mutex for the whole loop,
// blocking other threads. Phase-1 result sets are tiny (one run's results); the
// starvation concern is deferred to Phase 2 (trend/distribution queries), where
// per-thread connections or shorter critical sections can be revisited.

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
// loop. The connection mutex is held for the statement's whole lifetime (see the
// threading note above), so the entire prepare->bind->step->fetch sequence is
// one serialized critical section.
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
    sqlite3*      db_{nullptr};   // captured under the lock in the ctor; for error msgs
    std::unique_lock<std::recursive_mutex> lock_;  // held for the statement's lifetime
};

// Convenience: a scoped lock on the connection mutex. With PreparedStatement now
// holding the lock for its lifetime, this is rarely needed directly; it remains
// for cases that must serialize a sequence of raw sqlite3 calls as one unit.
class [[nodiscard]] DbLock {
public:
    DbLock();
    ~DbLock();
    DbLock(const DbLock&) = delete;
    DbLock& operator=(const DbLock&) = delete;
private:
    std::unique_lock<std::recursive_mutex> lock_;
};

}  // namespace arches::server::db