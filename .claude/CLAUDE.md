# CLAUDE.md — Arches-Server
Master SoCcentric CLAUDE.md applies. This file adds local knowledge only.
Arches-Server is the C++17 gRPC IV&V server (runs on the x86 host, not the
Jetson). It links the Arches-HAL subproject for the shared generated protos and
serves the `arches.stream::ClientService` bidi stream that Arches-Client drives
runs from. DB is SQLite (master §4 "gRPC + SQLite"), a deviation from
the prior reference implementation's PostgreSQL/Drogon stack — flagged in `server_update/10_arches_port.md`.

- Build: `make build` (Ninja, `build/native/`, C++17 strict, `ARCHES_SERVER_VERSION`)
- Test:  `make test` (runs `ctest -L arches_server`)
- Lint:  `make lint` (cppcheck + clang-tidy vs `compile_commands.json`, best-effort)
- Run:   `make run` (`build/native/bin/arches-server`)
- Clean: `make clean`

## Do
- **HAL subproject linking:** `add_subdirectory(${CMAKE_SOURCE_DIR}/../Arches-HAL
  ${CMAKE_BINARY_DIR}/arches_hal_build EXCLUDE_FROM_ALL)` then link `hal_protos_shared`
  (SHARED). No `find_package(gRPC/Protobuf)`, no local `protoc`. Single source of
  proto symbols → no ODR/duplicate-symbol risk. Mirrors Arches-Client exactly.
- **`ctest -L arches_server`:** always filter on this label. The HAL subproject's
  `enable_testing()` registers ~36 unbuilt HAL tests that fail bare `ctest`.
- **SQLite single-row enforcement:** use a `BEFORE INSERT ... WHEN (SELECT
  COUNT(*) FROM t) >= 1 BEGIN SELECT RAISE(FAIL,...) END` trigger. Postgres's
  `CREATE UNIQUE INDEX ON board ((TRUE))` does NOT port — SQLite rejects a
  constant-only index expression with "no such column: 1".
- **Migration version tracking:** `INSERT OR IGNORE INTO schema_migrations`
  (Postgres `ON CONFLICT (version) DO NOTHING` → SQLite `INSERT OR IGNORE`).
- **Run migrations whole-file:** `exec(sql_file_contents)` via `sqlite3_exec`
  handles `--` comments + multiple statements natively. Keep each migration's
  own `BEGIN/COMMIT` for atomicity; the runner issues a best-effort `ROLLBACK` on
  failure before re-throwing with the version name for attribution.
- **SQLite connection threading:** one shared `sqlite3*` (opened
  `SQLITE_OPEN_NOMUTEX` — SQLite does NO internal serialization) + one
  `std::recursive_mutex` (`g_db_mutex`). A `PreparedStatement` HOLDS the mutex
  for its ENTIRE lifetime (acquired in the ctor around `sqlite3_prepare_v2`,
  released in the dtor after `sqlite3_finalize`); bind/step/reset/column
  accessors run under that already-held lock and do NOT re-lock. This makes each
  PreparedStatement one fully-serialized critical section, so two statements on
  the shared connection can NEVER interleave their prepare/bind/step/reset
  sequences. The mutex is RECURSIVE because `ResultRepository::insert_recursive`
  opens a statement then recurses into subtests opening more statements on the
  same thread (and `upsert_run_summary` opens one inside `insert`); same-thread
  re-acquisition must not self-deadlock. Cross-thread statements still serialize
  (a second thread's PreparedStatement blocks at its ctor until the first is
  destroyed). `exec()`/`init`/`shutdown` lock per-call as before.
- **Migration test uses the REAL migrations dir** (`ARCHES_MIGRATIONS_DIR`,
  a PUBLIC compile def on `arches_db`) — validates the actual schema, no skip
  gate (SQLite is always available, unlike the prior reference implementation's Postgres `GTEST_SKIP`).
- **PRAGMA foreign_keys=ON** at `init_db_client` so `ON DELETE CASCADE`
  constraints in the migrations actually fire (SQLite defaults to FK off).
- **Server assigns ids** (`util::generate_uuid()`): the SQLite migrations have no
  `gen_random_uuid()` defaults, so the service layer sets `id` before calling
  `repo.create(...)`. No `RETURNING` clause needed (Postgres RETURNING dropped) —
  `create()` returns the struct the caller passed in.
- **Build `IN (?,?,...)` with bound params**, one placeholder per value — not
  string concatenation. Avoids the SQL-injection flavour the prior reference
  implementation's `find_distribution` had (`in_clause << "'" << run_ids[i] << "'"`).
- **DB-level invariants SQLite can't express** (the `results.status > 0` CHECK,
  which Postgres enforced via `ALTER TABLE ADD CONSTRAINT` — unsupported in
  SQLite post-create) are guaranteed by the service layer instead: HAL never
  produces TEST_STATUS_UNSPECIFIED. `ResultRepository::insert` stores the raw
  value (no coerce) to preserve auditability, and assigns ids itself
  (`util::generate_uuid()`) since `proto_to_result` leaves them empty.
- **Minimal JSON serializer** in `util/ResultConvert` (Step 4) for the specific
  TypedValue/string-map shapes — no JsonCpp dependency. Parse direction (REST
  output) deferred to Phase 2.
- **`std::random_device` for UUIDs** — no OpenSSL RAND_bytes dependency just for
  non-secret run/result ids.
- **§5 module-Result unpack** (`util::ResultConvert::unpack_module_results`):
  the server-side inverse of the client's `pack_module_result`. Uses
  `DescriptorPool::generated_pool()` + `DynamicMessageFactory` + reflection on the
  `results` field of `hal.modules.<id>.Result`. Falls back to a bare
  `hal.common.TestResult` when the descriptor is absent or the type_url is the
  bare-TestResult form. Relies on `hal_protos_shared` having registered every
  module `Result` descriptor (it does — same SHARED lib as the client).
- **In tests, build module Result messages via the generated C++ class**
  (`hal::modules::cpu::Result`) directly — the server links `hal_protos_shared`,
  so the generated classes are available. No reflection needed for test setup.
- **`proto_to_result` recurses into subtests** (populating `r.subtests`) so
  `ResultRepository::insert` persists the whole tree in one call. It leaves
  `r.id` empty — the repo assigns ids via `util::generate_uuid()`.
- **Services↔gRPC dependency cycle (Step 5/6):** `RunService` needs
  `ClientStreamRegistry` (to push run-control) and `ClientServiceImpl` needs
  `RunService` — a cycle if both live in one lib each. Resolution: three layered
  static libs, added in this order in `src/CMakeLists.txt`:
  `arches_stream_registry` (`src/stream_registry/`, ClientStreamRegistry only,
  deps on `hal_protos_shared`) → `arches_services` (`src/services/`, RunService
  + LogService, deps on `arches_repos` + `arches_stream_registry`) →
  `arches_grpc` (`src/grpc_server/`, ClientServiceImpl + HalServiceImpl +
  GrpcServerRunner, deps on `arches_services` + `arches_stream_registry`).
  `StreamBackpressure.h` (BoundedQueue template) is header-only in `grpc_server/`,
  reached by all three via the `src/` include root.
- **`ClientStreamRegistry` is the run-control push path (gap-closure #1):**
  `board_id → shared_ptr<BoundedQueue<ServerMessage>>`. `ClientServiceImpl::Connect`
  registers the board's outbound queue on `HelloReport`, deregisters on disconnect
  (guarded by pointer identity so a reconnect can't yank the fresh queue). `RunService`
  pushes `ConfigureRun/StartRun/PauseRun/ResumeRun/StopRun/RestartRun` via
  `registry.push(board_id, msg)` → `out_queue->enqueue`. Tolerates re-Hello
  (re-register replaces + closes the stale queue).
- **Serialize all `stream->Write()` on one writer thread:** gRPC forbids
  concurrent writes on a single bidi stream. `Connect` runs a dedicated
  out-bound writer thread draining `out_queue` (run-control + acks); the read
  loop enqueues acks to `out_queue` instead of writing inline. The prior reference
  implementation wrote acks inline from the read thread and never wrote run-control,
  so it had no concurrency hazard — Arches adds the writer because it pushes run-control.
- **`batch.state()` mapping (gap-closure #2):** on a terminal batch call
  `RunService::set_run_status(run_id, batch.state())`, which maps
  `RunState→runs.status` (`run_state_to_status`: COMPLETED→completed, FAILED→
  failed, CANCELLED→cancelled, PAUSED→paused, RUNNING→running, CONFIGURED→queued,
  UNSPECIFIED→rejected). Terminal states set `ended_at` via `set_ended`; RUNNING
  sets `started_at`; others `update_status`. This replaces the prior reference
  implementation's blind `complete_run("completed")` on every terminal batch.
- **`switch` over a protobuf open enum needs a `default:`** — protoc emits
  `_INT_MIN_SENTINEL_DO_NOT_USE_` / `_INT_MAX_SENTINEL_DO_NOT_USE_` values;
  `-Wswitch` (under `-Wall`, on the path to `-Werror`) warns if they're unhandled.
  Add `default:` even when the trailing return already covers the fallback.
- **`results.mode` stores the `ExecutionMode` int as a string** (`std::to_string(
  static_cast<int>(batch.mode()))`) — a faithful port of the prior reference
  implementation's quirk, flagged for Phase-2 cleanup. `runs.mode` uses the readable
  name (`single_shot` etc.) via the file-local `execution_mode_name` in
  `RunService.cc`. Don't "fix" one without the other — they diverge by design
  (mirrors the prior reference implementation).
- **`main.cpp` wiring order (Step 7):** `load_settings()` → spdlog (stdout_color_mt
  + `set_level(level::from_str(log_level))`) → `db::init_db_client(db_path)` →
  `MigrationRunner{ARCHES_MIGRATIONS_DIR}.run()` → `ModuleRegistry::load` →
  `ClientStreamRegistry` → `RunService{stream_registry}` + `LogService` →
  `reconcile_stale_runs()` → `HalServiceImpl` + `ClientServiceImpl` +
  `GrpcServerRunner` → signal setup → `runner.start()` (blocks). `ARCHES_MIGRATIONS_DIR`
  is a PUBLIC compile def on `arches_db` — usable directly in main.
- **Signal-safe shutdown:** block SIGINT/SIGTERM in the main thread with
  `pthread_sigmask(SIG_BLOCK, …)` BEFORE `runner.start()` (so gRPC's internal
  threads inherit the blocked mask), then a dedicated worker thread calls
  `sigwait(&mask, &sig)` → `runner.shutdown()`. Never call `Server::Shutdown()`
  from a raw signal handler (not async-signal-safe). Verified: SIGTERM →
  "received signal 15 — shutting down" → clean exit 0 + DB close.
- **Env-var `Settings`** (`ARCHES_GRPC_LISTEN/DB_PATH/REGISTRY_PATH/STREAM_QUEUE_SIZE/
  LOG_LEVEL/SHUTDOWN_TIMEOUT_SEC`, env-var only, no TOML) via `config::load_settings()`.
- **§12 tooling (Step 9):** `CMakePresets.json` (Ninja, `native`/`debug`/`release`/
  `asan`/`strict` — `native` matches the Makefile's `build/native`; `strict` sets
  `WARNINGS_AS_ERRORS=ON` for the CI gate). `-Werror` is opt-in via the
  `WARNINGS_AS_ERRORS` option in `cmake/CompilerWarnings.cmake` (not on by default
  so local dev isn't blocked by new warnings). The codebase is `-Werror`-clean —
  verified by a forced strict rebuild. Doxygen is wired conditionally: `find_package(
  Doxygen QUIET)` + `add_custom_target(doc_doxygen)` from `Doxyfile.in`; if Doxygen
  isn't installed the target is absent and `make doc` reports it (best-effort, like
  `make lint`). CPack produces `.deb` + `.rpm` directly; the `.ipk` (Yocto/opkg) is
  a `cp`-rename of the `.deb` in the Makefile `installer` target (CPack has no
  first-class IPK generator). `ARCHES_SERVER_VERSION` is a compile def on
  `arches-server` (and `cmake/version.hpp.in` is generated for parity).
- **Makefile is the single entry point** (`make build|test|lint|run|clean|format|
  cppcheck|doc|installer`). `make lint` runs cppcheck then clang-tidy, both driven
  from `build/native/compile_commands.json`, both best-effort (skip + report if the
  tool is absent; fail nonzero if a present tool reports errors). Never bypass it
  with ad-hoc `clang-tidy`/`cppcheck` invocations.
- **`ENABLE_SANITIZERS` instruments the test binary + all in-repo libs, not just
  `arches-server`:** the option emits directory-level `add_compile_options/
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)`
  placed AFTER the HAL `add_subdirectory` (so the HAL subproject is NOT
  instrumented — it ships pre-built and its `-Werror`/external deps fight ASan)
  and BEFORE `src/`+`tests/`, so every arches_* static lib, the `arches-server`
  exe, and the test exes are instrumented. The earlier target-specific form
  (`target_compile_options(arches-server PRIVATE …)`) left the test binary
  uninstrumented, so ASan could not see the bidi-stream interop heap corruption
  — that's why the SQLite interleave race hid for so long. Run instrumented tests
  with `ASAN_OPTIONS=detect_leaks=0` (gRPC leaves threads alive at exit → leak
  false positives) and loop the interop test: `cmake --preset asan && ninja -C
  build/asan && ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/asan -L
  arches_server`.

## Don't
- **Don't `ALTER TABLE ADD CONSTRAINT CHECK`** — SQLite only allows CHECK at
  CREATE TABLE time. The `results.status > 0` invariant (Postgres enforced via
  ALTER TABLE ADD CONSTRAINT) is enforced in `ResultRepository` instead.
- **Don't use per-call locking for `PreparedStatement` on the shared `sqlite3*`**
  — an earlier design held `g_db_mutex` only during each bind/step/reset call,
  reasoning that bound params live in the `sqlite3_stmt*` so interleaving was
  safe. It was NOT: `sqlite3_prepare_v2`/`sqlite3_step`/column accessors touch
  the CONNECTION's shared internal state (parser, vdbe memory pool, lookaside
  allocator, per-connection error state), so the db-writer thread's `insert`
  interleaved with the gRPC read-loop thread's `set_run_status`→`set_ended` on
  the same `SQLITE_OPEN_NOMUTEX` connection and corrupted SQLite's memory pool.
  It surfaced as an intermittent SEGV deep in `sqlite3_prepare_v2`
  (`sqlite3DbMallocRawNN`) — caught deterministically by ASan once the test
  binary was instrumented (the original `ENABLE_SANITIZERS` only flagged
  `arches-server`, not the test exe / libs; fixed by directory-level
  `add_compile_options/add_link_options` placed AFTER the HAL `add_subdirectory`
  and BEFORE `src/`+`tests/`). Fix: hold the (recursive) mutex for the
  PreparedStatement's whole lifetime. The long-SELECT starvation that the
  per-call design tried to avoid is a Phase-2 concern (tiny result sets now).
- **Don't use Postgres-isms in SQLite migrations:** `JSONB`/`GIN`/`COMMENT ON`/
  `array_agg()[1]`/`gen_random_uuid()`/partial-`WHERE`-unique-index don't port
  directly. Adapt per the list in `server_update/10_arches_port.md`
  (JSONB→TEXT, drop GIN, COMMENT→`--`, gen_random_uuid()→server-assigned ids,
  `ON CONFLICT`→`INSERT OR IGNORE`, `TIMESTAMPTZ`→TEXT, `BYTEA`→BLOB).
- **Don't assume the `sqlite3` CLI is installed** on the host — it wasn't here.
  Test SQLite behaviour through the C API (the migration test does this).
- **Don't run bare `ctest`** — always `ctest -L arches_server` (HAL test pollution).
- **Don't port Postgres JSONB operators / array ops / FTS directly to SQLite:**
  `metrics -> k ->> 'value'`, `metrics ? k`, `status = ANY($1::int[])`,
  `to_tsvector @@ plainto_tsquery`, `FULL OUTER JOIN` have no direct SQLite form.
  Rewrite: `json_extract(metrics,'$.k.value')`, `IN (?,?,?)`, `LIKE '%q%'`,
  `LEFT JOIN ... UNION ... anti-join`. The trend/distribution/compare queries that
  used them are Phase 2 (deferred).
- **Don't use `std::mktime` to parse UTC ISO-8601** — it interprets the `tm` as
  local time (the prior reference implementation's `parse_iso8601` had this
  latent bug). Use `timegm`.
- **Don't leave unused helpers in an anonymous namespace** — `-Wunused-function`
  fires (and `-Werror` is the §12 option). Removed `add_clause` from
  `LogRepository.cc`.
- **Don't add JsonCpp / OpenSSL** as dependencies for Phase 1 — use the minimal
  built-in JSON serializer and `std::random_device` instead. Revisit if Phase 2
  REST needs richer JSON.
- **Don't chain `(*map)["k"].mutable_xxx()` to set a oneof on a protobuf map
  value** — it confuses the compiler (resolves to the `has_*` accessor). Build a
  temp `TypedValue`, call `set_*`, then assign into the map.
- **Don't use `Result` fields with only ResultConvert.h's forward declaration** —
  any TU that touches `Result` members must include
  `repositories/ResultRepository.h` for the full struct definition.
- **Don't put `ClientStreamRegistry` and `ClientServiceImpl` in the same lib as
  `RunService`** — `RunService` depends on the registry, `ClientServiceImpl`
  depends on `RunService`; one lib each is a link cycle. Use the three-layer
  split above (`arches_stream_registry` → `arches_services` → `arches_grpc`).
- **Don't call `stream->Write()` from the read thread while a writer thread
  also writes** — concurrent writes on one gRPC stream are UB. Route every
  `ServerMessage` (acks + run-control) through the single outbound queue +
  writer thread.
- **Don't `target_link_libraries(arches_grpc … arches_services)` before
  `arches_services` is defined** — CMake resolves target names at the call site.
  `add_subdirectory` order in `src/CMakeLists.txt` must be:
  `stream_registry` → `services` → `grpc_server`.
- **Don't write run-control `ServerMessage`s directly from `RunService` to the
  stream** — go through `ClientStreamRegistry::push(board_id, msg)` so the
  per-stream writer thread serializes the write and a missing board is logged,
  not crashed.
- **Don't use a bare `grpc::` type under `using namespace arches::server;`** —
  it's ambiguous between grpcpp's top-level `::grpc` and `arches::server::grpc`.
  Qualify as `arches::server::grpc::HalServiceImpl` etc. (main.cpp hit this, and
  `tests/integration/ClientStreamInteropTest.cc` hit it again — `spdlog::level::level::err`
  was also a typo there; the real fix was `spdlog::level::err` + qualifying every
  grpcpp use as `::grpc::` since the test file opens `using namespace arches::server;`).
- **Don't background a compound `rm … && server &` and use `$!` as the server
  PID** — `$!` is the subshell, not `server`; `kill $!` kills the subshell and the
  server keeps running (or gets re-parented). Background the server binary alone
  so `$!` is its PID.
- **Don't trust a single `ClientStreamInteropTest` segfault as a deterministic
  failure** — it was an intermittent (~1/5 full-suite, ~12-25% fresh-process)
  SQLite connection-interleave race (see the per-call-locking Don't above), NOT
  a protobuf/`tr`-transit bug. Long misdiagnosed as corruption in the §5 unpack
  / `IngestWork` queue transit (the `subtests=7` reading was freed-memory
  garbage from the SQLite pool corruption manifesting at `insert`, not a bad
  `TestResult`). gdb hid it (20 clean runs — its slowdown changes the
  interleaving); the `:0` port bind + `port_future()` did NOT fix it (port
  TOCTOU was a red herring). The decisive tool was ASan — but ONLY after
  rewiring `ENABLE_SANITIZERS` to instrument the test binary + libs, not just
  `arches-server`. Root cause + fix recorded above; the race is gone
  (15/15 native + 10/10 ASan + 31/31 full-suite ASan). The interop test runs
  ~8s — the two `wait_for` polls (status→"completed", result_repo.count≥1)
  approach their 4s budgets; revisit DB-writer / set_run_status latency in
  Phase 2.
## Validation gate — `.claude/validate.sh`

```bash
./.claude/validate.sh quick   # seconds: sibling-HAL presence, migration ordering, cppcheck/clang-tidy/clang-format
./.claude/validate.sh full    # + build + `ctest -L arches_server`
```

Exit codes follow the fleet contract: `0` pass, `1`-`76` fail, **`77` means a
check never ran** (missing toolchain) — which is not a pass. Runs automatically
via the `Stop` hook in `.claude/settings.json`, on demand via `/validate`, and
as part of any loop the engine executes here.

Human-owned: the Loop Engine runs this script and never edits it. If a check is
wrong, fix it yourself or raise it in the loop's Lessons Learned.

<!-- BEGIN loop-engine -->
## Do's and Don'ts (maintained by the Loop Engine)

*Appended by the Loop Engine on loop close; curated by a human when approving a
loop out of `in-review/`. Nothing outside this fence is engine-writable.*

<!-- END loop-engine -->
