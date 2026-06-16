/*-------------------------------------------------------------------------
 *
 * pg_backup_compliance.c
 *    Records backup-related sessions into a shared-memory hash table and
 *    persists state across restarts via a dump file in $PGDATA/pg_stat/.
 *    The captured rows are exposed through a set-returning function and
 *    a set of views built on top of it.
 *
 * Portions of the shared-memory and dump-file handling are derived from
 * the PostgreSQL pg_stat_statements contrib module.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 * Copyright (c) 2024-2026, KloudDB.
 *
 * IDENTIFICATION
 *    pg_backup_compliance.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/htup_details.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "pg_backup_compliance.h"

PG_MODULE_MAGIC;

/* GUC variables */
bool        pgbc_enabled = true;
bool        pgbc_save = true;
int         pgbc_max_entries = 1024;
char       *pgbc_track_apps = NULL;

/* Shared state pointers. */
pgbcSharedState *pgbc_state = NULL;
HTAB            *pgbc_hash = NULL;

/* Previous shmem hook chain. */
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Forward declarations for static functions. */
static void pgbc_shmem_request(void);
static void pgbc_shmem_startup(void);
static void pgbc_shmem_shutdown(int code, Datum arg);
static Size pgbc_memsize(void);
static void pgbc_load_dump_file(void);
static void pgbc_save_dump_file(void);
static void pgbc_evict_one_locked(void);
static void pgbc_archive_entry(const pgbcEntry *entry);
static void pgbc_truncate_archive(void);
static const char *pgbc_status_name(pgbcStatus s);
static Tuplestorestate *pgbc_make_tupstore(FunctionCallInfo fcinfo,
                                           TupleDesc *tupdesc_p);
static void pgbc_put_entry_tuple(Tuplestorestate *tupstore,
                                 TupleDesc tupdesc,
                                 const pgbcEntry *snap);
static void pgbc_archive_to_tupstore(Tuplestorestate *tupstore,
                                     TupleDesc tupdesc);

/* Public extension functions. */
PG_FUNCTION_INFO_V1(pg_backup_compliance_operations);
PG_FUNCTION_INFO_V1(pg_backup_compliance_archived_operations);
PG_FUNCTION_INFO_V1(pg_backup_compliance_reset);
PG_FUNCTION_INFO_V1(pg_backup_compliance_info);

void _PG_init(void);
void _PG_fini(void);

void
_PG_init(void)
{
    /* Shared memory can only be requested from shared_preload_libraries. */
    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(WARNING,
                (errmsg("pg_backup_compliance must be loaded via "
                        "shared_preload_libraries to be effective"),
                 errhint("Add \"pg_backup_compliance\" to shared_preload_libraries "
                         "in postgresql.conf and restart the server.")));
        return;
    }

    /* GUCs */
    DefineCustomBoolVariable("pg_backup_compliance.enabled",
                             "Enable / disable backup-compliance capture.",
                             NULL,
                             &pgbc_enabled,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_backup_compliance.save",
                             "Save state across server restarts.",
                             NULL,
                             &pgbc_save,
                             true,
                             PGC_SIGHUP,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("pg_backup_compliance.max_entries",
                            "Maximum concurrent / recent backup operations "
                            "tracked in shared memory.",
                            NULL,
                            &pgbc_max_entries,
                            1024,
                            64,
                            INT_MAX / 2,
                            PGC_POSTMASTER,
                            0,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("pg_backup_compliance.track_apps",
                               "Comma-separated, case-insensitive list of "
                               "application_name prefixes treated as backup "
                               "tools.",
                               NULL,
                               &pgbc_track_apps,
                               "pg_dump,pg_dumpall,pg_basebackup,"
                               "pgbackrest,pgBackRest",
                               PGC_SIGHUP,
                               0,
                               NULL, NULL, NULL);

    MarkGUCPrefixReserved("pg_backup_compliance");

    /* Chain through any pre-existing shmem hooks. */
#if PG_VERSION_NUM >= 150000
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pgbc_shmem_request;
#else
    pgbc_shmem_request();
#endif

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgbc_shmem_startup;

    pgbc_capture_init();
}

void
_PG_fini(void)
{
    pgbc_capture_fini();

#if PG_VERSION_NUM >= 150000
    shmem_request_hook = prev_shmem_request_hook;
#endif
    shmem_startup_hook = prev_shmem_startup_hook;
}

/* Shared-memory management. */

static Size
pgbc_memsize(void)
{
    Size        size;

    size = MAXALIGN(sizeof(pgbcSharedState));
    size = add_size(size, hash_estimate_size(pgbc_max_entries,
                                             sizeof(pgbcEntry)));
    return size;
}

static void
pgbc_shmem_request(void)
{
#if PG_VERSION_NUM >= 150000
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
#endif

    RequestAddinShmemSpace(pgbc_memsize());
    RequestNamedLWLockTranche("pg_backup_compliance", 1);
}

static void
pgbc_shmem_startup(void)
{
    bool        found;
    HASHCTL     info;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    pgbc_state = NULL;
    pgbc_hash = NULL;

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    pgbc_state = (pgbcSharedState *)
        ShmemInitStruct("pg_backup_compliance state",
                        sizeof(pgbcSharedState),
                        &found);
    if (!found)
    {
        pgbc_state->lock = &(GetNamedLWLockTranche("pg_backup_compliance"))->lock;
        SpinLockInit(&pgbc_state->mutex);
        pgbc_state->sequence_counter = 0;
        pgbc_state->total_captured = 0;
        pgbc_state->total_evicted = 0;
        pgbc_state->last_reset = GetCurrentTimestamp();
    }

    memset(&info, 0, sizeof(info));
    info.keysize = sizeof(pgbcHashKey);
    info.entrysize = sizeof(pgbcEntry);

    pgbc_hash = ShmemInitHash("pg_backup_compliance hash",
                              pgbc_max_entries,
                              pgbc_max_entries,
                              &info,
                              HASH_ELEM | HASH_BLOBS);

    LWLockRelease(AddinShmemInitLock);

    /* Persist state on clean shutdown; reload it on next startup. */
    if (!IsUnderPostmaster)
        on_shmem_exit(pgbc_shmem_shutdown, (Datum) 0);

    if (!IsUnderPostmaster && pgbc_save)
        pgbc_load_dump_file();
}

static void
pgbc_shmem_shutdown(int code, Datum arg)
{
    if (code)
        return;                 /* skip dump on crash */

    if (!pgbc_state || !pgbc_hash)
        return;

    if (!pgbc_save)
        return;

    pgbc_save_dump_file();
}

/* Persistence: load / save the dump file. */

static void
pgbc_load_dump_file(void)
{
    FILE       *file;
    uint32      header;
    uint32      pgver;
    int32       num_entries;
    int32       i;

    file = AllocateFile(PGBC_DUMP_FILE, PG_BINARY_R);
    if (file == NULL)
    {
        if (errno != ENOENT)
            ereport(LOG,
                    (errcode_for_file_access(),
                     errmsg("could not open pg_backup_compliance dump "
                            "file \"%s\": %m", PGBC_DUMP_FILE)));
        return;
    }

    if (fread(&header, sizeof(uint32), 1, file) != 1 ||
        fread(&pgver, sizeof(uint32), 1, file) != 1 ||
        fread(&num_entries, sizeof(int32), 1, file) != 1)
    {
        ereport(LOG,
                (errmsg("ignoring truncated pg_backup_compliance dump file")));
        goto done;
    }

    if (header != PGBC_FILE_HEADER || pgver != PGBC_PG_MAJOR_VERSION)
    {
        ereport(LOG,
                (errmsg("ignoring incompatible pg_backup_compliance "
                        "dump file (header=0x%08x ver=%u)", header, pgver)));
        goto done;
    }

    for (i = 0; i < num_entries; i++)
    {
        pgbcEntry   tmp;
        pgbcEntry  *entry;
        bool        found;

        if (fread(&tmp, sizeof(pgbcEntry), 1, file) != 1)
        {
            ereport(LOG,
                    (errmsg("ignoring truncated pg_backup_compliance "
                            "dump file at entry %d", i)));
            break;
        }

        entry = (pgbcEntry *) hash_search(pgbc_hash, &tmp.key,
                                          HASH_ENTER_NULL, &found);
        if (entry == NULL)
            break;              /* hash table full */

        memcpy(entry, &tmp, sizeof(pgbcEntry));
        SpinLockInit(&entry->mutex);
        if (entry->status == PGBC_STATUS_RUNNING)
        {
            entry->status = PGBC_STATUS_INTERRUPTED;
            if (entry->end_time == 0)
                entry->end_time = GetCurrentTimestamp();
            if (entry->error_message[0] == '\0')
                strlcpy(entry->error_message,
                        "PostgreSQL server restarted before operation completed",
                        sizeof(entry->error_message));
        }
    }

done:
    FreeFile(file);

    /* Unlink so basebackups and standbys do not carry stale state. */
    unlink(PGBC_DUMP_FILE);
}

static void
pgbc_save_dump_file(void)
{
    FILE       *file;
    HASH_SEQ_STATUS hash_seq;
    pgbcEntry  *entry;
    int32       num_entries;
    uint32      header = PGBC_FILE_HEADER;
    uint32      pgver = PGBC_PG_MAJOR_VERSION;

    /* The permanent stats dir normally exists, but be defensive. */
    (void) MakePGDirectory(PGSTAT_STAT_PERMANENT_DIRECTORY);

    file = AllocateFile(PGBC_DUMP_FILE_TMP, PG_BINARY_W);
    if (file == NULL)
    {
        ereport(LOG,
                (errcode_for_file_access(),
                 errmsg("could not write pg_backup_compliance dump file "
                        "\"%s\": %m", PGBC_DUMP_FILE_TMP)));
        return;
    }

    num_entries = hash_get_num_entries(pgbc_hash);

    if (fwrite(&header, sizeof(uint32), 1, file) != 1 ||
        fwrite(&pgver, sizeof(uint32), 1, file) != 1 ||
        fwrite(&num_entries, sizeof(int32), 1, file) != 1)
        goto write_error;

    hash_seq_init(&hash_seq, pgbc_hash);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        if (fwrite(entry, sizeof(pgbcEntry), 1, file) != 1)
        {
            hash_seq_term(&hash_seq);
            goto write_error;
        }
    }

    if (FreeFile(file) != 0)
    {
        file = NULL;
        goto write_error;
    }

    (void) durable_rename(PGBC_DUMP_FILE_TMP, PGBC_DUMP_FILE, LOG);
    return;

write_error:
    ereport(LOG,
            (errcode_for_file_access(),
             errmsg("could not finalise pg_backup_compliance dump file: %m")));
    if (file)
        FreeFile(file);
    unlink(PGBC_DUMP_FILE_TMP);
}

/* Persistence: the append-only archive of evicted entries. */

/*
 * Append one entry to the durable archive before it leaves the hash.  Caller
 * holds pgbc_state->lock EXCLUSIVE; a failed write is truncated back so a torn
 * record cannot corrupt the records that follow it.
 */
static void
pgbc_archive_entry(const pgbcEntry *entry)
{
    FILE       *file;
    struct stat st;
    off_t       prev_size = 0;
    bool        need_header;

    /* The permanent stats dir normally exists, but be defensive. */
    (void) MakePGDirectory(PGSTAT_STAT_PERMANENT_DIRECTORY);

    if (stat(PGBC_ARCHIVE_FILE, &st) == 0)
        prev_size = st.st_size;
    need_header = (prev_size == 0);

    file = AllocateFile(PGBC_ARCHIVE_FILE, PG_BINARY_A);
    if (file == NULL)
    {
        ereport(LOG,
                (errcode_for_file_access(),
                 errmsg("could not open pg_backup_compliance archive file "
                        "\"%s\": %m", PGBC_ARCHIVE_FILE)));
        return;
    }

    if (need_header)
    {
        uint32      header = PGBC_ARCHIVE_HEADER;
        uint32      pgver = PGBC_PG_MAJOR_VERSION;

        if (fwrite(&header, sizeof(uint32), 1, file) != 1 ||
            fwrite(&pgver, sizeof(uint32), 1, file) != 1)
            goto write_error;
    }

    if (fwrite(entry, sizeof(pgbcEntry), 1, file) != 1)
        goto write_error;

    /* Compliance data must survive an OS crash, not just a backend exit. */
    if (fflush(file) != 0)
        goto write_error;
    if (pg_fsync(fileno(file)) != 0)
        goto write_error;

    if (FreeFile(file) != 0)
    {
        file = NULL;
        goto write_error;
    }
    return;

write_error:
    ereport(LOG,
            (errcode_for_file_access(),
             errmsg("could not append to pg_backup_compliance archive file "
                    "\"%s\": %m", PGBC_ARCHIVE_FILE)));
    if (file)
    {
        /* Roll back any partial record so later reads stay aligned. */
        (void) fflush(file);
        if (ftruncate(fileno(file), prev_size) != 0)
            ereport(LOG,
                    (errcode_for_file_access(),
                     errmsg("could not truncate pg_backup_compliance archive "
                            "file \"%s\": %m", PGBC_ARCHIVE_FILE)));
        FreeFile(file);
    }
}

/* Remove the archive file.  Caller holds pgbc_state->lock EXCLUSIVE. */
static void
pgbc_truncate_archive(void)
{
    if (unlink(PGBC_ARCHIVE_FILE) != 0 && errno != ENOENT)
        ereport(LOG,
                (errcode_for_file_access(),
                 errmsg("could not remove pg_backup_compliance archive file "
                        "\"%s\": %m", PGBC_ARCHIVE_FILE)));
}

/* Hash-table helpers (called from the capture unit). */

/*
 * Evict one entry from the hash (caller holds pgbc_state->lock EXCLUSIVE).
 * Prefer the least-used terminal entry, else the oldest running one.
 */
static void
pgbc_evict_one_locked(void)
{
    HASH_SEQ_STATUS hash_seq;
    pgbcEntry  *entry;
    pgbcEntry  *victim = NULL;
    uint32      victim_usage = UINT32_MAX;
    bool        terminal_only = true;

    hash_seq_init(&hash_seq, pgbc_hash);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        if (entry->status != PGBC_STATUS_RUNNING && entry->usage <= victim_usage)
        {
            victim = entry;
            victim_usage = entry->usage;
        }
    }

    if (victim == NULL)
    {
        terminal_only = false;
        hash_seq_init(&hash_seq, pgbc_hash);
        while ((entry = hash_seq_search(&hash_seq)) != NULL)
        {
            if (entry->usage <= victim_usage)
            {
                victim = entry;
                victim_usage = entry->usage;
            }
        }
    }

    if (victim != NULL)
    {
        /* Persist the victim before removing it so it is never lost. */
        pgbc_archive_entry(victim);

        (void) hash_search(pgbc_hash, &victim->key, HASH_REMOVE, NULL);
        if (pgbc_state)
        {
            SpinLockAcquire(&pgbc_state->mutex);
            pgbc_state->total_evicted++;
            SpinLockRelease(&pgbc_state->mutex);
        }
        if (!terminal_only)
            ereport(LOG,
                    (errmsg("pg_backup_compliance: evicting a running entry "
                            "because hash table is full"),
                     errhint("Increase pg_backup_compliance.max_entries.")));
    }
}

pgbcEntry *
pgbc_lookup_or_create_entry(int pid, TimestampTz backend_start, bool create)
{
    pgbcHashKey key;
    pgbcEntry  *entry;
    bool        found;

    if (!pgbc_state || !pgbc_hash)
        return NULL;

    memset(&key, 0, sizeof(key));   /* zero padding for HASH_BLOBS keys */
    key.backend_pid = pid;
    key.backend_start = backend_start;

    if (!create)
    {
        LWLockAcquire(pgbc_state->lock, LW_SHARED);
        entry = (pgbcEntry *) hash_search(pgbc_hash, &key, HASH_FIND, &found);
        LWLockRelease(pgbc_state->lock);
        return found ? entry : NULL;
    }

    LWLockAcquire(pgbc_state->lock, LW_EXCLUSIVE);

    entry = (pgbcEntry *) hash_search(pgbc_hash, &key, HASH_ENTER_NULL, &found);
    if (entry == NULL)
    {
        pgbc_evict_one_locked();
        entry = (pgbcEntry *) hash_search(pgbc_hash, &key, HASH_ENTER_NULL, &found);
    }

    if (entry != NULL && !found)
    {
        memset(((char *) entry) + sizeof(pgbcHashKey),
               0,
               sizeof(pgbcEntry) - sizeof(pgbcHashKey));
        entry->key = key;
        SpinLockInit(&entry->mutex);
        entry->status = PGBC_STATUS_UNKNOWN;

        SpinLockAcquire(&pgbc_state->mutex);
        entry->usage = ++pgbc_state->sequence_counter;
        pgbc_state->total_captured++;
        SpinLockRelease(&pgbc_state->mutex);
    }

    LWLockRelease(pgbc_state->lock);
    return entry;
}

/* Compare two strings, treating NULL as the empty string. */
static bool
pgbc_streq(const char *a, const char *b)
{
    if (a == NULL)
        a = "";
    if (b == NULL)
        b = "";
    return strcmp(a, b) == 0;
}

/*
 * Does a connecting backend belong to an already-running operation?  Any
 * pg_dump is treated as part of a pg_dumpall while one is running.
 */
static bool
pgbc_type_folds_into(const char *child_type, const char *parent_type)
{
    if (child_type == NULL)
        child_type = "";
    if (parent_type == NULL)
        parent_type = "";

    /* pg_dumpall's child pg_dumps, or members sharing its name. */
    if (pg_strcasecmp(child_type, parent_type) == 0)
        return true;
    if (pg_strcasecmp(parent_type, "pg_dumpall") == 0 &&
        pg_strcasecmp(child_type, "pg_dump") == 0)
        return true;

    return false;
}

/* Multi-connection tools whose first connection starts a shared entry. */
static bool
pgbc_type_starts_aggregate(const char *backup_type)
{
    if (backup_type == NULL)
        return false;
    if (pg_strcasecmp(backup_type, "pg_dumpall") == 0)
        return true;
    if (pg_strcasecmp(backup_type, "pg_basebackup") == 0)
        return true;
    return false;
}

/*
 * Fold this backend into a running multi-connection operation, or start a new
 * shared entry if it is the umbrella connection.  Returns NULL when the type
 * does not aggregate, so the caller uses a per-backend entry.
 */
pgbcEntry *
pgbc_attach_aggregate_entry(const char *appname,
                            const char *dbname,
                            const char *username,
                            const char *client_addr,
                            const char *backup_type,
                            bool is_walsender)
{
    HASH_SEQ_STATUS hash_seq;
    pgbcEntry  *entry;
    pgbcEntry  *match = NULL;
    pgbcHashKey key;
    bool        found;

    if (!pgbc_state || !pgbc_hash)
        return NULL;
    if (appname == NULL || appname[0] == '\0')
        return NULL;

    LWLockAcquire(pgbc_state->lock, LW_EXCLUSIVE);

    /* Match on client + user; the database differs per child. */
    hash_seq_init(&hash_seq, pgbc_hash);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        if (entry->aggregated &&
            entry->status == PGBC_STATUS_RUNNING &&
            pgbc_streq(entry->client_addr, client_addr) &&
            pgbc_streq(entry->user_name, username) &&
            pgbc_type_folds_into(backup_type, entry->backup_type))
        {
            match = entry;
            hash_seq_term(&hash_seq);
            break;
        }
    }

    if (match != NULL)
    {
        SpinLockAcquire(&match->mutex);
        match->refcount++;
        match->conn_count++;
        SpinLockRelease(&match->mutex);
        LWLockRelease(pgbc_state->lock);
        return match;
    }

    /* Only multi-connection tools start a shared entry. */
    if (!pgbc_type_starts_aggregate(backup_type))
    {
        LWLockRelease(pgbc_state->lock);
        return NULL;
    }

    /* This is the umbrella connection: create the shared entry. */
    memset(&key, 0, sizeof(key));   /* zero padding for HASH_BLOBS keys */
    key.backend_pid = MyProcPid;
    key.backend_start = MyStartTimestamp;

    entry = (pgbcEntry *) hash_search(pgbc_hash, &key, HASH_ENTER_NULL, &found);
    if (entry == NULL)
    {
        pgbc_evict_one_locked();
        entry = (pgbcEntry *) hash_search(pgbc_hash, &key, HASH_ENTER_NULL,
                                          &found);
    }

    if (entry == NULL)
    {
        LWLockRelease(pgbc_state->lock);
        return NULL;
    }

    if (found)
    {
        /* Already created for this backend; just reuse it. */
        SpinLockAcquire(&entry->mutex);
        if (entry->aggregated)
        {
            entry->refcount++;
            entry->conn_count++;
        }
        SpinLockRelease(&entry->mutex);
        LWLockRelease(pgbc_state->lock);
        return entry;
    }

    memset(((char *) entry) + sizeof(pgbcHashKey),
           0,
           sizeof(pgbcEntry) - sizeof(pgbcHashKey));
    entry->key = key;
    SpinLockInit(&entry->mutex);

    if (appname)
        strlcpy(entry->application_name, appname,
                sizeof(entry->application_name));
    if (dbname)
        strlcpy(entry->database_name, dbname, sizeof(entry->database_name));
    if (username)
        strlcpy(entry->user_name, username, sizeof(entry->user_name));
    if (client_addr)
        strlcpy(entry->client_addr, client_addr, sizeof(entry->client_addr));
    if (backup_type)
        strlcpy(entry->backup_type, backup_type, sizeof(entry->backup_type));

    entry->is_walsender = is_walsender;
    entry->start_time = (MyStartTimestamp != 0) ? MyStartTimestamp
                                                : GetCurrentTimestamp();
    entry->status = PGBC_STATUS_RUNNING;
    entry->aggregated = true;
    entry->refcount = 1;
    entry->conn_count = 1;

    SpinLockAcquire(&pgbc_state->mutex);
    entry->usage = ++pgbc_state->sequence_counter;
    pgbc_state->total_captured++;
    SpinLockRelease(&pgbc_state->mutex);

    LWLockRelease(pgbc_state->lock);
    return entry;
}

void
pgbc_entry_mark_running(pgbcEntry *entry,
                        const char *appname,
                        const char *dbname,
                        const char *username,
                        const char *client_addr,
                        const char *backup_type,
                        bool is_walsender,
                        TimestampTz start_time)
{
    if (entry == NULL)
        return;

    SpinLockAcquire(&entry->mutex);
    if (appname)
        strlcpy(entry->application_name, appname,
                sizeof(entry->application_name));
    if (dbname)
        strlcpy(entry->database_name, dbname,
                sizeof(entry->database_name));
    if (username)
        strlcpy(entry->user_name, username, sizeof(entry->user_name));
    if (client_addr)
        strlcpy(entry->client_addr, client_addr,
                sizeof(entry->client_addr));
    if (backup_type)
        strlcpy(entry->backup_type, backup_type,
                sizeof(entry->backup_type));
    entry->is_walsender = is_walsender;
    if (start_time != 0)
        entry->start_time = start_time;
    else if (entry->start_time == 0)
        entry->start_time = GetCurrentTimestamp();
    entry->status = PGBC_STATUS_RUNNING;
    SpinLockRelease(&entry->mutex);
}

void
pgbc_entry_record_error(pgbcEntry *entry, const char *msg)
{
    if (entry == NULL || msg == NULL)
        return;

    SpinLockAcquire(&entry->mutex);
    if (entry->error_message[0] == '\0')
        strlcpy(entry->error_message, msg, sizeof(entry->error_message));
    SpinLockRelease(&entry->mutex);
}

void
pgbc_entry_mark_aborted(pgbcEntry *entry)
{
    if (entry == NULL)
        return;

    SpinLockAcquire(&entry->mutex);
    entry->backup_aborted = true;
    SpinLockRelease(&entry->mutex);
}

/*
 * Downgrade a would-be success to failure when terminal evidence says the
 * backup did not produce a usable result.  Must hold entry->mutex.
 */
static pgbcStatus
pgbc_apply_terminal_downgrade(pgbcEntry *entry, pgbcStatus status)
{
    if (status != PGBC_STATUS_SUCCEEDED)
        return status;

    /* Backup aborted before pg_backup_stop() (e.g. pgBackRest archive timeout). */
    if (entry->backup_aborted)
    {
        if (entry->error_message[0] == '\0')
            strlcpy(entry->error_message,
                    "backup aborted before pg_backup_stop was called",
                    sizeof(entry->error_message));
        return PGBC_STATUS_FAILED;
    }

    /* Walsender that neither streamed WAL nor ran a base backup sent nothing. */
    if (entry->is_walsender && !entry->data_sent)
    {
        if (entry->error_message[0] == '\0')
            strlcpy(entry->error_message,
                    "backup connection closed without sending any data",
                    sizeof(entry->error_message));
        return PGBC_STATUS_FAILED;
    }

    return status;
}

void
pgbc_entry_finalize(pgbcEntry *entry,
                    pgbcStatus status,
                    int32 exit_code,
                    TimestampTz end_time)
{
    if (entry == NULL)
        return;

    SpinLockAcquire(&entry->mutex);

    if (!entry->aggregated)
    {
        status = pgbc_apply_terminal_downgrade(entry, status);
        entry->status = status;
        entry->exit_code = exit_code;
        entry->end_time = (end_time != 0) ? end_time : GetCurrentTimestamp();
        SpinLockRelease(&entry->mutex);
        return;
    }

    /* One member exiting: remember a failure, but wait for the last. */
    if (status != PGBC_STATUS_SUCCEEDED && entry->status == PGBC_STATUS_RUNNING)
    {
        entry->status = status;
        entry->exit_code = exit_code;
    }

    if (entry->refcount > 0)
        entry->refcount--;

    if (entry->refcount <= 0)
    {
        if (entry->status == PGBC_STATUS_RUNNING)
        {
            /* No member failed; judge no-data now that all have reported. */
            status = pgbc_apply_terminal_downgrade(entry, status);
            entry->status = status;
            entry->exit_code = exit_code;
        }
        entry->end_time = (end_time != 0) ? end_time : GetCurrentTimestamp();
    }

    SpinLockRelease(&entry->mutex);
}

/* Track-apps GUC parsing. */

static bool
str_starts_with_ci(const char *s, const char *prefix, size_t prefix_len)
{
    size_t i;

    if (s == NULL || prefix == NULL)
        return false;
    for (i = 0; i < prefix_len; i++)
    {
        char a = s[i];
        char b = prefix[i];

        if (a == '\0')
            return false;
        if (a >= 'A' && a <= 'Z')
            a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z')
            b = b - 'A' + 'a';
        if (a != b)
            return false;
    }
    return true;
}

/* pgBackRest app names look like "pgBackRest [<command>]"; only backups count. */
static bool
pgbc_pgbackrest_is_backup(const char *appname)
{
    static const char backup_cmd[] = "backup";
    const size_t backup_len = sizeof(backup_cmd) - 1;
    const char *cmd;
    char        after;

    cmd = strchr(appname, '[');
    if (cmd == NULL)
        return false;

    cmd++;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;

    if (!str_starts_with_ci(cmd, backup_cmd, backup_len))
        return false;

    after = cmd[backup_len];
    return (after == ']' || after == ' ' || after == '\t' || after == '\0');
}

bool
pgbc_app_is_tracked(const char *appname, char *out_type, size_t out_type_len)
{
    const char *p;
    size_t      app_len;
    const char *best = NULL;
    size_t      best_len = 0;

    if (appname == NULL || appname[0] == '\0')
        return false;
    if (pgbc_track_apps == NULL || pgbc_track_apps[0] == '\0')
        return false;

    app_len = strlen(appname);
    p = pgbc_track_apps;

    while (*p)
    {
        const char *start;
        const char *end;
        size_t      tok_len;

        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        start = p;
        end = p;
        while (*end && *end != ',' && *end != ' ' && *end != '\t')
            end++;
        tok_len = end - start;
        p = end;

        if (tok_len == 0)
            continue;

        /* Longest prefix match wins, so "pg_dumpall" beats "pg_dump". */
        if (tok_len <= app_len && str_starts_with_ci(appname, start, tok_len) &&
            tok_len > best_len)
        {
            best = start;
            best_len = tok_len;
        }
    }

    if (best == NULL)
        return false;

    /* Skip pgBackRest non-backup commands (check, stanza-create, ...). */
    if (str_starts_with_ci(appname, "pgbackrest", strlen("pgbackrest")) &&
        !pgbc_pgbackrest_is_backup(appname))
        return false;

    if (out_type && out_type_len > 0)
    {
        size_t      copy = best_len;

        if (copy >= out_type_len)
            copy = out_type_len - 1;
        memcpy(out_type, best, copy);
        out_type[copy] = '\0';
    }
    return true;
}

static const char *
pgbc_status_name(pgbcStatus s)
{
    switch (s)
    {
        case PGBC_STATUS_RUNNING:     return "running";
        case PGBC_STATUS_SUCCEEDED:   return "success";
        case PGBC_STATUS_FAILED:      return "failed";
        case PGBC_STATUS_INTERRUPTED: return "interrupted";
        case PGBC_STATUS_AUTH_FAILED: return "auth_failed";
        default:                      return "unknown";
    }
}

/* SRF: pg_backup_compliance_operations() and _archived_operations(). */

#define PGBC_NUM_COLS 14

/* Validate the SRF call context and build the result tuplestore. */
static Tuplestorestate *
pgbc_make_tupstore(FunctionCallInfo fcinfo, TupleDesc *tupdesc_p)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext oldcontext;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
        (rsinfo->allowedModes & SFRM_Materialize) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that "
                        "cannot accept a set")));

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    *tupdesc_p = tupdesc;
    return tupstore;
}

/* Emit one entry (a stable copy) as a row in the result tuplestore. */
static void
pgbc_put_entry_tuple(Tuplestorestate *tupstore, TupleDesc tupdesc,
                     const pgbcEntry *snap)
{
    Datum       values[PGBC_NUM_COLS];
    bool        nulls[PGBC_NUM_COLS];
    int         i = 0;

    memset(nulls, 0, sizeof(nulls));

    values[i++] = Int32GetDatum(snap->key.backend_pid);
    values[i++] = TimestampTzGetDatum(snap->key.backend_start);
    values[i++] = CStringGetTextDatum(snap->application_name);
    values[i++] = CStringGetTextDatum(snap->backup_type[0] ? snap->backup_type
                                                           : snap->application_name);

    if (snap->database_name[0])
        values[i++] = CStringGetTextDatum(snap->database_name);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    if (snap->user_name[0])
        values[i++] = CStringGetTextDatum(snap->user_name);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    if (snap->client_addr[0])
        values[i++] = CStringGetTextDatum(snap->client_addr);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    values[i++] = BoolGetDatum(snap->is_walsender);

    if (snap->start_time != 0)
        values[i++] = TimestampTzGetDatum(snap->start_time);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    if (snap->end_time != 0)
        values[i++] = TimestampTzGetDatum(snap->end_time);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    values[i++] = CStringGetTextDatum(pgbc_status_name(snap->status));
    values[i++] = Int32GetDatum(snap->exit_code);

    if (snap->error_message[0])
        values[i++] = CStringGetTextDatum(snap->error_message);
    else
    { nulls[i] = true; values[i++] = (Datum) 0; }

    /* Number of connections folded into this operation (>= 1). */
    values[i++] = Int32GetDatum(snap->aggregated ? snap->conn_count : 1);

    Assert(i == PGBC_NUM_COLS);

    tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

/*
 * Read every archived (evicted) entry from disk into the result tuplestore.
 * Caller holds pgbc_state->lock SHARED, excluding a concurrent append.
 */
static void
pgbc_archive_to_tupstore(Tuplestorestate *tupstore, TupleDesc tupdesc)
{
    FILE       *file;
    uint32      header;
    uint32      pgver;

    file = AllocateFile(PGBC_ARCHIVE_FILE, PG_BINARY_R);
    if (file == NULL)
    {
        if (errno != ENOENT)
            ereport(LOG,
                    (errcode_for_file_access(),
                     errmsg("could not read pg_backup_compliance archive file "
                            "\"%s\": %m", PGBC_ARCHIVE_FILE)));
        return;
    }

    if (fread(&header, sizeof(uint32), 1, file) != 1 ||
        fread(&pgver, sizeof(uint32), 1, file) != 1)
    {
        /* Empty or header-only file: nothing to return. */
        FreeFile(file);
        return;
    }

    if (header != PGBC_ARCHIVE_HEADER || pgver != PGBC_PG_MAJOR_VERSION)
    {
        ereport(LOG,
                (errmsg("ignoring incompatible pg_backup_compliance archive "
                        "file (header=0x%08x ver=%u)", header, pgver)));
        FreeFile(file);
        return;
    }

    for (;;)
    {
        pgbcEntry   entry;

        if (fread(&entry, sizeof(pgbcEntry), 1, file) != 1)
            break;              /* EOF or trailing partial record */

        pgbc_put_entry_tuple(tupstore, tupdesc, &entry);
    }

    FreeFile(file);
}

Datum
pg_backup_compliance_operations(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;
    HASH_SEQ_STATUS hash_seq;
    pgbcEntry  *entry;

    tupstore = pgbc_make_tupstore(fcinfo, &tupdesc);

    if (!pgbc_state || !pgbc_hash)
        return (Datum) 0;

    LWLockAcquire(pgbc_state->lock, LW_SHARED);

    hash_seq_init(&hash_seq, pgbc_hash);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
    {
        pgbcEntry   snap;

        SpinLockAcquire(&entry->mutex);
        snap = *entry;
        SpinLockRelease(&entry->mutex);

        pgbc_put_entry_tuple(tupstore, tupdesc, &snap);
    }

    LWLockRelease(pgbc_state->lock);

    return (Datum) 0;
}

/* Return only the archived (evicted) operations; unioned with the live SRF. */
Datum
pg_backup_compliance_archived_operations(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;

    tupstore = pgbc_make_tupstore(fcinfo, &tupdesc);

    if (!pgbc_state)
        return (Datum) 0;

    LWLockAcquire(pgbc_state->lock, LW_SHARED);
    pgbc_archive_to_tupstore(tupstore, tupdesc);
    LWLockRelease(pgbc_state->lock);

    return (Datum) 0;
}

Datum
pg_backup_compliance_reset(PG_FUNCTION_ARGS)
{
    HASH_SEQ_STATUS hash_seq;
    pgbcEntry  *entry;

    if (!superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("must be superuser to reset "
                        "pg_backup_compliance state")));

    if (!pgbc_state || !pgbc_hash)
        PG_RETURN_VOID();

    LWLockAcquire(pgbc_state->lock, LW_EXCLUSIVE);

    hash_seq_init(&hash_seq, pgbc_hash);
    while ((entry = hash_seq_search(&hash_seq)) != NULL)
        (void) hash_search(pgbc_hash, &entry->key, HASH_REMOVE, NULL);

    /* total_evicted is cleared below, so drop the archived rows too. */
    pgbc_truncate_archive();

    SpinLockAcquire(&pgbc_state->mutex);
    pgbc_state->total_captured = 0;
    pgbc_state->total_evicted = 0;
    pgbc_state->sequence_counter = 0;
    pgbc_state->last_reset = GetCurrentTimestamp();
    SpinLockRelease(&pgbc_state->mutex);

    LWLockRelease(pgbc_state->lock);

    PG_RETURN_VOID();
}

Datum
pg_backup_compliance_info(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Datum       values[4];
    bool        nulls[4] = {false, false, false, false};
    HeapTuple   tuple;
    int64       captured = 0;
    int64       evicted = 0;
    TimestampTz last_reset = 0;
    int32       live;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    if (pgbc_state)
    {
        SpinLockAcquire(&pgbc_state->mutex);
        captured = pgbc_state->total_captured;
        evicted = pgbc_state->total_evicted;
        last_reset = pgbc_state->last_reset;
        SpinLockRelease(&pgbc_state->mutex);
    }
    live = pgbc_hash ? hash_get_num_entries(pgbc_hash) : 0;

    values[0] = Int32GetDatum(live);
    values[1] = Int64GetDatum(captured);
    values[2] = Int64GetDatum(evicted);
    if (last_reset != 0)
        values[3] = TimestampTzGetDatum(last_reset);
    else
        nulls[3] = true;

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
