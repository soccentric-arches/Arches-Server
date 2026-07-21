-- Migration: 0002_runs (Arches / SQLite port)
-- Port of Acadia-Server's 0002_runs.sql from PostgreSQL to SQLite.
-- Adaptations: UUID -> TEXT, TIMESTAMPTZ -> TEXT, JSONB -> TEXT, NOW() ->
-- CURRENT_TIMESTAMP, gen_random_uuid() removed (server assigns ids), partial
-- unique index -> regular unique index, ON CONFLICT -> INSERT OR IGNORE.
--
-- Phase 1 scope: test_configs and runs are needed (RunService creates runs rows
-- and snapshots a test config). templates and schedules are the scheduler /
-- Phase-2 surface and are deferred — only a minimal schedules stub is kept
-- here to satisfy the FK shape if Phase 2 is added later. created_by user FKs
-- are dropped (auth is Phase 2).

BEGIN;

-- ── Test Configurations ───────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS test_configs (
    id            TEXT NOT NULL PRIMARY KEY,
    name          TEXT NOT NULL,
    description   TEXT NOT NULL DEFAULT '',
    mode          TEXT NOT NULL DEFAULT 'single_shot'
        CHECK (mode IN ('single_shot', 'long_term', 'monitoring', 'event_based')),
    common_config TEXT NOT NULL DEFAULT '{}',
    modules       TEXT NOT NULL DEFAULT '[]',
    created_at    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_archived   INTEGER NOT NULL DEFAULT 0  -- SQLite BOOLEAN as INTEGER 0/1
);

CREATE INDEX IF NOT EXISTS test_configs_archived_idx ON test_configs (is_archived);

-- ── Runs ──────────────────────────────────────────────────────────────────────
-- Snapshot of the config at the time the run was started (frozen). status is
-- the run lifecycle state, updated from the client's RunState (batch.state()).
CREATE TABLE IF NOT EXISTS runs (
    id                    TEXT NOT NULL PRIMARY KEY,
    name                  TEXT NOT NULL DEFAULT '',
    description           TEXT NOT NULL DEFAULT '',
    test_config_snapshot  TEXT NOT NULL DEFAULT '{}',
    mode                  TEXT NOT NULL DEFAULT 'single_shot',
    board_id              TEXT,
    status                TEXT NOT NULL DEFAULT 'queued'
        CHECK (status IN ('queued','running','paused','completed','failed','cancelled')),
    started_at            TEXT,
    ended_at              TEXT,
    error_message         TEXT,
    tags                  TEXT NOT NULL DEFAULT '[]',
    notes                 TEXT NOT NULL DEFAULT '',
    baseline_flag         INTEGER NOT NULL DEFAULT 0,
    attached_files        TEXT NOT NULL DEFAULT '[]',
    created_at            TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS runs_status_idx     ON runs (status);
CREATE INDEX IF NOT EXISTS runs_started_at_idx ON runs (started_at);
CREATE INDEX IF NOT EXISTS runs_mode_idx       ON runs (mode);

-- At most one baseline run per mode. (SQLite has no partial unique indexes with
-- WHERE on older versions; a plain unique index on (mode) where baseline_flag=1
-- would prevent >1 baseline across modes. Kept as a non-unique index instead —
-- baseline uniqueness is enforced at the service layer in Phase 1.)
CREATE INDEX IF NOT EXISTS runs_baseline_mode_idx ON runs (mode, baseline_flag);

INSERT OR IGNORE INTO schema_migrations (version) VALUES ('0002_runs');

COMMIT;

-- DOWN:
-- DROP TABLE IF EXISTS runs;
-- DROP TABLE IF EXISTS test_configs;