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
LUAC ?= luac53
LUA ?= lua53

# LuaJIT
#LUA_INCDIR = /usr/local/include/luajit-2.1
#LUALIB = -L/usr/local/lib -lluajit-5.1
#LUAJIT = luajit

ifdef LUAJIT
LUAJITC ?= $(LUAJIT) -b -g -t raw
LUA = $(LUAJIT)
LUAC = $(REORDER_O) $(LUAJITC)
endif

# if no OBJCOPY or not needed, this can be set to true (or false)
OBJCOPY ?= objcopy

# We expect $(BIN_LD) -o foo.o foo.luac to create a foo.o with the
# content of foo.luac as a data section (plus appropriate symbols).
# GNU LD and compatible linkers (including recent clang lld) should be
# fine with -r -b binary, but this does break on some ports.

BIN_LD ?= $(LD) -r -b binary

# If BIN_ARCH and BIN_FMT are defined, we assume LD_BINARY is broken
# and do this instead. This is apparently needed for linux-mips64el,
# for which BIN_ARCH=mips:isa64r2 BIN_FMT=elf64-tradlittlemips seems
# to work.

ifdef BIN_ARCH
ifdef BIN_FMT
BIN_LD = $(REORDER_O) $(OBJCOPY) -B $(BIN_ARCH) -I binary -O $(BIN_FMT)
endif
endif

# no need to edit below here
MODULE_big = pllua
EXTENSION = pllua plluau
DATA = 	pllua--2.0.sql pllua--1.0--2.0.sql \
	plluau--2.0.sql plluau--1.0--2.0.sql

REGRESS = --schedule=$(srcdir)/serial_schedule
REGRESS_PARALLEL = --schedule=$(srcdir)/parallel_schedule
# only on pg10+
REGRESS_10 = triggers_10
# only on pg11+
REGRESS_11 = procedures

REORDER_O = tools/reorder-o.sh

INCS=   pllua.h pllua_pgver.h pllua_luaver.h pllua_luajit.h

OBJS_C= compile.o datum.o elog.o error.o exec.o globals.o init.o \
	jsonb.o numeric.o objects.o pllua.o preload.o spi.o trigger.o \
	trusted.o

SRCS_C = $(addprefix src/, $(OBJS_C:.o=.c))

OBJS_LUA=compat.o

SRCS_LUA = $(addprefix src/, $(OBJS_LUA:.o=.lua))

OBJS = $(addprefix src/, $(OBJS_C))

EXTRA_OBJS = $(addprefix src/, $(OBJS_LUA))

EXTRA_CLEAN = pllua_functable.h plerrcodes.h $(SRCS_LUA:.lua=.luac) $(EXTRA_OBJS)

PG_CPPFLAGS = -I$(LUA_INCDIR) $(PLLUA_CONFIG_OPTS)
SHLIB_LINK = $(EXTRA_OBJS) $(LUALIB)

# not done except for testing, for portability
ifdef HIDE_SYMBOLS
SHLIB_LINK += -Wl,--version-script=src/exports.x
endif

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

$(OBJS): $(addprefix src/, $(INCS))

# explicit deps on generated includes
src/init.o: pllua_functable.h
src/error.o: plerrcodes.h

$(shlib): $(EXTRA_OBJS)

ifdef HIDE_SYMBOLS
$(shlib): src/exports.x
endif

%.luac: %.lua
	$(LUAC) -o $@ $<

%.o: %.luac
	$(BIN_LD) -o $@ $<
	-$(OBJCOPY) --rename-section .data=.rodata,contents,alloc,load,readonly $@

pllua_functable.h: $(SRCS_C) tools/functable.lua
	$(LUA) tools/functable.lua $(SRCS_C) | sort -u >$@

ifneq ($(filter-out 9.% 10, $(MAJORVERSION)),)

#in pg 11+, we can get the server's errcodes.txt.
plerrcodes.h: $(datadir)/errcodes.txt tools/errcodes.lua
	$(LUA) tools/errcodes.lua $(datadir)/errcodes.txt >plerrcodes.h

else

plerrcodes.h: src/plerrcodes_old.h
	cp src/plerrcodes_old.h plerrcodes.h

endif

installcheck-parallel: submake $(REGRESS_PREP)
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_PARALLEL)
