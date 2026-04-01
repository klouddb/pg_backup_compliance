// pg_backup_compliance_dump.c - C-code to detect pg_dump sessions

/* ----------- pg_aud ---------------------*/

// Here we are using ProcessUtility_hook to detect pg_dump sessions
// and log them to auditdb.backup_operations_log table after the backend exits.
// As pg_dump uses utility commands to perform its operations,

/* ----------- pg_basebackup --------------*/

// Here we are using ClientAuthentication_hook to detect pg_basebackup sessions.
// For (FATAL) error cases (authentication failures), we log immediately from the hook.
// For successful connections, we schedule logging at backend exit using atexit().


// pg_backup_compliance_dump.c — Detect pg_dump/pg_basebackup sessions and authentication failures

#include "postgres.h"
#include "fmgr.h"
#include "access/xact.h"    /* Required for Transaction Status */
#include "tcop/tcopprot.h"  /* Required for QueryCancelPending */
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
#include "utils/elog.h"
#include "utils/memutils.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "access/transam.h"     /* TransactionIdIsValid if needed */
#include "storage/ipc.h"
#include "funcapi.h"
#include "nodes/pg_list.h"
#include "libpq/libpq-be.h"

#include "executor/executor.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "tcop/dest.h" /* Required for DestNone */
#include "pgstat.h"

/* * In Postgres 17, the 'kind' field in WalSnd is of type ReplicationKind.
 * We check if it is a physical replication kind being used for backup.
 */
#if PG_VERSION_NUM >= 150000
    /* * In PG17, we check:
     * 1. Is it a physical walsender?
     * 2. Is the state specifically WALSNDSTATE_BACKUP?
     */
    #define IS_BASEBACKUP_STREAMING() \
        (am_walsender && MyWalSnd != NULL && \
         MyWalSnd->kind == REPLICATION_KIND_PHYSICAL && \
         MyWalSnd->state == WALSNDSTATE_BACKUP)
#elif PG_VERSION_NUM >= 130000
    #define IS_BASEBACKUP_STREAMING() \
        (am_walsender && MyWalSnd != NULL && \
         MyWalSnd->kind == WALSNDKIND_BASEBACKUP)
#else
    #define IS_BASEBACKUP_STREAMING() (false)
#endif

#if PG_VERSION_NUM >= 150000
    /* * In PG15-17+:
     * 1. Kind must be Physical.
     * 2. State must be WALSNDSTATE_STREAMING.
     */
    #define IS_WAL_STREAMING() \
        (am_walsender && MyWalSnd != NULL && \
         MyWalSnd->kind == REPLICATION_KIND_PHYSICAL && \
         MyWalSnd->state == WALSNDSTATE_STREAMING)
#elif PG_VERSION_NUM >= 130000
    /* * In PG13-14:
     * We check for the specific WALSNDKIND_WALSENDER type.
     */
    #define IS_WAL_STREAMING() \
        (am_walsender && MyWalSnd != NULL && \
         MyWalSnd->kind == WALSNDKIND_WALSENDER)
#else
    #define IS_WAL_STREAMING() (false)
#endif


extern char *get_database_name(Oid dbid);
extern Oid get_database_oid(const char *dbname, bool missing_ok);
extern Oid get_role_oid(const char *rolename, bool missing_ok);

static ProcessUtility_hook_type prev_process_utility_hook = NULL;
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;

static bool scheduled_for_logging = false;
static bool scheduled_for_basebackup_logging = false;


static char app_name_copy[128];
static char db_name_copy[NAMEDATALEN];
static int pid_copy = 0;
static char start_time_copy[64];
// static char end_time_copy[64];
static char error_msg_copy[256] = "";


extern void _PG_init_dump(void);   
extern void _PG_fini_dump(void);
extern PGDLLIMPORT const char *debug_query_string;


extern void insert_into_log_immediate(const char *app, const char *db, int pid,
                          const char *start_time, const char *end_time, const char *err);


static bool is_data_worker = false;
static bool lsn_moved = false;


static void
check_backup_completed(int code, Datum arg)
{
    if (am_walsender && MyWalSnd != NULL)
    {
        XLogRecPtr  sentPtr;
        WalSndState state;

        SpinLockAcquire(&MyWalSnd->mutex);
        sentPtr = MyWalSnd->sentPtr;
        state = MyWalSnd->state;
        SpinLockRelease(&MyWalSnd->mutex);

        /* If sentPtr > 0, this connection moved data/WAL */
        if (sentPtr > 0)
        {
            elog(LOG, "[pg_backup_compliance_dump] Backup completed with LSN moved");
            lsn_moved = true;
        }

        /* Identify if this is the Data Worker (State 1: BACKUP) */
        if (state == WALSNDSTATE_BACKUP)
        {
            elog(LOG, "[pg_backup_compliance_dump] Backup session identified as Data Worker");
            is_data_worker = true;
        }
    }
}


static void
log_backup_event_basebackup(int code, Datum arg)
{
    const char *end_time = timestamptz_to_str(GetCurrentTimestamp());


    if (strncmp(app_name_copy, "pg_basebackup", 13) == 0)
    {
        /* --- SUCCESS PATH --- */
        if (is_data_worker && code == 0)
        {
            //  This captures:
            // 1. The Data Worker in -X fetch (PID 90090)
            // 2. The Main Connection in -X none (where it moves its own LSN)
            
            insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                      start_time_copy, end_time, "");
            return;
        }else if(is_data_worker && code!=0){
            //  Captured: Main Connection with LSN moved.
            // This happens in -X none mode where main connection moves LSN.
            // char err_msg_local[1024];
            switch(code){
                case 1:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 1(Termination/Fatal error)");
                break;
                case 2:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 2(Signal crash)");
                break;
                case 3:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 3(Thread/Process failure)");
                break; 
                default:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code %d", code);
            }
            insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                      start_time_copy, end_time, error_msg_copy);
            return;
        }


        /* --- FAILURE/FALLBACK PATH (Main Connection) --- */
        if (!is_data_worker)
        {
            if (code != 0)
            {
                switch(code){
                case 1:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 1(Termination/Fatal error)");
                break;
                case 2:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 2(Signal crash)");
                break;
                case 3:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 3(Thread/Process failure)");
                break; 
                default:
                    snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code %d", code);
            }
                // Captured: Ctrl+C or process crash 
                insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                          start_time_copy, end_time,error_msg_copy);
            }
            else if (code == 0 && !lsn_moved)
            {
                //  Captured: Directory Not Empty error.
                // Exit code is 0, but no LSN was ever moved.
                insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                          start_time_copy, end_time, "FAILED: some error (e.g. client-side error)");
            }
        }
    }
    else{
        // elog(LOG, "[pg_backup_compliance_dump] log_backup_event_basebackup called for non-pg_basebackup app: %s", app_name_copy);
        // elog(LOG, "[pg_backup_compliance_dump] code=%d", code);

        if (strncmp(app_name_copy, "pg_dump", 7) == 0)
        {
           
            if(code != 0){
                switch(code){
                    case 1:
                        snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 1(Termination/Fatal error)");
                    break;
                    case 2:
                        snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 2(Signal crash)");
                    break;
                    case 3:
                        snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code 3(Thread/Process failure)");
                    break; 
                    default:
                        snprintf(error_msg_copy, sizeof(error_msg_copy), "Failed: Aborted with code %d", code);
                }
                insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                    start_time_copy, end_time, error_msg_copy);
            }
            else{
                insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                    start_time_copy, end_time, error_msg_copy);
            }
        }
        else{
                insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy, 
                                    start_time_copy, end_time, error_msg_copy);
        }
    }
}

/* ---------------- Process Utility Hook ---------------- */
static void
pgaud_process_utility_hook(PlannedStmt *pstmt,
                           const char *queryString,
                           bool readOnlyTree,
                           ProcessUtilityContext context,
                           ParamListInfo params,
                           QueryEnvironment *queryEnv,
                           DestReceiver *dest,
                           QueryCompletion *qc)
{
    // Get application name
    const char *app_name = application_name;
    // const char *client_addr = NULL;

    if (app_name == NULL && MyProcPort && MyProcPort->application_name)
        app_name = MyProcPort->application_name;
        
    // if (MyProcPort && MyProcPort->remote_host)
    //     client_addr = MyProcPort->remote_host;

    // if (client_addr && strlen(client_addr) < sizeof(client_addr_copy)){
    //     strncpy(client_addr_copy, client_addr, sizeof(client_addr_copy) - 1);
    //     client_addr_copy[sizeof(client_addr_copy) - 1] = '\0';
    // }
    // If this is NOT a backup client, do nothing except forward to prev/standard 
    // We are avoiding other queries to reduce overhead
    // As it can take resources 
    // Other command can also invoke wrong insertion 
    //and can break system.
    if (!(app_name &&
         (strncmp(app_name, "pg_dump", 7) == 0 ||
          strncmp(app_name, "pg_basebackup", 13) == 0)))
    {
        // Forward the call immediately and return 
        if (prev_process_utility_hook)
            prev_process_utility_hook(pstmt, queryString, readOnlyTree,
                                      context, params, queryEnv, dest, qc);
        else
            standard_ProcessUtility(pstmt, queryString, readOnlyTree,
                                    context, params, queryEnv, dest, qc);
        return;
    }



    
    elog(LOG, "[pg_backup_compliance_dump] In process_utility_hook (backup client detected): app_name=%s", app_name ? app_name : "(null)");

    // Simple routine process_utility hook work starts with
    // checking prev hook and calling it inside PG_TRY
    PG_TRY();
    {
        if (prev_process_utility_hook)
            prev_process_utility_hook(pstmt, queryString, readOnlyTree,
                                      context, params, queryEnv, dest, qc);
        else
            standard_ProcessUtility(pstmt, queryString, readOnlyTree,
                                    context, params, queryEnv, dest, qc);
    }
    PG_CATCH();
    {
       
        ErrorData *edata = CopyErrorData();
        FlushErrorState();

        if (edata && edata->message)
            strncpy(error_msg_copy, edata->message, sizeof(error_msg_copy) - 1);
        else
            strncpy(error_msg_copy, "Unknown internal error", sizeof(error_msg_copy) - 1);

        // elog(WARNING, "[pg_backup_compliance_dump] Caught error during backup session: %s", error_msg_copy);

        

        if (MyProcPid)
            pid_copy = MyProcPid;


        if (!scheduled_for_logging)
        {
            on_proc_exit(log_backup_event_basebackup, (Datum)0);
            scheduled_for_logging = true;
        }

        FreeErrorData(edata);
        PG_RE_THROW();
    }
    PG_END_TRY();

    // If top-level utility and first time, capture some session details for later 
    // Simple entry of the data into static variables
    // and called atexit to log at the end of session
    if (context == PROCESS_UTILITY_TOPLEVEL)
    {
        const char *dbname = NULL;

        if (OidIsValid(MyDatabaseId))
            dbname = get_database_name(MyDatabaseId);

       
        if (!dbname && app_name && strncmp(app_name, "pg_dump", 7) == 0)
            dbname = "postgres";

        if (dbname)
            strncpy(db_name_copy, dbname, sizeof(db_name_copy) - 1);

        if (app_name)
            strncpy(app_name_copy, app_name, sizeof(app_name_copy) - 1);

        {
            const char *start_time = timestamptz_to_str(MyStartTimestamp);
            if (start_time)
                strncpy(start_time_copy, start_time, sizeof(start_time_copy) - 1);
        }

        if (!pid_copy && MyProcPid)
            pid_copy = MyProcPid;

        if (!scheduled_for_logging)
        {
            on_proc_exit(log_backup_event_basebackup, (Datum)0);
            scheduled_for_logging = true;
        }

        elog(LOG, "[pg_backup_compliance_dump] Backup session recorded: app=%s db=%s pid=%d",
             app_name_copy[0] ? app_name_copy : "(unknown)",
             db_name_copy[0] ? db_name_copy : "(unknown)",
             (int) pid_copy);
    }
}
// static void
// pgaud_process_utility_hook(PlannedStmt *pstmt,
//                            const char *queryString,
//                            bool readOnlyTree,
//                            ProcessUtilityContext context,
//                            ParamListInfo params,
//                            QueryEnvironment *queryEnv,
//                            DestReceiver *dest,
//                            QueryCompletion *qc)
// {
//     const char *app_name = application_name;

//     /* 1. Resolve application name from port if GUC is null */
//     if (app_name == NULL && MyProcPort && MyProcPort->application_name)
//         app_name = MyProcPort->application_name;

//     // if(app_name==NULL || app_name[0]=='\0'){
//     //      if (prev_process_utility_hook)
//     //         prev_process_utility_hook(pstmt, queryString, readOnlyTree,
//     //                                   context, params, queryEnv, dest, qc);
//     //     else
//     //         standard_ProcessUtility(pstmt, queryString, readOnlyTree,
//     //                                 context, params, queryEnv, dest, qc);
//     //     elog(LOG, "[pg_backup_compliance_dump] In process_utility_hook: application name is NULL/empty");
//     //     return;
//     // }
//     if (app_name == NULL || 
//         (strncmp(app_name, "pg_dump", 7) != 0 && 
//          strncmp(app_name, "pg_basebackup", 13) != 0))
//     {
//         if (prev_process_utility_hook)
//             prev_process_utility_hook(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
//         else
//             standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
//         return;
//     }
    
//     elog(LOG, "[pg_backup_compliance_dump] In process_utility_hook: app_name=%s", app_name);

//     /* 3. Execution with Error/Cancel Capture */
//     PG_TRY();
//     {
//         if (prev_process_utility_hook)
//             prev_process_utility_hook(pstmt, queryString, readOnlyTree,
//                                       context, params, queryEnv, dest, qc);
//         else
//             standard_ProcessUtility(pstmt, queryString, readOnlyTree,
//                                     context, params, queryEnv, dest, qc);
    
//     }
//     PG_CATCH();
//     {
//         ErrorData *edata = CopyErrorData();
//         FlushErrorState();
//         elog(LOG, "[pg_backup_compliance_dump] Caught error in process_utility_hook during backup session");    
       
//         if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED || QueryCancelPending)
//         {
//             // Mark the sticky flag for the exit loggers =
            
//             strncpy(error_msg_copy, "CANCELLED: User interrupted the backup (Ctrl+C)", sizeof(error_msg_copy) - 1);
//         }
//         else if (edata && edata->message)
//         {
//             // Capture actual database errors (Permission denied, etc.) 
//             strncpy(error_msg_copy, edata->message, sizeof(error_msg_copy) - 1);
//         }
//         else
//         {
//             strncpy(error_msg_copy, "Unknown internal error", sizeof(error_msg_copy) - 1);
//         }

//         elog(WARNING, "[pg_backup_compliance_dump] Caught error/cancel: %s", error_msg_copy);

//         if (MyProcPid)
//             pid_copy = MyProcPid;

//         // Ensure exit hooks are registered even on failure 
//         if (!scheduled_for_logging)
//         {
//             elog(LOG, "[pg_backup_compliance_dump] Scheduling logging at catch-exit for app=%s", app_name);
//             // before_shmem_exit(log_at_shmem_exit, (Datum)0);
//             on_proc_exit(log_backup_event_basebackup, (Datum)0);
//             scheduled_for_logging = true;
//         }

//         FreeErrorData(edata);
        
//         // Re-throw so the backend performs its standard cleanup 
//         PG_RE_THROW();
//     }
//     PG_END_TRY();

    
//     // 4. Session Recording (Only for Top Level) 
//     if (context == PROCESS_UTILITY_TOPLEVEL)
//     {
//         const char *dbname = NULL;

//         if (OidIsValid(MyDatabaseId))
//             dbname = get_database_name(MyDatabaseId);

//         if (!dbname && app_name && strncmp(app_name, "pg_dump", 7) == 0)
//             dbname = "postgres";

//         if (dbname)
//             strncpy(db_name_copy, dbname, sizeof(db_name_copy) - 1);

//         if (app_name)
//             strncpy(app_name_copy, app_name, sizeof(app_name_copy) - 1);

//         {
//             const char *start_time = timestamptz_to_str(MyStartTimestamp);
//             if (start_time)
//                 strncpy(start_time_copy, start_time, sizeof(start_time_copy) - 1);
//         }

//         if (!pid_copy && MyProcPid)
//             pid_copy = MyProcPid;

//         if (!scheduled_for_logging)
//         {
//             elog(LOG, "[pg_backup_compliance_dump] Scheduling logging at exit for app=%s", app_name);
//             // before_shmem_exit(log_at_shmem_exit, (Datum)0);
//             on_proc_exit(log_backup_event_basebackup, (Datum)0);
//             scheduled_for_logging = true;
//         }

//         elog(LOG, "[pg_backup_compliance_dump] Backup session recorded: app=%s db=%s pid=%d",
//              app_name_copy, db_name_copy, (int) pid_copy);
//     }
// }


static void
pgaud_client_auth_hook(Port *port, int status)
{
    bool is_replication_conn=false;
    // bool is_backend_conn = false;
    const char *dbname = NULL;
    const char *user = NULL;

    const char *appname = port->application_name ? port->application_name:NULL;
    if (appname && ((strncmp(appname, "pg_basebackup", 13) != 0)&&(strncmp(appname, "pg_dump", 7) != 0)))return;
    
   
    #if PG_VERSION_NUM >= 150000
        is_replication_conn = (MyBackendType == B_WAL_SENDER);
        // is_backend_conn = (MyBackendType == B_BACKEND);
    #else
        is_replication_conn = port->replication;
    #endif


    elog(LOG, "[pg_backup_compliance_dump] In client_auth_hook for appname=%s (status=%d)",
         appname, status);    

    appname = port->application_name ? port->application_name : "unknown";
    
    /* Filter for backup tools only */
    if (strncmp(appname, "pg_basebackup", 13) != 0 && 
        strncmp(appname, "pg_dump", 7) != 0 &&
        strncmp(appname, "pg_dumpall", 10) != 0)
        return;

    dbname = port->database_name ? port->database_name : NULL;
    user   = port->user_name ? port->user_name : NULL;

    if (strncmp(appname, "pg_dumpall", 10) == 0)
    {
        // TimestampTz now = GetCurrentTimestamp();
        // const char *now_str = timestamptz_to_str(now);
        // int pid = MyProcPid;

        // char cmd[4096];
        // char err[4096] = "";
        TimestampTz now; 
        const char *now_str; // const char *start_t; 
        // char cmd[2048]; 
        // int rc; 
        char err[256] = ""; 
        // int pid = MyProcPid; 
        now = GetCurrentTimestamp();
        now_str = timestamptz_to_str(now);

        if (status != STATUS_OK) { 
            strncpy(err, "FAILED: internal error", sizeof(err) - 1);
            return;
        }
        
    
        insert_into_log_immediate(appname, dbname, MyProcPid,
                                    now_str, now_str,
                                    err);

        return;
    }

    if(status==STATUS_EOF){
        return;
    }

    elog(LOG, "[pg_backup_compliance_dump] In client_auth_hook after auth success");

    if (status != STATUS_OK && strncmp(appname, "pg_basebackup", 13) == 0){
        TimestampTz now = GetCurrentTimestamp();
        const char *now_str = timestamptz_to_str(now);

        elog(LOG, "[pg_backup_compliance_dump] Authentication failed: user=%s db=%s app=%s",
            user, dbname ? dbname : "(null)", appname ? appname : "(null)");



       if (appname && strncmp(appname, "pg_basebackup", 13) == 0)
        {
            if(!is_replication_conn){
                elog(LOG, "[pg_backup_compliance_dump] Skipping non-replication pg_basebackup connection");
                return;
            }
            if(scheduled_for_basebackup_logging){
                elog(LOG, "[pg_backup_compliance_dump] Skipping duplicate pg_basebackup failure log");
                return;
            }
            memset(app_name_copy, 0, sizeof(app_name_copy));
            strncpy(app_name_copy, appname, sizeof(app_name_copy) - 1);

            if (dbname && dbname[0])
                strncpy(db_name_copy, dbname, sizeof(db_name_copy) - 1);
            else
                strncpy(db_name_copy, "", sizeof(db_name_copy) - 1);

            pid_copy = MyProcPid;
            memset(start_time_copy, 0, sizeof(start_time_copy));
            strncpy(start_time_copy, now_str, sizeof(start_time_copy) - 1);

            
            insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy,
                                    start_time_copy, now_str,
                                    "pg_basebackup FAILED: Authentication failed or connection rejected");
            elog(LOG, "[pg_backup_compliance_dump] Inserted failure entry for pg_basebackup (status=%d)", status);
        }
        else
        {
           
            insert_into_log_immediate(appname, dbname, MyProcPid,
                                    now_str, now_str,
                                    "Authentication failed or connection rejected");
        }


        scheduled_for_basebackup_logging=true;

        return;  
    }
 

    if(scheduled_for_basebackup_logging){
        return;
    }

    if ((strncmp(appname, "pg_basebackup", 13) == 0))
    {
        if (dbname == NULL || dbname[0]=='\0')
        {
            TimestampTz start_ts;
            const char *start_time;
            elog(LOG, "[pg_backup_compliance_dump] Basebackup main control connection detected (dbname=%s)",dbname);

            memset(app_name_copy, 0, sizeof(app_name_copy));
            strncpy(app_name_copy, appname, sizeof(app_name_copy) - 1);

            memset(db_name_copy, 0, sizeof(db_name_copy));
            // strncpy(db_name_copy, dbname, sizeof(db_name_copy) - 1);

            pid_copy = MyProcPid;

            start_ts = GetCurrentTimestamp();
            start_time= timestamptz_to_str(start_ts);
            memset(start_time_copy, 0, sizeof(start_time_copy));
            strncpy(start_time_copy, start_time, sizeof(start_time_copy) - 1);
            // we assume failure unless proven otherwise
            // strlcpy(error_msg_copy, "FAILED: Client-side error (e.g. directory exists or connection lost)", 256);
            
            before_shmem_exit(check_backup_completed, 0);
            on_proc_exit(log_backup_event_basebackup, (Datum)0);
            // before_shmem_exit(pgaud_basebackup_exit_hook, 0);
            scheduled_for_basebackup_logging = true;
    
            elog(LOG, "[pg_backup_compliance_dump] Backup session scheduled for logging: pid=%d", pid_copy);
        }
        else
        {
            elog(LOG, "[pg_backup_compliance_dump] Skipping secondary connection (dbname=%s)", dbname);
        }
    }

    elog(LOG, "[pg_backup_compliance_dump] Authentication OK: user=%s db=%s app=%s",user, dbname, appname);

    return;
}

void
_PG_init_dump(void)
{   
    /* Push it on top of the stack */
    // session_was_interrupted = false;
    elog(LOG, "[pg_backup_compliance_dump] extension initialized");
    prev_client_auth_hook = ClientAuthentication_hook;
    ClientAuthentication_hook = pgaud_client_auth_hook;

    prev_process_utility_hook = ProcessUtility_hook;
    ProcessUtility_hook = pgaud_process_utility_hook;


}

void
_PG_fini_dump(void)
{
    elog(LOG, "[pg_backup_compliance_dump] extension unloaded");
    ProcessUtility_hook = prev_process_utility_hook;
    ClientAuthentication_hook = prev_client_auth_hook;
}
