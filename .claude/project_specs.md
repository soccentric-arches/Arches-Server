# Arches-Server — specs

Target: x86 host (not the Jetson). C++17, gRPC, SQLite.

Serves the `arches.stream::ClientService` bidi stream that Arches-Client drives, and links the
Arches-HAL subproject for the shared generated protos. SQLite replaces the prior reference
implementation's PostgreSQL/Drogon stack — see `server_update/10_arches_port.md`.

## Architecture

* **HAL linking** — mirrors Arches-Client: `add_subdirectory` the HAL subproject, link `hal_protos_shared` (SHARED), no local `find_package` or protoc. Single source of proto symbols.
* **SQLite connection model** — one shared `sqlite3*` opened `SQLITE_OPEN_NOMUTEX`, plus one `std::recursive_mutex` (`g_db_mutex`). A `PreparedStatement` holds that mutex for its whole lifetime, making each statement a fully serialized critical section. Recursive because `ResultRepository::insert_recursive` opens a statement then recurses into subtests on the same thread.
* **Library layering** — `arches_stream_registry` (`ClientStreamRegistry`) -> `arches_services` (`RunService`, `LogService`) -> `arches_grpc` (`ClientServiceImpl`, `HalServiceImpl`, `GrpcServerRunner`). Added in that order; anything else is a link cycle.
* **Run-control push path** — `ClientStreamRegistry` maps `board_id` to a `shared_ptr<BoundedQueue<ServerMessage>>`. `ClientServiceImpl::Connect` registers the queue on `HelloReport`; `RunService` pushes Configure/Start/Pause/Resume/Stop/RestartRun through it.
* **One writer thread per stream** — gRPC forbids concurrent writes on a bidi stream, so `Connect` runs a dedicated writer draining `out_queue`. The read loop enqueues acks rather than writing inline.
* **Module-Result unpack** — `util::ResultConvert::unpack_module_results` inverts the client's `pack_module_result` using `DescriptorPool::generated_pool()`, `DynamicMessageFactory` and reflection, falling back to a bare `hal.common.TestResult`.
* **Startup order** (`main.cpp`) — `load_settings()` -> spdlog -> `init_db_client` -> `MigrationRunner.run()` -> `ModuleRegistry::load` -> `ClientStreamRegistry` -> `RunService`+`LogService` -> `reconcile_stale_runs()` -> gRPC impls + runner -> signals -> `runner.start()`.
* **Shutdown** — SIGINT/SIGTERM are blocked in the main thread before `runner.start()` so gRPC's threads inherit the mask; a worker thread calls `sigwait` then `runner.shutdown()`.
* **Tooling** — `CMakePresets.json` (native/debug/release/asan/strict), `-Werror` opt-in via `WARNINGS_AS_ERRORS`, CPack emits `.deb` + `.rpm`, `ARCHES_SERVER_VERSION` compile def.
* **ASan** — `ENABLE_SANITIZERS` instruments the test binary and all in-repo libs. Placed after the HAL `add_subdirectory` (so the HAL subproject stays uninstrumented) and before `src/` and `tests/`.
