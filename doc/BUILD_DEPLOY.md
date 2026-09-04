# Arches-Server Build and Deployment

## 1. Purpose and scope

Arches-Server is the x86 host-side C++ gRPC IV&V server. Its Makefile is native-only and builds with CMake/Ninja; it explicitly does not cross-build or deploy to the Jetson target. The CMake project also includes the sibling `Arches-HAL` subproject.

## 2. Prerequisites

Install a C++17 compiler, CMake, Ninja, CTest, and the CMake dependencies declared by the project, including Threads, gRPC/Protobuf, spdlog, yaml-cpp, SQLite3, and GTest for tests. Install `cppcheck`, `clang-tidy`, `clang-format`, `cmake-format`, and Doxygen as needed for optional checks/docs.

## 3. Setup

From the repository root, run `make configure`. This creates `build/native` and exports `compile_commands.json`. The build expects the sibling `Arches-HAL` repository at the path used by `CMakeLists.txt`; keep the nine-repository layout intact.

## 4. Build and compile

- `make build` configures if needed and runs the native Ninja build.
- `make` is an alias for `build`.
- `make format` runs the Ninja format target and best-effort `cmake-format`.
- `make doc` runs the configured documentation target when Doxygen is available.

## 5. Test and lint

- `make test` builds first, then runs `ctest -L arches_server --output-on-failure` so unrelated unbuilt HAL tests are excluded.
- `make cppcheck` runs best-effort cppcheck using compile commands.
- `make lint` runs the configured static-analysis path; missing analyzers are reported/skipped according to the Makefile.

## 6. Packaging and artifacts

`make installer` builds then invokes CPack, producing the configured `.deb`/`.rpm` package output in the native build directory. The executable and libraries are under `build/native`; CMake installs migrations below the configured data directory in the package.

## 7. Deployment, install, and run

- `make run` builds then runs `arches-server` in the x86 Docker container (gRPC 50051 published to the host; SQLite DB persisted at `build/docker/run/server.db`).
- Install the CPack artifact with the host package manager when a package is produced; the exact package manager is environment-dependent.
- No cross deployment, board flashing, SSH target deployment, cloud image, or registry publishing target is defined.

Runtime configuration, database/migration setup, service supervision, and production credentials must be supplied by the host deployment environment.

## 8. Cleanup

`make clean` removes `build`. Re-run `make configure` after cleanup before using compile-command-dependent lint or formatting targets.

## 9. Hardware, cloud, and manual verification caveats

The server is host-side software and its labeled tests do not validate a complete Jetson/HAL deployment. Pair it with a running compatible HAL/client environment for integration verification, and manually check gRPC connectivity, migrations, configuration, and service lifecycle. Cloud deployment is outside the Makefile.
