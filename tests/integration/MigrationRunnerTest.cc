// tests/integration/MigrationRunnerTest.cc
// Validates the SQLite migration layer against the REAL ported migrations
// (migrations/*.sql). SQLite is file-based and always available on the host, so
// unlike Acadia's Postgres variant there is no GTEST_SKIP gate — this always
// runs. It proves: all ported migrations apply cleanly, the expected tables
// exist, schema_migrations records every version, and re-running is idempotent.
#include "db/DbClient.h"
#include "db/MigrationRunner.h"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef ARCHES_MIGRATIONS_DIR
#error "ARCHES_MIGRATIONS_DIR must be defined by arches_db (PUBLIC compile def)"
#endif

namespace {

// RAII temp file: a unique path in the temp dir, removed on destruction.
class TempFile {
public:
    TempFile() {
        // The build forbids Date/time/random in workflow scripts, but this is a
        // normal test binary — use the PID + a counter for uniqueness.
        path_ = (std::filesystem::temp_directory_path()
                 / ("arches_mig_test_" + std::to_string(getpid()) + ".db")).string();
        std::filesystem::remove(path_);  // start clean
    }
    ~TempFile() {
        std::filesystem::remove(path_);
        // SQLite WAL files
        std::filesystem::remove(path_ + "-wal");
        std::filesystem::remove(path_ + "-shm");
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// Tables that must exist after all ported migrations apply. Phase 1 subset.
const std::vector<std::string> kExpectedTables = {
    "schema_migrations", "board", "test_configs", "runs",
    "results", "log_entries", "run_summaries",
};

// Versions that must be recorded in schema_migrations after the ported subset.
const std::vector<std::string> kExpectedVersions = {
    "0001_init", "0002_runs", "0003_results", "0004_logs", "0010_result_fields",
};

bool table_exists(const std::string& name) {
    arches::server::db::PreparedStatement stmt(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
    stmt.bind(1, name);
    stmt.step();
    return stmt.column_int(0) > 0;
}

int schema_migrations_count() {
    arches::server::db::PreparedStatement stmt(
        "SELECT count(*) FROM schema_migrations");
    stmt.step();
    return stmt.column_int(0);
}

}  // namespace

class MigrationRunnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        spdlog::set_level(spdlog::level::warn);  // quiet the migration INFO logs
        arches::server::db::init_db_client(tmp_.path());
    }
    void TearDown() override {
        arches::server::db::shutdown_db_client();
    }
    TempFile tmp_;
};

TEST_F(MigrationRunnerTest, AppliesAllPortedMigrationsAndCreatesTables) {
    arches::server::db::MigrationRunner runner{ARCHES_MIGRATIONS_DIR};
    EXPECT_NO_THROW(runner.run());

    for (const auto& t : kExpectedTables) {
        EXPECT_TRUE(table_exists(t)) << "missing table: " << t;
    }
}

TEST_F(MigrationRunnerTest, RecordsAllVersions) {
    arches::server::db::MigrationRunner runner{ARCHES_MIGRATIONS_DIR};
    runner.run();

    EXPECT_EQ(schema_migrations_count(),
              static_cast<int>(kExpectedVersions.size()));

    arches::server::db::PreparedStatement stmt(
        "SELECT version FROM schema_migrations ORDER BY version");
    std::vector<std::string> got;
    while (stmt.step()) got.push_back(stmt.column_text(0));
    EXPECT_EQ(got, kExpectedVersions);
}

TEST_F(MigrationRunnerTest, IdempotentOnReRun) {
    arches::server::db::MigrationRunner runner{ARCHES_MIGRATIONS_DIR};
    EXPECT_NO_THROW(runner.run());
    int first = schema_migrations_count();
    EXPECT_NO_THROW(runner.run());
    int second = schema_migrations_count();
    EXPECT_EQ(first, second);  // no duplicate rows, no new migrations applied
}

TEST_F(MigrationRunnerTest, ResultsTableHasStructuredColumns) {
    arches::server::db::MigrationRunner runner{ARCHES_MIGRATIONS_DIR};
    runner.run();

    // The 0010_result_fields migration adds structured columns to results.
    arches::server::db::PreparedStatement stmt(
        "PRAGMA table_info(results)");
    std::vector<std::string> cols;
    while (stmt.step()) cols.push_back(stmt.column_text(1));  // col 1 = name
    const auto has = [&](const std::string& c) {
        return std::find(cols.begin(), cols.end(), c) != cols.end();
    };
    EXPECT_TRUE(has("status"));        // new INTEGER TestStatus
    EXPECT_TRUE(has("severity"));
    EXPECT_TRUE(has("test_id"));
    EXPECT_TRUE(has("test_name"));
    EXPECT_TRUE(has("sequence"));
    EXPECT_TRUE(has("duration_ns"));
    EXPECT_TRUE(has("reason"));
    EXPECT_TRUE(has("metrics"));
    EXPECT_TRUE(has("parent_id"));
    EXPECT_TRUE(has("status_legacy"));  // renamed old TEXT status
    EXPECT_TRUE(has("payload_bytes")); // from 0003, BLOB
    EXPECT_TRUE(has("payload_type_url"));
}