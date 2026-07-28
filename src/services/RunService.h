#pragma once
// services/RunService.h — run lifecycle management (Phase-1 subset).
//
// Ported from the prior reference implementation with the two interop gap-closures the plan requires:
//   - run-control push: start_run/pause/resume/stop/restart push ServerMessages to
//     the owning client's ClientService stream via ClientStreamRegistry (the prior
//     reference implementation never did this — it only sent inline acks — so the Arches client, which
//     drives runs exclusively from those messages, sat idle).
//   - batch.state() mapping: set_run_status maps the client's RunState enum to the
//     runs.status TEXT column instead of hardcoding "completed" (the prior reference implementation's blind
//     complete_run("completed") on every terminal batch).
//
// Dropped vs the prior reference implementation (Phase 2 / Dashboard surface): the result-subscriber live-tail
// (broadcast_result / subscribe_results), trend/distribution/compare analytics,
// rerun, set_baseline, delete_run, notification classification helpers
// (is_terminal_failure / is_warning / module_badge), and the audit/notification
// repos (auth + notifications are Phase 2).

#include "repositories/RunRepository.h"
#include "repositories/ResultRepository.h"
#include "stream_registry/ClientStreamRegistry.h"
#include "stream_registry/RunStreamRegistry.h"

#include "client_service.pb.h"
#include "common.pb.h"

#include <google/protobuf/any.pb.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace arches::server {

class RunService {
public:
    // `run_stream_registry` is the SSE viewer fan-out for live run monitoring.
    // It may be a no-op registry when the HTTP layer is not wired (e.g. unit
    // tests of static mapping), but in production main.cpp wires the real one.
    explicit RunService(grpc::ClientStreamRegistry& stream_registry,
                        grpc::RunStreamRegistry&    run_stream_registry);

    // ── Run lifecycle ────────────────────────────────────────────────────────

    // Create a Run row (board_id owns it, status="queued") and push ConfigureRun
    // then StartRun to the client's stream. `common` + `module_config` are the
    // run's configuration; `module_config` is the hal.modules.<id>.Config packed
    // in an Any (empty Any for modules with no config). Returns the created Run
    // (id assigned). If the board is not connected, the run row is still created
    // (status "queued") and the push is logged + skipped — a reconnecting client
    // is not fed, but the run is traceable. Phase 1: MONITORING/EVENT_BASED need
    // monitoring/condition params not carried here; use SINGLE_SHOT/LONG_TERM.
    Run start_run(const std::string& board_id,
                  const std::string& module_id,
                  hal::common::ExecutionMode mode,
                  const hal::common::CommonConfig& common,
                  const google::protobuf::Any& module_config,
                  const std::string& name = "Run");

    Run get_run(const std::string& id);

    // Transition run -> running (sets started_at).
    void mark_running(const std::string& run_id);

    // ── Result ingest ────────────────────────────────────────────────────────

    // Persist an already-converted Result struct (legacy path for HalServiceImpl's
    // envelope fallback). Inserts recursively with subtests.
    Result ingest_result(const Result& r);

    // Primary ingest path: persist a TestResult proto received from the Client.
    // Converts the proto to a Result struct (proto_to_result), inserts with
    // subtests, returns the saved row. Rejects UNSPECIFIED status with
    // ValidationError (the Client should never produce it).
    Result ingest_test_result(const hal::common::TestResult& tr,
                              const std::string& run_id,
                              const std::string& module_version,
                              const std::string& mode);

    // ── Run-control push to the client stream ─────────────────────────────────

    void pause_run(const std::string& run_id);
    void resume_run(const std::string& run_id);
    void stop_run(const std::string& run_id);
    void restart_run(const std::string& old_run_id, const std::string& new_run_id);

    // ── Status finalization ───────────────────────────────────────────────────

    // Legacy finalizer (HalServiceImpl): set ended_at + status + error.
    void complete_run(const std::string& run_id, const std::string& status,
                      const std::string& error = "");

    void cancel_run(const std::string& run_id);

    // NEW (gap-closure #2): map the client's RunState -> runs.status. Terminal
    // states (COMPLETED/FAILED/CANCELLED) set ended_at; RUNNING sets started_at
    // (status="running"); PAUSED/CONFIGURED update status only. UNSPECIFIED is
    // rejected (no-op + warn). Called by ClientServiceImpl on terminal batches.
    void set_run_status(const std::string& run_id, arches::stream::RunState state);

    // Pure mapping functions (exposed for RunStateMappingTest — no DB needed).
    static std::string run_state_to_status(arches::stream::RunState s);
    static bool run_state_is_terminal(arches::stream::RunState s);

    // ── Queries ────────────────────────────────────────────────────────────────

    std::optional<Result> get_result(const std::string& id);
    std::vector<RunModuleSummary> get_module_summaries(const std::string& run_id);
    std::vector<Result> get_subtests(const std::string& parent_id);

    // SSE live-run endpoint support: replay already-landed top-level results
    // for a run (oldest first) and the total result count. Read-only on the
    // shared SQLite connection via PreparedStatement's lifetime-locked mutex.
    std::vector<Result> list_results(const std::string& run_id);
    std::int64_t        count_results(const std::string& run_id);

    // ── Minimal HTTP run-control surface (live-run monitoring loop) ───────────
    // These create / transition / ingest rows WITHOUT going through the gRPC
    // ClientService stream, so the HTTP live-run view and its contract tests
    // can drive a run end-to-end over HTTP when no embedded Client is connected.
    // The full REST API remains Phase 2; these are the minimal write-side
    // companions to the SSE read endpoint.
    Run create_run_row(const std::string& mode,
                       const std::string& board_id,
                       const std::string& name);
    // Inject a synthetic result row for a run (assigns id, persists, broadcasts
    // the `result` SSE event + a `module.transition` running event on first
    // sight of the module). `status` is a hal::common::TestStatus int (1=PASS…).
    Result inject_result(const std::string& run_id,
                         const std::string& module_id,
                         const std::string& test_id,
                         const std::string& test_name,
                         std::int16_t status,
                         std::int64_t sequence,
                         const std::string& mode);

    // On-startup reconciliation: mark any runs still 'running' or 'queued' as
    // 'failed' (the server crashed before completing them). Returns the count.
    int reconcile_stale_runs();

private:
    // Push a ServerMessage to the board owning `run_id`. Looks up the run's
    // board_id, then the stream. Logs + returns false if unreachable.
    bool push_to_owner(const std::string& run_id,
                       const arches::stream::ServerMessage& msg);

    // ── SSE live-run publishing helpers (non-blocking fan-out) ───────────────
    // Publish a `result` event (and a `module.transition` running event when a
    // module is seen for the first time in this run). Called from the ingest
    // path after the row is persisted.
    void publish_result(const std::string& run_id, const Result& saved);
    // Publish a `run.start` event for a run that just transitioned to running.
    void publish_run_start(const std::string& run_id);
    // Publish `module.transition` done events for every seen module, then a
    // terminal `run.complete` event. Called from every terminal path.
    void publish_run_complete(const std::string& run_id, const std::string& status);

    // Track which module_ids have been seen per run (for module.transition
    // running/done signals). Returns true if `module_id` was newly inserted.
    bool note_module(const std::string& run_id, const std::string& module_id);
    std::vector<std::string> seen_modules(const std::string& run_id);

    repositories::RunRepository    run_repo_;
    repositories::ResultRepository result_repo_;
    grpc::ClientStreamRegistry&    stream_registry_;
    grpc::RunStreamRegistry&       run_stream_registry_;

    std::mutex                                                         seen_mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> seen_modules_;
};

}  // namespace arches::server