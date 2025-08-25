
BASEPATH:=../../../

REGRESS := 1

PG_CONFIG ?= pg_config

# export pg_config for child make commands (check etc)
export PG_CONFIG

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

EXTENSIONLOAD := --load-extension=pg_cron --load-extension=tsm_system_rows --load-extension=vector --load-extension=postgis --load-extension=rum --load-extension=documentdb_core --load-extension=documentdb --load-extension=documentdb_extended_rum

MAKEFILE_DIR := $(dir $(realpath $(firstword $(MAKEFILE_LIST))))
export PATH := $(MAKEFILE_DIR)bin:$(PATH)
export PG_REGRESS_DIFF_OPTS = -dU10

export PGISOLATIONTIMEOUT = 60

.PHONY: check-basic check-minimal

define common_test
	$(top_builddir)/src/test/regress/pg_regress --encoding=UTF8 --dlpath=$(BASEPATH) $(EXTENSIONLOAD) --temp-instance ./tmp --temp-config ./postgresql.conf --host localhost --port 58090 $(1) $(2) || (cat regression.diffs && false)
endef

check-basic:
	$(call common_test,--schedule=./basic_schedule)

check-minimal:
	$(call common_test,--schedule=./minimal_schedule, $(EXTRA_TESTS))

all: check-basic
