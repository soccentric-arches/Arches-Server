# Porting Note: Acadia-Server → Arches-Server (Phase 1: client↔server interop)

**Date:** 2026-07-21
**Scope:** Arches-Server only (one project, one component — §5 blast radius respected)
**Target:** x86 Linux host (the server runs on the host, not the Jetson)
**Status:** Phase 1 complete — `make build && make test && make lint` green;
interop test green (15/15 native + 10/10 ASan + 31/31 full-suite ASan).

## What changed

This is a port of Acadia-Server's gRPC + persistence + run-lifecycle core onto
the Arches-native, §12-compliant toolchain, scoped to **what Arches-Client
needs to actually work against the server**. The subsystem shape
(config → db → migrations → repos → util → services → grpc_server → main)
mirrors Acadia-Server; the storage backend, tooling, namespace, and two
interop gap-closures differ.

### Decisions locked (via AskUserQuestion, pre-implementation)

1. **Full port (mirror the client)** — own namespace `arches::server`,
   executable `arches-server`, paths `/etc/arches` / `/var/lib/arches`,
   `ARCHES_SERVER_VERSION`.
2. **SQLite for the DB** (master §4 "gRPC + SQLite"; host-buildable like
   Arches-Client). **Deviation from Acadia-Server's PostgreSQL/Drogon** — see
   "SQLite vs PostgreSQL" below.
3. **Phase 1 = interop only** — defer the Dashboard-facing REST controllers,
   OTA, scheduler, anomaly/compare, auth, notifications, templates to Phase 2.

## Contract carve-outs (NOT changed — §5)

| Contract | Value | Why kept |
|---|---|---|
| Proto packages | `acadia.stream`, `hal.common`, `hal`, `hal.modules.*` | Shared wire contract with Client + PySDK |
| `client_service.proto` | package `acadia.stream` | Client drives runs from this |
| `hal_service.proto` / `common.proto` | packages `hal` / `hal.common` | SDK + Client expect this |
| §5 module-Result payload | `hal.modules.<id>.Result { repeated hal.common.TestResult results = 2; }` packed in `ResultSample.payload` Any | The §5-approved wire change from Arches-Client; the server unpacks it (no further contract change) |

Only the server's **own C++ namespace** (`acadia::server` → `arches::server`),
library/target names (`acadia_*` → `arches_*`), install paths, version macro,
and DB backend changed. The proto packages, `.proto` files, and HAL API are
unchanged.

## SQLite vs PostgreSQL (the headline deviation)

Acadia-Server is PostgreSQL via the Drogon ORM + a Drogon REST surface. Master
CLAUDE.md §4 fixes `*-Server` as "gRPC + **SQLite**", and Phase 1 needs no
Dashboard REST surface, so:

- **Drogon removed entirely.** No ORM, no REST controllers, no prometheus.
  gRPC (`ClientService` + `HalService`) is the only external surface in Phase 1.
- `db::DbClient` is a thin `sqlite3*` wrapper (one shared connection +
  `std::recursive_mutex`; see the threading note in `CLAUDE.md` and below).
- `MigrationRunner` splits `*.sql` by `;` and runs them via `sqlite3_exec` with
  an `INSERT OR IGNORE INTO schema_migrations` tracking table.
- Migrations 0001–0010 ported to SQLite syntax. Postgres-isms rewritten:

  | Postgres | SQLite |
  |---|---|
  | `SERIAL` / `BIGSERIAL` | `INTEGER PRIMARY KEY AUTOINCREMENT` |
  | `TIMESTAMPTZ` | `TEXT` (ISO-8601 UTC) |
  | `BYTEA` | `BLOB` |
  | `JSONB` + `GIN` | `TEXT` (minimal JSON serializer in `util/ResultConvert`) |
  | `gen_random_uuid()` defaults | server assigns ids (`util::generate_uuid()`) |
  | `ON CONFLICT (k) DO NOTHING` | `INSERT OR IGNORE` |
  | `ALTER TABLE ADD CONSTRAINT CHECK` | not supported post-create → enforced in `ResultRepository` (service-layer invariant: HAL never emits `TEST_STATUS_UNSPECIFIED`) |
  | `CREATE UNIQUE INDEX ON board ((TRUE))` | SQLite rejects a constant-only index expression → `BEFORE INSERT ... WHEN (SELECT COUNT(*) FROM board) >= 1` trigger |
  | Postgres JSONB ops (`->`, `?`, `= ANY($1::int[])`, `to_tsvector @@`) | `json_extract`, `IN (?,?,?)`, `LIKE` — the trend/distribution/compare queries that used them are Phase 2 |

Deferred to Phase 2 (listed for traceability): Drogon REST controllers +
AuthService (argon2) + OTA + Scheduler + AnomalyService + CompareService +
ThresholdService + NotificationService + TemplateService + ConfigService +
ModuleService(REST) + prometheus + GraphQL + PostgreSQL.

## Two interop gaps a faithful port would NOT close (implemented)

Acadia-Server never emits run-control over the `ClientService` bidi stream
(only `BatchAck`/`LogBatchAck`) and ignores `batch.state()` (blindly marks any
terminal run "completed"). Arches-Client drives runs **exclusively** from those
stream messages and now emits meaningful `RunState`s. Phase 1 closes both:

1. **Run-control push** — new `stream_registry/ClientStreamRegistry` maps
   `board_id → shared_ptr<BoundedQueue<ServerMessage>>`. `ClientServiceImpl::Connect`
   registers the board's outbound queue on `HelloReport`, deregisters on
   disconnect (guarded by pointer identity so a reconnect can't yank the fresh
   queue). `RunService::start_run/pause_run/resume_run/stop_run/restart_run`
   push `ConfigureRun/StartRun/PauseRun/ResumeRun/StopRun/RestartRun` via
   `registry.push(board_id, msg)`. No proto change — uses the existing
   `ServerMessage` oneof.
2. **`batch.state()` mapping** — on a terminal batch, `RunService::set_run_status`
   maps the client's `RunState` → `runs.status` (`run_state_to_status`:
   COMPLETED→completed, FAILED→failed, CANCELLED→cancelled, PAUSED→paused,
   RUNNING→running, CONFIGURED→queued, UNSPECIFIED→rejected) instead of
   Acadia's blind `complete_run("completed")`. Terminal states set `ended_at`;
   RUNNING sets `started_at`; others `update_status`.

Both are exercised end-to-end by `tests/integration/ClientStreamInteropTest.cc`
(no real Arches-Client binary needed — a mock gRPC client connects in-process).

## §5 module-Result unpack (server-side inverse of the client pack)

`util::ResultConvert::unpack_module_results(const google::protobuf::Any&,
module_id)` is the inverse of Arches-Client's
`hal_bridge::pack_module_result`. It resolves the message type from the Any's
`type_url` (`type.googleapis.com/hal.modules.<id>.Result`), finds the
descriptor in `DescriptorPool::generated_pool()` (registered by
`hal_protos_shared`), builds it via a per-call `DynamicMessageFactory`, reads
the repeated `results` field by reflection, and serialize→`ParseFromString`s
each element into a **generated** `hal::common::TestResult` (never crosses the
DynamicMessage/generated representation boundary). Falls back to a bare
`hal.common.TestResult` when the descriptor is absent or the type_url is the
bare form. Called from `ClientServiceImpl::handle_result_batch` (the
client↔server path) and `HalServiceImpl::persist_result` (parity on the
HAL-RPC path). `proto_to_result` (TestResult → `Result` row, recursing into
subtests) is ported unchanged from Acadia.

## HAL subproject linking (mirrors Arches-Client — single proto symbol source)

`add_subdirectory(${CMAKE_SOURCE_DIR}/../Arches-HAL
${CMAKE_BINARY_DIR}/arches_hal_build EXCLUDE_FROM_ALL)` then link
`hal_protos_shared` (SHARED; carries protobuf+gRPC PUBLIC). No
`find_package(gRPC/Protobuf)`, no local `protoc`. Linking it registers every
`hal.modules.<id>.Result` descriptor in the generated pool — which the §5
reflection unpack relies on. (Acadia-Server self-generated 3 protos; we do NOT
mirror that — we mirror the client to avoid ODR / duplicate-symbol risk.)

## Services↔gRPC dependency cycle

`RunService` needs `ClientStreamRegistry` (to push run-control) and
`ClientServiceImpl` needs `RunService` — a cycle if both live in one lib.
Resolution: three layered static libs, added in this order in `src/CMakeLists.txt`:
`arches_stream_registry` → `arches_services` → `arches_grpc`. `StreamBackpressure.h`
(`BoundedQueue` template) is header-only in `grpc_server/`, reached by all three.

## Concurrency: serialized stream writes + the SQLite connection lock

Two concurrency points, both load-bearing:

- **All `stream->Write()` on one writer thread.** gRPC forbids concurrent writes
  on a single bidi stream. `Connect` runs a dedicated outbound writer thread
  draining `out_queue` (run-control from RunService + acks produced in the read
  loop); the read loop enqueues acks to `out_queue` instead of writing inline.
- **`PreparedStatement` holds the SQLite connection mutex for its lifetime.**
  The shared `sqlite3*` is opened `SQLITE_OPEN_NOMUTEX`, so SQLite does no
  internal serialization. An earlier per-call-locking design let the db-writer
  thread's `insert` interleave with the read-loop thread's `set_run_status`
  on the same connection and corrupted SQLite's internal memory pool — an
  intermittent SEGV in `sqlite3_prepare_v2` (`sqlite3DbMallocRawNN`) that hid
  from gdb (timing) and from ASan (the test binary wasn't instrumented). Fix:
  `PreparedStatement` acquires the `std::recursive_mutex` in its ctor and
  releases it in its dtor; bind/step/reset/column run under that held lock.
  Recursive so `insert_recursive` (subtests) and `upsert_run_summary`-inside-
  `insert` can nest on the same thread without self-deadlock; cross-thread
  statements serialize. See `CLAUDE.md` Do/Don't for the full account. ASan is
  now wired to instrument the test binary + all in-repo libs (directory-level
  `add_compile_options/add_link_options` after the HAL subproject, before
  `src/`+`tests/`), which is what made the race catchable.

## Files (representative — pattern, not exhaustive enumeration)

### Build / tooling (new, mirror Arches-Client's §12 shape)
- `CMakeLists.txt` (root) — `project(arches-server VERSION 0.1.0)`, C++17 strict,
  HAL `add_subdirectory(... EXCLUDE_FROM_ALL)`, `find_package(spdlog/yaml-cpp/
  SQLite3/GTest)`, `-Werror` option (`cmake/CompilerWarnings.cmake`),
  `ENABLE_SANITIZERS` (directory-level, post-HAL), Doxygen target, CPack
  (.deb/.rpm/.ipk), `enable_testing()`, `ARCHES_SERVER_VERSION`.
- `CMakePresets.json` — Ninja presets: `native`/`debug`/`release`/`asan`/`strict`.
- `Makefile` — `build|test|lint|run|clean|format|cppcheck|doc|installer`
  (single entry point; `installer` = `cpack -G DEB && cpack -G RPM` + `.deb`→`.ipk` rename).
- `cmake/version.hpp.in`, `Doxyfile.in`, `cmake/CompilerWarnings.cmake`.

### Source (ported from Acadia-Server, adapted to C++17 + SQLite + `arches::server`)
- `src/main.cpp` — `load_settings` → spdlog → `init_db_client` →
  `MigrationRunner` → `ModuleRegistry::load` → `ClientStreamRegistry` →
  `RunService` + `LogService` → `reconcile_stale_runs` → `HalServiceImpl` +
  `ClientServiceImpl` + `GrpcServerRunner` → signal-safe shutdown
  (`pthread_sigmask` + `sigwait` worker, never `Server::Shutdown()` from a handler).
- `src/config/Settings.{h,cc}` — env-var `ARCHES_GRPCLISTEN/DB_PATH/REGISTRY_PATH/
  STREAM_QUEUE_SIZE/LOG_LEVEL/SHUTDOWN_TIMEOUT_SEC` (env-var only, no TOML).
- `src/db/{DbClient,MigrationRunner}.{h,cc}` — SQLite wrapper + migration runner.
- `migrations/0001..0010_*.sql` — ported subset (see the SQLite table above).
- `src/repositories/{Run,Result,Log,Target,TestConfig}Repository.{h,cc}` —
  SQLite prepared statements. Phase-2 repos (User/ApiToken/Audit/Schedule/Ota/
  Notification/Template) not ported.
- `src/registry/ModuleRegistry.{h,cc}` — ported as-is (YAML, carries
  `result_type_url`).
- `src/util/{ResultConvert,Time,Hash,Uuid,ProtoJson,Errors}.{h,cc}` —
  `ResultConvert` adds `unpack_module_results`; minimal JSON serializer (no
  JsonCpp); `std::random_device` UUIDs (no OpenSSL).
- `src/grpc_server/StreamBackpressure.h` — `BoundedQueue<T>` (ported as-is).
- `src/grpc_server/{ClientServiceImpl,HalServiceImpl,GrpcServerRunner}.{h,cc}`.
- `src/stream_registry/ClientStreamRegistry.{h,cc}` — **new** (run-control push).
- `src/services/{RunService,LogService}.{h,cc}` — run lifecycle + run-control
  push + `set_run_status` + `reconcile_stale_runs`; log ingest.

### Tests
- `tests/unit/util/ResultConvertTest.cc` — ported + §5 cases (pack
  `hal.modules.cpu.Result{results:[r1,r2]}`, assert 2 TestResults; bare-TestResult
  fallback → 1 element).
- `tests/unit/grpc/{StreamBackpressure,RunStateMapping}Test.cc` — ported + new
  RunState→status mapping.
- `tests/unit/registry/ModuleRegistryTest.cc`,
  `tests/unit/util/{Time,Hash,Uuid}Test.cc` — ported.
- `tests/integration/{MigrationRunnerTest,ClientStreamInteropTest}.cc` —
  migration test (real migrations dir) + the end-to-end interop proof.
- `tests/CMakeLists.txt` — unit → `arches-unit-tests`, integration →
  `arches-integration-tests`, all labelled `arches_server`; link
  `hal_protos_shared` + `arches-core` + GTest.

## Order & dependencies across components

This change is **server-only at the proto-package level**. It depends on
Arches-HAL exposing `hal_protos_shared` (shared generated protos + gRPC,
PUBLIC protobuf+gRPC). The §5 module-Result payload change was made in
**Arches-Client** (approved there); this port makes the server **consume** it
(no further contract change). Sibling status:
- **Arches-Client** — packs `hal.modules.<id>.Result` (done).
- **Arches-Server** — unpacks it (this port).
- **Arches-PySDK** — must unpack the same wrapper on the consume side (still
  pending; tracked separately).
- **Arches-Dashboard** — no change (reads server-resolved data, not the raw Any).

## Platform-specific considerations (Arches RT element = STM32 co-processor)

The server runs on the **x86 host**, not the Jetson. The STM32 co-processor is
reached by the client via Arches-HAL; the server never touches the RT element
directly — it only sees `TestResultBatch`es over the `ClientService` stream and
pushes `ServerMessage` run-control back. No server-side RT code. CPack emits
`.deb`/`.rpm`/`.ipk` (the `.ipk` for Yocto/opkg on the host-side packaging path).

## Verification

```
$ make build  → [23/23] Linking CXX executable bin/arches-server   (green, native)
$ make test   → 31/31 tests passed, label arches_server (16.4s)    (green)
$ make lint   → cppcheck style notes only; clang-tidy absent → exit 0 (pass)

# ASan gate (race regression check):
$ cmake --preset asan && ninja -C build/asan
$ ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/asan -L arches_server
  → 31/31 passed, 0 ASan/UBSan errors
$ # interop loop (native): 15/15 passed, 0 segfault, 0 hang
```

`compile_commands.json` exported so `make lint` works where the tools are
present. The interop test runs ~8s (two `wait_for` polls at 4s budgets);
revisit DB-writer / `set_run_status` latency in Phase 2.

## Acceptance

- [x] `make build` green (C++17, Ninja, `-Werror` option)
- [x] `make test` green via `ctest -L arches_server` (31/31, unit + integration)
- [x] `ClientStreamInteropTest` proves server→client run-control push +
      client→server §5 module-Result unpack/persist/ack + `batch.state()` mapping
- [x] §5 unpack in `ClientServiceImpl::handle_result_batch` and
      `HalServiceImpl::persist_result`; `proto_to_result` unchanged;
      bare-`TestResult` fallback preserved
- [x] Run-control push over the bidi stream implemented (`ClientStreamRegistry`
      + `RunService`)
- [x] SQLite DB (not Postgres); deviation flagged (this note)
- [x] Proto packages unchanged (`acadia.stream`, `hal.*`) — §5 respected
- [x] Namespace `arches::server`, targets `arches-*`, paths `/etc/arches` /
      `/var/lib/arches`, `ARCHES_SERVER_VERSION`
- [x] `make lint` runs (graceful if tools absent)
- [x] SQLite interleave race diagnosed (ASan) + fixed (lifetime-locking
      `PreparedStatement`); documented in `CLAUDE.md` Do/Don't
- [x] Local `CLAUDE.md` Do/Don't records the working approach (§11)
- [ ] No commit/push (user has not asked)