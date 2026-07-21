#include "db/MigrationRunner.h"
#include "db/DbClient.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace arches::server::db {

namespace fs = std::filesystem;

MigrationRunner::MigrationRunner(std::string migrations_dir)
    : migrations_dir_(std::move(migrations_dir)) {}

void MigrationRunner::run() {
    // Ensure the migrations tracking table exists. SQLite adaptation of the
    // Postgres original: TIMESTAMPTZ -> TEXT (ISO-8601), NOW() -> CURRENT_TIMESTAMP.
    exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version     TEXT NOT NULL PRIMARY KEY,"
        "  applied_at  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ")");

    // Fetch already-applied versions.
    std::set<std::string> applied;
    {
        PreparedStatement stmt("SELECT version FROM schema_migrations ORDER BY version");
        while (stmt.step()) {
            applied.insert(stmt.column_text(0));
        }
    }

    // Collect and sort migration files lexicographically (Acadia convention).
    std::vector<fs::path> files;
    if (!fs::exists(migrations_dir_)) {
        spdlog::warn("MigrationRunner: migrations directory '{}' not found — skipping",
                     migrations_dir_);
        return;
    }
    for (const auto& entry : fs::directory_iterator(migrations_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".sql") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    int applied_count = 0;
    for (const auto& file : files) {
        std::string version = file.stem().string();

        if (applied.count(version)) {
            spdlog::debug("MigrationRunner: skip {} (already applied)", version);
            continue;
        }

        // Read the SQL file.
        std::ifstream f{file};
        if (!f.is_open()) {
            throw std::runtime_error(
                "MigrationRunner: cannot open migration file: " + file.string());
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string sql = ss.str();

        spdlog::info("MigrationRunner: applying {}", version);
        try {
            // sqlite3_exec parses the whole script (comments + multiple
            // statements). The ported migrations wrap their body in an explicit
            // BEGIN/COMMIT so a mid-migration failure rolls back cleanly.
            exec(sql);
        } catch (const std::exception& e) {
            // Best-effort rollback in case a BEGIN was opened but not committed.
            try {
                exec("ROLLBACK");
            } catch (const std::exception& rb_err) {
                spdlog::debug("MigrationRunner: rollback after failure on {}: {}",
                              version, rb_err.what());
            }
            throw std::runtime_error(
                std::string("MigrationRunner: migration ") + version + " failed: " + e.what());
        }

        // Record the migration. The SQL file already self-inserts the version
        // inside its transaction, but INSERT OR IGNORE makes this idempotent
        // for migrations that don't self-register (matches Acadia's belt-and-
        // braces behaviour; Postgres ON CONFLICT DO NOTHING -> SQLite INSERT OR
        // IGNORE per the §10 porting adaptation list).
        try {
            PreparedStatement ins(
                "INSERT OR IGNORE INTO schema_migrations (version) VALUES (?)");
            ins.bind(1, version);
            ins.step();
        } catch (const std::exception& e) {
            // Not fatal — the SQL file itself inserted the row.
            spdlog::debug("MigrationRunner: post-insert of {} ignored: {}", version, e.what());
        }

        ++applied_count;
        spdlog::info("MigrationRunner: applied {}", version);
    }

    spdlog::info("MigrationRunner: {} migration(s) applied, {} already up to date",
                 applied_count, static_cast<int>(applied.size()));
}

}  // namespace arches::server::db