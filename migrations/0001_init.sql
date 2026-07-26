-- Migration: 0001_init (Arches / SQLite port)
-- Port of the prior reference implementation's 0001_init.sql from PostgreSQL to SQLite.
-- Adaptations (see server_update/10_arches_port.md): UUID PK -> TEXT (caller-
-- supplied id), TIMESTAMPTZ -> TEXT (ISO-8601), JSONB -> TEXT, NOW() ->
-- CURRENT_TIMESTAMP, gen_random_uuid() removed (ids assigned by the server),
-- ON CONFLICT ... DO NOTHING -> INSERT OR IGNORE.
--
-- Phase 1 scope: only the board single-row table is needed for interop
-- (clients register a board_id on the ClientService stream). The users and
-- api_tokens tables (the prior reference's auth surface) are deferred to Phase 2.

BEGIN;

-- ── Migration tracking ────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS schema_migrations (
    version     TEXT NOT NULL PRIMARY KEY,
    applied_at  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- ── Board (single-row, enforced by an INSERT trigger) ──────────────────────────
-- The connected client registers itself here via the ClientService Hello.
-- tags / hal_module_inventory are JSON-encoded TEXT (SQLite has no JSONB).
CREATE TABLE IF NOT EXISTS board (
    id                   TEXT NOT NULL PRIMARY KEY,
    name                 TEXT NOT NULL,
    hostname             TEXT,
    model                TEXT,
    os_version           TEXT,
    hal_version          TEXT,
    client_version       TEXT,
    tags                 TEXT NOT NULL DEFAULT '{}',
    hal_module_inventory TEXT NOT NULL DEFAULT '[]',
    first_seen           TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen            TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    status               TEXT NOT NULL DEFAULT 'offline'
        CHECK (status IN ('online', 'offline'))
);

-- Enforce single-board: at most one row in the board table. SQLite does not
-- allow a constant-only expression in a UNIQUE INDEX (Postgres used `ON board
-- ((TRUE))`); the idiomatic SQLite equivalent is a BEFORE INSERT trigger that
-- fails once a row already exists. An UPSERT (INSERT ... ON CONFLICT(id) DO
-- UPDATE) re-uses the existing row instead of inserting, so it does not trip
-- this trigger.
CREATE TRIGGER IF NOT EXISTS board_single_row
BEFORE INSERT ON board
WHEN (SELECT COUNT(*) FROM board) >= 1
BEGIN
    SELECT RAISE(FAIL, 'board table is limited to a single row');
END;

INSERT OR IGNORE INTO schema_migrations (version) VALUES ('0001_init');

COMMIT;

-- DOWN (for reference — not auto-applied):
-- DROP TABLE IF EXISTS board;
-- DROP TABLE IF EXISTS schema_migrations;