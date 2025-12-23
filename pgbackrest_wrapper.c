

// /* -----------------------------REQUIREMENT------------------------------
//      This wrappers require a database `auditdb` with table `pgbackrest_logs`
//      to be created beforehand.
//      The actual extension `pgaud` creates the database and table if not exists,
//      but for the wrapper we assume it is already created.
    
//      In any case not present,  create it using the following SQL commands: 
//         CREATE DATABASE auditdb;        
//         \c auditdb; 
//         CREATE TABLE pgbackrest_logs (
//             id SERIAL PRIMARY KEY,
//             method TEXT NOT NULL,
//             command TEXT NOT NULL,
//             exit_code INT NOT NULL,
//             error TEXT,
//             start_time TIMESTAMP NOT NULL DEFAULT NOW(),
//             end_time TIMESTAMP,
//             duration INTERVAL,
//             pid INT NOT NULL
//         );
// */



// // #include <stdio.h>
// // #include <stdlib.h>
// // #include <string.h>
// // #include <time.h>
// // #include <unistd.h>
// // #include <sys/wait.h>

// // #define LOGFILE "/var/log/pgbackrest_wrapper_1.log"
// // #define REAL_PGBACKREST "/usr/bin/pgbackrest"

// // // Timestamp function
// // void get_timestamp(char *buffer, size_t len)
// // {
// //     time_t now = time(NULL);
// //     struct tm *t = localtime(&now);
// //     strftime(buffer, len, "%Y-%m-%d %H:%M:%S", t);
// // }

// // // Insert log using psql command
// // void insert_into_pg(const char *command, int exit_code)
// // {
// //     char timestamp[64];
// //     get_timestamp(timestamp, sizeof(timestamp));

// //     // Sanitize command for SQL
// //     char cmd_escaped[4096];
// //     int j = 0;
// //     for (int i = 0; command[i] != '\0' && j < 4090; i++)
// //     {
// //         if (command[i] == '\'') { // escape single quotes
// //             cmd_escaped[j++] = '\'';
// //             cmd_escaped[j++] = '\'';
// //         } else {
// //             cmd_escaped[j++] = command[i];
// //         }
// //     }
// //     cmd_escaped[j] = '\0';

// //     char sql[6000];
// //     snprintf(sql, sizeof(sql),
// //         "psql -d postgres -U postgres -c "
// //         "\"INSERT INTO pgbackrest_logs(command, exit_code, ts) "
// //         "VALUES('%s', %d, '%s');\"",
// //         cmd_escaped, exit_code, timestamp);

// //     // Execute SQL insert (no output needed)
// //     FILE *fp = popen(sql, "r");
// //     if (fp) pclose(fp);
// // }

// // int main(int argc, char *argv[])
// // {
// //     // Prevent recursion
// //     if (getenv("WRAPPER_ACTIVE"))
// //     {
// //         execv(REAL_PGBACKREST, argv);
// //         perror("execv failed");
// //         return 1;
// //     }
// //     setenv("WRAPPER_ACTIVE", "1", 1);

// //     // Build command
// //     char cmd[4096] = {0};
// //     strcat(cmd, REAL_PGBACKREST);
// //     for (int i = 1; i < argc; i++)
// //     {
// //         strcat(cmd, " ");
// //         strcat(cmd, argv[i]);
// //     }

// //     // Open log
// //     FILE *log = fopen(LOGFILE, "a");
// //     if (!log)
// //     {
// //         perror("fopen");
// //         return 1;
// //     }

// //     char timestamp[64];
// //     get_timestamp(timestamp, sizeof(timestamp));

// //     fprintf(log, "--------------------------------------------------\n");
// //     fprintf(log, "%s - pgbackrest called:\nCommand: %s\n", timestamp, cmd);
// //     fflush(log);

// //     // Execute pgBackRest
// //     FILE *fp = popen(cmd, "r");
// //     if (!fp)
// //     {
// //         fprintf(log, "ERROR: failed to run pgbackrest\n");
// //         fclose(log);
// //         return 1;
// //     }

// //     fprintf(log, "---- BEGIN OUTPUT ----\n");

// //     char buffer[2048];
// //     while (fgets(buffer, sizeof(buffer), fp))
// //     {
// //         fprintf(log, "%s", buffer);
// //         fflush(log);
// //     }

// //     int exit_code = pclose(fp);
// //     if (WIFEXITED(exit_code))
// //         exit_code = WEXITSTATUS(exit_code);

// //     fprintf(log, "---- END OUTPUT ----\nExit Code: %d\n", exit_code);
// //     fprintf(log, "--------------------------------------------------\n");
// //     fflush(log);
// //     fclose(log);

// //     // Insert into PostgreSQL table
// //     insert_into_pg(cmd, exit_code);

// //     return exit_code;
// // }

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <time.h>
// #include <unistd.h>
// #include <sys/wait.h>
// #include <unistd.h>
// #include <stdbool.h>
// #include <sys/stat.h>

// #define PGBACKREST_WRAPPER_VERSION "1.0.0"

// #define LOGFILE "/var/log/pgbackrest_wrapper_1.log"
// // #define REAL_PGBACKREST "/usr/bin/pgbackrest"
// static char pgbackrest_path[512];


// char *find_real_pgbackrest_by_inode(void)
// {
//     struct stat self_st;
//     if (stat("/proc/self/exe", &self_st) != 0)
//         return NULL;

//     FILE *fp = popen("which -a pgbackrest", "r");
//     if (!fp)
//         return NULL;

    
//     while (fgets(pgbackrest_path, sizeof(pgbackrest_path), fp))
//     {
//         pgbackrest_path[strcspn(pgbackrest_path, "\n")] = 0;

//         struct stat st;
//         if (stat(pgbackrest_path, &st) != 0)
//             continue;

//         // Compare inode + device
//         if (st.st_ino != self_st.st_ino || st.st_dev != self_st.st_dev)
//         {
//             pclose(fp);
//             return pgbackrest_path;   // this is NOT us → real binary
//         }
//     }
//     printf("path = %s\n", pgbackrest_path);
//     pclose(fp);
//     return NULL;
// }

// char psql_path[1024];
// // Timestamp function
// // Timestamp format: YYYY-MM-DD HH:MM:SS
// // This is compatible with PostgreSQL timestamp type
// void get_timestamp(char *buffer, size_t len)
// {
//     time_t now = time(NULL);
//     struct tm *t = localtime(&now);
//     strftime(buffer, len, "%Y-%m-%d %H:%M:%S", t);
// }

// // Detect method from argv for ENUM
// // pgbackrest methods include:
// // archive-push, backup, restore, check, stanza-create, stanza-upgrade
// // Default to 'check' if not found  
// // archive-push -> to read WAL files
// // backup -> to take backup
// // restore -> to restore backup
// // check -> to check backup integrity
// // stanza-create -> to create stanza
// // stanza-upgrade -> to upgrade stanza
// // Include other methods as needed
// const char* detect_method(int argc, char *argv[])
// {
//     for (int i = 1; i < argc; i++)
//     {
//         if (strcmp(argv[i], "archive-push") == 0) return "archive-push";
//         if (strcmp(argv[i], "backup") == 0)        return "backup";
//         if (strcmp(argv[i], "restore") == 0)       return "restore";
//         if (strcmp(argv[i], "check") == 0)         return "check";
//         if (strcmp(argv[i], "stanza-create") == 0) return "stanza-create";
//         if (strcmp(argv[i], "stanza-upgrade") == 0)return "stanza-upgrade";
//     }
//     return "check"; // fallback
// }

// // Escape string for SQL as 
// // single quotes need to be doubled to put into SQL
// // e.g. O'Reilly -> O''Reilly
// // Postges raise error with unescaped single quotes
// void escape_sql(const char *input, char *output, size_t out_size)
// {
//     size_t j = 0;
//     for (size_t i = 0; input[i] && j < out_size - 2; i++)
//     {
//         if (input[i] == '\'') { output[j++] = '\''; output[j++] = '\''; }
//         else { output[j++] = input[i]; }
//     }
//     output[j] = '\0';
// }

// // Insert into PostgreSQL using psql
// // Using popen for simplicity
// // Use better options in future if needed
// // like system, libpq, SPI_connection etc.
// // void insert_into_pg(const char *method, const char *command,
// //                     const char *start_ts, const char *end_ts,
// //                     int exit_code, const char *error_msg, int pid)
// // {
// //     char cmd_esc[4096], err_esc[2048];
// //     escape_sql(command, cmd_esc, sizeof(cmd_esc));
// //     escape_sql(error_msg, err_esc, sizeof(err_esc));

// //     char sql[8192];
// //     char checkcmd[4096];
// //     FILE *fp;
// //     char buffer[512];

// //   // checking if auditdb exists
// //   // in future use some better method for checking existence
// //   // like libpq or SPI connection.
// //   // Although system and popen are crash-proof in uitility hooks
// //   // but can slow down performance if used excessively.
// //     snprintf(checkcmd, sizeof(checkcmd),
// //              "\"%s\" -tAc \"SELECT 1 FROM pg_database WHERE datname='backupcompliance'\"",psql_path);

// //     fp = popen(checkcmd, "r");
// //     if (!fp) return;

// //     if (!fgets(buffer, sizeof(buffer), fp)) {
// //         pclose(fp);
// //         return;   // no output = db does not exist
// //     }
// //     pclose(fp);

// //     if (buffer[0] != '1') {
// //         return;  
// //     }

// //   // checking if pgbackrest_logs table exists
// //     snprintf(checkcmd, sizeof(checkcmd),
// //              "\"%s\" -d auditdb -tAc \"SELECT 1 FROM pg_tables "
// //              "WHERE tablename='backup_operations_log'\"",psql_path);

// //     fp = popen(checkcmd, "r");
// //     if (!fp) return;

// //     if (!fgets(buffer, sizeof(buffer), fp)) {
// //         pclose(fp);
// //         return;   // table missing
// //     }
// //     pclose(fp);

// //     if (buffer[0] != '1') {
// //         return;   // table missing
// //     }

// //    // Was getting a timestamp conversion error in PostgreSQL
// //     // when end_ts was NULL. So handling it separately.
// //     // If end_ts is empty string, insert NULL for end_time and duration
// //     // else insert actual values.
// //     if (end_ts[0] != '\0') {
// //         snprintf(sql, sizeof(sql),
// //             "\"%s\" -d backupcompliance -U postgres -c "
// //             "\"INSERT INTO backup_operations_log(application_name,start_time, end_time, database_name,error, pid) "
// //             "VALUES('%s', '%s', '%s', '%s', '%s', %d);\"",
// //             psql_path,
// //             "pgbackrest",
// //             start_ts,
// //             end_ts,
// //             NULL,
// //             (error_msg[0] != '\0' ? "'" : "NULL"),
// //             (error_msg[0] != '\0' ? err_esc : ""),
// //             (error_msg[0] != '\0' ? "'" : ""),
// //             pid
// //         );
// //     } else {
// //         snprintf(sql, sizeof(sql),
// //             "\"%s\" -d backupcompliance -U postgres -c "
// //             "\"INSERT INTO pgbackrest_logs(application_name,start_time,database_name,error, pid) "
// //             "VALUES('%s', '%s', '%s', NULL, NULL, %d);\"",
// //             psql_path,
// //             "pgbackrest",
// //             start_ts,
// //             NULL,
// //             (error_msg[0] != '\0' ? err_esc : ""),
// //             pid
// //         );
// //     }

// //     // Using popen here. Can use better methods if needed.
// //     fp = popen(sql, "r");
// //     if (fp) pclose(fp);
// // }

// void insert_into_pg(const char *method, const char *command,
//                     const char *start_ts, const char *end_ts,
//                     int exit_code, const char *error_msg, int pid)
// {
//     char cmd_esc[4096], err_esc[2048];
//     escape_sql(command, cmd_esc, sizeof(cmd_esc));   
//     escape_sql(error_msg, err_esc, sizeof(err_esc)); 

//     char sql[8192];
//     char checkcmd[4096];
//     FILE *fp;
//     char buffer[512];

//     // check if backupcompliance DB exists 
//     snprintf(checkcmd, sizeof(checkcmd),
//              "\"%s\" -tAc \"SELECT 1 FROM pg_database WHERE datname='backupcompliance'\"",
//              psql_path);

//     fp = popen(checkcmd, "r");
//     if (!fp) return;
//     if (!fgets(buffer, sizeof(buffer), fp)) { pclose(fp); return; }
//     pclose(fp);
//     if (buffer[0] != '1'){
//         return;
//     } 

//   // checking if pgbackrest_logs table exists
//     snprintf(checkcmd, sizeof(checkcmd),
//              "\"%s\" -d backupcompliance -tAc \"SELECT 1 FROM pg_tables "
//              "WHERE tablename='backup_operations_log'\"",psql_path);

//     fp = popen(checkcmd, "r");
//     if (!fp) return;
//     if (!fgets(buffer, sizeof(buffer), fp)) { pclose(fp); return; }
//     pclose(fp);
//     if (buffer[0] != '1'){ 
//         return;
//     }
//     // prepare error SQL
//     const char *error_sql = (error_msg[0] == '\0') ? "" : err_esc;
 
//     if (end_ts[0] != '\0') {
//         // with end_ts
//         snprintf(sql, sizeof(sql),
//             "\"%s\" -d backupcompliance -U postgres -c "
//             "\"INSERT INTO backup_operations_log("
//             "application_name,start_time,end_time,database_name,error,backend_pid"
//             ") VALUES ('pgbackrest','%s','%s','','%s',%d);\"",
//             psql_path, start_ts, end_ts, error_sql, pid);

//     } else {
//         // without end_ts
//         snprintf(sql, sizeof(sql),
//             "\"%s\" -d backupcompliance -U postgres -c "
//             "\"INSERT INTO backup_operations_log("
//             "application_name,start_time,database_name,error,backend_pid"
//             ") VALUES ('pgbackrest','%s','','%s',%d);\"",
//             psql_path, start_ts, error_sql, pid);

//     }
   
//     fp = popen(sql, "r");
//     if (fp) pclose(fp);
// }


// static void run_pg_config_cmd(const char *cmd, char *outbuf, size_t outbuf_size)
// {
//     FILE *fp = popen(cmd, "r");

//     if (!fp)
//     {
//         outbuf[0] = '\0';   // NOT snprintf("")
//         return;
//     }

//     if (fgets(outbuf, outbuf_size, fp) != NULL)
//     {
//         outbuf[strcspn(outbuf, "\n")] = '\0';  // strip newline
//     }
//     else
//     {
//         outbuf[0] = '\0';  // NOT snprintf("")
//     }

//     pclose(fp);
// }


// int main(int argc, char *argv[])
// {
//     fprintf(stdout, "pgbackrest wrapper version %s\n", PGBACKREST_WRAPPER_VERSION);
//     fflush(stdout);
//     fprintf(stdout, "Remove this wrapper, by running `sudo rm -f /usr/local/bin/pgbackrest`\n");
//     fprintf(stdout, "Call to pgbackrest for internal operations.\n");
//     fflush(stdout);
//     find_real_pgbackrest_by_inode();
//     fprintf(stdout, "Real pgbackrest path detected: %s\n", pgbackrest_path);
//     fflush(stdout); 



//     // Prevent recursion
//     // It is possible that pgbackrest calls itself internally
//     // in some operations like backup or restore
//     //  - so we set an env variable to detect recursion
//     //  and directly exec the real pgbackrest binary
//     if (getenv("WRAPPER_ACTIVE"))
//     {
 
//         execv(pgbackrest_path, argv);
//         perror("execv failed");
//         return 1;
//     }
//     setenv("WRAPPER_ACTIVE", "1", 1);

//     const char *method = detect_method(argc, argv);


//     char pg_config_path[1024] = {0};

//     /* 1. Find actual pg_config */
//     run_pg_config_cmd("which pg_config", pg_config_path, sizeof(pg_config_path));

//     if (pg_config_path[0] == '\0')
//     {
//         fprintf(stdout, "ERROR: pg_config not found in PATH\n");
//         return 1;
//     }

//     /* 2. Build the correct pg_config command */
//     char pg_config_bindir_cmd[1024];
//     snprintf(pg_config_bindir_cmd, sizeof(pg_config_bindir_cmd),
//          "\"%.900s\" --bindir 2>/dev/null", pg_config_path);

//     /* 3. Get bindir */
//     run_pg_config_cmd(pg_config_bindir_cmd, psql_path, sizeof(psql_path));

//     if (psql_path[0] != '\0')
//     {
//         /* Append /psql */
//         strncat(psql_path, "/psql", sizeof(psql_path) - strlen(psql_path) - 1);
//         psql_path[sizeof(psql_path) - 1] = '\0';
//         fprintf(stdout, "Using pg_config autodetected psql path: %s\n", psql_path);
//     }
//     else
//     {
//         /* Fallback search */
//         const char *dirs[] = {
//             "/usr/local/bin/psql",
//             "/usr/bin/psql",
//             "/bin/psql",
//             "/usr/local/pgsql/bin/psql",
//             NULL
//         };

//         bool found = false;
//         for (int i = 0; dirs[i] != NULL; i++)
//         {
//             if (access(dirs[i], R_OK) == 0)
//             {
//                 strncpy(psql_path, dirs[i], sizeof(psql_path));
//                 psql_path[sizeof(psql_path) - 1] = '\0';
//                 fprintf(stdout, "Using fallback psql path: %s\n", psql_path);
//                 found = true;
//                 break;
//             }
//         }

//         if (!found)
//         {
//             strncpy(psql_path, "/usr/bin/psql", sizeof(psql_path));
//             psql_path[sizeof(psql_path) - 1] = '\0';
//             fprintf(stdout, 
//                     "WARNING: Could not detect psql. Defaulting to /usr/bin/psql\n");
//         }
//     }

//     fprintf(stdout, "FINAL psql_path = %s\n", psql_path);


//     // Build full command
//     char cmd[4096] = {0};
//     strcat(cmd, pgbackrest_path);
//     for (int i = 1; i < argc; i++)
//     {
//         strcat(cmd, " ");
//         strcat(cmd, argv[i]);
//     }
//     strcat(cmd, " 2>&1"); // capture stderr as well

//     char start_ts[64];
//     get_timestamp(start_ts, sizeof(start_ts));


//     // Logging data into log file
//     // Altough not required, helps in debugging
//     // Can directly push into postgrsql table.
//     FILE *log = fopen(LOGFILE, "a");
//     if (!log) { perror("fopen"); return 1; }

//     fprintf(log, "--------------------------------------------------\n");
//     fprintf(stdout, "%s - pgbackrest called:\nCommand: %s\n", start_ts, cmd);
//     fflush(stdout);
//     fprintf(log, "%s - pgbackrest called:\nCommand: %s\n", start_ts, cmd);
//     fflush(log);
    

//     // Run pgBackRest
//     FILE *fp = popen(cmd, "r");
//     if (!fp)
//     {
//         fprintf(log, "ERROR: failed to run pgbackrest\n");
//         fclose(log);
//         return 1;
//     }

//     fprintf(log, "---- BEGIN OUTPUT ----\n");
//     char buffer[2048];
//     char first_error[2048] = {0};

//     while (fgets(buffer, sizeof(buffer), fp))
//     {
//         fprintf(stdout, "%s", buffer);
//         fflush(stdout);
//         fprintf(log, "%s", buffer);
//         fflush(log);
        

//         if (strstr(buffer, "ERROR:") && first_error[0] == '\0')
//             strncpy(first_error, buffer, sizeof(first_error)-1);
//     }

//     // Get exit code
//     // Close process and get exit code
//     int exit_code = pclose(fp);
//     if (WIFEXITED(exit_code))
//         exit_code = WEXITSTATUS(exit_code);

//     fprintf(log, "---- END OUTPUT ----\nExit Code: %d\n", exit_code);
//     fprintf(log, "--------------------------------------------------\n");
//     fclose(log);

//     char end_ts[64];
//     get_timestamp(end_ts, sizeof(end_ts));

//     int pid = getpid();


//     // Insert into PostgreSQL table 
//     if(method && strcmp(method,"backup")==0){
//         insert_into_pg(method, cmd, start_ts, end_ts, exit_code, first_error, pid);
//     }
//     return exit_code;
// }


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libpq-fe.h>
#include <time.h>

#define WRAPPER_ENV "PGBACKREST_WRAPPER_ACTIVE"
#define AUDIT_DB "backupcompliance"
#define CONNECT_TIMEOUT "2"
#define ERROR_BUF_SIZE 8192

static char real_pgbackrest[512];

static void get_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *detect_method(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "backup")) return "backup";
        if (!strcmp(argv[i], "restore")) return "restore";
        if (!strcmp(argv[i], "check")) return "check";
        if (!strcmp(argv[i], "archive-push")) return "archive-push";
        if (!strcmp(argv[i], "stanza-create")) return "stanza-create";
        if (!strcmp(argv[i], "stanza-upgrade")) return "stanza-upgrade";
    }
    return "unknown";
}

// static bool find_real_pgbackrest(void)
// {
//     struct stat self, st;
//     if (stat("/proc/self/exe", &self) != 0)
//         return false;

//     FILE *fp = popen("which -a pgbackrest", "r");
//     if (!fp) return false;

//     while (fgets(real_pgbackrest, sizeof(real_pgbackrest), fp))
//     {
//         real_pgbackrest[strcspn(real_pgbackrest, "\n")] = 0;
//         if (stat(real_pgbackrest, &st) != 0) continue;
//         if (st.st_ino != self.st_ino || st.st_dev != self.st_dev)
//         {
//             pclose(fp);
//             return true;
//         }
//     }
//     pclose(fp);
//     return false;
// }

static bool find_real_pgbackrest(void)
{
    struct stat self_st, file_st;
    if (stat("/proc/self/exe", &self_st) != 0) return false;

    char *path_env = getenv("PATH");
    if (!path_env) return false;

    char *paths = strdup(path_env);
    char *dir = strtok(paths, ":");

    while (dir != NULL)
    {
        snprintf(real_pgbackrest, sizeof(real_pgbackrest), "%s/pgbackrest", dir);
        
        // Check if file exists
        if (stat(real_pgbackrest, &file_st) == 0)
        {
            // Portable check: Is this file different from the wrapper currently running?
            if (file_st.st_ino != self_st.st_ino || file_st.st_dev != self_st.st_dev)
            {
                free(paths);
                return true;
            }
        }
        dir = strtok(NULL, ":");
    }
    free(paths);
    return false;
}

static void audit_insert(const char *method,
                         const char *start_ts,
                         const char *end_ts,
                         int exit_code,
                         const char *output,
                         pid_t pid)
{
    // char conninfo[256];
    // snprintf(conninfo, sizeof(conninfo),
    //          "dbname=%s connect_timeout=%s",
    //          AUDIT_DB, CONNECT_TIMEOUT);

    // PGconn *conn = PQconnectdb(conninfo);
    // if (PQstatus(conn) != CONNECTION_OK)
    // {
    //     fprintf(stderr, "[pgbackrest-wrapper] Cannot connect to DB: %s\n",
    //             PQerrorMessage(conn));
    //     PQfinish(conn);
    //     return;
    // }

    // 1. Check if PGHOST is set, if not, try to be helpful but stay portable
    // Libpq will automatically use PGHOST, PGPORT, etc., if we provide a minimal string
    const char *conninfo = "dbname=" AUDIT_DB " connect_timeout=" CONNECT_TIMEOUT;

    PGconn *conn = PQconnectdb(conninfo);
    
    // 2. If connection fails and we are on a source-built machine like yours, 
    // we can try a fallback to /tmp just to be safe before giving up.
    if (PQstatus(conn) != CONNECTION_OK)
    {
        PQfinish(conn);
        char fallback_info[256];
        snprintf(fallback_info, sizeof(fallback_info), 
                 "dbname=%s host=/tmp connect_timeout=%s", AUDIT_DB, CONNECT_TIMEOUT);
        conn = PQconnectdb(fallback_info);
    }

    if (PQstatus(conn) != CONNECTION_OK)
    {
        fprintf(stderr, "[pgbackrest-wrapper] Audit DB Error: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return;
    }

    PGresult *chk = PQexec(conn,
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_name='backup_operations_log'");
    if (PQresultStatus(chk) != PGRES_TUPLES_OK || PQntuples(chk) == 0)
    {
        PQclear(chk);
        PQfinish(conn);
        fprintf(stderr, "[pgbackrest-wrapper] Table backup_operations_log missing\n");
        return;
    }
    PQclear(chk);

    const char *sql =
        "INSERT INTO backup_operations_log "
        "(application_name, start_time, end_time, database_name, error, backend_pid) "
        "VALUES ($1,$2,$3,$4,$5,$6)";

    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", pid);
    const char *dbname = "";  // database_name column left empty
    const char *params[6] = {method, start_ts, end_ts, dbname, output, pidbuf};

    PGresult *res = PQexecParams(conn, sql, 6, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "[pgbackrest-wrapper] INSERT failed: %s\n",
                PQerrorMessage(conn));
    }
    else
    {
        fprintf(stderr, "[pgbackrest-wrapper] INSERT succeeded\n");
    }

    PQclear(res);
    PQfinish(conn);
}

int main(int argc, char **argv)
{
    // fprintf(stderr, "[pgbackrest-wrapper] Wrapper started (pid=%d)\n", getpid());
    const char *method = detect_method(argc, argv);

    // fprintf(stderr, "[pgbackrest-wrapper] Real pgbackrest path: %s\n", real_pgbackrest);
    
    if (!(strcmp(method, "backup") == 0)) {
        // Hardcode the known real path for the archive-push to be ultra-fast
        // or use a simplified find_real_pgbackrest.
        // execv(real_pgbackrest, argv);
        // If execv fails, it will fall through to the rest of the logic
        if (find_real_pgbackrest()) {
            execv(real_pgbackrest, argv);
        }

        execv("/usr/bin/pgbackrest", argv);
        
        // If the above failed (e.g. path different), try /usr/local/bin
        execv("/usr/local/bin/pgbackrest", argv);
    }

    fprintf(stderr, "[pgbackrest-wrapper] Wrapper started (pid=%d)\n", getpid());
    
    if (!find_real_pgbackrest())
    {
        fprintf(stderr, "[pgbackrest-wrapper] Real pgbackrest not found\n");
        return 127;
    }
    fprintf(stderr, "[pgbackrest-wrapper] Real pgbackrest path: %s\n", real_pgbackrest);
    char start_ts[32], end_ts[32];
    get_timestamp(start_ts, sizeof(start_ts));


    if (getenv("PGBW_ACTIVE"))
    {
        execv(real_pgbackrest, argv);
        perror("[pgbackrest-wrapper] execv failed");
        _exit(127);
    }

   setenv("PGBW_ACTIVE", "1", 1);

    // Use pipe to capture both stdout and stderr
    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        perror("[pgbackrest-wrapper] pipe failed");
        return 127;
    }

    pid_t child = fork();
    if (child == 0)
    {
        close(pipefd[0]); // close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(real_pgbackrest, argv);
        perror("[pgbackrest-wrapper] execv failed");
        _exit(127);
    }

    close(pipefd[1]); // parent closes write end
    char buffer[1024];
    char error_buf[ERROR_BUF_SIZE] = {0};
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer)-1)) > 0)
    {
        buffer[n] = '\0';
        fputs(buffer, stderr);  // stdout+stderr output to terminal
        strncat(error_buf, buffer, sizeof(error_buf) - strlen(error_buf) - 1);
    }
    close(pipefd[0]);

    int status;
    waitpid(child, &status, 0);
    get_timestamp(end_ts, sizeof(end_ts));

    int exit_code = WIFEXITED(status)
        ? WEXITSTATUS(status)
        : 128 + WTERMSIG(status);

    if(method && strcmp(method,"backup")==0)
    audit_insert("pgbackrest", start_ts, end_ts, exit_code, error_buf, getpid());

    return exit_code;
}
