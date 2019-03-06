MODULE_big = count_distinct
OBJS = src/count_distinct.o

EXTENSION = count_distinct
DATA = sql/count_distinct--3.0.0.sql sql/count_distinct--1.3.1--1.3.2.sql \
		sql/count_distinct--1.3.2--1.3.3.sql sql/count_distinct--1.3.3--2.0.0.sql \
		sql/count_distinct--2.0.0--3.0.0.sql
MODULES = count_distinct

CFLAGS=`/usr/pgsql-9.5/bin/pg_config --includedir-server`

TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test

PG_CONFIG = /usr/pgsql-9.5/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

count_distinct.so: src/count_distinct.o

src/count_distinct.o: src/count_distinct.c
