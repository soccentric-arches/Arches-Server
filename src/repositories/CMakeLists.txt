# ---------------------------------------------------------------------------
# src/repositories/CMakeLists.txt — SQLite-backed repositories (Phase 1 subset).
# Static lib so the server executable and tests can both link it. Depends on
# arches_db (DbClient/PreparedStatement) and util (Time/Uuid/Errors). No proto
# dependency here — repos store the packed payload as an opaque BLOB; the §5
# unpack (proto -> TestResult) lives in util/ResultConvert (Step 4), not here.
# ---------------------------------------------------------------------------

add_library(arches_repos STATIC
    RunRepository.cc
    ResultRepository.cc
    LogRepository.cc
    TargetRepository.cc
    TestConfigRepository.cc
)

target_include_directories(arches_repos PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(arches_repos PUBLIC
    arches_db
    spdlog::spdlog
)

# Repos use util::utc_now_iso8601 / util::generate_uuid in their .cc only
# (headers don't include util), so the util link is PRIVATE.
target_link_libraries(arches_repos PRIVATE
    arches_util
)

set_compiler_warnings(arches_repos)