/* globals.c */

#include "pllua.h"

bool pllua_ending = false;

pllua_context_type pllua_context = PLLUA_CONTEXT_PG;

/*
 * Addresses used as lua registry or table keys
 *
 * Note the key is the address, not the string; the string is only for
 * diagnostic purposes.
 */

char PLLUA_MEMORYCONTEXT[] = "memory context";
char PLLUA_ERRORCONTEXT[] = "error memory context";
char PLLUA_INTERP[] = "interp";
char PLLUA_FUNCS[] = "funcs";
char PLLUA_ACTIVATIONS[] = "activations";
char PLLUA_TYPES[] = "types";
char PLLUA_RECORDS[] = "records";
char PLLUA_PORTALS[] = "cursors";
char PLLUA_TRUSTED[] = "trusted";
char PLLUA_USERID[] = "userid";
char PLLUA_LANG_OID[] = "language oid";
char PLLUA_FUNCTION_OBJECT[] = "function object";
char PLLUA_ERROR_OBJECT[] = "error object";
char PLLUA_IDXLIST_OBJECT[] = "idxlist object";
char PLLUA_ACTIVATION_OBJECT[] = "activation object";
char PLLUA_TYPEINFO_OBJECT[] = "typeinfo object";
char PLLUA_TYPEINFO_PACKAGE_OBJECT[] = "typeinfo package object";
char PLLUA_TYPEINFO_PACKAGE_ARRAY_OBJECT[] = "typeinfo package array object";
char PLLUA_TUPCONV_OBJECT[] = "tupconv object";
char PLLUA_TRIGGER_OBJECT[] = "trigger object";
char PLLUA_SPI_STMT_OBJECT[] = "SPI statement object";
char PLLUA_SPI_CURSOR_OBJECT[] = "SPI cursor object";
char PLLUA_LAST_ERROR[] = "last error";
char PLLUA_RECURSIVE_ERROR[] = "recursive error";
char PLLUA_FUNCTION_MEMBER[] = "function element";
char PLLUA_THREAD_MEMBER[] = "thread element";
char PLLUA_TRUSTED_SANDBOX[] = "sandbox";
char PLLUA_TRUSTED_SANDBOX_LOADED[] = "sandbox loaded modules";
char PLLUA_TRUSTED_SANDBOX_ALLOW[] = "sandbox allowed modules";

#if LUA_VERSION_NUM == 501
/*
 * Lua compat funcs
 */
void pllua_setfuncs(lua_State *L, const luaL_Reg *l, int nup)
{
	luaL_checkstack(L, nup+1, "too many upvalues");
	for (; l->name != NULL; l++) {  /* fill the table with given functions */
		int i;
		lua_pushstring(L, l->name);
		for (i = 0; i < nup; i++)  /* copy upvalues to the top */
			lua_pushvalue(L, -(nup + 1));
		lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
		lua_settable(L, -(nup + 3)); /* table must be below the upvalues, the name and the closure */
	}
	lua_pop(L, nup);  /* remove upvalues */
}
int pllua_getsubtable(lua_State *L, int i, const char *name)
{
	int abs_i = lua_absindex(L, i);
	luaL_checkstack(L, 3, "not enough stack slots");
	lua_pushstring(L, name);
	lua_gettable(L, abs_i);
	if (lua_istable(L, -1))
		return 1;
	lua_pop(L, 1);
	lua_newtable(L);
	lua_pushstring(L, name);
	lua_pushvalue(L, -2);
	lua_settable(L, abs_i);
	return 0;
}
void pllua_requiref(lua_State *L, const char *modname,
					lua_CFunction openf, int glb)
{
	luaL_checkstack(L, 3, "not enough stack slots available");
	luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
	if (lua_getfield(L, -1, modname) == LUA_TNIL) {
		lua_pop(L, 1);
		lua_pushcfunction(L, openf);
		lua_pushstring(L, modname);
		lua_call(L, 1, 1);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, modname);
	}
	if (glb) {
		lua_pushvalue(L, -1);
		lua_setglobal(L, modname);
	}
	lua_replace(L, -2);
}
#endif
