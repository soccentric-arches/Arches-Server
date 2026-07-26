#pragma once
// repositories/RunRepository.h — SQLite port of the prior reference implementation's RunRepository.
//
// A `Run` is one execution of a test configuration against the board. RunService
// creates a Run row when the server pushes a ConfigureRun/StartRun to the client,
// and updates its status as the client reports batch.state() (the §5/interop
// gap-closure: map RunState -> runs.status instead of hardcoding "completed").
//
// Arches adaptation vs the prior reference implementation: the `board_id` column replaces the prior reference implementation's user-facing
// `created_by` (the runs.user FK was dropped in the SQLite migration 0002 — auth
// is Phase 2). board_id records which connected client owns the run so the server
// can route run-control ServerMessages to the right ClientService stream.
//
// Phase 1 implements: create, find_by_id, update_status, set_started, set_ended,
// list_by_status (for reconcile_stale_runs). The full list-with-filter, notes,
// tags, baseline, and remove methods are deferred (Dashboard/Phase 2 surface).

#include <optional>
#include <string>
#include <vector>

namespace arches::server {

struct Run {
    std::string id;
    std::string name;
    std::string description;
    std::string test_config_snapshot_json;  // frozen snapshot of the TestConfig
    std::string mode;                       // single_shot | long_term | monitoring | event_based
    std::string board_id;                   // owning connected client (FK to board.id)
    std::string status;                     // queued|running|paused|completed|failed|cancelled
    std::string started_at;                 // ISO-8601, empty until set_started
    std::string ended_at;                   // ISO-8601, empty until set_ended
    std::string error_message;
    std::string tags_json;                  // JSON array, default "[]"
    std::string notes;
    bool baseline_flag = false;
    std::string attached_files_json;        // JSON array, default "[]"
    std::string created_at;                 // ISO-8601
};

}  // namespace arches::server

namespace arches::server::repositories {

class RunRepository {
public:
    Run create(const Run& r);
    std::optional<Run> find_by_id(const std::string& id);

    // Update status (and optional error_message). Empty error_message -> NULL.
    void update_status(const std::string& id, const std::string& status,
                       const std::string& error_message = "");

    // Set started_at = now, status = 'running'.
    void set_started(const std::string& id);

    // Set ended_at = now, status, optional error_message. Empty -> NULL.
    void set_ended(const std::string& id, const std::string& status,
                   const std::string& error_message = "");

    // All runs whose status is in `statuses`. Used by RunService::reconcile_
    // stale_runs to find queued/running runs left over from a crashed server.
    std::vector<Run> list_by_status(const std::vector<std::string>& statuses);
};

}  // namespace arches::server::repositories