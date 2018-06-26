# Makefile for PL/Lua

PG_CONFIG ?= pg_config

# Lua specific

# two values can be set here, which only apply to luajit:
#  -DNO_LUAJIT        disables all use of luajit features
#  -DUSE_INT8_CDATA   convert sql bigints to cdata int64_t
# The latter is off by default because it has some possibly
# undesirable effects on bigint handling.

PLLUA_CONFIG_OPTS ?=

# General
LUA_INCDIR ?= /usr/local/include/lua53
LUALIB ?= -L/usr/local/lib -llua-5.3

# LuaJIT
#LUA_INCDIR = /usr/local/include/luajit-2.0
#LUALIB = -L/usr/local/lib -lluajit-5.1
#LUA_INCDIR = /usr/local/include/luajit-2.1
#LUALIB = -L/usr/local/lib -lluajit-5.1

# Debian/Ubuntu
#LUA_INCDIR = /usr/include/lua5.1
#LUALIB = -llua5.1

# Fink
#LUA_INCDIR = /sw/include -I/sw/include/postgresql
#LUALIB = -L/sw/lib -llua

# Lua for Windows
#LUA_INCDIR = C:/PROGRA~1/Lua/5.1/include
#LUALIB = -LC:/PROGRA~1/Lua/5.1/lib -llua5.1

# no need to edit below here
MODULE_big = pllua
EXTENSION = pllua plluau
DATA = pllua--1.0.sql plluau--1.0.sql

REGRESS = --schedule=$(srcdir)/serial_schedule
REGRESS_PARALLEL = --schedule=$(srcdir)/parallel_schedule
# only on pg10+
REGRESS_10 = triggers_10
# only on pg10+
REGRESS_11 = procedures

SRCOBJS=compile.o datum.o elog.o error.o exec.o globals.o init.o \
	jsonb.o numeric.o objects.o pllua.o spi.o trigger.o trusted.o

OBJS = $(addprefix src/, $(SRCOBJS))

EXTRA_CLEAN = pllua_functable.h plerrcodes.h

PG_CPPFLAGS = -I$(LUA_INCDIR) $(PLLUA_CONFIG_OPTS)
SHLIB_LINK = $(LUALIB)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifeq ($(filter-out 7.% 8.% 9.0 9.1 9.2 9.3 9.4, $(MAJORVERSION)),)
$(error unsupported PostgreSQL version)
endif
ifneq ($(filter-out 9.%, $(MAJORVERSION)),)
REGRESS += $(REGRESS_10)
REGRESS_PARALLEL += $(REGRESS_10)
endif
ifneq ($(filter-out 9.% 10, $(MAJORVERSION)),)
REGRESS += $(REGRESS_11)
REGRESS_PARALLEL += $(REGRESS_11)
endif

$(OBJS): src/pllua.h

src/init.o: pllua_functable.h
src/error.o: plerrcodes.h

pllua_functable.h: $(OBJS:.o=.c)
	cat $(OBJS:.o=.c) | perl -lne '/(pllua_pushcfunction|pllua_cpcall|pllua_initial_protected_call|pllua_register_cfunc)\(\s*([\w.]+)\s*,\s*(pllua_\w+)\s*/ and print "PLLUA_DECL_CFUNC($$3)"' | sort -u >pllua_functable.h

ifneq ($(filter-out 9.% 10, $(MAJORVERSION)),)

#in pg 11+, we can get the server's errcodes.txt.
plerrcodes.h: $(datadir)/errcodes.txt
	perl -lane '/^(?!Section:)[^#\s]/ and @F==4 and printf "{\n    \"%s\", %s\n},\n", $$F[3], $$F[2]' $(datadir)/errcodes.txt >plerrcodes.h

else

plerrcodes.h: src/plerrcodes_old.h
	cp src/plerrcodes_old.h plerrcodes.h

endif

installcheck-parallel: submake $(REGRESS_PREP)
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_PARALLEL)
