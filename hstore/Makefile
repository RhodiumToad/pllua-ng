# Makefile for PL/Lua + hstore

PG_CONFIG ?= pg_config

# unfortunately, this needs access to a PG source tree to get hstore.h,
# which is never installed anywhere
HSTORE_SRC ?= ..

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
MODULE_big = hstore_pllua
EXTENSION = hstore_pllua hstore_plluau
DATA = hstore_pllua--1.0.sql hstore_plluau--1.0.sql

OBJS =	hstore_pllua.o

REGRESS = hstore

PG_CPPFLAGS = -I$(LUA_INCDIR) -I../src -I$(HSTORE_SRC) #-DPLLUA_DEBUG
SHLIB_LINK = $(LUALIB)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifeq ($(filter-out 7.% 8.% 9.0 9.1 9.2 9.3 9.4, $(MAJORVERSION)),)
$(error unsupported PostgreSQL version)
endif

