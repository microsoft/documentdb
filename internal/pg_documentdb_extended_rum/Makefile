EXTENSION = documentdb_extended_rum
MODULE_big = pg_$(EXTENSION)
EXTVERSION = 1.0
PGFILEDESC = "DocumentDB Extended RUM index access method"

OSS_SRC_DIR = ../../

SOURCES = $(wildcard src/*.c)
OBJS = $(patsubst %.c,%.o,$(SOURCES)) $(WIN32RES)

SQL_DEPDIR=.deps/sql
SQL_BUILDDIR=build/sql

template_sql_files = $(wildcard sql/*.sql)
generated_sql_files = $(patsubst %,build/%,$(template_sql_files))
DATA_built = $(generated_sql_files)
sql_file_inputs = $(wildcard sql/**/*.sql)

OSS_COMMON_SQL_HEADER = $(wildcard $(OSS_SRC_DIR)/common_header.sql)

PG_CPPFLAGS += -I$(OSS_SRC_DIR)/pg_documentdb_core/include/ -I$(OSS_SRC_DIR)/pg_documentdb/include/

# INCLUDES = rum.h rumsort.h
RELATIVE_INCLUDES = $(addprefix src/, $(INCLUDES))

LDFLAGS_SL += $(filter -lm, $(LIBS))

USE_DOCUMENTDB = 1
SKIP_API_SCHEMA = yes
CORE_SCHEMA_NAME=documentdb_core
EXTENSION_OBJECT_PREFIX=documentdb_extended
include $(OSS_SRC_DIR)/Makefile.cflags

PG_CFLAGS += -Wno-sign-compare -Wno-type-limits

include $(OSS_SRC_DIR)/Makefile.global

# Default to hidden visibility
override LDFLAGS_SL += -fvisibility=hidden -DHAVE_VISIBILITY_ATTRIBUTE=1
override CFLAGS += -fvisibility=hidden  -DHAVE_VISIBILITY_ATTRIBUTE=1
override CXXFLAGS += -fvisibility=hidden  -DHAVE_VISIBILITY_ATTRIBUTE=1


clean-sql:
	rm -rf .deps/ build/


build-sql: $(generated_sql_files)

$(generated_sql_files): build/%: %
	@mkdir -p $(SQL_DEPDIR) $(SQL_BUILDDIR)
	@# -MF is used to store dependency files(.Po) in another directory for separation
	@# -MT is used to change the target of the rule emitted by dependency generation.
	@# -P is used to inhibit generation of linemarkers in the output from the preprocessor.
	@# -undef is used to not predefine any system-specific or GCC-specific macros.
	@# -imacros is used to specify a file that defines macros for the global context but its output is thrown away.
	@# `man cpp` for further information
	cpp -undef -w $(SQL_DEFINES) -imacros $(OSS_COMMON_SQL_HEADER) -imacros documentdb_rum_sql_defines.sql -I$(OSS_SRC_DIR) -P -MMD -MP -MF$(SQL_DEPDIR)/$(*F).Po -MT$@ $< > $@