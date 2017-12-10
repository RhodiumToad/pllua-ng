/* error.c */

#include "pllua.h"


/*
 * Only used in interpreter startup.
 */
int
pllua_panic(lua_State *L)
{
	elog(pllua_context == PLLUA_CONTEXT_PG ? ERROR : PANIC,
		 "Uncaught Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	return 0;
}

/*
 * Some places will downgrade errors in very rare circumstances, but we need to
 * know if this happens.
 */
void
pllua_poperror(lua_State *L)
{
	elog(WARNING,
		 "Ignored Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	lua_pop(L, 1);
}

/*
 * Create a new error object and record it as entering the error system.
 *
 * lightuserdata param is expected to be an ErrorData.
 */
int
pllua_newerror(lua_State *L)
{
	void		*p = lua_touserdata(L, 1);
	pllua_newrefobject(L, PLLUA_ERROR_OBJECT, p, false);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
	return 1;
}

/*
 * Execute a Lua protected call without rethrowing any error caught. The caller
 * must arrange the rethrow themselves; this is used when some cleanup is
 * needed between catch and rethrow.
 */
int
pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh)
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
void
pllua_rethrow_from_lua(lua_State *L, int rc)
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
 * PLLUA_TRY();
 * {
 * }
 * PLLUA_CATCH_RETHROW();
 *
 * There had better be space on the Lua stack for a couple of objects - it's
 * the caller's responsibility to deal with that.
 *
 * Remember the correct protocol for volatile variables! Any variable modified
 * in the try block and used in the catch block should be declared volatile,
 * but for pointer variables remember that it's the _variable_ which is
 * volatile, not the data pointed at. So declarations typically look like
 *
 *   ErrorData *volatile edata;
 *
 * (and NOT like  volatile ErrorData *edata)
 *
 * (This code is cautious about volatility issues and tends to use it much more
 * often than actually needed.)
 */
void
pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt)
{
	MemoryContext emcxt;
	ErrorData *volatile edata = NULL;

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

/*
 * Various workarounds for how to get into Lua without throwing error.
 *
 * We need these for initial entry into lua context, and also we use them when
 * entering lua context from callbacks from PG (e.g. cache management or memory
 * context reset).
 *
 * We can only safely pass 1 arg, which will be a light userdata.
 *
 * Annoyingly, the code needed for this changes with Lua version, since which
 * functions can throw errors changes. Specifically, on 5.1, neither
 * lua_checkstack nor lua_pushcfunction are safe, but lua_cpcall is; on 5.2 or
 * later, lua_cpcall may be missing, but lua_checkstack and lua_pushcfunction
 * are supposed to be safe. Unfortunately a bug in (at least) 5.3.4 allows
 * lua_pushcfunction to throw error anyway; so we currently take the worst-case
 * assumption and rely on lua_rawgetp being safe instead. (See definition of
 * macro pllua_pushcfunction in pllua.h.)
 */
#if LUA_VERSION_NUM > 501
int
pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	pllua_pushcfunction(L, func); /* can't throw */
	lua_pushlightuserdata(L, arg); /* can't throw */
	rc = lua_pcall(L, 1, 0, 0);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	pllua_setcontext(oldctx);
	return rc;
}
#else
int
pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
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

/*
 * Wrap lua_pcall to maintain our context tracking and to rethrow all errors
 * caught. This is the usual way to call lua-context code from within a PG
 * catch block.
 */
void
pllua_pcall(lua_State *L, int nargs, int nresults, int msgh)
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
 */
void
pllua_initial_protected_call(pllua_interpreter *interp,
							 lua_CFunction func,
							 pllua_activation_record *arg)
{
	sigjmp_buf *cur_catch_block PG_USED_FOR_ASSERTS_ONLY = PG_exception_stack;
	pllua_activation_record save_activation = interp->cur_activation;
	int rc;

	Assert(pllua_context == PLLUA_CONTEXT_PG);

#if LUA_VERSION_NUM > 501
	if (!lua_checkstack(interp->L, 5))
		elog(ERROR, "pllua: out of memory error on stack setup");
#endif

	interp->cur_activation = *arg;  /* copies content not pointer */

	rc = pllua_cpcall(interp->L, func, &interp->cur_activation);

	/*
	 * We better not have longjmp'd past any pg catch blocks.
	 */
	Assert(cur_catch_block == PG_exception_stack);

	*arg = interp->cur_activation;  /* copies content not pointer */
	interp->cur_activation = save_activation;

	if (rc)
		pllua_rethrow_from_lua(interp->L, rc);
}


/*
 * Finalizer for error objects.
 */
static int pllua_errobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_ERROR_OBJECT);
	void *obj = p ? *p : NULL;
	*p = NULL;
	if (obj)
	{
		PLLUA_TRY();
		{
			FreeErrorData(obj);
		}
		PLLUA_CATCH_RETHROW();
	}
	return 0;
}

/*
 * Replace the user-visible "pcall" and "xpcall" functions with
 * versions that catch lua errors but not pg errors.
 *
 * For now, we don't try and catch lua errors that got promoted to pg errors by
 * being thrown through a PG_CATCH block, though perhaps we could. Eventually
 * subtransaction handling will have to go here so leave that for now. (But
 * even with subxacts, there might be a place for a "light" pcall that doesn't
 * block yields, whereas the subxact handling obviously must do that.)
 *
 * These are lightly tweaked versions of the luaB_ functions.
 */

/*
** Continuation function for 'pcall' and 'xpcall'. Both functions
** already pushed a 'true' before doing the call, so in case of success
** 'finishpcall' only has to return everything in the stack minus
** 'extra' values (where 'extra' is exactly the number of items to be
** ignored).
*/
static int finishpcall (lua_State *L, int status, lua_KContext extra) {
  if (status != LUA_OK && status != LUA_YIELD) {  /* error? */
    lua_pushboolean(L, 0);  /* first result (false) */
    lua_pushvalue(L, -2);  /* error message */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
		pllua_rethrow_from_lua(L, status);
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L) - (int)extra;  /* return all results */
}

int pllua_t_pcall (lua_State *L) {
  int status;
  luaL_checkany(L, 1);
  lua_pushboolean(L, 1);  /* first result if no errors */
  lua_insert(L, 1);  /* put it in place */
  status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finishpcall);
  return finishpcall(L, status, 0);
}

/*
** Do a protected call with error handling. After 'lua_rotate', the
** stack will have <f, err, true, f, [args...]>; so, the function passes
** 2 to 'finishpcall' to skip the 2 first values when returning results.
*/
int pllua_t_xpcall (lua_State *L) {
  int status;
  int n = lua_gettop(L);
  luaL_checktype(L, 2, LUA_TFUNCTION);  /* check error function */
  lua_pushboolean(L, 1);  /* first result */
  lua_pushvalue(L, 1);  /* function */
  lua_rotate(L, 3, 2);  /* move them below function's arguments */
  status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishpcall);
  return finishpcall(L, status, 2);
}

/*
 * module init
 */
static struct luaL_Reg errobj_mt[] = {
	{ "__gc", pllua_errobject_gc },
	{ NULL, NULL }
};

static struct luaL_Reg errfuncs[] = {
	{ "pcall", pllua_t_pcall },
	{ "xpcall", pllua_t_xpcall },
	{ NULL, NULL }
};

void pllua_init_error(lua_State *L)
{
	pllua_newmetatable(L, PLLUA_ERROR_OBJECT, errobj_mt);
	lua_pop(L, 1);

	lua_pushcfunction(L, pllua_newerror);
	lua_pushlightuserdata(L, NULL);
	lua_call(L, 1, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);

	lua_pushglobaltable(L);
	luaL_setfuncs(L, errfuncs, 0);
	lua_pop(L,1);
}
