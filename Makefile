# Makefile for PL/Lua		# -*- mode: makefile-gmake -*-

PG_CONFIG ?= pg_config

# Lua specific

# two values can be set here, which only apply to luajit:
#  -DNO_LUAJIT        disables all use of luajit features
#  -DUSE_INT8_CDATA   convert sql bigints to cdata int64_t
# The latter is off by default because it has some possibly
# undesirable effects on bigint handling.

PLLUA_CONFIG_OPTS ?=

# General
LUA_INCDIR ?= /usr/local/include/lua54
LUALIB ?= -L/usr/local/lib -llua-5.4
LUAC ?= luac54
LUA ?= lua54

# LuaJIT
#LUA_INCDIR = /usr/local/include/luajit-2.1
#LUALIB = -L/usr/local/lib -lluajit-5.1
#LUAJIT = luajit

ifdef LUAJIT
LUAJITC ?= $(LUAJIT) -b -g -t raw
LUA = $(LUAJIT)
LUAC = $(REORDER_O) $(LUAJITC)
endif

LUAVER = $(shell $(LUA) -e 'print(_VERSION:match("^Lua ([0-9.]+)"))')
ifeq ($(filter 5.1 5.3 5.4, $(LUAVER)),)
$(error failed to get valid lua version)
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

# should be no need to edit below here

MODULE_big = pllua

EXTENSION = pllua plluau

SQL_SRC = pllua--2.0.sql pllua--1.0--2.0.sql \
	  plluau--2.0.sql plluau--1.0--2.0.sql
DATA = $(addprefix scripts/, $(SQL_SRC))

DOC_HTML = pllua.html

ifdef BUILD_DOCS
DOCS = $(DOC_HTML)
ifdef BUILD_ICON
ICON = icon.meta
endif
endif

objdir := src

# variables like $(srcdir) and $(MAJORVERSION) are not yet set, but
# are safe to use in recursively-expanded variables since they will be
# set before most values are needed. Can't use them in conditionals
# until after pgxs is loaded though.

# version-dependent regression tests
REGRESS_10 := triggers_10
REGRESS_11 := $(REGRESS_10) procedures
REGRESS_12 := $(REGRESS_11)
REGRESS_13 := $(REGRESS_12)
REGRESS_14 := $(REGRESS_13)

REGRESS_LUA_5.4 := lua54

EXTRA_REGRESS = $(REGRESS_$(MAJORVERSION)) $(REGRESS_LUA_$(LUAVER))

REGRESS = --schedule=$(srcdir)/serial_schedule $(EXTRA_REGRESS)
REGRESS_PARALLEL = --schedule=$(srcdir)/parallel_schedule $(EXTRA_REGRESS)

REORDER_O = $(srcdir)/tools/reorder-o.sh

DOC_MD = css.css script.js intro.md pllua.md building.md endnote.md

DOC_SRCS = logo.css $(ICON) $(addprefix $(srcdir)/doc/, $(DOC_MD))

INCS=   pllua.h pllua_pgver.h pllua_luaver.h pllua_luajit.h

HEADERS= $(addprefix src/, $(INCS))

OBJS_C= compile.o datum.o elog.o error.o exec.o globals.o init.o \
	jsonb.o numeric.o objects.o paths.o pllua.o preload.o spi.o \
	time.o trigger.o trusted.o

SRCS_C = $(addprefix $(srcdir)/src/, $(OBJS_C:.o=.c))

OBJS_LUA = compat.o

SRCS_LUA = $(addprefix $(srcdir)/src/, $(OBJS_LUA:.o=.lua))

OBJS = $(addprefix src/, $(OBJS_C))

EXTRA_OBJS = $(addprefix src/, $(OBJS_LUA))

EXTRA_CLEAN = pllua_functable.h plerrcodes.h \
	$(addprefix src/,$(OBJS_LUA:.o=.luac)) $(EXTRA_OBJS) \
	logo.css tmpdoc.html icon-16.png icon.meta $(DOC_HTML)

PG_CPPFLAGS = -I$(LUA_INCDIR) $(PLLUA_CONFIG_OPTS)

SHLIB_LINK = $(EXTRA_OBJS) $(LUALIB)

# if VPATH is not already set, but the makefile is not in the current
# dir, then assume a vpath build using the makefile's directory as
# source. PGXS will set $(srcdir) accordingly.
ifndef VPATH
ifneq ($(realpath $(CURDIR)),$(realpath $(dir $(firstword $(MAKEFILE_LIST)))))
VPATH := $(dir $(firstword $(MAKEFILE_LIST)))
endif
endif

# actually load pgxs

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# definitions that must follow pgxs

ifeq ($(filter-out 7.% 8.% 9.0 9.1 9.2 9.3 9.4, $(MAJORVERSION)),)
$(error unsupported PostgreSQL version)
endif

$(OBJS): $(addprefix $(srcdir)/src/, $(INCS))

# for a vpath build, we need src/ to exist in the build dir before
# building any objects.
ifdef VPATH
all: vpath-mkdirs
.PHONY: vpath-mkdirs
$(OBJS) $(EXTRA_OBJS): | vpath-mkdirs

vpath-mkdirs:
	$(MKDIR_P) $(objdir)
endif # VPATH

all: $(DOCS)

# explicit deps on generated includes
src/init.o: pllua_functable.h
src/error.o: plerrcodes.h

$(shlib): $(EXTRA_OBJS)

%.luac: %.lua
	$(LUAC) -o $@ $<

ifeq ($(PORTNAME),darwin)
# Apple of course has to do its own thing when it comes to object file
# format and linker options.

_stub.c:
	touch $@

%.o: %.luac _stub.o
	$(LD) -r -sectcreate binary $(subst .,_,$(<F)) $< _stub.o -o $@

else
# The objcopy here is optional, it's just cleaner for the loaded data
# to be in a readonly section. So we ignore errors on it.

%.o: %.luac
	$(BIN_LD) -o $@ $<
	-$(OBJCOPY) --rename-section .data=.rodata,contents,alloc,load,readonly $@
endif

pllua_functable.h: $(SRCS_C) $(srcdir)/tools/functable.lua
	$(LUA) $(srcdir)/tools/functable.lua $(SRCS_C) >$@

ifneq ($(filter-out 9.% 10, $(MAJORVERSION)),)

#in pg 11+, we can get the server's errcodes.txt.
plerrcodes.h: $(datadir)/errcodes.txt $(srcdir)/tools/errcodes.lua
	$(LUA) $(srcdir)/tools/errcodes.lua $(datadir)/errcodes.txt >plerrcodes.h

else

plerrcodes.h: $(srcdir)/src/plerrcodes_old.h
	cp $(srcdir)/src/plerrcodes_old.h plerrcodes.h

endif

installcheck-parallel: submake $(REGRESS_PREP)
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS_PARALLEL)

logo.css: $(srcdir)/doc/logo.svg $(srcdir)/tools/logo.lua
	$(LUA) $(srcdir)/tools/logo.lua -text -logo $(srcdir)/doc/logo.svg >$@

# Stripped PNGs are quite a bit smaller than .ico
#icon.ico: $(srcdir)/doc/logo.svg
#	convert -size 256x256 -background transparent $(srcdir)/doc/logo.svg \
#		-format ico -define icon:auto-resize=32,16 icon.ico

icon-16.png: $(srcdir)/doc/logo.svg
	convert -size 16x16 -background transparent $(srcdir)/doc/logo.svg \
		-format png -define png:format=png32 \
		-define png:exclude-chunk=all icon-16.png

icon.meta: icon-16.png
	$(LUA) $(srcdir)/tools/logo.lua -binary -icon="16x16" icon-16.png >$@

$(DOC_HTML): $(DOC_SRCS) $(srcdir)/doc/template.xsl $(srcdir)/tools/doc.sh
	$(srcdir)/tools/doc.sh $(DOC_SRCS) >tmpdoc.html
	xsltproc --encoding utf-8 $(srcdir)/doc/template.xsl tmpdoc.html >$@
	rm -- tmpdoc.html
