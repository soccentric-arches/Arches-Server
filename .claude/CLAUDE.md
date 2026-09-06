# Arches-Server

C++17 gRPC IV&V server. Runs on an x86 host, not the Jetson. Ingests Arches-Client's result stream
into SQLite and pushes run-control back.

## How you work here

* Commands: `.claude/commands/` — build, test, lint, validate.
* Write: three layered libs, in this order in `src/CMakeLists.txt` — `arches_stream_registry` -> `arches_services` -> `arches_grpc`. That order is what breaks the `RunService` / `ClientServiceImpl` cycle.
* Test: always `ctest -L arches_server`. Bare `ctest` runs ~36 unbuilt HAL tests and fails.
* Test under ASan with `ASAN_OPTIONS=detect_leaks=0` — gRPC leaves threads alive at exit, which reads as leaks.

## How you validate

* Gate: `./.claude/validate.sh quick` (seconds) or `full` (adds build + tests). Run it before you claim done.
* Exit codes: `0` pass, `1`-`76` fail, `77` a check never ran. **77 is not a pass.**
* The gate and everything under `.claude/lib`, `hooks`, `commands`, `settings.json` is human-owned. Run it, never edit it.

## Do

* Link the HAL subproject and link `hal_protos_shared` (SHARED). No `find_package(gRPC/Protobuf)`, no local protoc. Mirrors Arches-Client exactly.
* Enforce single-row tables with a `BEFORE INSERT ... RAISE(FAIL)` trigger. Postgres's constant-only unique index does not port — SQLite rejects it.
* Track migration versions with `INSERT OR IGNORE INTO schema_migrations`.
* Run migrations whole-file through `sqlite3_exec`; keep each migration's own `BEGIN`/`COMMIT`.
* Hold the mutex for a `PreparedStatement`'s entire lifetime — acquired around `sqlite3_prepare_v2`, released after `sqlite3_finalize`. Accessors run under the held lock and never re-lock.
* Point the migration test at the real migrations dir (`ARCHES_MIGRATIONS_DIR`), no skip gate.
* `PRAGMA foreign_keys=ON` at `init_db_client` — SQLite defaults FK enforcement off, so `ON DELETE CASCADE` silently does nothing without it.
* Assign ids server-side via `util::generate_uuid()` before `repo.create(...)`. The migrations have no `gen_random_uuid()` default.
* Build `IN (?,?,...)` with one bound placeholder per value, never string concatenation.
* Enforce `results.status > 0` in the service layer — SQLite can't add a CHECK after create. `ResultRepository::insert` stores the raw value for auditability.
* Use the minimal JSON serializer in `util/ResultConvert` and `std::random_device` for UUIDs. No JsonCpp, no OpenSSL.
* Build module Results in tests via the generated C++ class directly (`hal::modules::cpu::Result`) — no reflection needed for setup.
* Let `proto_to_result` recurse into subtests so one `insert` persists the whole tree. It leaves `r.id` empty for the repo to fill.
* Map `batch.state()` through `RunService::set_run_status`. Terminal states set `ended_at`, RUNNING sets `started_at`, others `update_status`.
* Add `default:` to any switch over a protobuf enum — protoc emits sentinel values and `-Wswitch` fires without it.
* Keep `results.mode` (int as string) and `runs.mode` (readable name) divergent. They differ by design; don't "fix" one alone.
* Configure through env vars only, via `config::load_settings()`. No TOML.
* Let `ClientStreamRegistry` tolerate a re-Hello — re-register replaces and closes the stale queue, guarded by pointer identity so a reconnect cannot yank the fresh one.
* Go through the Makefile for everything. Never ad-hoc clang-tidy or cppcheck.

## Instrumentation: this repo does NOT report to istate

**Decided 2026-09-06 (Task.md E-07). Recorded here so it stops being an open
question in every review.**

Arches-HAL and Arches-Client declare alarms to Arches-Instrumentation over
`ApplicationApi`. **Arches-Server does not, and should not.**

Instrumentation is an *on-device* suite. It ships in the Jetson image, its
socket is `/run/instrumentation/application.sock` on that device, and its whole
model is a device describing itself. Arches-Server runs on an **x86 bench
host** — it is the peer the device streams results *to*. There is no istate on
that host to talk to, and putting one there would mean shipping the device
management suite onto a workstation to report on a workstation.

So for this repo, E-07 reduces to **structured logging only**, and that is what
it got: `BoundedQueue::enqueue` now logs once per saturation episode and once
on recovery (hysteresis at half capacity). Before that, ingest falling behind
was completely silent — a blocked enqueue is the backpressure working, but from
outside it looked identical to ingest keeping up, while the client on the other
end was being throttled and its spool grew.

Alarm ids **are** allocated for `arches-server` in the catalog (application
domain, module `0x12`: `server_ingest_backlog`, `server_db_write_failed`). They
are reserved, not orphaned. Allocation is one-way — the alarm layout is locked
and an id that ships cannot be reused — so they were allocated at the same time
as HAL's and Client's rather than deferred. If Server is ever colocated with a
device, or an istate is ever run on the bench host, the ids are waiting.

Do not "finish" E-07 by adding an `ApplicationApi` client here. Reopen the
decision first.

## Don't

* **Don't `ALTER TABLE ADD CONSTRAINT CHECK`.** SQLite only allows CHECK at create time.
* **Don't lock per-call on a shared `sqlite3*`.** `prepare_v2`/`step`/column accessors touch the connection's shared internal state; interleaving corrupted SQLite's memory pool and surfaced as an intermittent SEGV. Hold the recursive mutex for the whole statement lifetime.
* **Don't use Postgres-isms in migrations** — JSONB, GIN, `COMMENT ON`, `gen_random_uuid()`, partial unique indexes, TIMESTAMPTZ, BYTEA. Adapt per `server_update/10_arches_port.md`.
* **Don't assume the `sqlite3` CLI exists.** Test SQLite behaviour through the C API.
* **Don't run bare `ctest`.**
* **Don't port Postgres JSONB / array / FTS operators.** Rewrite as `json_extract`, `IN (?,?,?)`, `LIKE`, and a LEFT JOIN + UNION anti-join.
* **Don't parse UTC ISO-8601 with `std::mktime`** — it interprets the `tm` as local time. Use `timegm`.
* **Don't leave unused helpers in an anonymous namespace** — `-Wunused-function` fires under `-Werror`.
* **Don't chain `(*map)["k"].mutable_xxx()`** to set a oneof on a map value; it resolves to the `has_*` accessor. Build a temp `TypedValue`, set, then assign.
* **Don't touch Result members with only `ResultConvert.h`'s forward declaration.** Include `repositories/ResultRepository.h`.
* **Don't put `ClientStreamRegistry` and `ClientServiceImpl` in the same lib as `RunService`** — link cycle. One lib each.
* **Don't `stream->Write()` from the read thread** while a writer thread also writes. Concurrent writes on one gRPC stream are UB — route every `ServerMessage` through the outbound queue.
* **Don't `target_link_libraries` a target before it is defined.** CMake resolves names at the call site.
* **Don't push run-control straight from `RunService` to the stream.** Go through `ClientStreamRegistry::push(board_id, msg)`.
* **Don't use a bare `grpc::` type under `using namespace arches::server;`** — ambiguous with `::grpc`. Qualify it.
* **Don't background `rm ... && server &` and use `$!`** — that is the subshell's PID, so `kill $!` leaves the server running. Background the binary alone.
* **Don't treat a single interop segfault as deterministic.** It was a SQLite connection-interleave race, ~12-25% on a fresh process, only caught reliably once ASan instrumented the test binary and libs — not just `arches-server`.

<!-- BEGIN loop-engine -->
## Do's and Don'ts (maintained by the Loop Engine)

Appended by the Loop Engine on loop close; curated by a human when approving a loop out of in-review/. Nothing outside this fence is engine-writable.

<!-- END loop-engine -->
