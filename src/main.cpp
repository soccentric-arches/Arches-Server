// arches-server — entry point (Step 7 wiring).
//
// Boots the Phase-1 server: env-var Settings -> spdlog -> SQLite DB + migrations
// -> HAL ModuleRegistry -> ClientStreamRegistry + RunService + LogService ->
// HalServiceImpl + ClientServiceImpl -> GrpcServerRunner (HalService +
// ClientService on one gRPC server) -> reconcile stale runs -> signal-safe
// shutdown.
//
// The client<->server interop path is ClientService::Connect (bidi):
//   - client sends HelloReport -> server registers board_id -> outbound queue
//   - client sends TestResultBatch -> server §5-unpacks each ResultSample.payload
//     (hal.modules.<id>.Result wrapper or bare TestResult) -> ingests -> acks;
//     on a terminal batch, maps batch.state() -> runs.status.
//   - RunService pushes ConfigureRun/StartRun/.../RestartRun to the client via
//     ClientStreamRegistry.
// HalService is registered for parity (Phase-2 wires the HalService->ClientService
// bridge; its RPCs are boardless in Phase 1).

#include "config/Settings.h"
#include "db/DbClient.h"
#include "db/MigrationRunner.h"
#include "registry/ModuleRegistry.h"
#include "services/RunService.h"
#include "services/LogService.h"
#include "stream_registry/ClientStreamRegistry.h"
#include "grpc_server/HalServiceImpl.h"
#include "grpc_server/ClientServiceImpl.h"
#include "grpc_server/GrpcServerRunner.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <csignal>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
    using namespace arches::server;

    // ── Settings + logging ──────────────────────────────────────────────────
    auto settings = config::load_settings();

    auto logger = spdlog::stdout_color_mt("arches");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::from_str(settings.log_level));
    spdlog::info("arches-server {} starting", ARCHES_SERVER_VERSION);
    spdlog::info("arches-server: grpc_listen={} db_path={} registry={}",
                 settings.grpc_listen, settings.db_path, settings.registry_path);

    try {
        // ── SQLite DB + migrations ──────────────────────────────────────────
        db::init_db_client(settings.db_path);
        db::MigrationRunner{ARCHES_MIGRATIONS_DIR}.run();
        spdlog::info("arches-server: database ready at {}", settings.db_path);

        // ── HAL module registry ──────────────────────────────────────────────
        registry::ModuleRegistry modules;
        modules.load(settings.registry_path);
        spdlog::info("arches-server: module registry loaded ({} modules)", modules.size());

        // ── Service layer ────────────────────────────────────────────────────
        // NOTE: qualify `arches::server::grpc::` fully — a bare `grpc::` is
        // ambiguous between grpcpp's top-level ::grpc and arches::server::grpc
        // under `using namespace arches::server`.
        arches::server::grpc::ClientStreamRegistry stream_registry;
        RunService run_svc{stream_registry};
        LogService log_svc;

        const int reconciled = run_svc.reconcile_stale_runs();
        if (reconciled > 0) {
            spdlog::info("arches-server: reconciled {} stale run(s) -> failed", reconciled);
        }

        // ── gRPC services + runner ──────────────────────────────────────────
        arches::server::grpc::HalServiceImpl    hal_svc{run_svc, log_svc, modules, settings};
        arches::server::grpc::ClientServiceImpl client_svc{run_svc, log_svc, stream_registry,
                                                            settings.stream_queue_size};
        arches::server::grpc::GrpcServerRunner  runner{hal_svc, client_svc, settings};

        // ── Signal handling: block SIGINT/SIGTERM here, sigwait in a thread ──
        // (async-signal-safe: the worker thread, not a handler, calls shutdown).
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);

        std::thread sig_thread([&runner, &mask] {
            int sig = 0;
            sigwait(&mask, &sig);
            spdlog::info("arches-server: received signal {} — shutting down", sig);
            try { runner.shutdown(); } catch (const std::exception& e) {
                spdlog::error("arches-server: shutdown error: {}", e.what());
            }
        });

        // ── Start (blocks until shutdown) ───────────────────────────────────
        runner.start();
        sig_thread.join();

        spdlog::info("arches-server: stopped");
        db::shutdown_db_client();
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("arches-server: fatal: {}", e.what());
        db::shutdown_db_client();
        return 1;
    }
}