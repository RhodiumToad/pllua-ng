
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
	FunctionCallInfo fcinfo = act->fcinfo;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	pllua_func_activation *fact = fcinfo->flinfo->fn_extra;
	lua_State *thr = fact->thread;
	int rc;

	Assert(thr != NULL);
	Assert(lua_gettop(L) == 1);

	rc = lua_resume(thr, L, 0);
	
	if (rc == LUA_OK)
	{
		pllua_deactivate_thread(L, fact, rsi->econtext);
		rsi->isDone = ExprEndResult;
		act->retval = (Datum)0;
		fcinfo->isnull = true;
		return 0;
	}
	else if (rc == LUA_YIELD)
	{
		lua_xmove(thr, L, lua_gettop(thr));
		/* leave thread active */
		rsi->isDone = ExprMultipleResult;
			
		/* drop out to normal result processing */
	}
	else
	{
		lua_xmove(thr, L, 1);
		pllua_deactivate_thread(L, fact, rsi->econtext);
		pllua_rethrow_from_lua(L, rc);
	}

	act->retval = pllua_return_result(L, lua_gettop(L) - 1,
									  fact->func_info,
									  &fcinfo->isnull);

	return 0;
}

int pllua_call_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	pllua_function_info *func_info;
	int nstack;
	int nargs;
	int rc;

	pllua_common_lua_init(L);

	/* pushes the activation on the stack */
	func_info = pllua_validate_and_push(L,
										fcinfo->flinfo,
										rsi,
										act->trusted);

	/* stack mark for result processing */
	nstack = lua_gettop(L);
	Assert(nstack == 2);

	/* get the function object from the activation and push that */
	pllua_activation_getfunc(L);

	/* func should be the only thing on the stack after the arg*/
	Assert(lua_gettop(L) == nstack + 1);

	/* XXX push the args at some point here */
	nargs = 0;
	
	if (func_info->retset)
	{
		/*
		 * This is the initial call into a SRF. We already registered the
		 * activation into the exprcontext (in validate_and_push above), but we
		 * haven't made a thread for it yet. We have to do that, install it
		 * into the activation, move the func and parameters over to the new
		 * thread and resume it.
		 */
		lua_State *thr = pllua_activate_thread(L, nstack, rsi->econtext);
		lua_xmove(L, thr, nargs + 1);  /* args plus function */
		rc = lua_resume(thr, L, nargs);

		/*
		 * If we got LUA_OK, the function returned without yielding. If it
		 * returned a result, then we treat it exactly as if it had been a
		 * non-SRF call. If it returned no result, then we treat it as 0 rows.
		 *
		 * If we get LUA_YIELD, we expect a result on the "thr" stack, and we
		 * notify the caller that this is a multiple result (further rows are
		 * handled in pllua_resume_func).
		 *
		 * If we got anything else, the function threw an error, which we
		 * propagate.
		 */
		if (rc == LUA_OK)
		{
			int nret = lua_gettop(thr);
			
			lua_xmove(thr, L, nret);

			pllua_deactivate_thread(L, fcinfo->flinfo->fn_extra, rsi->econtext);

			if (nret == 0)
			{
				rsi->isDone = ExprEndResult;
				act->retval = (Datum)0;
				fcinfo->isnull = true;
				return 0;
			}

			/* drop out to normal result processing */
		}
		else if (rc == LUA_YIELD)
		{
			lua_xmove(thr, L, lua_gettop(thr));
			/* leave thread active */
			rsi->isDone = ExprMultipleResult;
			
			/* drop out to normal result processing */
		}
		else
		{
			lua_xmove(thr, L, 1);
			pllua_deactivate_thread(L, fcinfo->flinfo->fn_extra, rsi->econtext);
			pllua_rethrow_from_lua(L, rc);
		}
	}
	else
	{
		lua_call(L, 0, 1);
	}

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
