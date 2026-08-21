# Arches-Server

## Project Summary

Arches-Server is the central C++ backend for Arches validation runs. It exposes REST controllers for authentication, targets, modules, templates, configurations, runs, schedules, logs, notifications, alerts, settings, and OTA operations, alongside gRPC implementations for client and HAL services. The orchestration layer contains run management, scheduling, target heartbeats, OTA coordination, retention, and backpressure. Service classes and repositories separate business behavior from PostgreSQL access, while migration files version runs, results, logs, OTA state, audit records, notifications, alerts, settings, and result fields.

Configuration and logging are handled by dedicated internal and source modules. `DrogonAppFactory` assembles the REST application, `AuthFilter` protects routes, and utility code covers hashing, UUIDs, time, protobuf/JSON conversion, and result conversion. Docker, local setup scripts, CMake presets, generated-protocol tooling, and CI workflows define supported development and deployment paths. The TOML configuration and `.env.example` show runtime configuration boundaries.

Automated tests include migration integration, gRPC stream/backpressure and run-state mapping, module registry, service, threshold, conversion, hash, time, and UUID cases. They provide focused unit and integration evidence, but do not establish a complete deployed database, client, browser, or hardware workflow. The feature table therefore uses partial and unknown statuses where source exists without a matching end-to-end test. Manual verification is intentionally left blank for every feature.

The server depends on PostgreSQL, generated protocol compatibility, runtime credentials, and deployment configuration. Review migration startup, authentication, streaming reconnects, retention, OTA failure handling, and REST/gRPC interoperability with real client components before release. No manual verification is implied by focused unit tests.

## Features

| # | Name / Description | Implemented? | Test Coverage? | Manual Test |
|---:|---|---|---|---|
| 1 | REST API application and controllers | Implemented | Partial: focused service tests | [ ] Manual verification required |
| 2 | gRPC client and HAL services | Implemented | Stream and state mapping tests | [ ] Manual verification required |
| 3 | Run orchestration and scheduling | Implemented | Service tests; end-to-end partial | [ ] Manual verification required |
| 4 | Target registration and heartbeats | Implemented | Partial | [ ] Manual verification required |
| 5 | Results, logs, retention, and exports | Implemented | Result and migration tests | [ ] Manual verification required |
| 6 | OTA orchestration and persistence | Implemented | Partial | [ ] Manual verification required |
| 7 | Auth, API tokens, and route filtering | Implemented | Partial | [ ] Manual verification required |
| 8 | Module registry and templates/configuration | Implemented | `ModuleRegistryTest.cc` | [ ] Manual verification required |
| 9 | PostgreSQL migrations | Implemented | `MigrationRunnerTest.cc` | [ ] Manual verification required |
| 10 | Docker, local setup, and CI workflows | Implemented | Unknown | [ ] Manual verification required |

## Validation

Use CMake presets or the Makefile; Docker helpers are under `docker/`. Unit and integration targets are defined in `tests/` and `CMakeLists.txt`.

## Existing Documentation

TODO: add a description for Arches-Server.
