EXTENSION = documentdb_rum
MODULE_big = pg_$(EXTENSION)
EXTVERSION = 1.0
PGFILEDESC = "DocumentDB RUM index access method"

OSS_SRC_DIR = ../../

SOURCES = $(wildcard src/*.c)
OBJS = $(patsubst %.c,%.o,$(SOURCES)) $(WIN32RES)

DATA = documentdb_rum--$(EXTVERSION).sql


# INCLUDES = rum.h rumsort.h
RELATIVE_INCLUDES = $(addprefix src/, $(INCLUDES))

LDFLAGS_SL += $(filter -lm, $(LIBS))

include $(OSS_SRC_DIR)/Makefile.global

# Default to hidden visibility
override LDFLAGS_SL += -fvisibility=hidden -DHAVE_VISIBILITY_ATTRIBUTE=1
override CFLAGS += -fvisibility=hidden  -DHAVE_VISIBILITY_ATTRIBUTE=1
override CXXFLAGS += -fvisibility=hidden  -DHAVE_VISIBILITY_ATTRIBUTE=1


clean-sql:
