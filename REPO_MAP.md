# Arches-Server

**Remote:** `git@github.com:soccentric-arches/Arches-Server.git`
**Parent project:** [Arches](../Arches.md) (target hardware: **NVIDIA Jetson** (Tegra family, L4T/JetPack OS) application processor with an **STM32** real-time co-processor.)
**Leaf type:** `<Project>-Server`
**Status:** REAL — actively built out (Phase 1 complete)

**Status: real, not a stub.** `README.md` is still an untouched placeholder, but the repo has a full C++17 gRPC IV&V server implementation: `CMakeLists.txt`, `CMakePresets.json`, `Makefile`, `src/` (config, db, repositories, registry, services, stream_registry, grpc_server, main.cpp), `tests/` (unit + integration, ASan-verified), `migrations/` (SQLite, ported from the prior reference implementation's Postgres schema). See `CLAUDE.md` in this repo for the full architecture and gotchas.

Implements `arches.stream::ClientService`, the bidi-stream gRPC peer that Arches-Client drives test runs from — ingests HAL test results into SQLite and pushes run-control (Start/Stop/Pause/Resume/Restart) back to the client. Links Arches-HAL as a sibling subproject for all generated proto/gRPC code (single source of truth, same pattern as Arches-Client). Diverges intentionally from the prior reference implementation's Postgres/Drogon stack: SQLite instead of Postgres, no Drogon REST layer yet.

**Phase 1 (interop only) is done and tested** (15/15 native + 10/10 ASan + 31/31 full-suite ASan on the client-stream interop path). **Deferred to Phase 2:** Dashboard-facing REST controllers, OTA, scheduler, anomaly/compare queries, auth (users/api_tokens tables exist but are unused), notifications, templates, JSON parse direction.

---
Part of the `Arches` project in the `project-managers` fleet. See `docs/project-managers.md` for the full 9-project / 8-leaf hierarchy and shared conventions.
