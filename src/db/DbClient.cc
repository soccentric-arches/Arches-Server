#include "db/DbClient.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <sqlite3.h>

namespace arches::server::db {

namespace {

// The single shared connection and the recursive mutex guarding every call that
// touches it. Recursive so a PreparedStatement can hold it for its lifetime
// while same-thread code (subtest recursion, upsert-inside-insert) opens nested
// statements without self-deadlock; cross-thread statements still serialize.
// Initialised by init_db_client().
sqlite3* g_db = nullptr;
std::recursive_mutex g_db_mutex;

// RAII-safe busy handler: retry on SQLITE_BUSY for up to ~5s total, matching the
// busy_timeout PRAGMA. Used as a belt-and-braces alongside PRAGMA busy_timeout.
int busy_retry_callback(void* /*data*/, int /*count*/) {
    // sqlite3_busy_timeout already handles backoff; this is a no-op fallback.
    return 1;  // keep retrying
}

}  // namespace

void init_db_client(const std::string& db_path) {
    std::lock_guard<std::recursive_mutex> lk(g_db_mutex);

    if (g_db) {
        spdlog::warn("db: init_db_client() called twice — ignoring second init");
        return;
    }

    sqlite3* handle = nullptr;
    int rc = sqlite3_open_v2(
        db_path.c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        std::string err = handle ? sqlite3_errmsg(handle) : "cannot open database";
        if (handle) sqlite3_close(handle);
        throw std::runtime_error("db: failed to open SQLite '" + db_path + "': " + err);
    }

    // Enable extended result codes for richer error reporting.
    sqlite3_extended_result_codes(handle, 1);
    // 5s busy timeout so concurrent writers don't immediately fail with BUSY.
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_busy_handler(handle, &busy_retry_callback, nullptr);

    g_db = handle;

    // Apply connection-level pragmas. Each call is individually safe.
    auto pragma = [&](const char* sql) {
        char* err = nullptr;
        int prc = sqlite3_exec(handle, sql, nullptr, nullptr, &err);
        if (prc != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            if (err) sqlite3_free(err);
            sqlite3_close(handle);
            g_db = nullptr;
            throw std::runtime_error(std::string("db: PRAGMA '") + sql + "' failed: " + msg);
        }
    };

    // WAL for better read concurrency while a writer is active.
    pragma("PRAGMA journal_mode=WAL");
    // Normal sync is a good durability/speed tradeoff for a host server.
    pragma("PRAGMA synchronous=NORMAL");
    // Enforce FK constraints (cascades in the migrations rely on this).
    pragma("PRAGMA foreign_keys=ON");
    // Store timestamps as UTC ISO-8601 text consistently.
    pragma("PRAGMA timezone='UTC'");

    spdlog::info("db: opened SQLite '{}' (WAL, foreign_keys=ON)", db_path);
}

void shutdown_db_client() {
    std::lock_guard<std::recursive_mutex> lk(g_db_mutex);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
        spdlog::info("db: closed SQLite connection");
    }
}

sqlite3* get_db() {
    std::lock_guard<std::recursive_mutex> lk(g_db_mutex);
    if (!g_db) {
        throw std::runtime_error("db: DbClient not initialised — call init_db_client() first");
    }
    return g_db;
}

void exec(const std::string& sql) {
    sqlite3* db = get_db();
    std::lock_guard<std::recursive_mutex> lk(g_db_mutex);
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        if (err) sqlite3_free(err);
        throw std::runtime_error("db: exec failed: " + msg);
    }
}

// ---------------------------------------------------------------------------
// DbLock
// ---------------------------------------------------------------------------

DbLock::DbLock() : lock_(g_db_mutex) {}
DbLock::~DbLock() = default;

// ---------------------------------------------------------------------------
// PreparedStatement
// ---------------------------------------------------------------------------

PreparedStatement::PreparedStatement(const std::string& sql)
    : lock_(g_db_mutex) {  // held for the statement's whole lifetime
    if (!g_db) {
        throw std::runtime_error(
            "db: DbClient not initialised — call init_db_client() first");
    }
    db_ = g_db;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        stmt_ = nullptr;
        throw std::runtime_error("db: prepare failed: " + err + " — SQL: " + sql);
    }
}

PreparedStatement::~PreparedStatement() {
    // Still under lock_ here (released by lock_'s own dtor after this body).
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

PreparedStatement::PreparedStatement(PreparedStatement&& other) noexcept
    : stmt_(other.stmt_), db_(other.db_), lock_(std::move(other.lock_)) {
    other.stmt_ = nullptr;
    other.db_   = nullptr;
}

PreparedStatement& PreparedStatement::operator=(PreparedStatement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_  = other.stmt_;
        db_    = other.db_;
        lock_  = std::move(other.lock_);
        other.stmt_ = nullptr;
        other.db_   = nullptr;
    }
    return *this;
}

void PreparedStatement::check(int rc, const char* context) const {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "no connection";
        throw std::runtime_error(std::string("db: ") + context + " failed: " + err);
    }
}

void PreparedStatement::bind(int idx, std::nullptr_t) {
    check(sqlite3_bind_null(stmt_, idx), "bind null");
}
void PreparedStatement::bind(int idx, int value) {
    check(sqlite3_bind_int(stmt_, idx, value), "bind int");
}
void PreparedStatement::bind(int idx, int64_t value) {
    check(sqlite3_bind_int64(stmt_, idx, value), "bind int64");
}
void PreparedStatement::bind(int idx, double value) {
    check(sqlite3_bind_double(stmt_, idx, value), "bind double");
}
void PreparedStatement::bind(int idx, const std::string& value) {
    // SQLITE_TRANSIENT makes sqlite copy the bytes.
    check(sqlite3_bind_text(stmt_, idx, value.c_str(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "bind text");
}
void PreparedStatement::bind(int idx, const char* value) {
    if (value == nullptr) {
        check(sqlite3_bind_null(stmt_, idx), "bind null");
    } else {
        check(sqlite3_bind_text(stmt_, idx, value, -1, SQLITE_TRANSIENT), "bind text");
    }
}
void PreparedStatement::bind_text_or_null(int idx, const std::optional<std::string>& value) {
    if (value) bind(idx, *value);
    else bind(idx, nullptr);
}
void PreparedStatement::bind_int64_or_null(int idx, const std::optional<int64_t>& value) {
    if (value) bind(idx, *value);
    else bind(idx, nullptr);
}
void PreparedStatement::bind_blob(int idx, const void* data, std::size_t len) {
    check(sqlite3_bind_blob(stmt_, idx, data, static_cast<int>(len), SQLITE_TRANSIENT),
          "bind blob");
}
void PreparedStatement::bind_blob(int idx, const std::vector<std::uint8_t>& blob) {
    bind_blob(idx, blob.empty() ? nullptr : blob.data(), blob.size());
}

bool PreparedStatement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    std::string err = db_ ? sqlite3_errmsg(db_) : "no connection";
    throw std::runtime_error("db: step failed: " + err);
}

void PreparedStatement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

int PreparedStatement::column_int(int idx) const {
    return sqlite3_column_int(stmt_, idx);
}
int64_t PreparedStatement::column_int64(int idx) const {
    return sqlite3_column_int64(stmt_, idx);
}
double PreparedStatement::column_double(int idx) const {
    return sqlite3_column_double(stmt_, idx);
}
std::string PreparedStatement::column_text(int idx) const {
    const unsigned char* text = sqlite3_column_text(stmt_, idx);
    if (!text) return {};
    int n = sqlite3_column_bytes(stmt_, idx);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(n));
}
std::vector<std::uint8_t> PreparedStatement::column_blob(int idx) const {
    const void* blob = sqlite3_column_blob(stmt_, idx);
    int n = sqlite3_column_bytes(stmt_, idx);
    if (!blob || n <= 0) return {};
    const auto* p = static_cast<const std::uint8_t*>(blob);
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(n));
}
bool PreparedStatement::column_is_null(int idx) const {
    return sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
}
int PreparedStatement::column_count() const {
    return sqlite3_column_count(stmt_);
}

}  // namespace arches::server::db