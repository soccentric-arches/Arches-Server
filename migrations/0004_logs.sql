-- Migration: 0004_logs (Arches / SQLite port)
-- Port of Acadia-Server's 0004_logs.sql from PostgreSQL to SQLite.
-- Adaptations: UUID -> TEXT, TIMESTAMPTZ -> TEXT, JSONB -> TEXT, NOW() ->
-- CURRENT_TIMESTAMP, gen_random_uuid() removed, GIN/fts indexes dropped (SQLite
-- FTS5 would be a separate virtual table — deferred to Phase 2), partial index ->
-- regular index, ON CONFLICT -> INSERT OR IGNORE.
--
-- Phase 1 scope: log_entries is needed (LogService ingests client LogBatches).
-- log_bookmarks and saved_log_views are per-user Dashboard surfaces (Phase 2)
-- and are deferred.

BEGIN;

CREATE TABLE IF NOT EXISTS log_entries (
    id         TEXT NOT NULL PRIMARY KEY,
    source     TEXT NOT NULL DEFAULT 'app',
    severity   TEXT NOT NULL DEFAULT 'info'
        CHECK (severity IN ('trace', 'debug', 'info', 'warn', 'error', 'critical')),
    timestamp  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    message    TEXT NOT NULL DEFAULT '',
    metadata   TEXT NOT NULL DEFAULT '{}',
    run_id     TEXT REFERENCES runs(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS log_entries_ts_source_idx  ON log_entries (timestamp DESC, source);
CREATE INDEX IF NOT EXISTS log_entries_run_idx        ON log_entries (run_id);
CREATE INDEX IF NOT EXISTS log_entries_severity_idx   ON log_entries (severity, timestamp DESC);

INSERT OR IGNORE INTO schema_migrations (version) VALUES ('0004_logs');

COMMIT;

-- DOWN:
-- DROP TABLE IF EXISTS log_entries;