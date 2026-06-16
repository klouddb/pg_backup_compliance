# pg_backup_compliance Makefile (PGXS)

EXTENSION   = pg_backup_compliance
MODULE_big  = pg_backup_compliance

OBJS = pg_backup_compliance.o pg_backup_compliance_capture.o

DATA = sql/pg_backup_compliance--1.0.sql

PGFILEDESC = "pg_backup_compliance - audit of backup operations"

PG_CFLAGS += -Wall -Wextra -Wmissing-prototypes -Wpointer-arith \
             -Wno-unused-parameter -Wno-sign-compare

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
