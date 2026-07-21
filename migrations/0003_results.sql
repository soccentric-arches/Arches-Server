-- Migration: 0003_results (Arches / SQLite port)
-- Port of Acadia-Server's 0003_results.sql from PostgreSQL to SQLite.
-- Adaptations: UUID -> TEXT, TIMESTAMPTZ -> TEXT, BYTEA -> BLOB, NOW() ->
-- CURRENT_TIMESTAMP, gen_random_uuid() removed, ON CONFLICT -> INSERT OR IGNORE.
--
-- payload_bytes (BLOB) + payload_type_url (TEXT) hold the §5 module-specific
-- Result: the google.protobuf.Any carrying hal.modules.<id>.Result. The server
-- persists the raw wire bytes + type_url; the structured columns added in
-- 0010_result_fields mirror the HAL TestResult/TestOutput fields for fast
-- querying without unpacking.

BEGIN;

CREATE TABLE IF NOT EXISTS results (
    id               TEXT NOT NULL PRIMARY KEY,
    run_id           TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    module_id        TEXT NOT NULL,
    module_version   TEXT NOT NULL DEFAULT '',
    timestamp        TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mode             TEXT NOT NULL DEFAULT 'single_shot',
    status           TEXT NOT NULL DEFAULT 'pass'
        CHECK (status IN ('pass', 'fail', 'error', 'unknown')),
    error_message    TEXT,
    payload_bytes    BLOB,
    payload_type_url TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS results_run_ts_idx     ON results (run_id, timestamp);
CREATE INDEX IF NOT EXISTS results_module_ts_idx  ON results (module_id, timestamp);
CREATE INDEX IF NOT EXISTS results_status_idx     ON results (status);

INSERT OR IGNORE INTO schema_migrations (version) VALUES ('0003_results');

COMMIT;

-- DOWN:
-- DROP TABLE IF EXISTS results;