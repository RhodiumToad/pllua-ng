
#include "pllua.h"

static void pllua_common_lua_init(lua_State *L)
{
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	luaL_checkstack(L, 20 + FUNC_MAX_ARGS, NULL);
}

static Datum pllua_return_result(lua_State *L, int nret,
						  pllua_function_info *func_info,
						  bool *isnull)
{
	/* XXX much work needed here. */

	if (nret > 0)
	{
		lua_Integer r = lua_tointeger(L, -1);
		*isnull = false;
		return Int32GetDatum(r);
	}

	*isnull = true;
	return (Datum)0;
}

/*
 * Resume an SRF in value-per-call mode (second and subsequent calls come here)
 */
int pllua_resume_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);

	/* XXX todo */

	return 0;
}

int pllua_call_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;
	pllua_function_info *func_info;
	int nstack;

	pllua_common_lua_init(L);

	nstack = lua_gettop(L);
	Assert(nstack == 1);
	
	func_info = pllua_validate_and_push(L,
										fcinfo->flinfo,
										(ReturnSetInfo *) fcinfo->resultinfo,
										act->trusted);

	/* func should be the only thing on the stack after the arg*/
	Assert(lua_gettop(L) == nstack + 1);
	
	/* XXX push the args at some point here */

	lua_call(L, 0, 1);

	/*
	 * func and args are popped by the call, so everything left is a function
	 * result. the func_info is not on the stack any more, but we know it must
	 * be referenced from the activation
	 */
	
	act->retval = pllua_return_result(L, lua_gettop(L) - nstack,
									  func_info,
									  &fcinfo->isnull);
	
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
