---
name: arches-server
description: Working in Arches-Server, the C++17 gRPC IV&V server for the Arches fleet (runs on the x86 host, not the Jetson). Covers build/test/lint/run via the Makefile, the HAL-subproject link pattern, SQLite schema/migration porting from the prior Postgres reference implementation, the layered services/stream_registry/grpc_server architecture, and the concurrency rules around the shared SQLite connection and the bidi gRPC stream. Use when writing, reviewing, or debugging code anywhere in this repo.
---

# Arches-Server development

Arches-Server implements arches.stream::ClientService, the bidi-stream gRPC
peer that Arches-Client drives test runs from. It ingests HAL test results
into SQLite and pushes run-control (Start/Stop/Pause/Resume/Restart) back to
the client. It links Arches-HAL as a sibling subproject for all generated
proto/gRPC code — the same single-source-of-truth pattern as Arches-Client.
DB is SQLite, a deliberate deviation from the prior reference implementation's
Postgres/Drogon stack (see server_update/10_arches_port.md).

## Entry points

- Build: make build (Ninja, build/native/, C++17 strict, ARCHES_SERVER_VERSION)
- Test:  make test (runs ctest -L arches_server — never run bare ctest,
  the linked HAL subproject registers ~36 unbuilt HAL tests that fail otherwise)
- Lint:  make lint (cppcheck + clang-tidy vs compile_commands.json, best-effort)
- Run:   make run (Docker x86; binary build/docker/bin/arches-server, gRPC 50051 published)
- Clean: make clean
- Validate gate: ./.claude/validate.sh quick|full (quick = seconds; full = + build + test).
  Exit codes: 0 pass, 1-76 fail, 77 a check never ran (BLOCKED, not a pass —
  never report it as success).

## Architecture (layered, in link order)

1. arches_stream_registry (src/stream_registry/) — ClientStreamRegistry:
   board_id → shared_ptr<BoundedQueue<ServerMessage>>, the run-control push path.
2. arches_services (src/services/) — RunService + LogService, depend on
   arches_repos + arches_stream_registry.
3. arches_grpc (src/grpc_server/) — ClientServiceImpl + HalServiceImpl +
   GrpcServerRunner, depend on arches_services + arches_stream_registry.

This order exists to break a cycle: RunService needs the registry (to push
run-control), ClientServiceImpl needs RunService. Add new cross-cutting
logic at the correct layer instead of reaching across it.

- ClientServiceImpl::Connect registers/deregisters the board's outbound
  queue on HelloReport/disconnect (guarded by pointer identity so a reconnect
  can't yank the fresh queue), and runs a dedicated outbound writer thread
  draining the queue — gRPC forbids concurrent writes on one bidi stream, so
  acks (from the read loop) and run-control (from RunService) both route
  through ClientStreamRegistry::push(board_id, msg), never a direct
  stream->Write() from more than one thread.
- On a terminal batch, RunService::set_run_status(run_id, batch.state()) maps
  RunState → runs.status and sets started_at/ended_at appropriately.
- util::ResultConvert::unpack_module_results is the server-side inverse of
  the client's pack_module_result (reflection over hal.modules.<id>.Result,
  falling back to bare hal.common.TestResult). Any TU touching Result
  fields needs repositories/ResultRepository.h, not just the forward decl in
  ResultConvert.h.
- main.cpp wiring order: settings → spdlog → db::init_db_client →
  MigrationRunner.run() → ModuleRegistry::load → ClientStreamRegistry →
  RunService/LogService → reconcile_stale_runs() → HalServiceImpl +
  ClientServiceImpl + GrpcServerRunner → signal setup → runner.start().
- Signal-safe shutdown: block SIGINT/SIGTERM with pthread_sigmask before
  runner.start(), then a dedicated thread sigwaits and calls
  runner.shutdown(). Never call Server::Shutdown() from a signal handler.

## SQLite concurrency (the one rule that matters most)

One shared sqlite3* opened SQLITE_OPEN_NOMUTEX (no internal serialization)
+ one std::recursive_mutex (g_db_mutex). PreparedStatement holds the
mutex for its entire lifetime — acquired in the ctor around
sqlite3_prepare_v2, released in the dtor after sqlite3_finalize; bind/
step/reset/column accessors run under the already-held lock and don't
re-lock. This makes each PreparedStatement one fully-serialized critical
section so two statements on the shared connection can never interleave
prepare/bind/step/reset. Recursive because ResultRepository::insert_recursive
opens statements while recursing into subtests on the same thread.

Do not revert to per-call locking (locking only around each individual
bind/step/reset). That was tried and caused an intermittent SEGV deep in
sqlite3_prepare_v2 — the db-writer thread's insert interleaved with the
gRPC read-loop thread's set_run_status/set_ended on the same connection
and corrupted SQLite's memory pool. It was deterministic only under ASan, and
only once ENABLE_SANITIZERS was rewired to instrument the test binary + all
in-repo libs (not just arches-server — the earlier target-only form left
the test exe uninstrumented and ASan couldn't see it).

- Build IN (?,?,...) with bound params, one placeholder per value — never
  string-concatenate values into SQL.
- PRAGMA foreign_keys=ON is set at init_db_client (SQLite defaults it off).
- The server assigns all ids (util::generate_uuid(), std::random_device,
  no OpenSSL) — SQLite migrations have no gen_random_uuid() default and
  there's no RETURNING clause; create() returns the struct the caller passed.

## Porting Postgres → SQLite (when touching migrations/)

- No ALTER TABLE ADD CONSTRAINT CHECK — SQLite only allows CHECK at CREATE
  TABLE time. DB-level invariants added later live in the service layer instead.
- No constant-only unique index (CREATE UNIQUE INDEX ON t ((TRUE))) — use a
  BEFORE INSERT ... WHEN (SELECT COUNT(*) FROM t) >= 1 BEGIN SELECT
  RAISE(FAIL,...) END trigger for single-row tables.
- ON CONFLICT (version) DO NOTHING → INSERT OR IGNORE.
- No JSONB/GIN/COMMENT ON/array_agg()[1]/gen_random_uuid() — map to
  TEXT/no-index/--comment/server-assigned-ids respectively per
  server_update/10_arches_port.md. TIMESTAMPTZ → TEXT, BYTEA → BLOB.
- No JSONB operators (->, ->>, ?), array ops (= ANY($1::int[])), or FTS
  (to_tsvector @@ plainto_tsquery) — rewrite as json_extract(...),
  IN (?,?,?), LIKE '%q%'. These queries (trend/distribution/compare) are
  Phase 2, deferred.
- Migrations run whole-file via sqlite3_exec (handles -- comments and
  multiple statements). Keep each migration's own BEGIN/COMMIT.
- Don't assume the sqlite3 CLI is installed on the host — test SQLite
  behavior through the C API instead (see MigrationRunnerTest.cc).
- Never use std::mktime for UTC ISO-8601 parsing (it assumes local time) —
  use timegm.

## C++/CMake conventions

- Only Arches-HAL generates protos. Link hal_protos_shared via
  add_subdirectory(${CMAKE_SOURCE_DIR}/../Arches-HAL ${CMAKE_BINARY_DIR}/arches_hal_build EXCLUDE_FROM_ALL).
  No local find_package(gRPC/Protobuf), no local protoc — a "duplicate
  symbol" linker error against generated *.pb.h/.pb.cc means a second
  codegen step crept in.
- -Wall -Wextra -Wpedantic, -Werror opt-in via WARNINGS_AS_ERRORS
  (cmake/CompilerWarnings.cmake; the strict CMake preset turns it on). The
  codebase is -Werror-clean — keep it that way rather than suppressing.
- switch over an open protobuf enum needs a default: (protoc emits
  sentinel values that trip -Wswitch), even when a trailing return already
  covers the fallback.
- Qualify gRPC types as ::grpc::X (or arches::server::grpc::X) inside any
  scope with using namespace arches::server; — the nested arches::server::grpc
  namespace shadows plain grpc::.
- No unused helpers in anonymous namespaces (-Wunused-function fires).
- ENABLE_SANITIZERS instruments the test binary + all in-repo arches_*
  libs (directory-level add_compile_options/add_link_options, placed after
  the HAL add_subdirectory so HAL itself stays uninstrumented, and before
  src/+tests/). Run with ASAN_OPTIONS=detect_leaks=0 (gRPC leaves
  threads alive at exit → leak false positives):
  cmake --preset asan && ninja -C build/asan && ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/asan -L arches_server.

## Common pitfalls

- Bare ctest fails on HAL's unbuilt tests — always ctest -L arches_server
  or make test.
- Stale CMakeCache.txt silently keeps an old cached option value across
  reconfigures — rm -rf build/native when toggling one.
- Backgrounding rm … && server & and using $! captures the subshell PID,
  not the server's — background the server binary alone.
- An intermittent integration-test segfault is more likely the SQLite
  connection-interleave race than a protobuf/transit bug — reach for ASan
  (with libs instrumented) before assuming corruption elsewhere in the stack.

## Checklist before committing

1. make build clean with no new warnings.
2. make test (never bare ctest) — confirm -L arches_server ran.
3. ./.claude/validate.sh quick (or full if touching build/migrations).
4. If you touched a shared .proto or wire type, confirm Arches-Client and
   Arches-PySDK were updated in the same change set, not a follow-up.
