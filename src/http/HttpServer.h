#pragma once
// http/HttpServer.h — minimal HTTP/SSE server for the live run monitoring view.
//
// NEW for the live-run-monitoring loop. Arches-Server is otherwise gRPC-only
// (REST deferred to Phase 2); this server exposes exactly one endpoint the
// Dashboard's existing SSE stack can consume:
//
//   GET /api/v1/runs/{run_id}/stream  ->  text/event-stream
//       events: run.start, module.transition, result, run.complete, flow.paused
//
// It also serves a trivial GET /api/v1/health so the Dashboard's existing
// health probe (contract-tests/test_01_smoke.py) has something to hit when the
// HTTP server is up — the full REST surface remains Phase 2.
//
// Threading: HttpServer::start() blocks (cpp-httplib's listen loop). main.cpp
// runs it on a dedicated thread. stop() is signal-safe enough for the shutdown
// sequence (cpp-httplib's Server::stop() is thread-safe and not a raw signal
// handler — it's called from the sigwait worker, like GrpcServerRunner).
//
// Vendored dependency: cpp-httplib (src/http/httplib.h, single-header, MIT).
// No SSL (CPPHTTPLIB_OPENSSL_SUPPORT is NOT defined), no JsonCpp, no OpenSSL —
// per the repo "Don't" list. JSON payloads are built by util::ResultConvert's
// minimal serializer.
//
// Layering: arches_http depends on arches_services (RunService for run state)
// and arches_stream_registry (RunStreamRegistry for viewer fan-out). It does
// NOT depend on arches_grpc, keeping it out of the three-layer services->grpc
// link graph.

#include "services/RunService.h"
#include "stream_registry/RunStreamRegistry.h"
#include "config/Settings.h"

#include <atomic>
#include <future>
#include <memory>
#include <string>

namespace httplib { class Server; }  // forward-declare the vendored header

namespace arches::server::http {

class HttpServer {
public:
    HttpServer(RunService&                 run_svc,
               grpc::RunStreamRegistry&    run_stream_registry,
               const config::Settings&     settings);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Start listening on settings.http_listen. Blocks until stop() is called.
    // Resolves port_future() to the actual bound port (meaningful when
    // http_listen ends in ":0"); on bind failure, resolves to an exception —
    // mirroring GrpcServerRunner::port_future().
    void start();

    // Signal graceful shutdown (stop the listen loop and in-flight responses).
    void stop();

    std::future<int>& port_future() { return port_future_; }

private:
    void register_routes();

    RunService&                    run_svc_;
    grpc::RunStreamRegistry&       run_stream_registry_;
    const config::Settings&        settings_;
    std::unique_ptr<httplib::Server> svr_;
    std::promise<int>              port_promise_;
    std::future<int>               port_future_;
    std::atomic<bool>              started_{false};
};

}  // namespace arches::server::http