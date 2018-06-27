/* preload.c */

/*
 * Preloading of lua modules built into the binary.
 */

#include "pllua.h"

extern const char _binary_src_compat_luac_start[];
extern const char _binary_src_compat_luac_end[];

static void
pllua_load_binary_chunk(lua_State *L, const char *chunkname, const char *start, const char *end)
{
	int rc = luaL_loadbufferx(L, start, (size_t)(end - start), chunkname, "b");
	if (rc)
		lua_error(L);
}

/*
 * Upvalue 1 is the metatable to use with the environment
 */
int
pllua_preload_compat(lua_State *L)
{
	pllua_load_binary_chunk(L, "compat.lua",
							_binary_src_compat_luac_start,
							_binary_src_compat_luac_end);
	lua_newtable(L);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);
	pllua_set_environment(L, -2);
	lua_call(L, 0, 1);
	return 1;
}
