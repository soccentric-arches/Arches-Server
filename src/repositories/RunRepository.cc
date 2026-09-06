#include "repositories/RunRepository.h"
#include "db/DbClient.h"
#include "util/Errors.h"
#include "util/Time.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

namespace arches::server::repositories {

namespace {

using arches::server::db::PreparedStatement;

constexpr const char* kSelectColumns =
    "id, name, description, test_config_snapshot, mode, board_id, status, "
    "started_at, ended_at, error_message, tags, notes, baseline_flag, "
    "attached_files, created_at";

Run read_run(const PreparedStatement& stmt) {
    Run r;
    r.id = stmt.column_text(0);
    r.name = stmt.column_text(1);
    r.description = stmt.column_text(2);
    r.test_config_snapshot_json = stmt.column_text(3);
    r.mode = stmt.column_text(4);
    r.board_id = stmt.column_text(5);
    r.status = stmt.column_text(6);
    r.started_at = stmt.column_is_null(7) ? "" : stmt.column_text(7);
    r.ended_at = stmt.column_is_null(8) ? "" : stmt.column_text(8);
    r.error_message = stmt.column_is_null(9) ? "" : stmt.column_text(9);
    r.tags_json = stmt.column_text(10);
    r.notes = stmt.column_text(11);
    r.baseline_flag = stmt.column_int(12) != 0;
    r.attached_files_json = stmt.column_text(13);
    r.created_at = stmt.column_text(14);
    return r;
}

} // namespace

Run RunRepository::create(const Run& r) {
    if (r.id.empty()) {
        throw arches::server::ValidationError("RunRepository::create: run.id must be set");
    }
    PreparedStatement stmt("INSERT INTO runs ("
                           "  id, name, description, test_config_snapshot, mode, board_id, status,"
                           "  started_at, ended_at, error_message, tags, notes, baseline_flag,"
                           "  attached_files, created_at"
                           ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    stmt.bind(1, r.id);
    stmt.bind(2, r.name);
    stmt.bind(3, r.description);
    stmt.bind(4, r.test_config_snapshot_json.empty() ? "{}" : r.test_config_snapshot_json);
    stmt.bind(5, r.mode.empty() ? "single_shot" : r.mode);
    stmt.bind_text_or_null(6, r.board_id.empty() ? std::nullopt
                                                 : std::optional<std::string>(r.board_id));
    stmt.bind(7, r.status.empty() ? "queued" : r.status);
    stmt.bind_text_or_null(8, r.started_at.empty() ? std::nullopt
                                                   : std::optional<std::string>(r.started_at));
    stmt.bind_text_or_null(9, r.ended_at.empty() ? std::nullopt
                                                 : std::optional<std::string>(r.ended_at));
    stmt.bind_text_or_null(
        10, r.error_message.empty() ? std::nullopt : std::optional<std::string>(r.error_message));
    stmt.bind(11, r.tags_json.empty() ? "[]" : r.tags_json);
    stmt.bind(12, r.notes);
    stmt.bind(13, r.baseline_flag ? 1 : 0);
    stmt.bind(14, r.attached_files_json.empty() ? "[]" : r.attached_files_json);
    stmt.bind(15, r.created_at.empty() ? arches::server::util::utc_now_iso8601() : r.created_at);
    stmt.step();
    return r;
}

std::optional<Run> RunRepository::find_by_id(const std::string& id) {
    PreparedStatement stmt(std::string("SELECT ") + kSelectColumns + " FROM runs WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step())
        return std::nullopt;
    return read_run(stmt);
}

void RunRepository::update_status(const std::string& id, const std::string& status,
                                  const std::string& error_message) {
    PreparedStatement stmt(
        "UPDATE runs SET status = ?, error_message = NULLIF(?, '') WHERE id = ?");
    stmt.bind(1, status);
    stmt.bind(2, error_message);
    stmt.bind(3, id);
    stmt.step();
}

void RunRepository::set_started(const std::string& id) {
    PreparedStatement stmt("UPDATE runs SET status = 'running', started_at = "
                           "CURRENT_TIMESTAMP WHERE id = ?");
    stmt.bind(1, id);
    stmt.step();
}

void RunRepository::set_ended(const std::string& id, const std::string& status,
                              const std::string& error_message) {
    PreparedStatement stmt("UPDATE runs SET status = ?, ended_at = CURRENT_TIMESTAMP, "
                           "error_message = NULLIF(?, '') WHERE id = ?");
    stmt.bind(1, status);
    stmt.bind(2, error_message);
    stmt.bind(3, id);
    stmt.step();
}

std::vector<Run> RunRepository::list_by_status(const std::vector<std::string>& statuses) {
    std::vector<Run> out;
    if (statuses.empty())
        return out;

    // Build an IN (?, ?, ...) clause with one placeholder per status. Bound,
    // not string-concatenated (avoids the injection flavour the prior reference
    // implementation's find_distribution had).
    std::string sql = std::string("SELECT ") + kSelectColumns + " FROM runs WHERE status IN (";
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        sql += (i == 0 ? "?" : ",?");
    }
    sql += ") ORDER BY created_at DESC";

    PreparedStatement stmt(sql);
    int idx = 1;
    for (const auto& s : statuses)
        stmt.bind(idx++, s);
    while (stmt.step())
        out.push_back(read_run(stmt));
    return out;
}

} // namespace arches::server::repositories