
#include "pllua.h"

static void pllua_common_lua_init(lua_State *L)
{
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	luaL_checkstack(L, 20, NULL);
}

int pllua_call_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	pllua_validate_and_push(L, fcinfo->flinfo, act->trusted);
	
	/* XXX push the args at some point here */

	lua_call(L, 0, 0);

	fcinfo->isnull = true;
	act->retval = (Datum)0;
	return 0;
}

int pllua_call_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	fcinfo->isnull = true;
	act->retval = (Datum)0;
	return 0;
}

int pllua_call_event_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);
		
	return 0;
}

int pllua_call_inline(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	return 0;
}

int pllua_validate(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);
		
	return 0;
}
