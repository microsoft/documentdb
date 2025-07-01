# Delegate all rules to sub directories.

Makefile:;

.DEFAULT_GOAL := .DEFAULT

.PHONY: %

check-no-distributed:
	$(MAKE) -C pg_documentdb_core check
	$(MAKE) -C pg_documentdb check

install-no-distributed:
	$(MAKE) -C pg_documentdb_core install
	$(MAKE) -C pg_documentdb install

install-documentdb-rum:
	$(MAKE) -C internal/pg_documentdb_rum install

.DEFAULT:
	$(MAKE) -C pg_documentdb_core
	$(MAKE) -C pg_documentdb
	$(MAKE) -C internal/pg_documentdb_distributed
	$(MAKE) -C internal/pg_documentdb_rum

%:
	$(MAKE) -C pg_documentdb_core $@
	$(MAKE) -C pg_documentdb $@
	$(MAKE) -C internal/pg_documentdb_distributed $@
	$(MAKE) -C internal/pg_documentdb_rum $@