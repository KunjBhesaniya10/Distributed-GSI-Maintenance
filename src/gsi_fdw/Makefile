# src/gsi_fdw/Makefile

MODULE_big = gsi_fdw
OBJS = \
	$(WIN32RES) \
	connection.o \
	deparse.o \
	gsi.o \
	gsi_resolver.o \
	option.o \
	postgres_fdw.o \
	shippable.o
PGFILEDESC = "gsi_fdw - postgres_fdw with global secondary indexes"

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)

EXTENSION = gsi_fdw
DATA = gsi_fdw--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
