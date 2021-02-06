/* error.c */

#include "pllua.h"

#include "access/xact.h"
#include "utils/resowner.h"

/* errstart changed in pg13+. */
#if PG_VERSION_NUM >= 130000
#define pllua_errstart(elevel_) errstart((elevel_), TEXTDOMAIN)
#else
#define pllua_errstart(elevel_) \
	errstart((elevel_), __FILE__, __LINE__, PG_FUNCNAME_MACRO, TEXTDOMAIN)
#endif

bool pllua_pending_error = false;

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
	pllua_warning(L,
				  "Ignored Lua error: %s",
				  (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	lua_pop(L, 1);
}

/*
 * Something tried to switch to PG context with an error pending.
 */
void
pllua_pending_error_violation(lua_State *L)
{
	luaL_error(L, "cannot call into PostgreSQL with pending errors");
	pg_unreachable();
}


/*
 * Replacement for lua warn() function
 */
static int
pllua_t_warn(lua_State *L)
{
	const char *str;
	int nargs = lua_gettop(L);
	int i;

	luaL_checkstring(L, 1);  /* at least one argument */
	for (i = 2; i <= nargs; i++)
		luaL_checkstring(L, i);  /* make sure all arguments are strings */

	lua_concat(L, nargs);
	str = lua_tostring(L, 1);
	if (nargs == 1 && str && str[0] == '@')
		return 0;
	if (str)
		pllua_warning(L, "%s", str);
	return 0;
}

/*
 * Register an error object as being active. This can throw.
 *
 */
int
pllua_register_error(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp && interp->db_ready)
	{
		int oref = interp->cur_activation.active_error;
		/*
		 * do the ref call before the unref, so that if luaL_ref throws, the
		 * old value is unchanged and still valid
		 */
		lua_settop(L, 1);
		/* if we're in recursive error handling, then don't overwrite that. */
		if (oref == LUA_NOREF)
			return 0;
		/*
		 * if we're trying to register the current error, skip the luaL_ref
		 * call (which can throw in extreme cases)
		 */
		if (oref != LUA_REFNIL)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, oref);
			if (lua_rawequal(L, -1, -2))
				return 0;
		}
		interp->cur_activation.active_error = luaL_ref(L, LUA_REGISTRYINDEX);
		luaL_unref(L, LUA_REGISTRYINDEX, oref);
	}
	return 0;
}

static void
pllua_register_recursive_error(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, interp->cur_activation.active_error);
		interp->cur_activation.active_error = LUA_NOREF;
	}
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);
}

static void
pllua_deregister_error(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, interp->cur_activation.active_error);
		interp->cur_activation.active_error = LUA_REFNIL;
	}
}

static bool
pllua_get_active_error(lua_State *L)
{
	pllua_interpreter *interp = pllua_getinterpreter(L);
	if (interp && interp->cur_activation.active_error != LUA_REFNIL)
	{
		if (interp->cur_activation.active_error == LUA_NOREF)
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);
		else
			lua_rawgeti(L, LUA_REGISTRYINDEX, interp->cur_activation.active_error);
		return true;
	}
	return false;
}

/*
 * This is called as the last thing on the error path before rethrowing the
 * error completely back to pg (but note that we might still have catch blocks
 * on the stack from an outer nested activation, e.g. as a result of SPI
 * calls). Resetting errdepth keeps the error context traversal stuff working
 * in the event that the context traversal itself raises an error; we can also
 * safely remove any currently active PG error from the Lua registry because it
 * must have been copied into the PG error handling. (However, we have to do
 * this in a way that can't possibly throw a Lua error.)
 *
 * We can also reset the pending error flag here, since it's no longer our
 * problem, and we can't get back into Lua without catching the error further
 * up the stack which will set the flag again.
 */
void
pllua_error_cleanup(pllua_interpreter *interp, pllua_activation_record *act)
{
	interp->errdepth = 0;
	if (act->active_error != LUA_REFNIL)
	{
		/* luaL_unref is guaranteed to not throw */
		luaL_unref(interp->L, LUA_REGISTRYINDEX, act->active_error);
		act->active_error = LUA_REFNIL;
	}
	pllua_pending_error = false;
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
	lua_pushcfunction(L, pllua_register_error);
	lua_pushvalue(L, -2);
	lua_call(L, 1, 0);
	return 1;
}

/*
 * Create an ErrorData (allocated in the caller's memory context) for our
 * special "recursive error in error handling" error.
 *
 * This might itself throw a PG error, which is why it's essentially isolated
 * from the rest of the module; init.c calls it at a safe point before actually
 * entering Lua. Hence it doesn't even get a lua_State param.
 */

ErrorData *
pllua_make_recursive_error(void)
{
	ErrorData *volatile edata = NULL;

	/*
	 * The sole reason for a catch block is to ensure that even if we're called
	 * from postmaster, we don't promote the error to FATAL on account of
	 * having an empty catch stack.
	 */
	PG_TRY();
	{
		MemoryContext oldcontext = CurrentMemoryContext;

		if (!pllua_errstart(ERROR))
			elog(ERROR, "errstart tried to ignore ERROR");

		/* populate error data */
		errcode(ERRCODE_INTERNAL_ERROR);
		errmsg("Unexpected error in error handling");

		/* errstart switched to error context. switch back */
		MemoryContextSwitchTo(oldcontext);

		/* grab copy of error */
		edata = CopyErrorData();

		/* and flush it back out of the error subsystem */
		FlushErrorState();
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();

	return edata;
}

/*
 * Execute a Lua protected call without rethrowing any error caught. The caller
 * must arrange the rethrow themselves; this is used when some cleanup is
 * needed between catch and rethrow.
 */
int
pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh)
{
	pllua_context_type oldctx = pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	pllua_setcontext(NULL, oldctx);

	return rc;
}

/*
 * To be called in an ereport() parameter list; if the top of the lua stack is
 * a string, then pass it to errmsg_internal (which will copy it); if not,
 * supply a default message. Either way, pop the lua stack.
 */
static int
pllua_errmsg(lua_State *L)
{
	if (lua_type(L, -1) == LUA_TSTRING)
		errmsg_internal("pllua: %s", lua_tostring(L, -1));
	else
		errmsg_internal("pllua: (error is not a string: type=%d)", lua_type(L, -1));
	lua_pop(L, 1);
	return 0; /* ignored */
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
		lua_pop(L, 1);
		elog(ERROR, "pllua: out of memory");
	}

	/*
	 * The thing on top of the stack is either a lua object with a pg error, a
	 * string, or something else. If it's a pg error, ensure it is registered
	 * and rethrow it.
	 */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
	{
		ErrorData **p = lua_touserdata(L, -1);
		ErrorData *edata = *p;

		pllua_pushcfunction(L, pllua_register_error);
		lua_insert(L, -2);
		if (pllua_pcall_nothrow(L, 1, 0, 0) != 0)
		{
			pllua_poperror(L);
			pllua_register_recursive_error(L);
			p = lua_touserdata(L, -1);
			if (p && *p)
				edata = *p;
			/* safe to pop since the value is in the registry */
			lua_pop(L, 1);
		}

		if (edata)
			ReThrowError(edata);
		else
			elog(ERROR, "recursive error in Lua error handling");
	}

	ereport(ERROR,
			(pllua_errmsg(L)));
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

/*
 * Absorb a PG error leaving it on the Lua stack. Switches memory contexts!
 */
static ErrorData *
pllua_absorb_pg_error(lua_State *L)
{
	ErrorData *volatile edata = NULL;
	pllua_interpreter *interp = pllua_getinterpreter(L);
	MemoryContext emcxt = interp->emcxt;

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
			pllua_register_recursive_error(L);
		}
	}
	else
		pllua_register_recursive_error(L);

	return edata;
}

void
pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt)
{
	if (pllua_context == PLLUA_CONTEXT_PG)
		PG_RE_THROW();

	pllua_absorb_pg_error(L);

	/*
	 * We're going to hand off to Lua here, and we need to ensure that in the
	 * event that Lua code gets to execute (as can happen in 5.4 due to
	 * <close> variable handling), it can't call anything that might get back
	 * into PG. (We check this when switching to PG context - we assume that
	 * functions that can't throw a PG error are also safe to call in error
	 * handling.)
	 */
	pllua_pending_error = true;

	MemoryContextSwitchTo(mcxt);

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
	pllua_context_type oldctx;
	int rc;

	if (pllua_context == PLLUA_CONTEXT_PG)
	{
		if (!lua_checkstack(L, 3))
			elog(ERROR, "failed to extend Lua stack");
	}
	else
		luaL_checkstack(L, 3, NULL);

	oldctx = pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);

	pllua_pushcfunction(L, func); /* can't throw */
	lua_pushlightuserdata(L, arg); /* can't throw */
	rc = lua_pcall(L, 1, 0, 0);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	pllua_setcontext(NULL, oldctx);
	return rc;
}
#else
int
pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_cpcall(L, func, arg);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	pllua_setcontext(NULL, oldctx);
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
	pllua_context_type oldctx = pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	pllua_setcontext(NULL, oldctx);

	if (rc)
		pllua_rethrow_from_lua(L, rc);
}

/*
 * Supplementary modules like transforms need pllua_pcall functionality, but
 * have no way to register their addresses for pllua_pushcfunction. So provide
 * this trampoline so that they can use pushlightuserdata instead.
 */

/* DO NOT REMOVE THIS, IT'S NEEDED FOR pllua_functable.h */
int pllua_register_cfunc(L, pllua_trampoline)(lua_State *L);

int
pllua_trampoline(lua_State *L)
{
	lua_CFunction f = (lua_CFunction) lua_touserdata(L, 1);
	lua_pushcfunction(L, f);
	lua_replace(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	return lua_gettop(L);
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

	Assert(arg->active_error == LUA_REFNIL);
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
 * Replace the user-visible coroutine.resume function with a version that
 * propagates PG errors from the coroutine.
 */

static int pllua_t_coresume (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  int narg = lua_gettop(L) - 1;
  int nret;
  int rc;
  luaL_argcheck(L, co, 1, "thread expected");

  if (!lua_checkstack(co, narg)) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "too many arguments to resume");
    return 2;  /* error flag */
  }
  if (lua_status(co) == LUA_OK && lua_gettop(co) == 0) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "cannot resume dead coroutine");
    return 2;  /* error flag */
  }
  lua_xmove(L, co, narg);
  rc = lua_resume(co, L, narg, &nret);
  if (rc == LUA_OK || rc == LUA_YIELD) {
    if (!lua_checkstack(L, nret + 1)) {
      lua_pop(co, nret);  /* remove results anyway */
	  lua_pushboolean(L, 0);
      lua_pushliteral(L, "too many results to resume");
      return 2;  /* error flag */
    }
	lua_pushboolean(L, 1);
    lua_xmove(co, L, nret);  /* move yielded values */
    return nret + 1;
  }
  else {
    lua_pushboolean(L, 0);
    lua_xmove(co, L, 1);  /* move error message */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
		pllua_rethrow_from_lua(L, rc);
    return 2;  /* error flag */
  }
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

#if LUA_VERSION_NUM >= 502
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
	/*
	 * To plug the lxpcall hole (lxpcall's error handler throws a pg error,
	 * which lua transforms into "error in error handling"), we substitute the
	 * pg error in the registry if any.
	 */
	if (pllua_get_active_error(L))
		pllua_rethrow_from_lua(L, LUA_ERRERR);
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L) - (int)extra;  /* return all results */
}

int pllua_t_lpcall (lua_State *L) {
  int status;
  PLLUA_CHECK_PG_STACK_DEPTH();
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
**
** XXX XXX we need to intercept the error handling function somehow to
** ensure it doesn't mess with a pg error.
*/
int pllua_t_lxpcall (lua_State *L) {
  int status;
  int n = lua_gettop(L);
  PLLUA_CHECK_PG_STACK_DEPTH();
  luaL_checktype(L, 2, LUA_TFUNCTION);  /* check error function */
  lua_pushboolean(L, 1);  /* first result */
  lua_pushvalue(L, 1);  /* function */
  lua_rotate(L, 3, 2);  /* move them below function's arguments */
  status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishpcall);
  return finishpcall(L, status, 2);
}

#else

int pllua_t_lpcall (lua_State *L) {
  int status;
  PLLUA_CHECK_PG_STACK_DEPTH();
  luaL_checkany(L, 1);
  lua_pushboolean(L, 1);  /* first result if no errors */
  lua_insert(L, 1);  /* put it in place */
  status = lua_pcall(L, lua_gettop(L) - 2, LUA_MULTRET, 0);
  if (status) {  /* error? */
    lua_pushboolean(L, 0);  /* first result (false) */
    lua_pushvalue(L, -2);  /* error message */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
		pllua_rethrow_from_lua(L, status);
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L);  /* return all results */
}

/*
** XXX XXX we need to intercept the error handling function somehow to
** ensure it doesn't mess with a pg error.
*/
int pllua_t_lxpcall (lua_State *L) {
  int status;
  int n = lua_gettop(L);
  PLLUA_CHECK_PG_STACK_DEPTH();
  luaL_checktype(L, 2, LUA_TFUNCTION);  /* check error function */
  lua_pushboolean(L, 1);  /* first result */
  lua_insert(L, 3);
  lua_pushvalue(L, 1);  /* function */
  lua_insert(L, 4);
  status = lua_pcall(L, n - 2, LUA_MULTRET, 2);
  if (status) {  /* error? */
    lua_pushboolean(L, 0);  /* first result (false) */
    lua_pushvalue(L, -2);  /* error message */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
		pllua_rethrow_from_lua(L, status);
	if (pllua_get_active_error(L))
		pllua_rethrow_from_lua(L, LUA_ERRERR);
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L) - 2;  /* return all results */
}

#endif

/*
 * Wrap pcall and xpcall for subtransaction handling.
 *
 * pcall func,args...
 * xpcall func,errfunc,args...
 *
 * In the case of xpcall, the subxact is aborted and released before the
 * user-supplied error function is called, though the stack isn't unwound. If
 * the error function itself throws a Lua error, Lua would replace that with
 * its own "error in error handling" error; if this happens while catching a PG
 * error, then we replace it in turn with our own recursive error object which
 * we rethrow. If the error handling function throws a PG error, we rethrow
 * that into the outer context.
 *
 * This is all aimed at preserving the following invariant: we can only run the
 * user's Lua code inside an error-free subtransaction.
 */

typedef struct pllua_subxact
{
	volatile struct pllua_subxact *prev;
	bool				onstack;
    ResourceOwner		resowner;
    MemoryContext		mcontext;
	ResourceOwner		own_resowner;
} pllua_subxact;

static volatile pllua_subxact *subxact_stack_top = NULL;

static void
pllua_subxact_abort(lua_State *L)
{
	PLLUA_TRY();
	{
		volatile pllua_subxact *xa = subxact_stack_top;
		Assert(xa->onstack);
		xa->onstack = false;
		subxact_stack_top = xa->prev;
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(xa->mcontext);
		CurrentResourceOwner = xa->resowner;
		pllua_pending_error = false;
	}
	PLLUA_CATCH_RETHROW();
}

/*
 * Wrap the user-supplied error function (which is in upvalue 1)
 * Upvalue 2 is a recursion flag.
 *
 * We don't provide the user with another subxact, though they can
 * do that themselves. We do have to guarantee that if they throw
 * their own pg error (and don't catch it in a subxact) then it
 * gets rethrown outwards eventually.
 */
static int
pllua_intercept_error(lua_State *L)
{
	int rc;

	if (!lua_toboolean(L, lua_upvalueindex(2)))
	{
		lua_pushboolean(L, 1);
		lua_replace(L, lua_upvalueindex(2));

		/*
		 * It's possible to get here with a non-pg error as the current error
		 * value while there's a pg error in the registry. But if we're
		 * catching a pg error, it should be the most recent one thrown.
		 *
		 * However, if the user did error(e) using an old caught error object
		 * (which is now deregistered), there might not be a pending registered
		 * pg error at all; this is a valid case.
		 */
		if (pllua_isobject(L, 1, PLLUA_ERROR_OBJECT))
		{
			if (pllua_get_active_error(L))
			{
				Assert(lua_rawequal(L, 1, -1));
				lua_pop(L, 1);
			}
		}
		/*
		 * At this point, the error (which could be either a lua error or a PG
		 * error) has been caught and is on stack as our first arg; if it's a
		 * pg error, it's been flushed from PG's error handling but is still
		 * referenced from the registry. The current subxact is not aborted
		 * yet.
		 *
		 * If it's a lua error, we don't actually have to abort the subxact now
		 * to preserve our invariant, but it would be deeply inconsistent not
		 * to.
		 *
		 * We are still within the subtransaction's pcall at this stage; any
		 * error will recurse here unless we establish another pcall (which we
		 * do below).
		 *
		 * Abort the subxact and pop it.
		 */
		pllua_subxact_abort(L);

		/*
		 * The original pg error if any is now only of interest to the error
		 * handler function and/or the caller of pcall. It's the error
		 * handler's privilege to throw it away and replace it if they choose
		 * to. So we deregister it from the registry at this point.
		 */
		pllua_deregister_error(L);
	}

	/*
	 * Call the user's error function, with itself (unwrapped) as the error
	 * handler.
	 */
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	/* stack: errfunc errfunc errobj */
	rc = pllua_pcall_nothrow(L, 1, 1, 1);
	if (rc == LUA_ERRRUN &&
		pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
	{
		/*
		 * PG error from within the error handler. This means that we just
		 * broke the subtransaction that was the parent of the one we're in
		 * now. The new error should already be in the registry.
		 */
		if (pllua_get_active_error(L))
		{
			Assert(lua_rawequal(L, -2, -1));
			lua_pop(L, 1);
		}
		/*
		 * the pcall wrapper itself will rethrow if need be, since we can't
		 * rethrow from here without recursing
		 */
		return 1;
	}
	/* stack: errfunc newerrobj */
	return 1;
}

/*
 * pcall(func,args...)
 * xpcall(func,errfunc,args...)
 *
 * returns either  true, ...  on success
 * or  false, errobj   on error
 *
 * We don't check "func" except for existence - if it's not a function then the
 * error will happen inside the catch. errfunc must be a function though.
 *
 * Subtlety: we could get here with a PG error in flight if we're called from a
 * user's error handler for lxpcall. If we then catch another pg error inside
 * the subxact, that would result in deregistering the original error.
 *
 * We could preserve the original error for rethrow, but there's a worse
 * problem: there is no way we can be sure that extended interaction with pg
 * would be even safe considering that we are supposed to be in error handling.
 *
 * So the right thing to do would seem to be to refuse to do anything in the
 * presence of a pending error; but because this same argument applies to a lot
 * of other functions too, lxpcall is disabled completely now. If it gets
 * re-enabled again, a lot of places will need checks added, including here.
 */
static int
pllua_t_pcall_guts(lua_State *L, bool is_xpcall)
{
	volatile pllua_subxact xa;
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile int rc;
	volatile bool rethrow = false;

	PLLUA_CHECK_PG_STACK_DEPTH();

	luaL_checkany(L, 1);

	if (is_xpcall)
	{
		luaL_checktype(L, 2, LUA_TFUNCTION);
		/* intercept the error func */
		lua_pushvalue(L, 2);
		lua_pushboolean(L, 0);
		lua_pushcclosure(L, pllua_intercept_error, 2);
		lua_replace(L, 2);
		/* set up stack for return */
		lua_pushboolean(L, 1);
		lua_pushvalue(L, 1);
		/* func errfunc args... true func */
		lua_insert(L, 3);
		/* func errfunc func args... true */
		lua_insert(L, 3);
		/* func errfunc true func args... */
	}
	else
	{
		lua_pushboolean(L, 1);
		lua_insert(L, 1);
		/* true func args ... */
	}

	ASSERT_LUA_CONTEXT;

	pllua_setcontext(L, PLLUA_CONTEXT_PG);
	PG_TRY();
	{
		xa.resowner = CurrentResourceOwner;
		xa.mcontext = oldcontext;
		xa.onstack = false;
		xa.prev = subxact_stack_top;
		xa.own_resowner = NULL;

		BeginInternalSubTransaction(NULL);

		xa.onstack = true;
		xa.own_resowner = CurrentResourceOwner;
		subxact_stack_top = &xa;

		rc = pllua_pcall_nothrow(L,
								 lua_gettop(L) - (is_xpcall ? 4 : 2),
								 LUA_MULTRET,
								 (is_xpcall ? 2 : 0));

		if (rc == LUA_OK)
		{
			/* Commit the inner transaction, return to outer xact context */
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcontext);
			CurrentResourceOwner = xa.resowner;

			Assert(subxact_stack_top == &xa);
			subxact_stack_top = xa.prev;
		}
		else if (xa.onstack)
			pllua_subxact_abort(L);
		else
		{
			/*
			 * error handler must have intercepted and done the abort already.
			 * But this implies that we need to check the registry for a
			 * rethrow, rather than clearing it out.
			 */
			rethrow = true;
		}
	}
	PG_CATCH();
	{
		pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);
		/* absorb the error and get out of pg's error handling */
		pllua_absorb_pg_error(L);
		if (xa.onstack)
			pllua_subxact_abort(L);
		/*
		 * Can only get here if begin or release of the subxact threw an error.
		 * (We assume that release of a subxact can only result in aborting it
		 * instead.) Treat this as an error within the parent context.
		 */
		MemoryContextSwitchTo(oldcontext);
		lua_error(L);
	}
	PG_END_TRY();
	pllua_setcontext(NULL, PLLUA_CONTEXT_LUA);

	if (rc == LUA_OK)
	{
		/*
		 * Normal return.
		 *
		 * For pcall, everything on the stack is the return value.
		 *
		 * For xpcall, ignore two stack slots.
		 */

		/*
		 * something is wrong if there's a pg error still in the registry at
		 * this point.
		 */
		if (pllua_get_active_error(L))
		{
			Assert(false);
			lua_pop(L, 1);
		}

		return lua_gettop(L) - (is_xpcall ? 2 : 0);
	}

	if (rethrow)
	{
		if (pllua_get_active_error(L))
			lua_error(L);
	}
	else
	{
		pllua_deregister_error(L);
	}

	lua_pushboolean(L, 0);
	lua_insert(L, -2);
	return 2;
}

int
pllua_t_pcall(lua_State *L)
{
	if (pllua_getinterpreter(L)->db_ready)
		return pllua_t_pcall_guts(L, false);
	else
		return pllua_t_lpcall(L);
}

int
pllua_t_xpcall(lua_State *L)
{
	if (pllua_getinterpreter(L)->db_ready)
		return pllua_t_pcall_guts(L, true);
	else
		return pllua_t_lxpcall(L);
}

/*
 * local rc,... = subtransaction(func)
 *
 * We explicitly reserve multiple args, or table args, for future use
 * (e.g. for providing a table of error handlers)
 */

static int
pllua_subtransaction(lua_State *L)
{
	lua_settop(L, 1);
	if (!pllua_getinterpreter(L))
		luaL_error(L, "cannot create subtransaction inside on_init string");
	return pllua_t_pcall_guts(L, false);
}

/*
 * Functions to access data from the error object
 *
 * keys:
 *
 * These match up with the ones in elog.c:
 *  "message"
 *  "detail"
 *  "hint"
 *  "column"
 *  "constraint"
 *  "datatype"
 *	"table"
 *	"schema"
 *
 *  "severity"  -- error, warning, etc. (downcased)
 *	"sqlstate"  -- always the 5-char code
 *	"errcode"   -- the long name or the sqlstate
 *	"context"
 *	"pg_source_file"
 *	"pg_source_line"
 *	"pg_source_function"
 *  "position"
 *	"internal_query"
 *	"internal_position"
 *
 */

static bool
pllua_decode_sqlstate(char *buf, lua_Integer errcode)
{
	int i;
	for (i = 0; i < 5; ++i)
	{
		buf[i] = PGUNSIXBIT(errcode);
		errcode = errcode >> 6;
	}
	buf[5] = 0;
	return (errcode == 0 && strspn(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 5);
}

static void
pllua_push_sqlstate(lua_State *L, lua_Integer errcode)
{
	char buf[8];
	pllua_decode_sqlstate(buf, errcode);
	lua_pushstring(L, buf);
}

static void
pllua_push_errcode(lua_State *L, int errcode)
{
	if (lua_geti(L, lua_upvalueindex(1), errcode) == LUA_TNIL)
	{
		char buf[8];
		lua_pop(L, 1);
		/*
		 * Should only get here if the errcode is somehow invalid; return it
		 * anyway
		 */
		pllua_decode_sqlstate(buf, errcode);
		lua_pushstring(L, buf);
	}
}

static void
pllua_push_severity(lua_State *L, int elevel, bool uppercase)
{
	switch (elevel)
	{
		default:
			lua_pushnil(L);	break;
		case DEBUG1:
		case DEBUG2:
		case DEBUG3:
		case DEBUG4:
		case DEBUG5:
			lua_pushstring(L, uppercase ? "DEBUG" : "debug"); break;
		case LOG:
		case COMMERROR:
#if defined(LOG_SERVER_ONLY) && LOG_SERVER_ONLY != COMMERROR
		case LOG_SERVER_ONLY:
#endif
			lua_pushstring(L, uppercase ? "LOG" : "log"); break;
		case INFO:
			lua_pushstring(L, uppercase ? "INFO" : "info"); break;
		case NOTICE:
			lua_pushstring(L, uppercase ? "NOTICE" : "notice"); break;
		case WARNING:
			lua_pushstring(L, uppercase ? "WARNING" : "warning"); break;
		case ERROR:
			lua_pushstring(L, uppercase ? "ERROR" : "error"); break;
		/* below cases can't actually be seen here */
		case FATAL:
			lua_pushstring(L, uppercase ? "FATAL" : "fatal"); break;
		case PANIC:
			lua_pushstring(L, uppercase ? "PANIC" : "panic"); break;
	}
}

static int
pllua_errobject_index(lua_State *L)
{
	ErrorData *e = *pllua_checkrefobject(L, 1, PLLUA_ERROR_OBJECT);
	const char *key = luaL_checkstring(L, 2);

#define PUSHINT(s_) do { lua_pushinteger(L, (s_)); } while(0)
#define PUSHSTR(s_) do { if (s_) lua_pushstring(L, (s_)); else lua_pushnil(L); } while(0)

	switch (key[0])
	{
		case 'c':
			if (strcmp(key,"category") == 0) pllua_push_errcode(L, ERRCODE_TO_CATEGORY(e->sqlerrcode));
			else if (strcmp(key,"context") == 0) PUSHSTR(e->context);
			else if (strcmp(key,"column") == 0) PUSHSTR(e->column_name);
			else if (strcmp(key,"constraint") == 0) PUSHSTR(e->constraint_name);
			else lua_pushnil(L);
			break;
		case 'd':
			if (strcmp(key,"datatype") == 0) PUSHSTR(e->datatype_name);
			else if (strcmp(key,"detail") == 0) PUSHSTR(e->detail);
			else lua_pushnil(L);
			break;
		case 'e':
			if (strcmp(key,"errcode") == 0)	pllua_push_errcode(L, e->sqlerrcode);
			else lua_pushnil(L);
			break;
		case 'h':
			if (strcmp(key,"hint") == 0) PUSHSTR(e->hint);
			else lua_pushnil(L);
			break;
		case 'i':
			if (strcmp(key,"internal_position") == 0) PUSHINT(e->internalpos);
			else if (strcmp(key,"internal_query") == 0) PUSHSTR(e->internalquery);
			else lua_pushnil(L);
			break;
		case 'm':
			if (strcmp(key,"message") == 0) PUSHSTR(e->message);
#if PG_VERSION_NUM >= 90600
			else if (strcmp(key,"message_id") == 0) PUSHSTR(e->message_id);
#endif
			else lua_pushnil(L);
			break;
		case 'p':
			if (strcmp(key,"pg_source_file") == 0) PUSHSTR(e->filename);
			else if (strcmp(key,"pg_source_function") == 0) PUSHSTR(e->funcname);
			else if (strcmp(key,"pg_source_line") == 0) PUSHINT(e->lineno);
			else if (strcmp(key,"position") == 0) PUSHINT(e->cursorpos);
			else lua_pushnil(L);
			break;
		case 's':
			if (strcmp(key,"schema") == 0) PUSHSTR(e->schema_name);
			else if (strcmp(key,"severity") == 0) pllua_push_severity(L, e->elevel, false);
			else if (strcmp(key,"sqlstate") == 0) pllua_push_sqlstate(L, e->sqlerrcode);
			else lua_pushnil(L);
			break;
		case 't':
			if (strcmp(key,"table") == 0) PUSHSTR(e->table_name);
			else lua_pushnil(L);
			break;
		default:
			lua_pushnil(L);
			break;
	}
#undef PUSHINT
#undef PUSHSTR

	return 1;
}

static int
pllua_errobject_tostring(lua_State *L)
{
	ErrorData *e = *pllua_checkrefobject(L, 1, PLLUA_ERROR_OBJECT);
	luaL_Buffer b;
	char buf[8];
	luaL_buffinit(L, &b);
	pllua_push_severity(L, e->elevel, true);
	luaL_addvalue(&b);
	luaL_addstring(&b, ": ");
	pllua_decode_sqlstate(buf, e->sqlerrcode);
	luaL_addstring(&b, buf);
	luaL_addstring(&b, " ");
	luaL_addstring(&b, e->message ? e->message : "(no message)");
	luaL_pushresult(&b);
	return 1;
}

static int
pllua_errobject_errcode(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_ERROR_OBJECT);
	if (p && *p)
	{
		ErrorData *e = *p;
		pllua_push_errcode(L, e->sqlerrcode);
		return 1;
	}
	else
		return 0;
}

static int
pllua_errobject_category(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_ERROR_OBJECT);
	if (p && *p)
	{
		ErrorData *e = *p;
		pllua_push_errcode(L, ERRCODE_TO_CATEGORY(e->sqlerrcode));
		return 1;
	}
	else
		return 0;
}

static int
pllua_errobject_type(lua_State *L)
{
	if (pllua_isobject(L, 1, PLLUA_ERROR_OBJECT))
		lua_pushstring(L, "error");
	else
		lua_pushnil(L);
	return 1;
}

static struct { const char *str; int val; } ecodes[] = {
#include "plerrcodes.h"
	{ NULL, 0 }
};

static void
pllua_get_errcodes(lua_State *L, int nidx)
{
	int ncodes = sizeof(ecodes)/sizeof(ecodes[0]) - 1;
	int i;

	nidx = lua_absindex(L, nidx);

	for (i = 0; i < ncodes; ++i)
	{
		lua_pushstring(L, ecodes[i].str);
		lua_pushvalue(L, -1);
		lua_rawseti(L, nidx, ecodes[i].val);
		lua_pushinteger(L, ecodes[i].val);
		lua_rawset(L, nidx);
	}
}

/*
 * __index(tab,key)
 */
static int
pllua_errcodes_index(lua_State *L)
{
	lua_settop(L, 2);

	if (!lua_toboolean(L, lua_upvalueindex(1)))
	{
		pllua_get_errcodes(L, 1);

		lua_pushboolean(L, 1);
		lua_replace(L, lua_upvalueindex(1));

		lua_pushvalue(L, 2);
		if (lua_rawget(L, 1) != LUA_TNIL)
			return 1;
	}
	switch (lua_type(L, 2))
	{
		default:
			return 0;
		case LUA_TSTRING:
			{
				const char *str = lua_tostring(L, 2);
				if (strlen(str) == 5
					&& strspn(str, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 5)
				{
					lua_pushinteger(L, MAKE_SQLSTATE(str[0], str[1], str[2], str[3], str[4]));
					return 1;
				}
				return 0;
			}
		case LUA_TNUMBER:
			{
				int isint = 0;
				lua_Integer errcode = lua_tointegerx(L, 2, &isint);
				char buf[8];

				if (!isint)
					return 0;

				if (!pllua_decode_sqlstate(buf, errcode))
					return 0;

				lua_pushstring(L, buf);
				return 1;
			}
	}
}

/*
 * __newindex(tab,key,val)
 */
static int
pllua_errcodes_newindex(lua_State *L)
{
	return luaL_error(L, "errcodes table is immutable");
}

static struct luaL_Reg errtab_mt[] = {
	{ "__index", pllua_errcodes_index },
	{ "__newindex", pllua_errcodes_newindex },
	{ NULL, NULL }
};

/*
 * module init
 */

static struct luaL_Reg errobj_mt[] = {
	{ "__gc", pllua_errobject_gc },
	{ "__tostring", pllua_errobject_tostring },
	{ NULL, NULL }
};

static struct luaL_Reg errfuncs[] = {
	{ "pcall", pllua_t_pcall },
	{ "xpcall", pllua_t_xpcall },
	{ "spcall", pllua_t_pcall },
	{ "sxpcall", pllua_t_xpcall },
	{ "lpcall", pllua_t_lpcall },
#if 0
	/* unsafe, see comment on pcall */
	{ "lxpcall", pllua_t_lxpcall },
#endif
	{ "subtransaction", pllua_subtransaction },
	{ "type", pllua_errobject_type },
	{ NULL, NULL }
};

static struct luaL_Reg errfuncs2[] = {
	{ "errcode", pllua_errobject_errcode },
	{ "category", pllua_errobject_category },
	{ NULL, NULL }
};

static struct luaL_Reg glob_errfuncs[] = {
	{ "warn", pllua_t_warn },
	{ "pcall", pllua_t_pcall },
	{ "xpcall", pllua_t_xpcall },
	{ "lpcall", pllua_t_lpcall },
#if 0
	/* unsafe, see comment on pcall */
	{ "lxpcall", pllua_t_lxpcall },
#endif
	{ NULL, NULL }
};

static struct luaL_Reg co_errfuncs[] = {
	{ "resume", pllua_t_coresume },
	{ NULL, NULL }
};


int pllua_open_error(lua_State *L)
{
	int ncodes = sizeof(ecodes)/sizeof(ecodes[0]) - 1;
	int i;
	int refs[30];

	lua_settop(L, 0);

	/*
	 * Create and drop a few registry reference entries so that there's a
	 * freelist; this reduces the chance that we have to extend the registry
	 * table (with the possibility of error) while actually doing error
	 * handling.
	 */
	for (i = 0; i < sizeof(refs)/sizeof(int); ++i)
	{
		lua_pushboolean(L, 1);
		refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	while (--i >= 0)
		luaL_unref(L, LUA_REGISTRYINDEX, refs[i]);

	lua_createtable(L, 0, 2 * ncodes);  /* index 1 */
	lua_newtable(L);
	lua_pushboolean(L, 0);
	luaL_setfuncs(L, errtab_mt, 1);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ERRCODES_TABLE);

	pllua_newmetatable(L, PLLUA_ERROR_OBJECT, errobj_mt);
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, pllua_errobject_index, 1);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	/*
	 * init.c allocated the pre-built error object; make a full userdata for
	 * it
	 */
	lua_pushcfunction(L, pllua_newerror);
	lua_pushlightuserdata(L, pllua_getinterpreter(L)->edata);
	lua_call(L, 1, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);

	lua_pushglobaltable(L);
	luaL_setfuncs(L, glob_errfuncs, 0);
	luaL_getsubtable(L, -1, "coroutine");
	luaL_setfuncs(L, co_errfuncs, 0);
	lua_pop(L,2);

	lua_newtable(L);
	luaL_setfuncs(L, errfuncs, 0);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ERRCODES_TABLE);
	luaL_setfuncs(L, errfuncs2, 1);
	return 1;
}
