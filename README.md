# pg_backup_compliance

A PostgreSQL extension that records every backup-related session against
the server -- `pg_dump`, `pg_dumpall`, `pg_basebackup`, `pgBackRest`,
 and any tool that calls the backup
SQL API -- and exposes the captures as SQL views.

## Features

* Tracks all backup utilities with a single, configurable allow-list
  (`pg_backup_compliance.track_apps`).
* Captures who, when, from where, how long, and the final status
  (`success`, `failed`, `interrupted`, `auth_failed`).
* Survives server restarts via a dump file in `$PGDATA/pg_stat/`.
* `pg_monitor`-accessible views for the common "did backups happen?"
  questions.
* No third-party C dependencies; builds with PGXS on PostgreSQL 13+.

## Build and install

PostgreSQL server development headers are required.

On Debian / Ubuntu:

```sh
sudo apt-get install postgresql-server-dev-$(pg_config --version | awk '{print $2}' | cut -d. -f1) libkrb5-dev
```

`libkrb5-dev` supplies GSSAPI headers required to build the capture module.

On RHEL / Rocky / AlmaLinux:

```sh
sudo dnf install postgresqlNN-devel    # NN = your major version
```

Build:

```sh
make
sudo make install
```

If you have multiple PostgreSQL major versions installed, point at the
one you want:

```sh
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

## Configuration

Add the library to `shared_preload_libraries` in `postgresql.conf`:

```ini
shared_preload_libraries = 'pg_backup_compliance'
```

Restart the server, then install the SQL surface in whichever database
you want to query from:

```sql
CREATE EXTENSION pg_backup_compliance;
```

### Known limitation
> Because a `pg_dumpall` child and a standalone
> `pg_dump` are protocol-identical, a genuine standalone `pg_dump` that runs
> from the same host and user *while a `pg_dumpall` is in progress* is folded
> into that `pg_dumpall`'s entry. This is unavoidable without cooperation
> from the client.

## GUCs

| GUC | Default | Description |
| --- | --- | --- |
| `pg_backup_compliance.enabled` | `on` | Enable / disable capture. |
| `pg_backup_compliance.save` | `on` | Persist state across restarts. |
| `pg_backup_compliance.max_entries` | `1024` | Max operations tracked in shared memory. |
| `pg_backup_compliance.track_apps` | `pg_dump,pg_dumpall,pg_basebackup,pgbackrest,pgBackRest` | `application_name` prefixes treated as backup tools. |

## Usage

```sql
-- Live in-memory view: every captured backup session.
SELECT backend_pid, application_name, backup_type, status,
       start_time, end_time, error_message
  FROM pg_backup_compliance
 ORDER BY start_time DESC
 LIMIT 20;

-- Built-in convenience views.
SELECT * FROM pg_backup_compliance_failed   ORDER BY start_time DESC;
SELECT * FROM pg_backup_compliance_running;
SELECT * FROM pg_backup_compliance_last_24h ORDER BY start_time DESC;
```

### Time-windowed and failure views

These views slice the live capture by recency and outcome, providing
ready-made answers to common backup-compliance questions without writing
ad-hoc predicates. They are thin wrappers over `pg_backup_compliance` and
therefore expose the same column set; only the row filter differs.

| View | Scope | Filter predicate |
| --- | --- | --- |
| `v_quarterly_backups` | All backups started in the last three months. | `start_time >= now() - interval '3 months'` |
| `v_monthly_backups` | All backups started in the last month. | `start_time >= now() - interval '1 month'` |
| `v_failed_backups` | Every unsuccessful backup attempt, regardless of age. | `status IN ('failed','interrupted','auth_failed')` |
| `v_quarterly_failed_backups` | Unsuccessful attempts started in the last three months. | failed statuses `AND start_time >= now() - interval '3 months'` |
| `v_monthly_failed_backups` | Unsuccessful attempts started in the last month. | failed statuses `AND start_time >= now() - interval '1 month'` |

All five views are granted `SELECT` to the `pg_monitor` role and return the
full column set of `pg_backup_compliance` (see [Columns](#columns) below).

```sql
-- Backups from the last three months.
SELECT * FROM v_quarterly_backups ORDER BY start_time DESC;

-- Backups from the last month.
SELECT * FROM v_monthly_backups ORDER BY start_time DESC;

-- All failed backup attempts.
SELECT * FROM v_failed_backups ORDER BY start_time DESC;

-- Failed backups from the last three months.
SELECT * FROM v_quarterly_failed_backups ORDER BY start_time DESC;

-- Failed backups from the last month.
SELECT * FROM v_monthly_failed_backups ORDER BY start_time DESC;
```

> **Note:** Captures live in shared memory and are subject to eviction once
> `pg_backup_compliance.max_entries` is exceeded. The quarterly and monthly
> windows therefore report only the operations still resident in the cache,
> not a guaranteed historical record across the full interval.

#### Columns

| Column | Type | Description |
| --- | --- | --- |
| `backend_pid` | `int` | PID of the backend serving the backup session. |
| `backend_start` | `timestamptz` | When the backend was started. |
| `application_name` | `text` | Reported `application_name` of the client. |
| `backup_type` | `text` | Backup category (e.g. `dump`, `dumpall`, `basebackup`). |
| `database_name` | `text` | Target database. |
| `user_name` | `text` | Role that initiated the backup. |
| `client_addr` | `text` | Client network address. |
| `is_walsender` | `boolean` | Whether the session is a WAL sender (physical backups). |
| `start_time` | `timestamptz` | When the backup operation started. |
| `end_time` | `timestamptz` | When it finished (`NULL` while running). |
| `status` | `text` | `running`, `success`, `failed`, `interrupted`, or `auth_failed`. |
| `exit_code` | `int` | Process exit code, when available. |
| `error_message` | `text` | Failure detail for unsuccessful attempts. |
| `connection_count` | `int` | Number of connections associated with the operation. |


## Limitations
Currently, this extension is primarily designed to detect and track unauthorized backup activity. For example, if someone initiates a backup outside the approved backup window, the extension can identify and report that activity.

For third-party backup tools such as pgBackRest, there are certain edge cases where failed backup attempts may not always be reported accurately. However, the extension can reliably monitor backup activity to detect unauthorized backup or dump operations across the following tools:

pg_dump
pg_dumpall
pg_basebackup
pgBackRest

## Copyright

Copyright (c) 2024-2026, KloudDB.

Portions of the shared-memory and dump-file handling are derived from the
PostgreSQL `pg_stat_statements` contrib module and remain
Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group.
