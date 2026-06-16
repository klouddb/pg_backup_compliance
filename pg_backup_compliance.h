/*-------------------------------------------------------------------------
 *
 * pg_backup_compliance.h
 *    Shared types and prototypes for the pg_backup_compliance extension.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 * Copyright (c) 2024-2026, KloudDB.
 *
 * IDENTIFICATION
 *    pg_backup_compliance.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BACKUP_COMPLIANCE_H
#define PG_BACKUP_COMPLIANCE_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/hsearch.h"

/* Fixed-width string fields, kept independent of NAMEDATALEN. */
#define PGBC_APP_LEN     128
#define PGBC_DB_LEN      128
#define PGBC_USER_LEN    128
#define PGBC_ADDR_LEN    128
#define PGBC_TYPE_LEN    32
#define PGBC_ERR_LEN     512

/* Magic at the head of the dump file; bump when the on-disk layout changes. */
#define PGBC_FILE_HEADER        0x504742C4   /* "PGBC" + version */
#define PGBC_PG_MAJOR_VERSION   (PG_VERSION_NUM / 100)

#define PGBC_DUMP_FILE      PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_backup_compliance.stat"
#define PGBC_DUMP_FILE_TMP  PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_backup_compliance.stat.tmp"

/*
 * Append-only archive of evicted entries so compliance history is never lost.
 * Grows without bound until pg_backup_compliance_reset() removes it.
 */
#define PGBC_ARCHIVE_HEADER 0x504742C5   /* "PGBC" + archive version */
#define PGBC_ARCHIVE_FILE   PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_backup_compliance_archive.stat"

typedef enum pgbcStatus
{
    PGBC_STATUS_UNKNOWN = 0,
    PGBC_STATUS_RUNNING,
    PGBC_STATUS_SUCCEEDED,
    PGBC_STATUS_FAILED,
    PGBC_STATUS_INTERRUPTED,
    PGBC_STATUS_AUTH_FAILED
} pgbcStatus;

/* Hash key: pid plus backend start timestamp defeats pid recycling. */
typedef struct pgbcHashKey
{
    int             backend_pid;
    TimestampTz     backend_start;
} pgbcHashKey;

typedef struct pgbcEntry
{
    pgbcHashKey key;                            /* must be first */

    /* Identity (immutable after creation) */
    char        application_name[PGBC_APP_LEN];
    char        database_name[PGBC_DB_LEN];
    char        user_name[PGBC_USER_LEN];
    char        client_addr[PGBC_ADDR_LEN];
    char        backup_type[PGBC_TYPE_LEN];

    /* Mutable */
    TimestampTz start_time;
    TimestampTz end_time;
    pgbcStatus  status;
    bool        is_walsender;
    bool        data_sent;                      /* streamed WAL or base backup */
    bool        backup_aborted;                 /* aborted before backup_stop */
    int32       exit_code;
    char        error_message[PGBC_ERR_LEN];

    uint32      usage;                          /* lower => evict first */

    /* A pg_dumpall and its child pg_dumps share one entry. */
    bool        aggregated;                     /* shared by many backends */
    int32       refcount;                       /* live members */
    int32       conn_count;                     /* total members joined */

    slock_t     mutex;                          /* protects mutable fields */
} pgbcEntry;

typedef struct pgbcSharedState
{
    LWLock     *lock;                           /* protects hash search */
    slock_t     mutex;                          /* protects counters */

    uint32      sequence_counter;
    int64       total_captured;
    int64       total_evicted;
    TimestampTz last_reset;
} pgbcSharedState;

/* GUC variables. */
extern bool pgbc_enabled;
extern bool pgbc_save;
extern int  pgbc_max_entries;
extern char *pgbc_track_apps;

/* Pointers to the shared state, populated at shmem startup. */
extern pgbcSharedState *pgbc_state;
extern HTAB *pgbc_hash;

/* Capture API (used by the hook layer). */
extern bool pgbc_app_is_tracked(const char *appname,
                                char *out_type, size_t out_type_len);
extern pgbcEntry *pgbc_lookup_or_create_entry(int pid,
                                              TimestampTz backend_start,
                                              bool create);
extern pgbcEntry *pgbc_attach_aggregate_entry(const char *appname,
                                              const char *dbname,
                                              const char *username,
                                              const char *client_addr,
                                              const char *backup_type,
                                              bool is_walsender);
extern void pgbc_entry_mark_running(pgbcEntry *entry,
                                    const char *appname,
                                    const char *dbname,
                                    const char *username,
                                    const char *client_addr,
                                    const char *backup_type,
                                    bool is_walsender,
                                    TimestampTz start_time);
extern void pgbc_entry_record_error(pgbcEntry *entry,
                                    const char *msg);
extern void pgbc_entry_mark_aborted(pgbcEntry *entry);
extern void pgbc_entry_finalize(pgbcEntry *entry,
                                pgbcStatus status,
                                int32 exit_code,
                                TimestampTz end_time);

/* Capture-unit lifecycle (called from _PG_init). */
extern void pgbc_capture_init(void);
extern void pgbc_capture_fini(void);

#endif                          /* PG_BACKUP_COMPLIANCE_H */
