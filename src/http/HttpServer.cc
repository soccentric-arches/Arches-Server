// http/HttpServer.cc — see header.
#include "http/HttpServer.h"

#include "util/ResultConvert.h"

#include <spdlog/spdlog.h>

// cpp-httplib is a vendored third-party single header that legitimately uses
// old-style casts and other patterns the repo's warning set flags. Suppress the
// strict warnings around the include so a -Werror strict build stays clean
// (the warnings are not ours to fix upstream). Keep the suppression scoped to
// this include only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#include "httplib.h"
#pragma GCC diagnostic pop

#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arches::server::http {

namespace {

using grpc::EventQueuePtr;
using grpc::SseEvent;

// Split "host:port" into host and port. Returns false on malformed input.
bool split_host_port(const std::string& s, std::string& host, int& port) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    try {
        port = std::stoi(s.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return true;
}

// Write one SSE frame to the sink: `event: <name>\ndata: <json>\n\n`.
// Returns the result of sink.write (false if the client has disconnected).
bool write_event(httplib::DataSink& sink, const SseEvent& ev) {
    std::string frame = "event: " + ev.name + "\ndata: " + ev.data + "\n\n";
    return sink.write(frame.data(), frame.size());
}

// ── Minimal JSON field extraction ────────────────────────────────────────────
// NOT a general JSON parser — extracts a single string or integer field from a
// flat JSON object body (the controlled payloads the run-control endpoints and
// their contract tests send). Sufficient for `{"mode":"single_shot", ...}` and
// `{"status":"pass","sequence":3, ...}` shapes without pulling in JsonCpp
// (forbidden by the repo "Don't" list). The full REST API + a real JSON parser
// are Phase 2.
std::optional<std::string> json_string_field(const std::string& body, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    auto k = body.find(pat);
    if (k == std::string::npos) return std::nullopt;
    auto colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return std::nullopt;
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n')) ++i;
    if (i >= body.size() || body[i] != '"') return std::nullopt;
    std::string out;
    ++i;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char esc = body[i + 1];
            switch (esc) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            default:   out += esc;  break;
            }
            i += 2;
        } else {
            out += body[i++];
        }
    }
    return out;
}

std::optional<long long> json_int_field(const std::string& body, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    auto k = body.find(pat);
    if (k == std::string::npos) return std::nullopt;
    auto colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return std::nullopt;
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n')) ++i;
    if (i >= body.size() && (body[i] < '0' || body[i] > '9') && body[i] != '-') return std::nullopt;
    size_t start = i;
    if (body[i] == '-') ++i;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') ++i;
    if (i == start || (i == start + 1 && body[start] == '-')) return std::nullopt;
    try {
        return std::stoll(body.substr(start, i - start));
    } catch (...) {
        return std::nullopt;
    }
}

// Map a status string ("pass"/"fail"/"error"/"skipped"/"aborted"/...) to the
// hal::common::TestStatus int the Result row stores. Mirrors util::string_to_status.
std::int16_t status_string_to_int(const std::string& s) {
    return static_cast<std::int16_t>(util::string_to_status(s));
}

// Per-SSE-connection state: gates the initial replay and dedups result ids
// across the replay/live boundary so a reconnecting viewer sees no duplicates.
struct ViewerState {
    bool replayed = false;
    std::unordered_set<std::string> seen;
};

}  // namespace

HttpServer::HttpServer(RunService&              run_svc,
                       grpc::RunStreamRegistry& run_stream_registry,
                       const config::Settings&  settings)
    : run_svc_(run_svc),
      run_stream_registry_(run_stream_registry),
      settings_(settings),
      port_future_(port_promise_.get_future()) {}

HttpServer::~HttpServer() {
    stop();  // idempotent — safe if never started or already stopped
}

void HttpServer::start() {
    svr_ = std::make_unique<httplib::Server>();
    register_routes();

    std::string host = "0.0.0.0";
    int port = 8080;
    if (!split_host_port(settings_.http_listen, host, port)) {
        spdlog::warn("http: malformed ARCHES_HTTP_LISTEN '{}' — defaulting to 0.0.0.0:8080",
                     settings_.http_listen);
        host = "0.0.0.0";
        port = 8080;
    }

    // bind_to_any_port lets the kernel choose a free port when port==0, then
    // listen_after_bind blocks. publish the chosen port (or -1 on bind failure)
    // via port_future_ so tests can connect to the real port and a bind failure
    // surfaces as a clean test failure (mirrors GrpcServerRunner).
    int bound = svr_->bind_to_any_port(host.c_str());
    if (bound < 0) {
        port_promise_.set_exception(std::make_exception_ptr(
            std::runtime_error("HTTP server failed to bind on " + settings_.http_listen)));
        return;
    }
    spdlog::info("http: SSE server listening on {}:{} (/api/v1/runs/:id/stream, /api/v1/health)",
                 host, bound);
    started_ = true;
    port_promise_.set_value(bound);
    svr_->listen_after_bind();  // blocks until stop()
    started_ = false;
}

void HttpServer::stop() {
    if (svr_) {
        svr_->stop();  // thread-safe; breaks listen_after_bind and in-flight responses
    }
}

void HttpServer::register_routes() {
    // ── Health (parity with the Dashboard's existing smoke probe) ───────────
    svr_->Get("/api/v1/health",
              [](const httplib::Request& /*req*/, httplib::Response& res) {
                  res.set_content("{\"status\":\"ok\"}", "application/json");
              });

    // ── Minimal run-control surface (drives the SSE stream over HTTP) ────────
    // POST /api/v1/runs                       -> create a run row (status queued)
    // POST /api/v1/runs/:run_id/start         -> mark running (broadcasts run.start)
    // POST /api/v1/runs/:run_id/results       -> inject one result (broadcasts result)
    // POST /api/v1/runs/:run_id/complete      -> mark terminal (broadcasts run.complete)
    //
    // These bypass the gRPC ClientService stream so a live run is drivable over
    // HTTP when no embedded Client is connected. The full REST API (richer
    // payloads, GET list/detail, modules, settings) is Phase 2.
    svr_->Post("/api/v1/runs",
               [this](const httplib::Request& req, httplib::Response& res) {
                   auto mode = json_string_field(req.body, "mode").value_or("single_shot");
                   auto name = json_string_field(req.body, "name").value_or("Run");
                   auto board_id = json_string_field(req.body, "board_id").value_or("");
                   auto run = run_svc_.create_run_row(mode, board_id, name);
                   res.set_content(util::run_to_json(run), "application/json");
               });

    svr_->Post("/api/v1/runs/:run_id/start",
               [this](const httplib::Request& req, httplib::Response& res) {
                   const std::string run_id = req.path_params.at("run_id");
                   try {
                       run_svc_.mark_running(run_id);
                       res.set_content("{\"ok\":true}", "application/json");
                   } catch (const std::exception& e) {
                       res.status = 404;
                       res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
                   }
               });

    svr_->Post("/api/v1/runs/:run_id/results",
               [this](const httplib::Request& req, httplib::Response& res) {
                   const std::string run_id = req.path_params.at("run_id");
                   auto module_id = json_string_field(req.body, "module_id").value_or("unknown");
                   auto test_id   = json_string_field(req.body, "test_id").value_or("test");
                   auto test_name = json_string_field(req.body, "name").value_or("");
                   auto status_s  = json_string_field(req.body, "status").value_or("pass");
                   auto mode      = json_string_field(req.body, "mode").value_or("single_shot");
                   std::int16_t status = status_string_to_int(status_s);
                   if (status == 0) status = 1;  // default PASS if unparsable
                   std::int64_t sequence = json_int_field(req.body, "sequence").value_or(0);
                   try {
                       auto saved = run_svc_.inject_result(run_id, module_id, test_id,
                                                           test_name, status, sequence, mode);
                       res.set_content(util::result_to_json(saved), "application/json");
                   } catch (const std::exception& e) {
                       res.status = 404;
                       res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
                   }
               });

    svr_->Post("/api/v1/runs/:run_id/complete",
               [this](const httplib::Request& req, httplib::Response& res) {
                   const std::string run_id = req.path_params.at("run_id");
                   auto status = json_string_field(req.body, "status").value_or("completed");
                   try {
                       run_svc_.complete_run(run_id, status);
                       res.set_content("{\"ok\":true}", "application/json");
                   } catch (const std::exception& e) {
                       res.status = 404;
                       res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
                   }
               });

    // ── SSE live-run stream ──────────────────────────────────────────────────
    // GET /api/v1/runs/{run_id}/stream -> text/event-stream
    //
    // On connect: register a viewer queue for the run, then replay current
    // state (run.start + a result event per already-landed top-level row) and
    // drain live events from the queue. The queue is registered BEFORE the
    // replay query so events arriving during the replay are captured and
    // deduped against the replayed rows by result id (a viewer that reconnects
    // mid-run therefore resumes without duplication).
    svr_->Get("/api/v1/runs/:run_id/stream",
              [this](const httplib::Request& req, httplib::Response& res) {
                  auto it = req.path_params.find("run_id");
                  if (it == req.path_params.end() || it->second.empty()) {
                      res.status = 400;
                      res.set_content("{\"error\":\"missing run_id\"}", "application/json");
                      return;
                  }
                  const std::string run_id = it->second;

                  // Verify the run exists so a non-existent id gets a clean 404
                  // instead of an infinite empty SSE stream.
                  try {
                      (void)run_svc_.get_run(run_id);
                  } catch (const std::exception&) {
                      res.status = 404;
                      res.set_content("{\"error\":\"run not found\"}", "application/json");
                      return;
                  }

                  constexpr std::size_t kQueueCapacity = 1024;
                  auto queue = std::make_shared<grpc::BoundedEventQueue>(kQueueCapacity);
                  run_stream_registry_.register_stream(run_id, queue);

                  res.set_header("Cache-Control", "no-cache");
                  res.set_header("Connection", "keep-alive");
                  res.set_header("X-Accel-Buffering", "no");  // disable proxy buffering

                  // Per-connection state captured into the provider lambda.
                  // `replayed` gates the initial state dump; `seen` dedups
                  // result ids across the replay/live boundary.
                  auto state = std::make_shared<ViewerState>();

                  res.set_chunked_content_provider(
                      "text/event-stream",
                      [this, run_id, queue, state](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                          // Liveness check: a stalled/disconnected viewer is
                          // dropped here, and the releaser below deregisters
                          // the queue so the publisher never blocks on it.
                          if (!sink.is_writable()) return false;

                          if (!state->replayed) {
                              state->replayed = true;
                              // Snapshot current run + already-landed results.
                              // The queue was registered before this query, so
                              // any results inserted concurrently are also in
                              // the queue and will be deduped by id below.
                              try {
                                  auto run_opt = run_svc_.get_run(run_id);
                                  std::string modules_json = "[";
                                  bool first = true;
                                  for (const auto& m : run_svc_.get_module_summaries(run_id)) {
                                      if (!first) modules_json += ",";
                                      first = false;
                                      modules_json += "\"" + util::json_escape_string(m.module_id) + "\"";
                                  }
                                  modules_json += "]";
                                  SseEvent start_ev;
                                  start_ev.name = "run.start";
                                  start_ev.data = "{\"run\":" + util::run_to_json(run_opt) +
                                                  ",\"modules\":" + modules_json + "}";
                                  start_ev.critical = true;
                                  if (!write_event(sink, start_ev)) return false;

                                  for (const auto& r : run_svc_.list_results(run_id)) {
                                      state->seen.insert(r.id);
                                      SseEvent rev;
                                      rev.name      = "result";
                                      rev.data      = util::result_to_json(r);
                                      rev.critical  = false;
                                      rev.dedup_key = r.id;
                                      if (!write_event(sink, rev)) return false;
                                  }

                                  // If the run is already terminal, emit the
                                  // terminal event now so a viewer connecting
                                  // after completion (or reconnecting to a
                                  // finished run) sees the definite end state
                                  // instead of an idle, never-completing stream.
                                  const std::string& st = run_opt.status;
                                  bool terminal = (st == "completed" || st == "failed" ||
                                                   st == "cancelled"  || st == "rejected");
                                  if (terminal) {
                                      const std::int64_t total = run_svc_.count_results(run_id);
                                      SseEvent done_ev;
                                      done_ev.name     = "run.complete";
                                      done_ev.critical = true;
                                      done_ev.data     = "{\"run\":" + util::run_to_json(run_opt) +
                                                         ",\"total\":" + std::to_string(total) + "}";
                                      if (!write_event(sink, done_ev)) return false;
                                  }
                              } catch (const std::exception& e) {
                                  spdlog::warn("http: replay failed for run_id={}: {}", run_id, e.what());
                                  return false;
                              }
                              return true;  // continue to live drain
                          }

                          // Live drain: block briefly for the next event so we
                          // can re-check is_writable periodically (a plain
                          // blocking dequeue would never wake on a silent
                          // disconnect while idle).
                          auto ev = queue->dequeue_for(std::chrono::milliseconds(500));
                          if (!sink.is_writable()) return false;
                          if (!ev) return true;  // timed out, still connected, keep going
                          // Dedup result events by id so a reconnecting viewer
                          // does not see a row it already saw in the replay.
                          if (ev->name == "result" && !ev->dedup_key.empty()) {
                              if (!state->seen.insert(ev->dedup_key).second) {
                                  return true;  // duplicate of a replayed/seen row — skip
                              }
                          }
                          if (!write_event(sink, *ev)) return false;
                          return true;
                      },
                      /*releaser=*/[this, run_id, queue](bool /*success*/) {
                          // Connection ended (client disconnect, server stop,
                          // or provider returned false). Close the queue so the
                          // publisher's try_enqueue becomes a no-op for this
                          // viewer, then deregister it from the fan-out set.
                          queue->close();
                          run_stream_registry_.deregister_stream(run_id, queue);
                      });
              });
}

}  // namespace arches::server::http