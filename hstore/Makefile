# Makefile for PL/Lua + hstore

PG_CONFIG ?= pg_config

# Lua specific

# General
LUA_INCDIR ?= /usr/local/include/lua53
#LUALIB ?= -L/usr/local/lib -llua-5.3

# no need to edit below here

MODULE_big = hstore_pllua

EXTENSION = hstore_pllua hstore_plluau

DATA = hstore_pllua--1.0.sql hstore_plluau--1.0.sql

OBJS =	hstore_pllua.o

REGRESS = create_ext hstore

# MAJORVERSION and includedir_server are not defined yet, but will be
# defined before PG_CPPFLAGS is expanded

# for pg11+, hstore.h will have been installed
HSTORE_INCDIR = $(includedir_server)/extension/hstore

# for pg 9.5/9.6/10, we have a local copy of hstore.h since it happens
# to be the same, barring non-semantic whitespace, between the three
# versions
HSTORE_INCDIR_OLD = old_inc

HSTORE_INC = -I$(HSTORE_INCDIR$(if $(filter 9.% 10,$(MAJORVERSION)),_OLD))

PG_CPPFLAGS = -I$(LUA_INCDIR) -I$(srcdir)/../src $(HSTORE_INC) #-DPLLUA_DEBUG

SHLIB_LINK = $(LUALIB)

# if VPATH is not already set, but the makefile is not in the current
# dir, then assume a vpath build using the makefile's directory as
# source. PGXS will set $(srcdir) accordingly.
ifndef VPATH
ifneq ($(CURDIR),$(dir $(firstword $(MAKEFILE_LIST))))
VPATH := $(dir $(firstword $(MAKEFILE_LIST)))
endif
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifeq ($(filter-out 7.% 8.% 9.0 9.1 9.2 9.3 9.4, $(MAJORVERSION)),)
$(error unsupported PostgreSQL version)
endif

