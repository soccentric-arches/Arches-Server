# ---------------------------------------------------------------------------
# tests/CMakeLists.txt — Arches-Server tests. All Arches-Server tests carry the
# `arches_server` label; the Makefile runs `ctest -L arches_server` because the
# HAL subproject's enable_testing() also registers ~36 unbuilt HAL tests that
# would otherwise fail bare `ctest`.
#
# Step 2: SQLite migration test. Step 4: ResultConvert unit test (incl. §5
# module-Result unpack cases). Step 8 adds StreamBackpressure, ModuleRegistry,
# RunStateMapping, and the ClientStreamInterop integration test.
# ---------------------------------------------------------------------------

# ── SQLite migration test (integration) ───────────────────────────────────────
add_executable(arches-migration-test integration/MigrationRunnerTest.cc)
target_link_libraries(arches-migration-test PRIVATE arches_db GTest::gtest
                                                    GTest::gtest_main)
target_include_directories(arches-migration-test
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_compiler_warnings(arches-migration-test)
gtest_discover_tests(
  arches-migration-test PROPERTIES LABELS "arches_server" ENVIRONMENT
                                   "GTEST_COLOR=1")

# ── ResultConvert unit test (incl. §5 unpack) ────────────────────────────────
add_executable(arches-resultconvert-test unit/util/ResultConvertTest.cc)
target_link_libraries(
  arches-resultconvert-test
  PRIVATE arches_util # ResultConvert + protos (transitively)
          GTest::gtest GTest::gtest_main)
target_include_directories(arches-resultconvert-test
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_compiler_warnings(arches-resultconvert-test)
gtest_discover_tests(
  arches-resultconvert-test PROPERTIES LABELS "arches_server" ENVIRONMENT
                                       "GTEST_COLOR=1")

# ── StreamBackpressure unit test (BoundedQueue, pure std)
# ──────────────────────
add_executable(arches-streambackpressure-test
               unit/grpc/StreamBackpressureTest.cc)
target_link_libraries(
  arches-streambackpressure-test
  PRIVATE arches_stream_registry # header-only BoundedQueue, but pulls the
                                 # include path
          GTest::gtest GTest::gtest_main Threads::Threads)
target_include_directories(arches-streambackpressure-test
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_compiler_warnings(arches-streambackpressure-test)
gtest_discover_tests(
  arches-streambackpressure-test PROPERTIES LABELS "arches_server" ENVIRONMENT
                                            "GTEST_COLOR=1")

# ── RunStateMapping unit test (RunState -> runs.status, static, no DB)
# ─────────
add_executable(arches-runstatemap-test unit/grpc/RunStateMappingTest.cc)
target_link_libraries(arches-runstatemap-test
                      PRIVATE arches_services GTest::gtest GTest::gtest_main)
target_include_directories(arches-runstatemap-test
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_compiler_warnings(arches-runstatemap-test)
gtest_discover_tests(
  arches-runstatemap-test PROPERTIES LABELS "arches_server" ENVIRONMENT
                                     "GTEST_COLOR=1")

# ── ClientStreamInterop integration test (end-to-end client<->server)
# ────────── Spin up the real gRPC server in-process + a mock gRPC client;
# proves run-control push, §5 module-Result unpack + persist + ack, and
# batch.state() mapping.
add_executable(arches-clientstream-interop-test
               integration/ClientStreamInteropTest.cc)
target_link_libraries(
  arches-clientstream-interop-test
  PRIVATE arches_grpc
          arches_services
          arches_stream_registry
          arches_repos
          arches_db
          arches_config
          hal_protos_shared
          spdlog::spdlog
          GTest::gtest
          GTest::gtest_main
          Threads::Threads)
target_include_directories(arches-clientstream-interop-test
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_compiler_warnings(arches-clientstream-interop-test)
gtest_discover_tests(
  arches-clientstream-interop-test PROPERTIES LABELS "arches_server"
                                              ENVIRONMENT "GTEST_COLOR=1")
