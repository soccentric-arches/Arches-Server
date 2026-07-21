-- Migration: 0010_result_fields (Arches / SQLite port)
-- Port of Acadia-Server's 0010_result_fields.sql from PostgreSQL to SQLite.
--
-- Adds structured columns to `results` mirroring the HAL TestResult /
-- TestOutput shape, and adds the `run_summaries` table for fast per-module
-- last-status lookups.
--
-- SQLite adaptations vs the Postgres original:
--   - SMALLINT -> INTEGER (SQLite has no SMALLINT; INTEGER stores compactly).
--   - JSONB -> TEXT (SQLite has no JSONB; values are JSON-encoded TEXT).
--   - `COMMENT ON COLUMN/TABLE` (Postgres) -> `--` SQL comments (SQLite has no
--     COMMENT statement).
--   - `ALTER TABLE ... ADD CONSTRAINT ... CHECK` (Postgres) is NOT supported by
--     SQLite (CHECK constraints must be defined at CREATE TABLE time). The
--     `status > 0` invariant is therefore enforced in the repository
--     (ResultRepository) rather than at the DB. Documented in the porting note.
--   - GIN indexes on JSONB columns dropped (no JSONB in SQLite). Fast filtering
--     on metrics/monitoring is a Phase-2 concern.
--   - `array_agg(status ORDER BY timestamp DESC)[1]` (Postgres array syntax) in
--     the run_summaries backfill -> a SQLite-compatible correlated subquery.
--     On a fresh install there are no results rows, so the backfill is a no-op;
--     it is kept correct for the case of an existing 0003-only database.
--   - Partial indexes (WHERE expr) ARE supported by SQLite, so they are kept.
--   - thresholds table is the Phase-2 threshold/anomaly surface -> deferred.
--
-- Proto integer mappings (from Arches-HAL/protos/common.proto):
--   TestStatus:  0 UNSPECIFIED(disallowed), 1 PASS, 2 FAIL, 3 ERROR, 4 TIMEOUT,
--                5 ABORTED, 6 PARTIAL(deprecated), 7 SKIPPED
--   TestSeverity: 0 UNSPECIFIED, 1 INFO, 2 WARNING, 3 ERROR, 4 CRITICAL

BEGIN;

-- ── Step 1: rename the old TEXT status column ────────────────────────────────
ALTER TABLE results RENAME COLUMN status TO status_legacy;
-- status_legacy: DEPRECATED — old string status (pass/fail/error/unknown).
-- Use the new `status` INTEGER column. Scheduled for removal in a future migration.

-- ── Step 2: add new structured columns ──────────────────────────────────────
ALTER TABLE results ADD COLUMN status       INTEGER NOT NULL DEFAULT 0;  -- TestStatus enum int
ALTER TABLE results ADD COLUMN severity     INTEGER NOT NULL DEFAULT 0;  -- TestSeverity enum int
ALTER TABLE results ADD COLUMN test_id      TEXT    NOT NULL DEFAULT '';
ALTER TABLE results ADD COLUMN test_name    TEXT    NOT NULL DEFAULT '';
ALTER TABLE results ADD COLUMN sequence     INTEGER NOT NULL DEFAULT 0;
ALTER TABLE results ADD COLUMN duration_ns INTEGER NOT NULL DEFAULT 0;
ALTER TABLE results ADD COLUMN reason       TEXT    NOT NULL DEFAULT '{}';  -- JSON map<string,string>
ALTER TABLE results ADD COLUMN metrics      TEXT    NOT NULL DEFAULT '{}';  -- JSON map<string,TypedValue>
ALTER TABLE results ADD COLUMN monitoring   TEXT    NOT NULL DEFAULT '{}';
ALTER TABLE results ADD COLUMN event        TEXT    NOT NULL DEFAULT '{}';
ALTER TABLE results ADD COLUMN parent_id    TEXT    REFERENCES results(id) ON DELETE CASCADE;

-- ── Step 3: backfill `status` from `status_legacy` (no-op on a fresh DB) ─────
UPDATE results
SET status = CASE status_legacy
    WHEN 'pass'    THEN 1  -- TEST_STATUS_PASS
    WHEN 'fail'    THEN 2  -- TEST_STATUS_FAIL
    WHEN 'error'   THEN 3  -- TEST_STATUS_ERROR
    WHEN 'unknown' THEN 2  -- map unknown -> FAIL (conservative)
    ELSE                2  -- any other legacy value -> FAIL
END;

-- ── Step 4: status > 0 invariant ─────────────────────────────────────────────
-- Postgres enforces this with ALTER TABLE ADD CONSTRAINT; SQLite cannot, so the
-- invariant is enforced in ResultRepository (it never writes status=0). See
-- the note above.

-- ── Step 5: replace old status_idx with new indexes ──────────────────────────
DROP INDEX IF EXISTS results_status_idx;

CREATE INDEX IF NOT EXISTS results_status_ts_idx
    ON results (status, timestamp DESC);
CREATE INDEX IF NOT EXISTS results_module_test_ts_idx
    ON results (module_id, test_id, timestamp DESC);
CREATE INDEX IF NOT EXISTS results_run_sequence_idx
    ON results (run_id, module_id, sequence);
CREATE INDEX IF NOT EXISTS results_parent_idx
    ON results (parent_id) WHERE parent_id IS NOT NULL;

-- ── Step 6: run_summaries table ──────────────────────────────────────────────
-- Per-(run,module,test) latest result summary. Upserted on every result insert.
CREATE TABLE IF NOT EXISTS run_summaries (
    run_id          TEXT    NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    module_id       TEXT    NOT NULL,
    test_id         TEXT    NOT NULL DEFAULT '',
    last_status     INTEGER NOT NULL DEFAULT 0,
    last_severity   INTEGER NOT NULL DEFAULT 0,
    last_timestamp  TEXT    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    result_count    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (run_id, module_id, test_id)
);

CREATE INDEX IF NOT EXISTS run_summaries_run_idx ON run_summaries (run_id);

-- ── Step 7: backfill run_summaries from existing results (no-op on fresh DB) ─
INSERT OR IGNORE INTO run_summaries
    (run_id, module_id, test_id, last_status, last_severity, last_timestamp, result_count)
SELECT r.run_id, r.module_id, '' AS test_id,
       r.status   AS last_status,
       0           AS last_severity,
       r.timestamp AS last_timestamp,
       c.result_count
FROM results r
JOIN (
    SELECT run_id, module_id, MAX(timestamp) AS max_ts, COUNT(*) AS result_count
    FROM results
    WHERE parent_id IS NULL
    GROUP BY run_id, module_id
) c
  ON c.run_id = r.run_id
 AND c.module_id = r.module_id
 AND c.max_ts = r.timestamp
WHERE r.parent_id IS NULL;

INSERT OR IGNORE INTO schema_migrations (version) VALUES ('0010_result_fields');

COMMIT;

-- DOWN (for reference):
-- DROP TABLE IF EXISTS run_summaries;
-- ALTER TABLE results DROP COLUMN parent_id;
-- ALTER TABLE results DROP COLUMN event;
-- ALTER TABLE results DROP COLUMN monitoring;
-- ALTER TABLE results DROP COLUMN metrics;
-- ALTER TABLE results DROP COLUMN reason;
-- ALTER TABLE results DROP COLUMN duration_ns;
-- ALTER TABLE results DROP COLUMN sequence;
-- ALTER TABLE results DROP COLUMN test_name;
-- ALTER TABLE results DROP COLUMN test_id;
-- ALTER TABLE results DROP COLUMN severity;
-- ALTER TABLE results DROP COLUMN status;
-- ALTER TABLE results RENAME COLUMN status_legacy TO status;