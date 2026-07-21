# ---------------------------------------------------------------------------
# tests/CMakeLists.txt — Arches-Server tests.
# All Arches-Server tests carry the `arches_server` label; the Makefile runs
# `ctest -L arches_server` because the HAL subproject's enable_testing() also
# registers ~36 unbuilt HAL tests that would otherwise fail bare `ctest`.
#
# Step 2: SQLite migration test. Step 4: ResultConvert unit test (incl. §5
# module-Result unpack cases). Step 8 adds StreamBackpressure, ModuleRegistry,
# RunStateMapping, and the ClientStreamInterop integration test.
# ---------------------------------------------------------------------------

# ── SQLite migration test (integration) ───────────────────────────────────────
add_executable(arches-migration-test
    integration/MigrationRunnerTest.cc
)
target_link_libraries(arches-migration-test PRIVATE
    arches_db
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(arches-migration-test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
set_compiler_warnings(arches-migration-test)
gtest_discover_tests(arches-migration-test
    PROPERTIES LABELS "arches_server" ENVIRONMENT "GTEST_COLOR=1"
)

# ── ResultConvert unit test (incl. §5 unpack) ────────────────────────────────
add_executable(arches-resultconvert-test
    unit/util/ResultConvertTest.cc
)
target_link_libraries(arches-resultconvert-test PRIVATE
    arches_util            # ResultConvert + protos (transitively)
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(arches-resultconvert-test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
set_compiler_warnings(arches-resultconvert-test)
gtest_discover_tests(arches-resultconvert-test
    PROPERTIES LABELS "arches_server" ENVIRONMENT "GTEST_COLOR=1"
)