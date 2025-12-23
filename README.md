## pg_backup_compliance extension

This extension tracks calls of pg_dump, pg_dumpall, pg_basebackup, and pgBackRest from inside PostgreSQL.

The logs are stored in a database named backupcompliance, which is created when the extension is created.
A separate database is chosen because pg_basebackup, pg_dumpall, and pgbackrest are cluster-level commands and run across the entire cluster. Keeping data in a separate global database ensures that the whole server remains updated when running a global command.

** Admins can access all backup operation logs from the table backup_operations_log inside the backupcompliance database. 

## Requirement

** A database named `backupcompliance` is needed by the extension


Run this command via psql, or directly in your pgadmin

```
    CREATE DATABASE backupcompliance;

```


## Setup

1. Clone the extension

2. Run the following in shell

```

make clean
make
sudo make install

```


3. Add following to postgresql.conf

```

shared_preload_libraries = 'pg_backup_compliance'

```

4. Restart the server 

```
    sudo systemctl restart postgresql@<version>-main

```


5. Create the extension on the database to be tracked

```

CREATE EXTENSION pg_backup_compliance;

```
# critical point
If extension dont create schema or views or any problem. Always repeat setup steps, especially restart potgresql again.

Now, when pg_dump, pg_basebackup, or pg_dumpall are executed, extra logs will appear in database backupcompliance, table backup_operations_log.

## Example commands(may differ for different environments/paths)

1. pg_dump example command

```

sudo -u postgres pg_dump <database_name> > dump.sql

```

2. pg_basebackup example command 

```

   pg_basebackup -h <server_name> -p 5432 -U <backup_user> -D /tmp/pg_backup_test_2 -F tar -X fetch -P
 
```

3. pg_dumpall example command

```

    sudo -u postgres pg_dumpall > dumpall.sql

```


In pg_basebackup, your user <backup_user> must have replication permissions and backup permissions, so register it with backup/replication permissions

##  For pgbackrest integration

    1. Ensure the original pgbackrest binary exists at: /usr/bin/pgbackrest
    
    2. Install the extension

    ```
        sudo make pgbackrest_install

    ```

    3. In `postgresql.conf` file set. These may be already set. If these are already set, please change/replace if any command is not there or not matched.
    
    ```

        archive_mode = on
        archive_command = 'pgbackrest --stanza=<your staza> archive-push %p'
        wal_level = replica
        max_wal_senders = 3

    ```

---

## Views Available

1. v_quarterly_backups          --> provides backups from last three months from now()
2. v_monthly_backups            --> provides backups from last months from now()
3. v_failed_backups             --> provides all failed backup attempts. 
4. v_quarterly_failed_backups   -->  provides failed backups from last three months from now()
5. v_monthly_failed_backups     --> provides failed backups from last months from now()

## 📌 Notes

- All time-based views compute date ranges using `now()` at query time.
- Failed views include only operations recorded as "error is not empty".

---


