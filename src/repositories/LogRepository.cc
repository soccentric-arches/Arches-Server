#include "repositories/LogRepository.h"
#include "db/DbClient.h"
#include "util/Time.h"

#include <stdexcept>
#include <string>

namespace arches::server::repositories {

namespace {

using arches::server::db::PreparedStatement;

LogEntry read_entry(const PreparedStatement& stmt) {
    LogEntry e;
    e.id            = stmt.column_text(0);
    e.source        = stmt.column_text(1);
    e.severity      = stmt.column_text(2);
    e.timestamp     = stmt.column_text(3);
    e.message       = stmt.column_text(4);
    e.metadata_json = stmt.column_text(5);
    e.run_id        = stmt.column_is_null(6) ? std::nullopt
                                            : std::optional<std::string>(stmt.column_text(6));
    return e;
}

constexpr const char* kEntryColumns =
    "id, source, severity, timestamp, message, metadata, run_id";

}  // namespace

LogEntry LogRepository::insert(const LogEntry& e) {
    if (e.id.empty()) {
        throw std::runtime_error("LogRepository::insert: entry.id must be set");
    }
    PreparedStatement stmt(
        "INSERT INTO log_entries "
        "  (id, source, severity, timestamp, message, metadata, run_id) "
        "VALUES (?,?,?,?,?,?,?)");
    stmt.bind(1, e.id);
    stmt.bind(2, e.source.empty() ? "app" : e.source);
    stmt.bind(3, e.severity.empty() ? "info" : e.severity);
    stmt.bind(4, e.timestamp.empty() ? arches::server::util::utc_now_iso8601() : e.timestamp);
    stmt.bind(5, e.message);
    stmt.bind(6, e.metadata_json.empty() ? "{}" : e.metadata_json);
    stmt.bind_text_or_null(7, e.run_id);
    stmt.step();
    return e;
}

// Builds the shared WHERE clause + bind sequence for list() and count(). The
// caller binds the params in the order the clauses are appended.
static std::string build_where(const LogFilter& f, std::vector<std::string>& binds) {
    std::string where;
    if (!f.source.empty())   { where += (where.empty()?"":" AND ") + std::string("source = ?");     binds.push_back(f.source); }
    if (!f.severity.empty()) { where += (where.empty()?"":" AND ") + std::string("severity = ?");   binds.push_back(f.severity); }
    if (!f.run_id.empty())   { where += (where.empty()?"":" AND ") + std::string("run_id = ?");     binds.push_back(f.run_id); }
    if (!f.from_ts.empty())  { where += (where.empty()?"":" AND ") + std::string("timestamp >= ?"); binds.push_back(f.from_ts); }
    if (!f.to_ts.empty())    { where += (where.empty()?"":" AND ") + std::string("timestamp <= ?"); binds.push_back(f.to_ts); }
    if (!f.query.empty())    { where += (where.empty()?"":" AND ") + std::string("message LIKE ?"); binds.push_back("%" + f.query + "%"); }
    return where;
}

std::vector<LogEntry> LogRepository::list(const LogFilter& f) {
    std::vector<std::string> binds;
    std::string where = build_where(f, binds);
    std::string sql = std::string("SELECT ") + kEntryColumns + " FROM log_entries";
    if (!where.empty()) sql += " WHERE " + where;
    sql += " ORDER BY timestamp DESC LIMIT ? OFFSET ?";

    PreparedStatement stmt(sql);
    int idx = 1;
    for (const auto& b : binds) stmt.bind(idx++, b);
    stmt.bind(idx++, f.limit);
    stmt.bind(idx++, f.offset);

    std::vector<LogEntry> out;
    while (stmt.step()) out.push_back(read_entry(stmt));
    return out;
}

std::int64_t LogRepository::count(const LogFilter& f) {
    std::vector<std::string> binds;
    std::string where = build_where(f, binds);
    std::string sql = "SELECT count(*) FROM log_entries";
    if (!where.empty()) sql += " WHERE " + where;

    PreparedStatement stmt(sql);
    int idx = 1;
    for (const auto& b : binds) stmt.bind(idx++, b);
    stmt.step();
    return stmt.column_int64(0);
}

}  // namespace arches::server::repositories