/* pllua_luajit.h */

#ifndef PLLUA_LUAJIT_H
#define PLLUA_LUAJIT_H

#include <luajit.h>

#include "pllua_luaver.h"

/*
 * Parts of the lua 5.1 compatibility cruft here is derived from the
 * lua-compat-5.3 project, which is licensed under the same terms as this
 * project and carries the following copyright:
 *
 * Copyright (c) 2015 Kepler Project.
 */
extern const char *luaL_tolstring(lua_State *L, int nd, size_t *len);

static inline int lua_absindex(lua_State *L, int nd)
{
	return (nd < 0 && nd > LUA_REGISTRYINDEX) ? nd + lua_gettop(L) + 1 : nd;
}

static inline int lua_rawgetp(lua_State *L, int nd, void *p)
{
	int tnd = lua_absindex(L, nd);
	lua_pushlightuserdata(L, p);
	lua_rawget(L, tnd);
	return lua_type(L, -1);
}

static inline void lua_rawsetp(lua_State *L, int nd, void *p)
{
	int tnd = lua_absindex(L, nd);
	lua_pushlightuserdata(L, p);
	lua_insert(L, -2);
	lua_rawset(L, tnd);
}

static inline int lua_geti(lua_State *L, int nd, lua_Integer i)
{
	int tnd = lua_absindex(L, nd);
	lua_pushinteger(L, i);
	lua_gettable(L, tnd);
	return lua_type(L, -1);
}

static inline void lua_seti(lua_State *L, int nd, lua_Integer i)
{
	int tnd = lua_absindex(L, nd);
	lua_pushinteger(L, i);
	lua_insert(L, -2);
	lua_settable(L, tnd);
}

#define lua_rawgeti(L_,nd_,i_) ((lua_rawgeti)(L_,nd_,i_),lua_type(L_, -1))

#define lua_rawget(L_,nd_) ((lua_rawget)(L_,nd_),lua_type(L_, -1))

#define lua_getfield(L_,nd_,i_) ((lua_getfield)(L_,nd_,i_),lua_type(L_, -1))

#define lua_gettable(L_,nd_) ((lua_gettable)(L_,nd_),lua_type(L_, -1))

#define luaL_getmetafield(L_,nd_,f_) ((luaL_getmetafield)(L_,nd_,f_) ? lua_type(L_, -1) : LUA_TNIL)

/* luajit 2.1's version of this one is ok. */
#if LUAJIT_VERSION_NUM < 20100
static inline lua_Number lua_tonumberx(lua_State *L, int i, int *isnum)
{
	lua_Number n = lua_tonumber(L, i);
	if (isnum != NULL) {
		*isnum = (n != 0 || lua_isnumber(L, i));
	}
	return n;
}
#endif

/*
 * but for these we need to kill luajit's version and use ours: we depend on
 * isinteger/tointegerx/checkinteger accepting only actually integral values,
 * not rounded/truncated floats.
 */
#if LUAJIT_VERSION_NUM >= 20100
#define lua_isinteger pllua_isinteger
#define lua_tointegerx pllua_tointegerx
#endif

/* no luajit version of these is usable: */
#define luaL_checkinteger pllua_checkinteger
#define luaL_optinteger pllua_optinteger

static inline bool lua_isinteger(lua_State *L, int nd)
{
	if (lua_type(L, nd) == LUA_TNUMBER)
	{
		lua_Number n = lua_tonumber(L, nd);
		lua_Integer i = lua_tointeger(L, nd);
		if (i == n)
			return 1;
	}
	return 0;
}

static inline lua_Integer lua_tointegerx(lua_State *L, int i, int *isint)
{
	lua_Integer n = lua_tointeger(L, i);
	if (isint != NULL) {
		int isnum = 0;
		/* be careful, it might not be a number at all */
		if (n == lua_tonumberx(L, i, &isnum))
			*isint = isnum;
		else
			*isint = 0;
	}
	return n;
}

static inline lua_Integer luaL_checkinteger(lua_State *L, int i)
{
	int isint = 0;
	lua_Integer res = lua_tointegerx(L, i, &isint);
	if (!isint)
		luaL_argerror(L, i, "integer");
	return res;
}

static inline lua_Integer luaL_optinteger(lua_State *L, int i, lua_Integer def)
{
	if (lua_isnoneornil(L, i))
		return def;
	else
		return luaL_checkinteger(L, i);
}

/*
 * Miscellaneous functions
 */
#define lua_getuservalue(L_,nd_) (lua_getfenv(L_,nd_), lua_type(L_,-1))
#define lua_setuservalue(L_,nd_) lua_setfenv(L_,nd_)

#define lua_pushglobaltable(L_) lua_pushvalue((L_), LUA_GLOBALSINDEX)

#if LUAJIT_VERSION_NUM < 20100
#define luaL_setfuncs(L_,f_,u_) pllua_setfuncs(L_,f_,u_)
void pllua_setfuncs(lua_State *L, const luaL_Reg *reg, int nup);
#endif

#define luaL_getsubtable(L_,i_,n_) pllua_getsubtable(L_,i_,n_)
int pllua_getsubtable(lua_State *L, int i, const char *name);

#define luaL_requiref(L_,m_,f_,g_) pllua_requiref(L_,m_,f_,g_)
void pllua_requiref(lua_State *L, const char *modname, lua_CFunction openf, int glb);

#ifndef luaL_newlibtable
#define luaL_newlibtable(L, l)							\
  (lua_createtable((L), 0, sizeof((l))/sizeof(*(l))-1))
#define luaL_newlib(L, l) \
  (luaL_newlibtable((L), (l)), luaL_register((L), NULL, (l)))
#endif

#define LUA_OK 0
/*
 * these should be the largest and smallest integers representable exactly in a
 * Lua number; must be compile-time constant
 */
#define LUA_MAXINTEGER (INT64CONST(9007199254740991))
#define LUA_MININTEGER (-INT64CONST(9007199254740991))

#endif
