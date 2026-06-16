/*-------------------------------------------------------------------------
 *
 * pg_backup_compliance_capture.c
 *    Per-backend capture hooks: ClientAuthentication, ProcessUtility,
 *    ExecutorRun, ExecutorEnd, emit_log and on_proc_exit.  Sessions
 *    that match pg_backup_compliance.track_apps are recognised here
 *    and forwarded to the shared-memory state declared in
 *    pg_backup_compliance.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, The Regents of the University of California
 * Copyright (c) 2024-2026, KloudDB.
 *
 * IDENTIFICATION
 *    pg_backup_compliance_capture.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "pg_backup_compliance.h"

/* Previous hooks. */
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;
static ProcessUtility_hook_type prev_process_utility_hook = NULL;
static ExecutorRun_hook_type    prev_executor_run_hook = NULL;
static ExecutorEnd_hook_type    prev_executor_end_hook = NULL;
static emit_log_hook_type       prev_emit_log_hook = NULL;

/* Per-backend state: the resolved shared-memory entry is cached in my_entry. */
static pgbcEntry *my_entry = NULL;
static bool       my_entry_lookup_done = false;
static bool       my_proc_exit_registered = false;
static int        emit_log_in_progress = 0;

/* Forward declarations. */
static void pgbc_capture_client_auth(Port *port, int status);
static void pgbc_capture_process_utility(PlannedStmt *pstmt,
                                         const char *queryString,
                                         bool readOnlyTree,
                                         ProcessUtilityContext context,
                                         ParamListInfo params,
                                         QueryEnvironment *queryEnv,
                                         DestReceiver *dest,
                                         QueryCompletion *qc);
static void pgbc_capture_executor_run(QueryDesc *queryDesc,
                                      ScanDirection direction,
                                      uint64 count
#if PG_VERSION_NUM < 180000
                                      , bool execute_once
#endif
);
static void pgbc_capture_executor_end(QueryDesc *queryDesc);
static void pgbc_capture_emit_log(ErrorData *edata);
static void pgbc_proc_exit(int code, Datum arg);
static void pgbc_ensure_my_entry(const char *appname,
                                 const char *backup_type,
                                 bool is_walsender,
                                 Port *port_for_init);

/* Public lifecycle. */

void
pgbc_capture_init(void)
{
    prev_client_auth_hook = ClientAuthentication_hook;
    ClientAuthentication_hook = pgbc_capture_client_auth;

    prev_process_utility_hook = ProcessUtility_hook;
    ProcessUtility_hook = pgbc_capture_process_utility;

    prev_executor_run_hook = ExecutorRun_hook;
    ExecutorRun_hook = pgbc_capture_executor_run;

    prev_executor_end_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = pgbc_capture_executor_end;

    prev_emit_log_hook = emit_log_hook;
    emit_log_hook = pgbc_capture_emit_log;
}

void
pgbc_capture_fini(void)
{
    ClientAuthentication_hook = prev_client_auth_hook;
    ProcessUtility_hook = prev_process_utility_hook;
    ExecutorRun_hook = prev_executor_run_hook;
    ExecutorEnd_hook = prev_executor_end_hook;
    emit_log_hook = prev_emit_log_hook;
}

/* Helpers. */

static const char *
pgbc_client_addr_str(Port *port)
{
    static char buf[PGBC_ADDR_LEN];

    if (port == NULL)
        return "";

    if (port->raddr.addr.ss_family == AF_UNIX)
        return "[local]";

    if (port->remote_host && port->remote_host[0])
    {
        strlcpy(buf, port->remote_host, sizeof(buf));
        return buf;
    }
    return "";
}

static bool
pgbc_current_backend_is_walsender(void)
{
#if PG_VERSION_NUM >= 150000
    return (MyBackendType == B_WAL_SENDER);
#else
    return am_walsender;
#endif
}

/*
 * Effective application_name for the current backend, or NULL.  Prefer the
 * GUC over the port value so later SET application_name (e.g. pgBackRest) wins.
 */
static const char *
pgbc_effective_appname(void)
{
    if (application_name != NULL && application_name[0] != '\0')
        return application_name;
    if (MyProcPort != NULL && MyProcPort->application_name != NULL &&
        MyProcPort->application_name[0] != '\0')
        return MyProcPort->application_name;
    return NULL;
}

/*
 * Late-binding capture: attach an entry if the current application_name now
 * matches the tracked-apps list.  Called from every hook to catch mid-session
 * application_name changes.
 */
static void
pgbc_recheck_and_attach(void)
{
    const char *appname;
    char        type[PGBC_TYPE_LEN];

    if (!pgbc_enabled)
        return;
    if (my_entry != NULL)
        return;

    appname = pgbc_effective_appname();
    if (appname == NULL)
        return;
    if (!pgbc_app_is_tracked(appname, type, sizeof(type)))
        return;

    pgbc_ensure_my_entry(appname, type,
                         pgbc_current_backend_is_walsender(),
                         MyProcPort);
}

static void
pgbc_ensure_my_entry(const char *appname,
                     const char *backup_type,
                     bool is_walsender,
                     Port *port_for_init)
{
    if (!pgbc_enabled)
        return;
    if (my_entry != NULL)
        return;
    if (my_entry_lookup_done && my_entry == NULL)
    {
        /* Only retry when a later hook supplied an application_name. */
        if (appname == NULL || appname[0] == '\0')
            return;
        my_entry_lookup_done = false;
    }

    {
        const char *dbname = (port_for_init && port_for_init->database_name)
            ? port_for_init->database_name : NULL;
        const char *username = (port_for_init && port_for_init->user_name)
            ? port_for_init->user_name : NULL;
        const char *client_addr = pgbc_client_addr_str(port_for_init);

        /* Fold multi-connection operations into one entry; else track alone. */
        if (appname != NULL)
            my_entry = pgbc_attach_aggregate_entry(appname, dbname, username,
                                                   client_addr, backup_type,
                                                   is_walsender);

        if (my_entry == NULL)
        {
            my_entry = pgbc_lookup_or_create_entry(MyProcPid, MyStartTimestamp,
                                                   true);
            if (my_entry != NULL)
                pgbc_entry_mark_running(my_entry, appname, dbname, username,
                                        client_addr, backup_type, is_walsender,
                                        MyStartTimestamp);
        }
    }

    my_entry_lookup_done = true;

    if (my_entry == NULL)
        return;

    if (!my_proc_exit_registered)
    {
        /* before_shmem_exit: MyWalSnd is still valid for data-transfer detection. */
        before_shmem_exit(pgbc_proc_exit, (Datum) 0);
        my_proc_exit_registered = true;
    }
}

/* ClientAuthentication_hook. */

static void
pgbc_capture_client_auth(Port *port, int status)
{
    const char *appname;
    char        backup_type[PGBC_TYPE_LEN];
    bool        is_tracked;

    if (prev_client_auth_hook)
        prev_client_auth_hook(port, status);

    if (!pgbc_enabled || port == NULL)
        return;

    appname = (port->application_name && port->application_name[0])
                ? port->application_name : NULL;

    /*
     * Track by application_name match only; there is intentionally no generic
     * walsender fallback, which would record every replication peer.
     */
    is_tracked = (appname && pgbc_app_is_tracked(appname, backup_type,
                                                 sizeof(backup_type)));

    if (!is_tracked)
        return;

    /* Failed auth never reaches ProcessUtility/ExecutorEnd; record it now. */
    if (status != STATUS_OK)
    {
        pgbcEntry  *e;

        e = pgbc_lookup_or_create_entry(MyProcPid,
                                        MyStartTimestamp != 0
                                            ? MyStartTimestamp
                                            : GetCurrentTimestamp(),
                                        true);
        if (e == NULL)
            return;

        pgbc_entry_mark_running(e,
                                appname ? appname : "(unknown)",
                                port->database_name,
                                port->user_name,
                                pgbc_client_addr_str(port),
                                backup_type,
                                am_walsender,
                                GetCurrentTimestamp());
        pgbc_entry_record_error(e,
                                "client authentication failed or "
                                "connection rejected");
        pgbc_entry_finalize(e, PGBC_STATUS_AUTH_FAILED, 1,
                            GetCurrentTimestamp());
        return;
    }

    pgbc_ensure_my_entry(appname ? appname : "(unknown)",
                         backup_type, am_walsender, port);
}

/* ProcessUtility_hook. */

static void
pgbc_capture_process_utility(PlannedStmt *pstmt,
                             const char *queryString,
                             bool readOnlyTree,
                             ProcessUtilityContext context,
                             ParamListInfo params,
                             QueryEnvironment *queryEnv,
                             DestReceiver *dest,
                             QueryCompletion *qc)
{
    /* Pre-utility check: catches sessions identified at connect time. */
    pgbc_recheck_and_attach();

    if (prev_process_utility_hook)
        prev_process_utility_hook(pstmt, queryString, readOnlyTree,
                                  context, params, queryEnv, dest, qc);
    else
        standard_ProcessUtility(pstmt, queryString, readOnlyTree,
                                context, params, queryEnv, dest, qc);

    /* Post-utility re-check: catches SET application_name (notably pgBackRest). */
    pgbc_recheck_and_attach();
}

/* ExecutorRun + ExecutorEnd hooks. */

static void
pgbc_capture_executor_run(QueryDesc *queryDesc,
                          ScanDirection direction,
                          uint64 count
#if PG_VERSION_NUM < 180000
                          , bool execute_once
#endif
)
{
    /* Cheap no-op once attached; catches late SET application_name. */
    pgbc_recheck_and_attach();

    if (prev_executor_run_hook)
        prev_executor_run_hook(queryDesc, direction, count
#if PG_VERSION_NUM < 180000
                               , execute_once
#endif
        );
    else
        standard_ExecutorRun(queryDesc, direction, count
#if PG_VERSION_NUM < 180000
                             , execute_once
#endif
        );
}

static void
pgbc_capture_executor_end(QueryDesc *queryDesc)
{
    /* Re-check application_name in case it was set by the statement. */
    pgbc_recheck_and_attach();

    if (prev_executor_end_hook)
        prev_executor_end_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/* emit_log_hook. */

static void
pgbc_capture_emit_log(ErrorData *edata)
{
    if (prev_emit_log_hook)
        prev_emit_log_hook(edata);

    if (!pgbc_enabled || my_entry == NULL || edata == NULL)
        return;
    if (edata->elevel < WARNING)
        return;

    /* Guard against re-entry if our own code logs anything. */
    if (emit_log_in_progress)
        return;
    emit_log_in_progress++;

    if (edata->message)
        pgbc_entry_record_error(my_entry, edata->message);

    /* Backup aborted before pg_backup_stop(); match funcname (locale-safe). */
    if (edata->funcname &&
        strcmp(edata->funcname, "do_pg_abort_backup") == 0)
        pgbc_entry_mark_aborted(my_entry);

    emit_log_in_progress--;
}

/* on_proc_exit callback: finalises this backend's entry. */

static void
pgbc_proc_exit(int code, Datum arg)
{
    pgbcStatus  status;
    char        autogenmsg[64];

    if (my_entry == NULL)
        return;

    /* Mark data sent if this walsender streamed WAL or ran a base backup. */
    if (MyWalSnd != NULL)
    {
        XLogRecPtr  sentPtr;
        WalSndState wstate;

        SpinLockAcquire(&MyWalSnd->mutex);
        sentPtr = MyWalSnd->sentPtr;
        wstate = MyWalSnd->state;
        SpinLockRelease(&MyWalSnd->mutex);

        if (sentPtr != 0 || wstate == WALSNDSTATE_BACKUP)
        {
            SpinLockAcquire(&my_entry->mutex);
            my_entry->data_sent = true;
            SpinLockRelease(&my_entry->mutex);
        }
    }

    if (code == 0)
        status = PGBC_STATUS_SUCCEEDED;
    else
        status = PGBC_STATUS_FAILED;

    if (status == PGBC_STATUS_FAILED && my_entry->error_message[0] == '\0')
    {
        snprintf(autogenmsg, sizeof(autogenmsg),
                 "backend exited with code %d", code);
        pgbc_entry_record_error(my_entry, autogenmsg);
    }

    pgbc_entry_finalize(my_entry, status, code, GetCurrentTimestamp());
}
