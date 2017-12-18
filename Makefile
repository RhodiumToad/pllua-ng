# Makefile for PL/Lua

PG_CONFIG ?= pg_config

# Lua specific

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

SRCOBJS=compile.o datum.o elog.o error.o exec.o globals.o init.o \
	jsonb.o numeric.o objects.o pllua.o spi.o trigger.o trusted.o

OBJS = $(addprefix src/, $(SRCOBJS))

EXTRA_CLEAN = pllua_functable.h

PG_CPPFLAGS = -I$(LUA_INCDIR) #-DPLLUA_DEBUG
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

$(OBJS): src/pllua.h

src/init.o: pllua_functable.h

pllua_functable.h: $(OBJS:.o=.c)
	cat $(OBJS:.o=.c) | perl -lne '/(pllua_pushcfunction|pllua_cpcall|pllua_initial_protected_call)\(\s*([\w.]+)\s*,\s*(pllua_\w+)\s*/ and print "PLLUA_DECL_CFUNC($$3)"' | sort -u >pllua_functable.h

installcheck-parallel: submake $(REGRESS_PREP)
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_PARALLEL)
