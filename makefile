# Build pg_trace_tool, need pg_config support

EXTENSION = pg_trace_tool
EXTVERSION = 1.0
DATA = pg_trace_tool--1.0.sql

MODULE_big = pg_trace_tool
OBJS = pg_trace_tool.o

# PGXS

USE_PGXS = 1
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
