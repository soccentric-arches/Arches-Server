# Arches-Server — specs

Parent project: Arches (target hardware: NVIDIA Jetson, Tegra family, L4T/JetPack OS, with an STM32 real-time co-processor)
Leaf type: `<Project>-Server`

## What this is

Arches-Server is the C++17 gRPC IV&V server (runs on the x86 host, not the Jetson). It links the Arches-HAL subproject for the shared generated protos and serves the `arches.stream::ClientService` bidi stream that Arches-Client drives. DB is SQLite (master §4 "gRPC + SQLite"), a deviation from the prior reference implementation's PostgreSQL/Drogon stack — flagged in `server_update/10_arches_port.md`.

## Architecture

* **HAL linking** — mirrors Arches-Client: `add_subdirectory` the HAL subproject, link `hal_protos_shared` (SHARED), no local `find_package`/protoc. Single source of proto symbols, no ODR risk.
* **SQLite connection model** — one shared `sqlite3*` (opened `SQLITE_OPEN_NOMUTEX`) + one `std::recursive_mutex` (`g_db_mutex`). A `PreparedStatement` holds the mutex for its entire lifetime, making each statement a fully-serialized critical section so two statements on the shared connection can never interleave. The mutex is recursive because `ResultRepository::insert_recursive` opens a statement then recurses into subtests opening more statements on the same thread.
* **Services↔gRPC layering** (to break a dependency cycle between `RunService` and `ClientServiceImpl`): three layered static libs, added in this order in `src/CMakeLists.txt`: `arches_stream_registry` (src/stream_registry/, `ClientStreamRegistry` only) → `arches_services` (src/services/, `RunService` + `LogService`) → `arches_grpc` (src/grpc_server/, `ClientServiceImpl` + `HalServiceImpl` + `GrpcServerRunner`).
* **Run-control push path** — `ClientStreamRegistry` maps `board_id` → `shared_ptr<BoundedQueue<ServerMessage>>`. `ClientServiceImpl::Connect` registers the board's outbound queue on `HelloReport`; `RunService` pushes `ConfigureRun`/`StartRun`/`PauseRun`/`ResumeRun`/`StopRun`/`RestartRun` via `registry.push(board_id, msg)`.
* **Single writer thread per stream** — gRPC forbids concurrent writes on one bidi stream, so `Connect` runs a dedicated outbound writer thread draining `out_queue`; the read loop enqueues acks instead of writing inline.
* **§5 module-Result unpack** — `util::ResultConvert::unpack_module_results` is the server-side inverse of the client's `pack_module_result`, using `DescriptorPool::generated_pool()` + `DynamicMessageFactory` + reflection, falling back to a bare `hal.common.TestResult` when the descriptor is absent.
* **main.cpp wiring order** — `load_settings()` → spdlog → `db::init_db_client(db_path)` → `MigrationRunner{ARCHES_MIGRATIONS_DIR}.run()` → `ModuleRegistry::load` → `ClientStreamRegistry` → `RunService`+`LogService` → `reconcile_stale_runs()` → `HalServiceImpl`+`ClientServiceImpl`+`GrpcServerRunner` → signal setup → `runner.start()`.
* **Signal-safe shutdown** — block SIGINT/SIGTERM in the main thread before `runner.start()` so gRPC's internal threads inherit the blocked mask; a dedicated worker thread calls `sigwait` → `runner.shutdown()`.
* **Tooling** — `CMakePresets.json` (native/debug/release/asan/strict), `-Werror` opt-in via `WARNINGS_AS_ERRORS`, CPack produces `.deb`+`.rpm` (`.ipk` is a cp-rename in the Makefile installer target), `ARCHES_SERVER_VERSION` compile def.
* **ASan instrumentation** — `ENABLE_SANITIZERS` instruments the test binary + all in-repo libs (placed after the HAL `add_subdirectory` so the pre-built HAL subproject stays uninstrumented, and before `src/`+`tests/`). Needed to catch a SQLite connection-interleave race that target-specific instrumentation missed.

Part of the Arches project in the project-managers fleet. See `docs/project-managers.md` for the full 9-project / 8-leaf hierarchy and shared conventions.
