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
static char client_addr_copy[INET_ADDRSTRLEN] = "";

extern void _PG_init_dump(void);   
extern void _PG_fini_dump(void);


extern void insert_into_log_immediate(const char *app, const char *db, int pid,
                          const char *start_time, const char *end_time, const char *err);


static void
log_backup_event_basebackup(int code ,Datum arg)
{
    TimestampTz end_ts;
    const char *end_time;
    elog(LOG, "[pg_backup_compliance_dump] In log_backup_event atexit handler");
    if (strlen(app_name_copy) == 0)
        return;

    end_ts = GetCurrentTimestamp();
    end_time = timestamptz_to_str(end_ts);

    insert_into_log_immediate(app_name_copy, db_name_copy, pid_copy,
                              start_time_copy, end_time, error_msg_copy);

    scheduled_for_logging = false;  /* <— reset for next session */
    scheduled_for_basebackup_logging = false;
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
    const char *client_addr = NULL;

    if (app_name == NULL && MyProcPort && MyProcPort->application_name)
        app_name = MyProcPort->application_name;
        
    if (MyProcPort && MyProcPort->remote_host)
        client_addr = MyProcPort->remote_host;

    if (client_addr && strlen(client_addr) < sizeof(client_addr_copy)){
        strncpy(client_addr_copy, client_addr, sizeof(client_addr_copy) - 1);
        client_addr_copy[sizeof(client_addr_copy) - 1] = '\0';
    }
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

        elog(WARNING, "[pg_backup_compliance_dump] Caught error during backup session: %s", error_msg_copy);

        

        if (MyProcPid)
            pid_copy = MyProcPid;


        if (!scheduled_for_logging)
        {

            on_proc_exit(log_backup_event_basebackup, (Datum)0);
            scheduled_for_logging = true;
        }

        FreeErrorData(edata);
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


static void
pgaud_client_auth_hook(Port *port, int status)
{
    bool is_replication_conn=false;
    // bool is_backend_conn = false;
    const char *dbname = NULL;
    const char *user = NULL;

    const char *appname = port->application_name ? port->application_name : "unknown";
    if (appname && ((strncmp(appname, "pg_basebackup", 13) != 0)&&(strncmp(appname, "pg_dump", 7) != 0)))return;
    
   
    #if PG_VERSION_NUM >= 150000
        is_replication_conn = (MyBackendType == B_WAL_SENDER);
        // is_backend_conn = (MyBackendType == B_BACKEND);
    #else
        is_replication_conn = port->replication;
    #endif

    // if (client_addr && strlen(client_addr) < sizeof(client_addr_copy)){
    //     strncpy(client_addr_copy, client_addr, sizeof(client_addr_copy) - 1);
    //     client_addr_copy[sizeof(client_addr_copy) - 1] = '\0';
    // }

    elog(LOG, "[pg_backup_compliance_dump] In client_auth_hook for appname=%s (status=%d)",
         appname, status);
    
    
    elog(LOG, "[pg_backup_compliance_dump] checked this is a wal writer process =%s", appname);

    dbname = port->database_name ? port->database_name : NULL;
    user   = port->user_name ? port->user_name : "unknown";


    elog(LOG, "[pg_backup_compliance_dump] In client_auth_hook (status=%d)", status);
 
    if(status==STATUS_EOF){
        return;
    }

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
            strncpy(err, "error", sizeof(err) - 1);
            return;
        }
        
    
        insert_into_log_immediate(appname, dbname, MyProcPid,
                                    now_str, now_str,
                                    err);

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
                                    "pg_basebackup FAILED (authentication error)");
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
            elog(LOG, "[pg_backup_compliance_dump] Basebackup main control connection detected (dbname=NULL)");

            memset(app_name_copy, 0, sizeof(app_name_copy));
            strncpy(app_name_copy, appname, sizeof(app_name_copy) - 1);

            memset(db_name_copy, 0, sizeof(db_name_copy));
            // strncpy(db_name_copy, dbname, sizeof(db_name_copy) - 1);

            pid_copy = MyProcPid;

            start_ts = GetCurrentTimestamp();
            start_time= timestamptz_to_str(start_ts);
            memset(start_time_copy, 0, sizeof(start_time_copy));
            strncpy(start_time_copy, start_time, sizeof(start_time_copy) - 1);
            
       
            on_proc_exit(log_backup_event_basebackup, (Datum)0);
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
