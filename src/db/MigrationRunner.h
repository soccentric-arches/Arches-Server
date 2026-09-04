#pragma once
// MigrationRunner — applies SQL migration files from migrations/ at startup.
//
// Port of the prior reference implementation's Postgres MigrationRunner to
// SQLite. Migrations are applied in lexicographic order of filename.
// Idempotent: already-applied versions are tracked in the `schema_migrations`
// table and skipped. Aborts startup with std::runtime_error on any failure.
//
// Each migration file is run as a unit via sqlite3_exec, which natively parses
// `--` comments and multiple statements separated by ';'. The ported migrations
// wrap their DDL in an explicit BEGIN/COMMIT transaction so a mid-migration
// failure leaves no partial schema. On failure the runner issues ROLLBACK
// (best-effort) before re-throwing with the version name for attribution.

#include <string>

namespace arches::server::db {

class MigrationRunner {
public:
  // migrations_dir: path to the directory containing *.sql migration files.
  explicit MigrationRunner(std::string migrations_dir);

  // Run all pending migrations against the shared SQLite connection.
  // Throws std::runtime_error if any migration fails.
  void run();

private:
  std::string migrations_dir_;
};

} // namespace arches::server::db