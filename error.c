
#include "pllua.h"


int pllua_panic(lua_State *L)
{
	elog(pllua_context == PLLUA_CONTEXT_PG ? ERROR : PANIC,
		 "Uncaught Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	return 0;
}

void pllua_poperror(lua_State *L)
{
	elog(WARNING,
		 "Ignored Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	lua_pop(L, 1);
}

int pllua_newerror(lua_State *L)
{
	void *p = lua_touserdata(L, 1);
	pllua_newrefobject(L, PLLUA_ERROR_OBJECT, p);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
	return 1;
}

int pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	
	return rc;
}

/*
 * Having caught an error in lua, which is now on top of the stack (as returned
 * by pcall), rethrow it either back into lua or into pg according to what
 * context we're now in.
 */
void pllua_rethrow_from_lua(lua_State *L, int rc)
{
	if (pllua_context == PLLUA_CONTEXT_LUA)
		lua_error(L);

	/*
	 * If out of memory, avoid doing anything even slightly fancy.
	 */
	
	if (rc == LUA_ERRMEM)
	{
		lua_pop(L, -1);
		elog(ERROR, "pllua: out of memory");
	}

	/*
	 * The thing on top of the stack is either a lua object with a pg error, a
	 * string, or something else.
	 */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
	{
		ErrorData **p = lua_touserdata(L, -1);
		ErrorData *edata = *p;

		/*
		 * safe to pop the object since it should still be referenced from the
		 * registry
		 */
		lua_pop(L, -1);

		if (edata)
			ReThrowError(edata);
		else
			elog(ERROR, "recursive error in Lua error handling");
	}
	
	ereport(ERROR,
			(errmsg_internal("pllua: %s",
							 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(error is not a string)")),
			 (lua_pop(L, 1), 1)));
}

/*
 * Having caught an error in PG_CATCH, rethrow it either into lua or back into
 * pg according to what context we're now in. Note, we must have restored the
 * previous context; the idiom is:
 *
 * oldmcxt = CurrentMemoryContext;
 * oldctx = pllua_setcontext(PLLUA_CONTEXT_PG);
 * PG_TRY();
 * { ... must not modify oldmcxt or oldctx ... }
 * PG_CATCH();
 * {
 *   pllua_setcontext(oldctx);
 *   pllua_rethrow_from_pg(L, oldmcxt);
 * }
 * PG_END_TRY();
 * pllua_setcontext(oldctx);
 *
 * There had better be space on the Lua stack for a couple of objects - it's
 * the caller's responsibility to deal with that.
 */
void pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt)
{
	MemoryContext emcxt;
	volatile ErrorData *edata = NULL;
	
	if (pllua_context == PLLUA_CONTEXT_PG)
		PG_RE_THROW();

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);
	emcxt = lua_touserdata(L, -1);
	lua_pop(L, 1);
	MemoryContextSwitchTo(emcxt);
	
	/*
	 * absorb the error and exit pg's error handling.
	 *
	 * Unfortunately, CopyErrorData can fail, and we can't afford to rethrow
	 * via pg since we're in lua context, so we have to catch that.
	 * FlushErrorState can fail too, but only if things are really badly
	 * screwed up (memory corruption in ErrorContext) so we can legitimately
	 * die if things get that far. (Honestly, the chances that elog won't
	 * already have recursed itself to death in that case are slim.)
	 */
	PG_TRY();
	{
		edata = CopyErrorData();
	}
	PG_CATCH();
	{
		/*
		 * recursive error, don't bother trying to figure out what.
		 */
		edata = NULL;
	}
	PG_END_TRY();
	
	PG_TRY();
	{
		FlushErrorState();
	}
	PG_CATCH();
	{
		elog(PANIC, "error recursion trouble: FlushErrorState failed");
	}
	PG_END_TRY();

	MemoryContextSwitchTo(mcxt);
		
	/*
	 * make a lua object to hold the error. This can throw an out of memory
	 * error from lua, but we don't want to let that supersede a pg error,
	 * because we sometimes want to let the user-supplied lua code catch lua
	 * errors but not pg errors. So if that happens, replace it with our
	 * special prebuilt "recursive error" object.
	 */

	if (edata)
	{
		pllua_pushcfunction(L, pllua_newerror);
		lua_pushlightuserdata(L, (void *) edata);
		if (pllua_pcall_nothrow(L, 1, 1, 0) != 0)
		{
			pllua_poperror(L);
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);
		}
	}
	else
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);

	lua_error(L);
}

#if LUA_VERSION_NUM > 501
int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	lua_pushcfunction(L, func); /* can't throw */
	lua_pushlightuserdata(L, arg); /* can't throw */
	rc = lua_pcall(L, 1, 0, 0);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	return rc;
}
#else
int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_cpcall(L, func, arg);
	
	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	return rc;
}
#endif

void pllua_pcall(lua_State *L, int nargs, int nresults, int msgh)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	
	if (rc)
		pllua_rethrow_from_lua(L, rc);
}

/*
 * We store a lot of our state inside lua for convenience, but that means
 * we have to consider possible lua errors (e.g. out of memory) happening
 * even outside the actual function call. So, arrange to do all the work in
 * a protected environment to catch any such errors.
 *
 * We can only safely pass 1 arg, which will be a light userdata.
 *
 * Annoyingly, the code needed for this changes with Lua version, since which
 * functions can throw errors changes.  Specifically, on 5.1, neither
 * lua_checkstack nor lua_pushcfunction are safe, but lua_cpcall is; on 5.2 or
 * later, lua_cpcall may be missing, but lua_checkstack and lua_pushcfunction
 * are safe.
 */

void pllua_initial_protected_call(lua_State *L,
										 lua_CFunction func,
										 void *arg)
{
	sigjmp_buf *cur_catch_block PG_USED_FOR_ASSERTS_ONLY = PG_exception_stack;
	int rc;

	Assert(pllua_context == PLLUA_CONTEXT_PG);

#if LUA_VERSION_NUM > 501
	if (!lua_checkstack(L, 5))
		elog(ERROR, "pllua: out of memory error on stack setup");
#endif

	rc = pllua_cpcall(L, func, arg);

	/*
	 * We better not have longjmp'd past any pg catch blocks.
	 */
	Assert(cur_catch_block == PG_exception_stack);
	
	if (rc)
		pllua_rethrow_from_lua(L, rc);
}	


static int pllua_errobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_ERROR_OBJECT);
	void *obj = *p;
	*p = NULL;
	if (obj)
		FreeErrorData(obj);
	return 0;
}

static struct luaL_Reg errobj_mt[] = {
	{ "__gc", pllua_errobject_gc },
	{ NULL, NULL }
};

void pllua_init_error(lua_State *L)
{
	pllua_newmetatable(L, PLLUA_ERROR_OBJECT, errobj_mt);
	
	lua_pushlightuserdata(L, NULL);
	pllua_newerror(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);
}
