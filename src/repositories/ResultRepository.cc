#include "repositories/ResultRepository.h"
#include "db/DbClient.h"
#include "util/Uuid.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace arches::server::repositories {

namespace {

using arches::server::db::PreparedStatement;
using arches::server::util::generate_uuid;

// Fixed SELECT column order used by find_by_id and get_subtests.
// KEEP IN SYNC with read_result() below.
constexpr const char* kSelectColumns =
    "id, run_id, module_id, module_version, timestamp, mode, "
    "status, severity, test_id, test_name, sequence, duration_ns, error_message, "
    "reason, metrics, monitoring, event, parent_id, "
    "payload_bytes, payload_type_url";

// Read a row from the current cursor position into a Result.
Result read_result(const PreparedStatement& stmt) {
    Result r;
    r.id               = stmt.column_text(0);
    r.run_id           = stmt.column_text(1);
    r.module_id        = stmt.column_text(2);
    r.module_version   = stmt.column_text(3);
    r.timestamp       = stmt.column_text(4);
    r.mode            = stmt.column_text(5);
    r.status          = static_cast<std::int16_t>(stmt.column_int(6));
    r.severity        = static_cast<std::int16_t>(stmt.column_int(7));
    r.test_id         = stmt.column_text(8);
    r.test_name       = stmt.column_text(9);
    r.sequence        = stmt.column_int64(10);
    r.duration_ns     = stmt.column_int64(11);
    r.error_message   = stmt.column_text(12);
    r.reason_json     = stmt.column_text(13);
    r.metrics_json    = stmt.column_text(14);
    r.monitoring_json = stmt.column_text(15);
    r.event_json      = stmt.column_text(16);
    r.parent_id       = stmt.column_is_null(17)
                           ? std::nullopt
                           : std::optional<std::string>(stmt.column_text(17));
    auto blob = stmt.column_blob(18);  // vector<uint8_t>
    r.payload_bytes.assign(blob.begin(), blob.end());
    r.payload_type_url = stmt.column_text(19);
    return r;
}

}  // namespace

void ResultRepository::insert_recursive(Result node, bool is_top_level) {
    // Assign an id to any node that lacks one (the server assigns ids; proto_to_
    // result leaves them empty). Covers both top-level and subtests.
    if (node.id.empty()) node.id = generate_uuid();

    PreparedStatement stmt(
        "INSERT INTO results ("
        "  id, run_id, module_id, module_version, timestamp, mode,"
        "  status, severity, test_id, test_name, sequence, duration_ns, error_message,"
        "  reason, metrics, monitoring, event, parent_id,"
        "  payload_bytes, payload_type_url"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    stmt.bind(1, node.id);
    stmt.bind(2, node.run_id);
    stmt.bind(3, node.module_id);
    stmt.bind(4, node.module_version);
    stmt.bind_text_or_null(5, node.timestamp.empty() ? std::nullopt
                                                     : std::optional<std::string>(node.timestamp));
    stmt.bind(6, node.mode);
    // The `status > 0` invariant (Postgres enforced via a DB CHECK; SQLite can't
    // ADD CONSTRAINT, see migration 0010) is guaranteed by the service layer
    // (HAL never produces TEST_STATUS_UNSPECIFIED). The repo stores what it's
    // given; it does not coerce, to preserve auditability of the raw value.
    stmt.bind(7, static_cast<int>(node.status));
    stmt.bind(8, static_cast<int>(node.severity));
    stmt.bind(9, node.test_id);
    stmt.bind(10, node.test_name);
    stmt.bind(11, node.sequence);
    stmt.bind(12, node.duration_ns);
    stmt.bind_text_or_null(13, node.error_message.empty() ? std::nullopt
                                                          : std::optional<std::string>(node.error_message));
    stmt.bind(14, node.reason_json.empty() ? "{}" : node.reason_json);
    stmt.bind(15, node.metrics_json.empty() ? "{}" : node.metrics_json);
    stmt.bind(16, node.monitoring_json.empty() ? "{}" : node.monitoring_json);
    stmt.bind(17, node.event_json.empty() ? "{}" : node.event_json);
    stmt.bind_text_or_null(18, node.parent_id);
    if (node.payload_bytes.empty()) {
        stmt.bind(19, nullptr);
    } else {
        stmt.bind_blob(19, node.payload_bytes.data(), node.payload_bytes.size());
    }
    stmt.bind(20, node.payload_type_url);
    stmt.step();  // exhausts the INSERT (one row)

    // Upsert the per-(run,module,test) summary for top-level results only.
    // Subtests inherit the parent's run/module/test grouping; updating the
    // summary on each subtest would inflate result_count incorrectly.
    if (is_top_level) {
        upsert_run_summary(node.run_id, node.module_id, node.test_id,
                           node.status, node.severity,
                           node.timestamp.empty() ? "" : node.timestamp);
    }

    // Recurse into subtests with parent_id = this row's (now-assigned) id.
    for (auto& sub : node.subtests) {
        sub.parent_id = node.id;
        insert_recursive(std::move(sub), /*is_top_level=*/false);
    }
}

Result ResultRepository::insert(const Result& r) {
    insert_recursive(r, /*is_top_level=*/true);
    return r;
}

std::optional<Result> ResultRepository::find_by_id(const std::string& id) {
    PreparedStatement stmt(std::string("SELECT ") + kSelectColumns +
                           " FROM results WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) return std::nullopt;
    return read_result(stmt);
}

std::int64_t ResultRepository::count_for_run(const std::string& run_id) {
    PreparedStatement stmt("SELECT count(*) FROM results WHERE run_id = ?");
    stmt.bind(1, run_id);
    stmt.step();
    return stmt.column_int64(0);
}

std::vector<RunModuleSummary> ResultRepository::find_last_per_module(const std::string& run_id) {
    PreparedStatement stmt(
        "SELECT module_id, test_id, last_status, last_severity, last_timestamp, result_count "
        "FROM run_summaries WHERE run_id = ? ORDER BY module_id, test_id");
    stmt.bind(1, run_id);
    std::vector<RunModuleSummary> out;
    while (stmt.step()) {
        RunModuleSummary s;
        s.module_id      = stmt.column_text(0);
        s.test_id        = stmt.column_text(1);
        s.last_status    = static_cast<std::int16_t>(stmt.column_int(2));
        s.last_severity   = static_cast<std::int16_t>(stmt.column_int(3));
        s.last_timestamp = stmt.column_text(4);
        s.result_count    = stmt.column_int64(5);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<Result> ResultRepository::get_subtests(const std::string& parent_id) {
    PreparedStatement stmt(std::string("SELECT ") + kSelectColumns +
                           " FROM results WHERE parent_id = ? ORDER BY sequence");
    stmt.bind(1, parent_id);
    std::vector<Result> out;
    while (stmt.step()) out.push_back(read_result(stmt));
    return out;
}

void ResultRepository::upsert_run_summary(const std::string& run_id,
                                          const std::string& module_id,
                                          const std::string& test_id,
                                          std::int16_t status,
                                          std::int16_t severity,
                                          const std::string& timestamp) {
    PreparedStatement stmt(
        "INSERT INTO run_summaries "
        "  (run_id, module_id, test_id, last_status, last_severity, last_timestamp, result_count) "
        "VALUES (?,?,?,?,?,?,1) "
        "ON CONFLICT(run_id, module_id, test_id) DO UPDATE SET "
        "  last_status   = excluded.last_status, "
        "  last_severity  = excluded.last_severity, "
        "  last_timestamp = excluded.last_timestamp, "
        "  result_count   = run_summaries.result_count + 1");
    stmt.bind(1, run_id);
    stmt.bind(2, module_id);
    stmt.bind(3, test_id);
    stmt.bind(4, static_cast<int>(status));
    stmt.bind(5, static_cast<int>(severity));
    stmt.bind_text_or_null(6, timestamp.empty() ? std::nullopt
                                                : std::optional<std::string>(timestamp));
    stmt.step();
}

}  // namespace arches::server::repositories