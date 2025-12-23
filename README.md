

Backup compliance plays a critical role in security and regulatory audits because data protection, availability, and recoverability are foundational aspects of information security. Most security frameworks and standards include explicit or implicit requirements for backup and recovery 

## Why Do You Need This Extension for Backup Compliance?

1. In security and compliance frameworks such as ISO 27001, NIST, and HIPAA, organizations must maintain tamper-evident evidence proving that backups are being performed as required. This extension provides auditable records that serve as backup compliance proof.

2. While it is possible to track pgBackRest, pg_basebackup, and pg_dump individually, there is no easy way to automatically and consistently track all three utilities together.

3. One of the major security risks is unauthorized database exports using pg_dump. Tracking all backup and dump activity is critical for audits and for detecting potential data exfiltration.

4. This extension includes built-in views that generate monthly and quarterly compliance reports, making audits significantly easier.

5. Although backup activity can be inferred from logs, that evidence may be rotated or lost over time. This extension stores the evidence centrally and persistently.

6. With this extension, you can quickly identify successful and failed backups across all supported tools for any given time period




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


