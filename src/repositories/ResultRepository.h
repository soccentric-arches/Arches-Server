#pragma once
// repositories/ResultRepository.h — SQLite port of the prior reference implementation's ResultRepository.
//
// The `Result` struct is the projection of a single HAL TestResult row in the
// `results` table (plus its packed module-specific payload). It is the unit the
// §5 module-Result unpack produces: ResultConvert::unpack_module_results yields a
// list of hal::common::TestResult, ResultConvert::proto_to_result maps each to a
// `Result`, and ResultRepository::insert persists it (recursively, for subtests).
//
// The struct shape mirrors the prior reference implementation's exactly so ResultConvert ports
// unchanged. Phase 1 implements the interop-critical methods (insert with
// recursive subtests + run_summaries upsert, count_for_run, find_last_per_module,
// upsert_run_summary, find_by_id, get_subtests). The trend/distribution/compare
// query methods are Phase 2 (anomaly/compare services) and are deferred.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arches::server {

// Projection of one row in `results` (a single HAL TestResult).
struct Result {
    std::string id;
    std::string run_id;
    std::string module_id;
    std::string module_version;
    std::string timestamp;        // ISO-8601 text
    std::string mode;             // single_shot | long_term | monitoring | event_based

    std::int16_t status    = 0;    // hal::common::TestStatus enum int (0 disallowed on store)
    std::int16_t severity  = 0;    // hal::common::TestSeverity enum int
    std::string test_id;
    std::string test_name;
    std::int64_t sequence    = 0;
    std::int64_t duration_ns = 0;
    std::string error_message;

    // JSON-encoded TEXT columns (SQLite has no JSONB; values are JSON strings).
    std::string reason_json;
    std::string metrics_json;
    std::string monitoring_json;
    std::string event_json;

    std::optional<std::string> parent_id;  // FK to results.id for subtests; nullopt for top-level

    // The raw serialized proto bytes of the module-specific Result message
    // (hal.modules.<id>.Result) packed into a google.protobuf.Any, plus the
    // Any's type_url. The §5 wire contract. Persisted as BLOB + TEXT.
    std::vector<char> payload_bytes;
    std::string payload_type_url;

    // In-memory only (subtests are not a separate query in Phase 1 insert path;
    // populated by get_subtests for callers that want the tree).
    std::vector<Result> subtests;
};

// Per-(run,module,test) latest-status summary, read from the run_summaries table.
struct RunModuleSummary {
    std::string module_id;
    std::string test_id;
    std::int16_t last_status   = 0;
    std::int16_t last_severity  = 0;
    std::string last_timestamp;
    std::int64_t result_count   = 0;
};

}  // namespace arches::server

namespace arches::server::repositories {

class ResultRepository {
public:
    // Insert a top-level Result (and recursively its subtests, setting each
    // subtest's parent_id to the parent's id). After each top-level insert,
    // upserts the run_summaries row. The caller must set r.id (server-assigned
    // via util::generate_uuid). Returns the inserted Result (with subtests
    // populated for the in-memory tree).
    Result insert(const Result& r);

    std::optional<Result> find_by_id(const std::string& id);
    std::int64_t count_for_run(const std::string& run_id);

    // Latest result summary per (module, test) for a run — reads run_summaries.
    std::vector<RunModuleSummary> find_last_per_module(const std::string& run_id);

    // Direct subtests of a top-level result.
    std::vector<Result> get_subtests(const std::string& parent_id);

    // Upsert the run_summaries row for (run_id, module_id, test_id). Called by
    // insert() after each top-level result; exposed for direct use by tests.
    void upsert_run_summary(const std::string& run_id,
                            const std::string& module_id,
                            const std::string& test_id,
                            std::int16_t status,
                            std::int16_t severity,
                            const std::string& timestamp);

private:
    // Recursive helper: insert `node` (assigning an id if absent), then subtests
    // with parent_id = node.id. Each top-level result also upserts its run_summary.
    // Takes Result by value so it can assign ids in place.
    void insert_recursive(Result node, bool is_top_level);
};

}  // namespace arches::server::repositories