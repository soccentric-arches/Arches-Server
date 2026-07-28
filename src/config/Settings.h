#pragma once
// config/Settings.h — Arches-Server runtime configuration.
//
// Phase 1 holds the plain struct; Step 7 adds env-var loading (ARCHES_GRPC_LISTEN,
// ARCHES_DB_PATH, ARCHES_REGISTRY_PATH, ARCHES_STREAM_QUEUE_SIZE, ARCHES_LOG_LEVEL,
// ARCHES_SHUTDOWN_TIMEOUT_SEC) in config/Settings.cc. GrpcServerRunner (Step 6)
// and HalServiceImpl already depend on this struct, so it is introduced here.
//
// Deviation from the prior reference implementation: it carries a TOML `internal/config` stack +
// a `NetworkSettings` sub-struct; Arches collapses both into one env-var-driven
// struct (per the plan: "Drop the TOML internal/config legacy stack entirely").

#include <cstddef>
#include <string>

namespace arches::server::config {

struct Settings {
    // gRPC listen address (host:port).
    std::string grpc_listen = "0.0.0.0:50051";

    // HTTP/SSE listen address (host:port) for the live-run monitoring view.
    // Empty disables the HTTP server. NEW for the live-run-monitoring loop.
    std::string http_listen = "0.0.0.0:8080";

    // SQLite database file path.
    std::string db_path = "/var/lib/arches/server.db";

    // HAL module registry manifest (modules.yaml) path.
    std::string registry_path = "../Arches-HAL/protos/registry/modules.yaml";

    // Per-stream bounded-queue capacity (ingest + outbound).
    std::size_t stream_queue_size = 1024;

    // spdlog level string: trace/debug/info/warn/err/critical/off.
    std::string log_level = "info";

    // Graceful shutdown timeout (seconds).
    int shutdown_timeout_sec = 10;
};

// Load settings from ARCHES_* env vars, falling back to the struct defaults.
Settings load_settings();

}  // namespace arches::server::config