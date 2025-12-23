DO $$
BEGIN
   IF NOT EXISTS (
        SELECT 1
        FROM pg_catalog.pg_type
        WHERE pg_catalog.pg_type.typname = 'log_method'
   ) THEN
       CREATE TYPE log_method AS ENUM (
           'archive-push',
           'backup',
           'restore',
           'check',
           'stanza-create',
           'stanza-upgrade'
       );
   END IF;
END
$$;

CREATE TABLE IF NOT EXISTS backup_operations_log (
   id SERIAL PRIMARY KEY,
   application_name TEXT NOT NULL,
   start_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
   end_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
   database_name TEXT NOT NULL,
   error TEXT,
   backend_pid INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_backup_log_pid
    ON backup_operations_log (backend_pid);

GRANT SELECT, INSERT ON backup_operations_log TO public;
GRANT USAGE, SELECT ON SEQUENCE backup_operations_log_id_seq TO public;

-- CREATE TABLE IF NOT EXISTS pgbackrest_logs (
--     id SERIAL PRIMARY KEY,
--     method log_method NOT NULL,
--     command TEXT NOT NULL,
--     exit_code INT NOT NULL,
--     error TEXT,
--     start_time TIMESTAMP NOT NULL DEFAULT NOW(),
--     end_time TIMESTAMP,
--     duration INTERVAL,
--     pid INTEGER NOT NULL CHECK (pid > 0)
-- );

-- CREATE TABLE IF NOT EXISTS pg_dumpall_logs(
--    id SERIAL PRIMARY KEY,
--    application_name TEXT NOT NULL,
--    start_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
--    end_time TIMESTAMPTZ DEFAULT NULL,
--    error TEXT,
--    backend_pid INTEGER NOT NULL
-- );
-- GRANT SELECT, INSERT ON pgbackrest_logs TO public;
-- GRANT USAGE, SELECT ON SEQUENCE pgbackrest_logs_id_seq TO public;
-- GRANT SELECT, INSERT ON pg_dumpall_logs TO public;
-- GRANT USAGE, SELECT ON SEQUENCE pg_dumpall_logs_id_seq TO public; 

-- CREATE TABLE copy_operations_log (
--     id              bigserial PRIMARY KEY,
--     copy_direction  text,               -- IN or OUT
--     table_name      text,
--     start_time      timestamptz DEFAULT CURRENT_TIMESTAMP,
--     end_time        timestamptz DEFAULT CURRENT_TIMESTAMP,
--     database_name   text,
--     application_name text,
--     error           text
--     backend_pid             integer,
-- );

-- GRANT SELECT, INSERT ON copy_operations_log TO public;
-- GRANT USAGE, SELECT ON SEQUENCE copy_operations_log_id_seq TO public;

CREATE OR REPLACE VIEW v_quarterly_pg_dump AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND start_time >= date_trunc('quarter', CURRENT_DATE);
GRANT SELECT ON v_quarterly_pg_dump TO public;

CREATE OR REPLACE VIEW v_quarterly_pg_basebackup AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND start_time >= date_trunc('quarter', CURRENT_DATE);
GRANT SELECT ON v_quarterly_pg_basebackup TO public;

CREATE OR REPLACE VIEW v_quarterly_pgbackrest AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pgbackrest'
  AND start_time >= date_trunc('quarter', CURRENT_DATE);
GRANT SELECT ON v_quarterly_pgbackrest TO public;


CREATE OR REPLACE VIEW v_monthly_pg_dump AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND start_time >= date_trunc('month', CURRENT_DATE);
GRANT SELECT ON v_monthly_pg_dump TO public;

CREATE OR REPLACE VIEW v_monthly_pg_basebackup AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND start_time >= date_trunc('month', CURRENT_DATE);
GRANT SELECT ON v_monthly_pg_basebackup TO public;

CREATE OR REPLACE VIEW v_monthly_pgbackrest AS
SELECT *
FROM backup_operations_log
  WHERE application_name = 'pgbackrest' 
AND start_time >= date_trunc('month', CURRENT_DATE);
GRANT SELECT ON v_monthly_pgbackrest TO public;

CREATE OR REPLACE VIEW v_failed_pg_dump AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND error IS NOT NULL
  AND length(trim(error)) > 0;
GRANT SELECT ON v_failed_pg_dump TO public;

CREATE OR REPLACE VIEW v_failed_pg_basebackup AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL
  AND length(trim(error)) > 0;
GRANT SELECT ON v_failed_pg_basebackup TO public;

CREATE OR REPLACE VIEW v_failed_pgbackrest AS
SELECT *
FROM backup_operations_log
  WHERE application_name = 'pgbackrest'
AND error IS NOT NULL AND length(trim(error)) > 0;
GRANT SELECT ON v_failed_pgbackrest TO public;

CREATE OR REPLACE VIEW v_failed_backup_operations AS
SELECT
    'pg_dump' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND error IS NOT NULL
  AND length(trim(error)) > 0

UNION ALL
SELECT
    'pg_basebackup' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL
  AND length(trim(error)) > 0

UNION ALL
SELECT
    'pgbackrest' AS type,
    application_name,
    start_time, 
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pgbackrest'
AND error IS NOT NULL AND length(trim(error)) > 0;
GRANT SELECT ON v_failed_backup_operations TO public;

CREATE OR REPLACE VIEW v_quarterly_failed_pg_dump AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('quarter', CURRENT_DATE);
GRANT SELECT ON v_quarterly_failed_pg_dump TO public;

CREATE OR REPLACE VIEW v_quarterly_failed_pg_basebackup AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('quarter', CURRENT_DATE);
GRANT SELECT ON v_quarterly_failed_pg_basebackup TO public;

CREATE OR REPLACE VIEW v_quarterly_failed_backup_operations AS
SELECT
    'pg_dump' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('quarter', CURRENT_DATE)

UNION ALL
SELECT
    'pg_basebackup' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('quarter', CURRENT_DATE)

UNION ALL
SELECT
    'pgbackrest' AS type,
    application_name,
    start_time,   
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pgbackrest'
AND error IS NOT NULL AND length(trim(error)) > 0
AND start_time >= date_trunc('quarter', CURRENT_DATE);

GRANT SELECT ON v_quarterly_failed_backup_operations TO public;

CREATE OR REPLACE VIEW v_monthly_failed_pg_dump AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_dump'  
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('month', CURRENT_DATE);
GRANT SELECT ON v_monthly_failed_pg_dump TO public;

CREATE OR REPLACE VIEW v_monthly_failed_pg_basebackup AS
SELECT *
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL             
  AND length(trim(error)) > 0 
AND start_time >= date_trunc('month', CURRENT_DATE);


GRANT SELECT ON v_monthly_failed_pg_basebackup TO public;

CREATE OR REPLACE VIEW v_monthly_failed_backup_operations AS
SELECT
    'pg_dump' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_dump'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('month', CURRENT_DATE)

UNION ALL
SELECT
    'pg_basebackup' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pg_basebackup'
  AND error IS NOT NULL
  AND length(trim(error)) > 0
  AND start_time >= date_trunc('month', CURRENT_DATE)

UNION ALL
SELECT
    'pgbackrest' AS type,
    application_name,
    start_time,
    end_time,
    database_name,
    error,
    backend_pid AS pid
FROM backup_operations_log
WHERE application_name = 'pgbackrest'
AND error IS NOT NULL AND length(trim(error)) > 0
AND start_time >= date_trunc('month', CURRENT_DATE);
  
GRANT SELECT ON v_monthly_failed_backup_operations TO public;


