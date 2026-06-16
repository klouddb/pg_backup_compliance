/* pg_backup_compliance--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_backup_compliance" to load this file. \quit

-- Core SRF: live in-memory view of every captured backup operation.
CREATE FUNCTION pg_backup_compliance_operations(
    OUT backend_pid       int,
    OUT backend_start     timestamptz,
    OUT application_name  text,
    OUT backup_type       text,
    OUT database_name     text,
    OUT user_name         text,
    OUT client_addr       text,
    OUT is_walsender      boolean,
    OUT start_time        timestamptz,
    OUT end_time          timestamptz,
    OUT status            text,
    OUT exit_code         int,
    OUT error_message     text,
    OUT connection_count  int
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_backup_compliance_operations'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

REVOKE ALL ON FUNCTION pg_backup_compliance_operations() FROM PUBLIC;

-- Archived operations: rows evicted from the in-memory table, kept on disk.
CREATE FUNCTION pg_backup_compliance_archived_operations(
    OUT backend_pid       int,
    OUT backend_start     timestamptz,
    OUT application_name  text,
    OUT backup_type       text,
    OUT database_name     text,
    OUT user_name         text,
    OUT client_addr       text,
    OUT is_walsender      boolean,
    OUT start_time        timestamptz,
    OUT end_time          timestamptz,
    OUT status            text,
    OUT exit_code         int,
    OUT error_message     text,
    OUT connection_count  int
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_backup_compliance_archived_operations'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

REVOKE ALL ON FUNCTION pg_backup_compliance_archived_operations() FROM PUBLIC;

-- Main view: live in-memory rows unioned with archived rows (full history).
CREATE VIEW pg_backup_compliance AS
  SELECT * FROM pg_backup_compliance_operations()
  UNION ALL
  SELECT * FROM pg_backup_compliance_archived_operations();

GRANT SELECT ON pg_backup_compliance TO pg_monitor;

-- Only the evicted/archived operations.
CREATE VIEW pg_backup_compliance_archived AS
  SELECT * FROM pg_backup_compliance_archived_operations();

GRANT SELECT ON pg_backup_compliance_archived TO pg_monitor;

-- Drop all in-memory entries (superuser only).
CREATE FUNCTION pg_backup_compliance_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_backup_compliance_reset'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_backup_compliance_reset() FROM PUBLIC;

-- Cache-level statistics.
CREATE FUNCTION pg_backup_compliance_info(
    OUT live_entries   int,
    OUT total_captured bigint,
    OUT total_evicted  bigint,
    OUT last_reset     timestamptz
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_backup_compliance_info'
LANGUAGE C STRICT VOLATILE;

GRANT EXECUTE ON FUNCTION pg_backup_compliance_info() TO pg_monitor;

-- Convenience views over the live in-memory state.
CREATE VIEW pg_backup_compliance_failed AS
  SELECT *
    FROM pg_backup_compliance
   WHERE status IN ('failed','interrupted','auth_failed');

GRANT SELECT ON pg_backup_compliance_failed TO pg_monitor;

CREATE VIEW pg_backup_compliance_running AS
  SELECT *
    FROM pg_backup_compliance
   WHERE status = 'running';

GRANT SELECT ON pg_backup_compliance_running TO pg_monitor;

CREATE VIEW pg_backup_compliance_last_24h AS
  SELECT *
    FROM pg_backup_compliance
   WHERE start_time >= now() - interval '24 hours';

GRANT SELECT ON pg_backup_compliance_last_24h TO pg_monitor;

-- Backups from the last three months.
CREATE VIEW v_quarterly_backups AS
  SELECT *
    FROM pg_backup_compliance
   WHERE start_time >= now() - interval '3 months';

GRANT SELECT ON v_quarterly_backups TO pg_monitor;

-- Backups from the last month.
CREATE VIEW v_monthly_backups AS
  SELECT *
    FROM pg_backup_compliance
   WHERE start_time >= now() - interval '1 month';

GRANT SELECT ON v_monthly_backups TO pg_monitor;

-- All failed backup attempts.
CREATE VIEW v_failed_backups AS
  SELECT *
    FROM pg_backup_compliance
   WHERE status IN ('failed','interrupted','auth_failed');

GRANT SELECT ON v_failed_backups TO pg_monitor;

-- Failed backups from the last three months.
CREATE VIEW v_quarterly_failed_backups AS
  SELECT *
    FROM pg_backup_compliance
   WHERE status IN ('failed','interrupted','auth_failed')
     AND start_time >= now() - interval '3 months';

GRANT SELECT ON v_quarterly_failed_backups TO pg_monitor;

-- Failed backups from the last month.
CREATE VIEW v_monthly_failed_backups AS
  SELECT *
    FROM pg_backup_compliance
   WHERE status IN ('failed','interrupted','auth_failed')
     AND start_time >= now() - interval '1 month';

GRANT SELECT ON v_monthly_failed_backups TO pg_monitor;

COMMENT ON EXTENSION pg_backup_compliance IS
  'audit of backup operations against the server';
