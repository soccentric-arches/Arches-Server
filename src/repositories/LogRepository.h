#pragma once
// repositories/LogRepository.h — SQLite port of Acadia-Server's LogRepository.
//
// Ingests client LogBatch lines (LogService) and serves log queries. Phase 1
// implements insert + list + count. The per-user bookmark / saved-view surfaces
// (Phase 2 Dashboard) are deferred.
//
// SQLite adaptation: Acadia's `to_tsvector('english', message) @@ plainto_tsquery`
// full-text search is replaced with `LIKE '%query%'` (message is short log text;
// FTS5 is a Phase-2 optimisation). Acadia's `count()` ignored its filter (a
// pre-existing bug); the port applies the filter.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arches::server {

struct LogEntry {
    std::string id;
    std::string source;
    std::string severity;
    std::string timestamp;     // ISO-8601
    std::string message;
    std::string metadata_json; // JSON object, default "{}"
    std::optional<std::string> run_id;  // FK to runs.id; nullopt for system logs
};

struct LogFilter {
    std::string source;       // empty = any
    std::string severity;      // empty = any
    std::string from_ts;       // ISO-8601 inclusive lower bound; empty = any
    std::string to_ts;        // ISO-8601 inclusive upper bound; empty = any
    std::string run_id;       // empty = any
    std::string query;        // substring on message; empty = any
    int limit  = 100;
    int offset = 0;
};

}  // namespace arches::server

namespace arches::server::repositories {

class LogRepository {
public:
    LogEntry insert(const LogEntry& e);
    std::vector<LogEntry> list(const LogFilter& f);
    std::int64_t count(const LogFilter& f);
};

}  // namespace arches::server::repositories