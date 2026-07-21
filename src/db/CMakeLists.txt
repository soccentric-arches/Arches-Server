# ---------------------------------------------------------------------------
# src/db/CMakeLists.txt
# SQLite database access layer (DbClient + MigrationRunner). Static lib so the
# server executable and the tests can both link it. Depends only on SQLite3 +
# spdlog (no Drogon/Postgres — that is the §4 SQLite port).
# ---------------------------------------------------------------------------

add_library(arches_db STATIC
    DbClient.cc
    MigrationRunner.cc
)

target_include_directories(arches_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}          # db/ headers as "db/DbClient.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/..       # src/ headers as "db/..." from src
)

target_link_libraries(arches_db PUBLIC
    SQLite::SQLite3
    spdlog::spdlog
)

# Path to the migrations directory, baked in at compile time so MigrationRunner
# can locate the SQL files without a runtime flag. Mirrors Acadia's
# ACADIA_MIGRATIONS_DIR.
target_compile_definitions(arches_db PUBLIC
    ARCHES_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/migrations"
)

set_compiler_warnings(arches_db)