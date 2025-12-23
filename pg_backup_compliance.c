#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "catalog/pg_database.h"
#include "catalog/pg_database_d.h"
#include "tcop/utility.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "catalog/pg_database.h"
#include "catalog/pg_database_d.h"
#include "storage/proc.h"
#include "utils/timestamp.h"
#include "utils/lsyscache.h"      // for get_database_name()
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdio.h>
#include "utils/elog.h"
#include "utils/memutils.h"
#include "commands/extension.h"
#include "utils/snapmgr.h"


// ----------------------------------------------
// Required ADDITIONS for Background Worker
// ----------------------------------------------
#include "postmaster/bgworker.h"     // Background worker
#include "storage/ipc.h"             // for on_proc_exit
#include "storage/latch.h"           // WaitLatch
#include "storage/lwlock.h"          // LWLocks
#include "storage/shmem.h"           // shared memory API
#include "storage/dsm.h"             // dynamic shared memory (if used)
#include "storage/sinval.h"          // sinval handling in workers
#include "storage/spin.h"            // spinlocks
#include "storage/procsignal.h"      // for procsignal
#include "storage/pmsignal.h"        // PMSignal support
#include "utils/ps_status.h"         // set_ps_display()
#include "access/xact.h"             // StartTransaction/CommitTransaction
#include "access/xlog.h"             // XLogFlush
#include "tcop/tcopprot.h"           // ProcessUtilityHook
#include <signal.h>

//for database creation
#include "utils/syscache.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "commands/dbcommands.h"

#if PG_VERSION_NUM >= 170000
#include "parser/parse_node.h"
#endif


PG_MODULE_MAGIC;

static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup  = false;

// #define PGAUD_PROC_SIGNAL PROCSIG_STARTUP_DEADLOCK
#define PGAUD_PROC_SIGNAL 0
// static bool tables_created = false;

/* forward declarations for dump hook module */
extern void _PG_init_dump(void);
extern void _PG_fini_dump(void);
// static void pgaud_exit_hook(int code, Datum arg);

extern char *get_database_name(Oid dbid);
extern Oid get_database_oid(const char *dbname, bool missing_ok);
extern Oid get_role_oid(const char *rolename, bool missing_ok);

PG_FUNCTION_INFO_V1(pgaud_init);
Datum pgaud_init(PG_FUNCTION_ARGS);

// static ErrorContextCallback pgaud_error_context;
// static void pgaud_error_callback(void *arg);
void pgaud_run_schema(void);
void pgaud_run_views_schema(void);
// void (*prev_emit_log_hook)(ErrorData *edata) = NULL;

void insert_into_log_immediate(const char *app, const char *db, int pid,
                          const char *start_time, const char *end_time, const char *err);

/* ----------------- Backgroud worker configuration ---------------------------- */

#define PGAUD_QUEUE_SLOTS 256
#define PGAUD_APP_LEN     128
#define PGAUD_DB_LEN      128
#define PGAUD_TIME_LEN    64
#define PGAUD_ERR_LEN     1024

// job types - This denotes various job bg worker will do
// created switch-case block in worker call function 
// and this variable can be used to denote various task in switch-case
#define PGAUD_JOB_BACKUP_LOG 1
#define PGAUD_JOB_SCHEMA_CREATE 2

// polling interval for worker (ms) 
#define PGAUD_WORKER_POLL_MS 500 

#define BACKUP_DATABASE "backupcompliance"

//GUC to enable/disable the bgworker logging

// bool pgaud_enabled = true;
// bool schema_initialized = false;

/* ----------------------------- shared types ------------------------------ */


// creating a job which is basically a stack of all the data
// we want to put into table for one row.
typedef struct
{
    int job_type;  
    char application[PGAUD_APP_LEN];
    char database[PGAUD_DB_LEN];
    int  pid;
    char start_time[PGAUD_TIME_LEN];
    char end_time[PGAUD_TIME_LEN];
    char err[PGAUD_ERR_LEN];  // dont forget to escape single quote
    char client_addr[PGAUD_ERR_LEN];
} PGAudJob;


// This is shared memory queue.
// This is a circular queue, we can write various jobs onto it
// Worker will take the job data from it and will insert into table.
typedef struct
{
    slock_t lock;           // spinlock for head/tail + jobs 
    int head;               // next free slot index 
    int tail;               // first occupied slot index 
    PGAudJob jobs[PGAUD_QUEUE_SLOTS];
    pid_t worker_pid;       // bgworker pid when running 
} PGAudSharedState;


// WaitLatch wait-mode: use PG_WAIT_EXTENSION if available, otherwise 0 
//Although not required as this is harcoded in code below
// but kept it for future improvement and function generic
#ifndef PG_WAIT_EXTENSION
#define PG_WAIT_EXTENSION 0
#endif


// global pointer to SHM state. 
// This tells us if our global state is set, 
// so that we ca use and no error is thrown
static PGAudSharedState *pgaud_state = NULL;

static void pgaud_shmem_startup(void);
PGDLLEXPORT void pgaud_worker_main(Datum main_arg);
static void pgaud_shmem_request(void);

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;

// Escape single quotes: ' -> '' into dst (dst_size must be >0).
// SQL dont allow '. e.g.  this's is not allowed. It should be this''s.
static void
escape_single_quotes(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;
    if (!dst || dst_size == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    while (src[i] != '\0' && j + 1 < dst_size)
    {
        if (src[i] == '\'')
        {
            // need two chars for "''" 
            if (j + 2 >= dst_size)
                break;
            dst[j++] = '\'';
            dst[j++] = '\'';
        }
        else
            dst[j++] = src[i];
        i++;
    }
    dst[j] = '\0';
}



// Format current timestamptz into buf.
static void
format_now_timestamp(char *buf, size_t buf_size)
{
    TimestampTz now = GetCurrentTimestamp();
    const char *s = timestamptz_to_str(now);
    strlcpy(buf, s, buf_size);
}

//check if database exists
static
bool
database_exists(const char *dbname)
{
    bool exists = false;
    int rc;

    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    rc = SPI_execute(
    "SELECT 1 FROM pg_database WHERE datname = 'backupcompliance'",
    true,
    0
    );
    if (rc == SPI_OK_SELECT && SPI_processed > 0)
        exists = true;

    SPI_finish();
    return exists;
}



//Initializing shmem-queue to put jobs 
static void
pgaud_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    pgaud_state = (PGAudSharedState *)
        ShmemInitStruct("pgaud_shared_state",
                        sizeof(PGAudSharedState),
                        &found);

    if (!found)
    {
        SpinLockInit(&pgaud_state->lock);
        pgaud_state->head = 0;
        pgaud_state->tail = 0;
        pgaud_state->worker_pid = 0;
        MemSet(pgaud_state->jobs, 0, sizeof(pgaud_state->jobs));
    }
}


static void
pgaud_shmem_request(void)
{
    /* Request the shared memory size and LWLock tranche here.
     * This runs at the correct time during server startup.
     */
    RequestAddinShmemSpace(sizeof(PGAudSharedState));
    RequestNamedLWLockTranche("pgaud_queue_tranche", 1);

    /* chain previous hook if present */
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
}


// Enqueue job into ring buffer.
// Safe for error-hook usage: uses only SpinLock and stack buffers.
// Used Spinlock because i thought, it is safe for small work
// For large work can use LWLock
//Spinlock can not be used in the situation where process will sleep.
// Returns true on success, false if queue full.
static bool
pgaud_enqueue_job(const PGAudJob *job)
{
    bool ok = false;
    int next;
    // If SHM not ready, drop quietly and log  
    if (!pgaud_state)
        return false;

    SpinLockAcquire(&pgaud_state->lock);

    next = (pgaud_state->head + 1) % PGAUD_QUEUE_SLOTS;
    if (next == pgaud_state->tail)
    {
        // queue full 
        ok = false;
    }
    else
    {
        // copy job into slot
        pgaud_state->jobs[pgaud_state->head] = *job;
        pgaud_state->head = next;
        ok = true;
    }

    SpinLockRelease(&pgaud_state->lock);
    return ok;
}

/* Dequeue job (worker side). Returns true & fills out on success. */
static bool
pgaud_dequeue_job(PGAudJob *out)
{
    bool ok = false;

    if (!pgaud_state)
        return false;

    SpinLockAcquire(&pgaud_state->lock);

    if (pgaud_state->head == pgaud_state->tail)
    {
        ok = false;
    }
    else
    {
        *out = pgaud_state->jobs[pgaud_state->tail];
        pgaud_state->tail = (pgaud_state->tail + 1) % PGAUD_QUEUE_SLOTS;
        ok = true;
    }

    SpinLockRelease(&pgaud_state->lock);
    return ok;
}

// insert_into_log_immediate - this prepares data and puts it into 
// shared memory and then sends a signal to wake up the worker 
void insert_into_log_immediate(const char *app, const char *db, int pid,
                          const char *start_time, const char *end_time, const char *err)
{
    PGAudJob job;
    char err_escaped[PGAUD_ERR_LEN];
    char startbuf[PGAUD_TIME_LEN];
    char endbuf[PGAUD_TIME_LEN];

    if (start_time && start_time[0] != '\0')
        strlcpy(startbuf, start_time, sizeof(startbuf));
    else
        format_now_timestamp(startbuf, sizeof(startbuf));

    if (end_time && end_time[0] != '\0')
        strlcpy(endbuf, end_time, sizeof(endbuf));
    else
        strlcpy(endbuf, startbuf, sizeof(endbuf));

    // filling job fields safely 
    job.job_type = PGAUD_JOB_BACKUP_LOG;

    
    strlcpy(job.application, app ? app : "(unknown)", sizeof(job.application));
    strlcpy(job.database, db ? db : "", sizeof(job.database));

    job.pid = pid;

    strlcpy(job.start_time, startbuf, sizeof(job.start_time));
    strlcpy(job.end_time, endbuf, sizeof(job.end_time));

   
    escape_single_quotes(err ? err : "", err_escaped, sizeof(err_escaped));
    strlcpy(job.err, err_escaped, sizeof(job.err));

    // strlcpy(job.client_addr, client_addr ? client_addr : "", sizeof(job.client_addr));
  
   
    if (!pgaud_enqueue_job(&job))
    {
        elog(WARNING, "[pg_backup_compliance] job queue full or unavailable; dropped log for app=%s pid=%d",
             job.application, job.pid);
        return;
    } else{
        elog(LOG,"[pg_backup_compliance] Job added to queue for app=%s pid=%d",job.application, job.pid);
    }

    // if (pgaud_state && pgaud_state->worker_pid > 0)
    // {
        #if PG_VERSION_NUM < 170000
                SendProcSignal(pgaud_state->worker_pid,
                            PGAUD_PROC_SIGNAL,
                            MyBackendId);
            #else
                SendProcSignal(pgaud_state->worker_pid,
                            PGAUD_PROC_SIGNAL,
                            MyBackendType);
            #endif
    // }
    return;
}


/* 
    This hook only triggers at the end of a dump/basebackup/backrest session to log the event    
    those ends with FATAL/ERROR/PANIC/PGERROR 

    This helps in catching those sessions that fails without even calling the hooks 
    or ends in console errors.
*/

static void
pgaud_emit_log_hook(ErrorData *edata)
{
    static bool in_hook;
    const char *app;
    // 1. Call the previous hook in the chain if it exists 
    if (prev_emit_log_hook)
        prev_emit_log_hook(edata);

    // 2. Recursion Guard: Prevent infinite loops if our logic errors out 
    in_hook = false;
    if (in_hook)
        return;

    // 3. Filter: Only process ERROR, FATAL, or PANIC 
    if (edata->elevel < ERROR)
        return;

    in_hook = true;

    // 4. Filter: Only process specific applications (pg_dump / pg_basebackup) 
    app = (MyProcPort && MyProcPort->application_name)
                        ? MyProcPort->application_name
                        : "";

    if (strncmp(app, "pg_dump", 7) == 0 || strncmp(app, "pg_basebackup", 13) == 0)
    {
        PGAudJob job;
        const char *db;
        TimestampTz now;
        const char *now_str;

        // Safe database name lookup 
        db = (OidIsValid(MyDatabaseId)) ? get_database_name(MyDatabaseId) : "[no db]";

        memset(&job, 0, sizeof(PGAudJob));
        job.job_type = PGAUD_JOB_BACKUP_LOG;
        
        strlcpy(job.application, app, PGAUD_APP_LEN);
        strlcpy(job.database, db ? db : "", PGAUD_DB_LEN);
        job.pid = MyProcPid;

        now = GetCurrentTimestamp();
        now_str = timestamptz_to_str(now);
        strlcpy(job.start_time, now_str, PGAUD_TIME_LEN);
        strlcpy(job.end_time, now_str, PGAUD_TIME_LEN);

        // Extract error message from edata 
        if (edata->message)
            escape_single_quotes(edata->message, job.err, PGAUD_ERR_LEN);
        else
            strlcpy(job.err, "unknown error/termination", PGAUD_ERR_LEN);

        /* * Use write_stderr for internal extension debugging. 
         * Calling elog(LOG) inside an error hook can be risky.
         */
        write_stderr("[pgaud_dump] Enqueuing error job for app=%s\n", app);

        // 5. Enqueue and Notify
        if (pgaud_enqueue_job(&job))
        {
            if (pgaud_state && pgaud_state->worker_pid != 0)
            {
                #if PG_VERSION_NUM < 170000
                    SendProcSignal(pgaud_state->worker_pid, PGAUD_PROC_SIGNAL, MyBackendId);
                #else
                    SendProcSignal(pgaud_state->worker_pid, PGAUD_PROC_SIGNAL, MyBackendType);
                #endif
            }
        }
    }

    in_hook = false;
}


// Handle a backup log job: insert or update the log entry.
static void
pgaud_handle_backup_log(PGAudJob *job)
{
    elog(LOG, "[pg_backup_compliance] Worker inserting job for pid=%d", job->pid);
    // CHECK_FOR_INTERRUPTS();

    PG_TRY();
    {
        int rc;
        char sql_chk[256];
        char sql[2048];
        bool exists = false;
        elog(LOG, "[pg_backup_compliance] Worker processing backup log for pid=%d",
             job->pid);
        /* ---- CHECK EXISTENCE ---- */
        
        snprintf(sql_chk, sizeof(sql_chk),
                 "SELECT COUNT(*) FROM backup_operations_log WHERE backend_pid=%d",
                 job->pid);

        rc = SPI_execute(sql_chk, true, 1);
        
        elog(LOG, "[pg_backup_compliance] SPI_execute returned rc=%d for pid=%d", rc, job->pid);
        if (rc != SPI_OK_SELECT)
            elog(ERROR, "SPI_execute failed (check)");

        elog(LOG, "[pg_backup_compliance] SPI_processed=%lu for pid=%d", SPI_processed, job->pid);
        if (SPI_processed == 1)
        {
            bool isnull;
            Datum d = SPI_getbinval(
                SPI_tuptable->vals[0],
                SPI_tuptable->tupdesc,
                1,
                &isnull);

            exists = (!isnull && DatumGetInt32(d) > 0);
        }
        elog(LOG, "[pg_backup_compliance] Log existence for pid=%d: %s",
             job->pid, exists ? "yes" : "no");
        /* ---- INSERT OR UPDATE ---- */
        

        if (exists)
        {
            snprintf(sql, sizeof(sql),
                "UPDATE backup_operations_log "
                "SET end_time='%s', error='%s' "
                "WHERE backend_pid=%d",
                job->end_time,
                job->err,
                job->pid);
        }
        else
        {
            snprintf(sql, sizeof(sql),
                "INSERT INTO backup_operations_log "
                "(application_name, database_name, backend_pid, start_time, end_time, error) "
                "VALUES ('%s','%s',%d,'%s','%s','%s')",
                job->application,
                job->database,
                job->pid,
                job->start_time,
                job->end_time,
                job->err);
        }
        elog(LOG, "[pg_backup_compliance] Executing SQL for pid=%d: %s", job->pid, sql);
        rc = SPI_execute(sql, false, 0);
        if (rc != SPI_OK_INSERT && rc != SPI_OK_UPDATE)
            elog(ERROR, "SPI_execute failed (insert/update)");

        elog(LOG, "[pg_backup_compliance] Log written for pid=%d", job->pid);
    }
    PG_CATCH();
    {
        /* DO NOT abort transaction here */
        elog(WARNING,
             "[pg_backup_compliance] Worker failed to insert log for pid=%d",
             job->pid);

        FlushErrorState();
    }
    PG_END_TRY();
}


void
pgaud_run_schema(void)
{
   

    const char *cmds[]={
        "DO $$ BEGIN "
        "IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'log_method') THEN "
        "   CREATE TYPE public.log_method AS ENUM ("
        "       'archive-push','backup','restore','check','stanza-create','stanza-upgrade'"
        "   ); "
        "END IF; "
        "END $$;",

        "CREATE TABLE IF NOT EXISTS public.backup_operations_log ("
        "   id SERIAL PRIMARY KEY,"
        "   application_name TEXT NOT NULL,"
        "   start_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,"
        "   end_time TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,"
        "   database_name TEXT NOT NULL,"
        "   error TEXT,"
        "   backend_pid INTEGER NOT NULL"
        ");",

        "CREATE INDEX IF NOT EXISTS idx_backup_log_pid "
        "ON public.backup_operations_log (backend_pid);",

        "GRANT SELECT, INSERT ON public.backup_operations_log TO public;",
        "GRANT USAGE, SELECT ON SEQUENCE public.backup_operations_log_id_seq TO public;",
        NULL
    };;

     elog(LOG, "[pg_backup_compliance] Running schema creation in backupcompliance");
    
    // StartTransactionCommand();
    // PushActiveSnapshot(GetTransactionSnapshot());

    // if (SPI_connect() != SPI_OK_CONNECT)
    //     elog(ERROR, "[pg_backup_compliance] worker schema SPI_connect failed");

    // elog(LOG, "[pg_backup_compliance] connected to SPI for schema creation");


    elog(LOG, "[pg_backup_compliance] executing schema commands");
    for (int i = 0; cmds[i] != NULL; i++)
    {
        int ret = SPI_execute(cmds[i], false, 0);
        if (ret != SPI_OK_UTILITY)
            elog(WARNING, "[pg_backup_compliance] SPI_execute failed: %s", cmds[i]);
    }

    // SPI_finish();

    /* End transaction */
    // PopActiveSnapshot();
    // CommitTransactionCommand();

    elog(LOG, "[pg_backup_compliance] schema created or already existed");
}

void
pgaud_run_views_schema(void)
{
   
    const char *cmds[] = {

        "CREATE OR REPLACE VIEW v_quarterly_backups AS "
        "SELECT * "
        "FROM backup_operations_log "
        "WHERE application_name IN ('pg_dump', 'pg_basebackup', 'pgbackrest','pg_dumpall') "
        "AND start_time >= date_trunc('quarter', CURRENT_DATE);",

        "GRANT SELECT ON v_quarterly_backups TO public;",

        "CREATE OR REPLACE VIEW v_monthly_backups AS "
        "SELECT * "
        "FROM backup_operations_log "
        "WHERE application_name IN ('pg_dump', 'pg_basebackup', 'pgbackrest','pg_dumpall') "
        "AND start_time >= date_trunc('month', CURRENT_DATE);",

        "GRANT SELECT ON v_monthly_backups TO public;",

        "CREATE OR REPLACE VIEW v_failed_backups AS "
        "SELECT * "
        "FROM backup_operations_log "
        "WHERE application_name IN ('pg_dump', 'pg_basebackup', 'pgbackrest','pg_dumpall') "
        "AND error IS NOT NULL "
        "AND length(trim(error)) > 0;",

        "GRANT SELECT ON v_failed_backups TO public;",

        "CREATE OR REPLACE VIEW v_quarterly_failed_backups AS "
        "SELECT * "
        "FROM backup_operations_log "
        "WHERE application_name IN ('pg_dump', 'pg_basebackup', 'pgbackrest','pg_dumpall') "
        "AND error IS NOT NULL "
        "AND length(trim(error)) > 0 "
        "AND start_time >= date_trunc('quarter', CURRENT_DATE);",

        "GRANT SELECT ON v_quarterly_failed_backups TO public;",

        "CREATE OR REPLACE VIEW v_monthly_failed_backups AS "
        "SELECT * "
        "FROM backup_operations_log "
        "WHERE application_name IN ('pg_dump', 'pg_basebackup', 'pgbackrest','pg_dumpall') "
        "AND error IS NOT NULL "
        "AND length(trim(error)) > 0 "
        "AND start_time >= date_trunc('month', CURRENT_DATE);",

        "GRANT SELECT ON v_monthly_failed_backups TO public;",
        NULL
    };
    int ret;

     
    
    elog(LOG, "[pg_backup_compliance] executing views creation commands");
    for (int i = 0; cmds[i] != NULL; i++)
    {
        elog(LOG, "Executing SQL: %s", cmds[i]);

        ret = SPI_execute(cmds[i], false, 0);
        if (ret != SPI_OK_UTILITY)
            elog(WARNING, "[pg_backup_compliance] SPI_execute failed: %s", cmds[i]);
    }

    elog(LOG, "[pg_backup_compliance] views created or already existed");
}


static void
pgaud_bgw_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;

    got_sigterm = true;

    if (MyProc)
        SetLatch(&MyProc->procLatch);

    errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *      Set a flag to tell the main loop to reread the config file, and set
 *      our latch to wake it up.
 */
static void pgaud_bgw_sighup(SIGNAL_ARGS) {
    int         save_errno = errno;

    got_sighup = true;

    if (MyProc)
        SetLatch(&MyProc->procLatch);

    errno = save_errno;
}


static bool
pgaud_schema_exists(void)
{
    Oid nsp_oid = get_namespace_oid("public", true);

    if (!OidIsValid(nsp_oid))
        return false;

    return SearchSysCacheExists2(
        RELNAMENSP,
        CStringGetDatum("backup_operations_log"),
        ObjectIdGetDatum(nsp_oid)
    );
}


void
pgaud_worker_main(Datum arg)
{
    int rc;
    PGAudJob job;
    bool schema_initialized = false;

    pqsignal(SIGHUP, pgaud_bgw_sighup);
    pqsignal(SIGTERM, pgaud_bgw_sigterm);

    BackgroundWorkerUnblockSignals();


    /* ---- CONNECT TO BACKUP DATABASE ---- */
    BackgroundWorkerInitializeConnection(BACKUP_DATABASE, NULL, 0);
    elog(LOG, "[pg_backup_compliance] connected to backupcompliance");

    for (;;)
    {
        /* ---- WAIT FOR LATCH / SHUTDOWN ---- */
        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                       PGAUD_WORKER_POLL_MS,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();

        if (rc & WL_POSTMASTER_DEATH || got_sigterm)
        {
            elog(LOG, "[pg_backup_compliance] BGWorker received shutdown signal, exiting");
            proc_exit(0);
        }

        /* ---- SCHEMA INIT ---- */
        if (!schema_initialized)
        {
            PG_TRY();
            {
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                CHECK_FOR_INTERRUPTS();

                if (!pgaud_schema_exists())
                {
                    elog(LOG, "[pg_backup_compliance] initializing schema");

                    if (SPI_connect() == SPI_OK_CONNECT)
                    {
                        pgaud_run_schema();  // function does SPI_execute internally
                        CHECK_FOR_INTERRUPTS();
                        pgaud_run_views_schema(); // create views
                        SPI_finish();
                    }
                    else
                    {
                        elog(WARNING, "[pg_backup_compliance] SPI_connect failed during schema init");
                    }

                    elog(LOG, "[pg_backup_compliance] schema initialization done");
                }

                PopActiveSnapshot();
                CommitTransactionCommand();
                schema_initialized = true;
            }
            PG_CATCH();
            {
                AbortCurrentTransaction();
                PopActiveSnapshot();
                FlushErrorState();
                elog(WARNING, "[pg_backup_compliance] Schema init failed, will retry next loop");
            }
            PG_END_TRY();
        }

        /* ---- PROCESS JOBS ---- */
        while (pgaud_dequeue_job(&job))
        {
            PG_TRY();
            {
                StartTransactionCommand();
                PushActiveSnapshot(GetTransactionSnapshot());
                CHECK_FOR_INTERRUPTS();
                elog(LOG, "[pg_backup_compliance] Worker dequeued job of type=%d for pid=%d",
                     job.job_type, job.pid);
                switch (job.job_type)
                {
                    case PGAUD_JOB_BACKUP_LOG:
                        elog(LOG, "[pg_backup_compliance] Worker processing backup log for pid=%d",
                             job.pid);

                        if (SPI_connect() == SPI_OK_CONNECT)
                        {
                            pgaud_handle_backup_log(&job); // now safe to execute SPI
                            SPI_finish();
                        }
                        else
                        {
                            elog(WARNING, "[pg_backup_compliance] SPI_connect failed for backup log pid=%d",
                                 job.pid);
                        }
                        break;

                    case PGAUD_JOB_SCHEMA_CREATE:
                        elog(LOG, "[pg_backup_compliance] Worker running schema creation job");

                        if (SPI_connect() == SPI_OK_CONNECT)
                        {
                            pgaud_run_schema();
                            SPI_finish();
                        }
                        else
                        {
                            elog(WARNING, "[pg_backup_compliance] SPI_connect failed during schema creation job");
                        }
                        break;

                    default:
                        elog(WARNING, "[pg_backup_compliance] Unknown job type %d", job.job_type);
                        break;
                }

                PopActiveSnapshot();
                CommitTransactionCommand();
            }
            PG_CATCH();
            {
                AbortCurrentTransaction();
                PopActiveSnapshot();
                FlushErrorState();
                elog(WARNING, "[pg_backup_compliance] Job failed, will continue next job");
            }
            PG_END_TRY();

            CHECK_FOR_INTERRUPTS();
            if (got_sigterm)
            {
                elog(LOG, "[pg_backup_compliance] BGWorker received shutdown signal during job, exiting");
                proc_exit(0);
            }
        }
    }
}



// This function checks if backupcompliance database is available or not
// If not found cretes the datatabase
// Similarly, it creates two tables, backup_operations_log and pgbackrest_logs
// IF tables exists in backupcompliance database, it skips
// We need a global database to keep data.

Datum
pgaud_init(PG_FUNCTION_ARGS)
{
    elog(LOG, "[pg_backup_compliance] pgaud_init() started");

    if (!database_exists("backupcompliance"))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_DATABASE),
                errmsg("backupcompliance database does not exist. Please create it before using this extension.")));
    }else
    {
        elog(LOG, "[pg_backup_compliance] backupcompliance already exists");
    }

    PG_RETURN_VOID();
}

void
_PG_init(void)
{

    BackgroundWorker worker;
    if (!process_shared_preload_libraries_in_progress)
        return;

    elog(LOG, "[pg_backup_compliance] _PG_init_dump() registering error context");


    prev_emit_log_hook = emit_log_hook;
    emit_log_hook = pgaud_emit_log_hook;

    elog(LOG, "[pg_backup_compliance] _PG_init() called: registering dump hooks");
    _PG_init_dump();   
    
    // Allocating shared memory and lock
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pgaud_shmem_request;
    // RequestNamedLWLockTranche("pgaud_queue_tranche", 1);

    // shared memory startup hook
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pgaud_shmem_startup;
    
    
    // GUC to enable/disable the bgworker logging

    // DefineCustomBoolVariable(
    //     "pg_backup_compliance.enable",
    //     "Enable or disable pg_backup_compliance background worker",
    //     NULL,
    //     &pgaud_enabled,
    //     true,                       /* default */
    //     PGC_SIGHUP,                 /* reloadable */
    //     0,
    //     NULL,
    //     NULL,
    //     NULL
    // );

  
    // Registering BGWorker for deferred logging    
    
    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags =
        BGWORKER_SHMEM_ACCESS |
        BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = BGW_NEVER_RESTART;  /* restart every 5 seconds if crash */
    snprintf(worker.bgw_name, BGW_MAXLEN, "pgaud_log_worker");
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_backup_compliance");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgaud_worker_main");
    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
    elog(LOG, "[pg_backup_compliance] Background worker registered successfully");

    elog(LOG, "[pg_backup_compliance] _PG_init_dump() registering error context");

}

//  _PG_fini: cleanup, unregister hooks.
void
_PG_fini(void)
{
    elog(LOG, "[pg_backup_compliance] _PG_fini() called: unloading dump hooks");
    _PG_fini_dump();
   
}


