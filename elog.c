
#include "pllua.h"

static void pllua_info(lua_State *L, const char *str)
{
	MemoryContext oldmcxt = CurrentMemoryContext;
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_PG);
	
	PG_TRY();
	{
		ereport(INFO,
				(errmsg_internal("%s", str)));
	}
	PG_CATCH();
	{
		pllua_setcontext(oldctx);
		pllua_rethrow_from_pg(L, oldmcxt);
	}
	PG_END_TRY();
	pllua_setcontext(oldctx);
}

int pllua_p_print (lua_State *L)
{
	int i, n = lua_gettop(L); /* nargs */
	int fidx;
	const char *s;
	luaL_Buffer b;

	lua_getglobal(L, "tostring");
	fidx = lua_absindex(L, -1);
	
	luaL_buffinit(L, &b);

	for (i = 1; i <= n; i++)
	{
		lua_pushvalue(L, fidx); /* tostring */
		lua_pushvalue(L, i); /* arg */
		lua_call(L, 1, 1);
		s = lua_tostring(L, -1);
		if (s == NULL)
			return luaL_error(L, "cannot convert to string");
		if (i > 1) luaL_addchar(&b, '\t');
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	s = lua_tostring(L, -1);
	pllua_info(L, s);
	return 0;
}

