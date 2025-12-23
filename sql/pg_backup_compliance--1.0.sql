-- pg_backup_compliance--1.0.sql
-- Install script for pg_backup_compliance extension.
-- This script: loads the shared library (triggers _PG_init), defines the
-- pgaud_init() wrapper and calls it so auditdb + table are created automatically.


-- We are making schema and databases using C code in pgaud_init().
-- As we are making global DB, which can not be initialized via normal
-- extension mechanism, we do it using C-code.

-- this invokes _PG_init which registers hooks
LOAD 'pg_backup_compliance';

--Create SQL wrapper for the C initializer
CREATE FUNCTION pgaud_init() RETURNS void
AS 'pg_backup_compliance', 'pgaud_init'
LANGUAGE C STRICT;

--Running initialization to create auditdb and audit table(s)
SELECT pgaud_init();

COMMENT ON EXTENSION pg_backup_compliance IS 'pg_backup_compliance: auto-create backupcompliance and log pg_dump/pg_basebackup';