EXTENSION = pg_backup_compliance
MODULE_big = pg_backup_compliance
OBJS = pg_backup_compliance.o pg_backup_compliance_dump.o

DATA = sql/pg_backup_compliance--1.0.sql sql/pg_backup_compliance_table.sql # initialization SQL

PGFILEDESC = "pg_backup_compliance - PostgreSQL backup compliance logging extension"

# Default, can be overridden externally
PG_CONFIG ?= pg_config
# PG_CONFIG = /usr/lib/postgresql/15/bin/pg_config

# If PG_CONFIG is not an absolute path, try resolving it
ifeq ($(PG_CONFIG),pg_config)
  PG_CONFIG := $(shell command -v pg_config 2>/dev/null)
endif
# Now validate
ifeq ($(PG_CONFIG),)
  $(error "pg_config not found. Install PostgreSQL dev package or run")
endif

PSQL_BIN := $(shell $(PG_CONFIG) --bindir)/psql
PG_BINDIR := $(shell $(PG_CONFIG) --bindir)
PGAUD_SHDIR := $(shell $(PG_CONFIG) --sharedir)/extension/pg_backup_compliance_table.sql


PG_CFLAGS += -DPSQL_BIN=\"$(PSQL_BIN)\"
PG_CFLAGS += -DPGAUD_LIBDIR=\"$(shell $(PG_CONFIG) --pkglibdir)\"
PG_CFLAGS += -DPGAUD_SHDIR=\"$(PGAUD_SHDIR)\"

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)


# .PHONY: pgbackrest_install

# pgbackrest_install:
# 	@chmod +x ./pgbackrest_installer.sh
# 	@bash ./pgbackrest_installer.sh

.PHONY: pgbackrest_install

pgbackrest_install:
	@chmod +x ./pgbackrest_installer.sh
	@export PATH="$$PATH:$(dir $(PG_CONFIG))"; \
	./pgbackrest_installer.sh