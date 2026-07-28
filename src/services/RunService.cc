#include "services/RunService.h"

#include "util/Errors.h"
#include "util/ResultConvert.h"
#include "util/Time.h"
#include "util/Uuid.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

namespace arches::server {

namespace {

using arches::server::util::generate_uuid;

// ExecutionMode enum -> runs.mode TEXT name. File-local: no new public surface.
// (The client's batch.mode() -> results.mode path keeps the prior reference implementation's int-string form;
//  see the porting note quirk. runs.mode always uses the readable name.)
std::string execution_mode_name(hal::common::ExecutionMode m) {
    switch (m) {
    case hal::common::SINGLE_SHOT: return "single_shot";
    case hal::common::LONG_TERM:    return "long_term";
    case hal::common::MONITORING:   return "monitoring";
    case hal::common::EVENT_BASED:  return "event_based";
    default:                        return "unspecified";
    }
}

}  // namespace

RunService::RunService(grpc::ClientStreamRegistry& stream_registry,
                       grpc::RunStreamRegistry&    run_stream_registry)
    : stream_registry_(stream_registry),
      run_stream_registry_(run_stream_registry) {}

Run RunService::start_run(const std::string& board_id,
                           const std::string& module_id,
                           hal::common::ExecutionMode mode,
                           const hal::common::CommonConfig& common,
                           const google::protobuf::Any& module_config,
                           const std::string& name) {
    Run r;
    r.id          = generate_uuid();
    r.name        = name.empty() ? "Run" : name;
    r.mode        = execution_mode_name(mode);
    r.board_id    = board_id;
    r.status      = "queued";
    // Snapshot the configuration into the run row for traceability/audit.
    // Phase 1 stores a minimal JSON envelope; the real structured snapshot is
    // carried in the pushed ConfigureRun. (Dashboard/Phase 2 will formalize this.)
    r.test_config_snapshot_json = "{}";
    auto created = run_repo_.create(r);
    spdlog::info("run: created id={} board_id={} module_id={} mode={}",
                 created.id, board_id, module_id, created.mode);

    // Push ConfigureRun, then StartRun, to the owning client's stream.
    arches::stream::ServerMessage cfg_msg;
    auto* cfg = cfg_msg.mutable_configure_run();
    cfg->set_run_id(created.id);
    cfg->set_module_id(module_id);
    cfg->set_mode(mode);
    *cfg->mutable_common() = common;
    *cfg->mutable_module_config() = module_config;
    if (!push_to_owner(created.id, cfg_msg)) {
        spdlog::warn("run: board_id={} not connected; ConfigureRun for run_id={} not delivered",
                     board_id, created.id);
    }

    arches::stream::ServerMessage start_msg;
    start_msg.mutable_start_run()->set_run_id(created.id);
    if (!push_to_owner(created.id, start_msg)) {
        spdlog::warn("run: board_id={} not connected; StartRun for run_id={} not delivered",
                     board_id, created.id);
    }

    return created;
}

Run RunService::get_run(const std::string& id) {
    auto opt = run_repo_.find_by_id(id);
    if (!opt) throw NotFoundError("run " + id);
    return *opt;
}

void RunService::mark_running(const std::string& run_id) {
    run_repo_.set_started(run_id);
    spdlog::info("run: started id={}", run_id);
    publish_run_start(run_id);
}

Result RunService::ingest_result(const Result& r) {
    auto saved = result_repo_.insert(r);
    publish_result(r.run_id, saved);
    return saved;
}

Result RunService::ingest_test_result(const hal::common::TestResult& tr,
                                        const std::string& run_id,
                                        const std::string& module_version,
                                        const std::string& mode) {
    // Guard: reject UNSPECIFIED status — the Client should never produce this.
    if (tr.output().status() == hal::common::TEST_STATUS_UNSPECIFIED) {
        throw ValidationError("TestResult.output.status must not be UNSPECIFIED");
    }

    Result r = util::proto_to_result(tr, run_id, module_version, mode);
    auto saved = result_repo_.insert(r);
    spdlog::debug("run: ingested result run_id={} module_id={} test_id={} status={}",
                  run_id, saved.module_id, saved.test_id, saved.status);
    publish_result(run_id, saved);
    return saved;
}

void RunService::pause_run(const std::string& run_id) {
    arches::stream::ServerMessage msg;
    msg.mutable_pause_run()->set_run_id(run_id);
    push_to_owner(run_id, msg);
}

void RunService::resume_run(const std::string& run_id) {
    arches::stream::ServerMessage msg;
    msg.mutable_resume_run()->set_run_id(run_id);
    push_to_owner(run_id, msg);
}

void RunService::stop_run(const std::string& run_id) {
    arches::stream::ServerMessage msg;
    msg.mutable_stop_run()->set_run_id(run_id);
    push_to_owner(run_id, msg);
}

void RunService::restart_run(const std::string& old_run_id,
                               const std::string& new_run_id) {
    arches::stream::ServerMessage msg;
    auto* r = msg.mutable_restart_run();
    r->set_old_run_id(old_run_id);
    r->set_new_run_id(new_run_id);
    push_to_owner(old_run_id, msg);
}

void RunService::complete_run(const std::string& run_id, const std::string& status,
                                const std::string& error) {
    run_repo_.set_ended(run_id, status, error);
    spdlog::info("run: {} id={}", status, run_id);
    publish_run_complete(run_id, status);
}

void RunService::cancel_run(const std::string& run_id) {
    run_repo_.update_status(run_id, "cancelled");
    spdlog::info("run: cancelled id={}", run_id);
    publish_run_complete(run_id, "cancelled");
}

// static
std::string RunService::run_state_to_status(arches::stream::RunState s) {
    switch (s) {
    case arches::stream::COMPLETED:            return "completed";
    case arches::stream::FAILED:               return "failed";
    case arches::stream::CANCELLED:            return "cancelled";
    case arches::stream::PAUSED:               return "paused";
    case arches::stream::RUNNING:              return "running";
    case arches::stream::CONFIGURED:           return "queued";
    case arches::stream::RUN_STATE_UNSPECIFIED: return "unspecified";
    // protobuf open enums emit _INT_MIN/MAX_SENTINEL_DO_NOT_USE_ values; a
    // default clause silences -Wswitch over those sentinels.
    default:                                   return "unspecified";
    }
}

// static
bool RunService::run_state_is_terminal(arches::stream::RunState s) {
    return s == arches::stream::COMPLETED
        || s == arches::stream::FAILED
        || s == arches::stream::CANCELLED;
}

void RunService::set_run_status(const std::string& run_id,
                                  arches::stream::RunState state) {
    if (state == arches::stream::RUN_STATE_UNSPECIFIED) {
        spdlog::warn("run: ignoring UNSPECIFIED RunState for run_id={}", run_id);
        return;
    }
    const std::string status = run_state_to_status(state);
    if (run_state_is_terminal(state)) {
        run_repo_.set_ended(run_id, status);
    } else if (state == arches::stream::RUNNING) {
        run_repo_.set_started(run_id);  // status="running" + started_at=now
    } else {
        run_repo_.update_status(run_id, status);  // CONFIGURED->queued, PAUSED->paused
    }
    spdlog::info("run: state->status run_id={} state={} -> {}",
                 run_id, static_cast<int>(state), status);

    // Broadcast the transition to SSE viewers: RUNNING -> run.start, terminal ->
    // run.complete. PAUSED/CONFIGURED produce no SSE event (no terminal banner,
    // and the dashboard reads status from run.start/run.complete only).
    if (state == arches::stream::RUNNING) {
        publish_run_start(run_id);
    } else if (run_state_is_terminal(state)) {
        publish_run_complete(run_id, status);
    }
}

std::optional<Result> RunService::get_result(const std::string& id) {
    return result_repo_.find_by_id(id);
}

std::vector<RunModuleSummary> RunService::get_module_summaries(const std::string& run_id) {
    return result_repo_.find_last_per_module(run_id);
}

std::vector<Result> RunService::get_subtests(const std::string& parent_id) {
    return result_repo_.get_subtests(parent_id);
}

std::vector<Result> RunService::list_results(const std::string& run_id) {
    return result_repo_.list_for_run(run_id);
}

std::int64_t RunService::count_results(const std::string& run_id) {
    return result_repo_.count_for_run(run_id);
}

Run RunService::create_run_row(const std::string& mode,
                                const std::string& board_id,
                                const std::string& name) {
    Run r;
    r.id       = generate_uuid();
    r.name     = name.empty() ? "Run" : name;
    r.mode     = mode.empty() ? "single_shot" : mode;
    r.board_id = board_id;
    r.status   = "queued";
    r.test_config_snapshot_json = "{}";
    auto created = run_repo_.create(r);
    spdlog::info("run: http-created id={} board_id={} mode={}",
                 created.id, board_id, created.mode);
    return created;
}

Result RunService::inject_result(const std::string& run_id,
                                  const std::string& module_id,
                                  const std::string& test_id,
                                  const std::string& test_name,
                                  std::int16_t status,
                                  std::int64_t sequence,
                                  const std::string& mode) {
    Result r;
    r.run_id         = run_id;
    r.module_id      = module_id;
    r.test_id        = test_id;
    r.test_name      = test_name;
    r.status         = status;
    r.severity       = 0;
    r.sequence       = sequence;
    r.mode           = mode.empty() ? "single_shot" : mode;
    r.timestamp      = util::utc_now_iso8601();
    r.parent_id      = std::nullopt;
    auto saved = result_repo_.insert(r);
    publish_result(run_id, saved);
    spdlog::debug("run: http-injected result run_id={} module_id={} status={}",
                  run_id, saved.module_id, saved.status);
    return saved;
}

int RunService::reconcile_stale_runs() {
    auto stale = run_repo_.list_by_status({"running", "queued"});
    for (const auto& r : stale) {
        run_repo_.update_status(r.id, "failed",
                                "Server restarted — run was interrupted");
    }
    if (!stale.empty()) {
        spdlog::info("run: reconciled {} stale run(s) -> failed", stale.size());
    }
    return static_cast<int>(stale.size());
}

bool RunService::push_to_owner(const std::string& run_id,
                                const arches::stream::ServerMessage& msg) {
    auto opt = run_repo_.find_by_id(run_id);
    if (!opt) {
        spdlog::warn("run: push_to_owner run_id={} not found", run_id);
        return false;
    }
    if (opt->board_id.empty()) {
        spdlog::warn("run: push_to_owner run_id={} has no board_id", run_id);
        return false;
    }
    return stream_registry_.push(opt->board_id, msg);
}

// ── SSE live-run publishing (non-blocking fan-out via RunStreamRegistry) ─────

bool RunService::note_module(const std::string& run_id, const std::string& module_id) {
    std::lock_guard<std::mutex> lock{seen_mutex_};
    return seen_modules_[run_id].insert(module_id).second;
}

std::vector<std::string> RunService::seen_modules(const std::string& run_id) {
    std::lock_guard<std::mutex> lock{seen_mutex_};
    std::vector<std::string> out;
    auto it = seen_modules_.find(run_id);
    if (it != seen_modules_.end()) {
        out.assign(it->second.begin(), it->second.end());
    }
    return out;
}

void RunService::publish_result(const std::string& run_id, const Result& saved) {
    // A new module seen in this run -> emit a module.transition running event
    // before the result so a viewer sees pending->running->result.
    if (note_module(run_id, saved.module_id)) {
        grpc::SseEvent tr;
        tr.name = "module.transition";
        tr.data = "{\"module_id\":\"" + util::json_escape_string(saved.module_id) +
                  "\",\"state\":\"running\"}";
        tr.critical = false;
        run_stream_registry_.publish(run_id, tr);
    }
    grpc::SseEvent ev;
    ev.name      = "result";
    ev.data      = util::result_to_json(saved);
    ev.critical  = false;
    ev.dedup_key = saved.id;
    run_stream_registry_.publish(run_id, ev);
}

void RunService::publish_run_start(const std::string& run_id) {
    auto opt = run_repo_.find_by_id(run_id);
    if (!opt) return;
    // Build run.start payload: {run: <run json>, modules: [...seen...]}.
    std::string modules_json = "[";
    bool first = true;
    for (const auto& m : seen_modules(run_id)) {
        if (!first) modules_json += ",";
        first = false;
        modules_json += "\"" + util::json_escape_string(m) + "\"";
    }
    modules_json += "]";
    grpc::SseEvent ev;
    ev.name     = "run.start";
    ev.data     = "{\"run\":" + util::run_to_json(*opt) +
                  ",\"modules\":" + modules_json + "}";
    ev.critical = true;
    run_stream_registry_.publish(run_id, ev);
}

void RunService::publish_run_complete(const std::string& run_id, const std::string& status) {
    // module.transition done for every seen module, then the terminal event.
    for (const auto& m : seen_modules(run_id)) {
        grpc::SseEvent tr;
        tr.name = "module.transition";
        tr.data = "{\"module_id\":\"" + util::json_escape_string(m) +
                  "\",\"state\":\"done\"}";
        tr.critical = false;
        run_stream_registry_.publish(run_id, tr);
    }
    auto opt = run_repo_.find_by_id(run_id);
    const std::int64_t total = result_repo_.count_for_run(run_id);
    grpc::SseEvent ev;
    ev.name     = "run.complete";
    ev.critical = true;
    if (opt) {
        ev.data = "{\"run\":" + util::run_to_json(*opt) +
                  ",\"total\":" + std::to_string(total) + "}";
    } else {
        ev.data = "{\"run_id\":\"" + util::json_escape_string(run_id) +
                  "\",\"status\":\"" + util::json_escape_string(status) +
                  "\",\"total\":" + std::to_string(total) + "}";
    }
    run_stream_registry_.publish(run_id, ev);
}

}  // namespace arches::server