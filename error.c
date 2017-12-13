/* error.c */

#include "pllua.h"

#include "access/xact.h"
#include "utils/resowner.h"

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
	MemoryContext emcxt;

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

	return edata;
}

void
pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt)
{
	if (pllua_context == PLLUA_CONTEXT_PG)
		PG_RE_THROW();

	pllua_absorb_pg_error(L);

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
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L) - (int)extra;  /* return all results */
}

int pllua_t_lpcall (lua_State *L) {
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
**
** XXX XXX we need to intercept the error handling function somehow to
** ensure it doesn't mess with a pg error.
*/
int pllua_t_lxpcall (lua_State *L) {
  int status;
  int n = lua_gettop(L);
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
    return 2;  /* return false, msg */
  }
  else
    return lua_gettop(L) - 2;  /* return all results */
}

#endif

/*
 * Wrap the user-visible error() and assert() so that we can see when the user
 * throws a pg error object into the lua error handler.
 *
 * error(err[,levelsup])
 */
int
pllua_t_error(lua_State *L)
{
	int level = (int) luaL_optinteger(L, 2, 1);

	lua_settop(L, 1);

	if (pllua_isobject(L, 1, PLLUA_ERROR_OBJECT))
	{
		lua_pushvalue(L, 1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
	}
	else if (lua_type(L, 1) == LUA_TSTRING && level > 0)
	{
		luaL_where(L, level);   /* add extra information */
		lua_pushvalue(L, 1);
		lua_concat(L, 2);
	}
	return lua_error(L);
}

/*
 * assert(cond[,error])
 */
int
pllua_t_assert(lua_State *L)
{
	if (lua_toboolean(L, 1))  /* condition is true? */
		return lua_gettop(L);  /* return all arguments */
	else
	{  /* error */
		luaL_checkany(L, 1);  /* there must be a condition */
		lua_remove(L, 1);  /* remove it */
		lua_pushliteral(L, "assertion failed!");  /* default message */
		lua_settop(L, 1);  /* leave only message (default if no other one) */
		return pllua_t_error(L);  /* call 'error' */
	}
}

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

		if (pllua_isobject(L, 1, PLLUA_ERROR_OBJECT))
		{
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
			Assert(lua_rawequal(L, 1, -1));
			lua_pop(L, 1);
		}
		else
		{
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
			Assert(lua_isnil(L, -1));
			lua_pop(L, 1);
		}
		/*
		 * At this point, the error (which could be either a lua error or a PG
		 * error) has been caught and is on stack as our first arg; if it's a
		 * pg error, it's been flushed from PG's error handling but is still
		 * referenced from reg[LAST_ERROR]. The current subxact is not aborted
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
		 * The original pg error is now only of interest to the error handler
		 * function and/or the caller of pcall. It's the error handler's
		 * privilege to throw it away and replace it if they choose to. So we
		 * deregister it from the registry at this point.
		 *
		 * rawsetp can throw error. If it does, we'll recurse (unless it was a
		 * GC error or some such). The registry may be left unchanged in that
		 * case; we have to be aware of that (probably very remote) possibility
		 * elsewhere.
		 */
		lua_pushnil(L);
		lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
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
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
		Assert(lua_rawequal(L, -2, -1));
		lua_pop(L, 1);
		/*
		 * the pcall wrapper itself will rethrow, since we can't rethrow from
		 * here without recursing
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
 */
static int
pllua_t_pcall_guts(lua_State *L, bool is_xpcall)
{
	volatile pllua_subxact xa;
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile int rc;

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

	pllua_setcontext(PLLUA_CONTEXT_PG);
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
	}
	PG_CATCH();
	{
		pllua_setcontext(PLLUA_CONTEXT_LUA);
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
	pllua_setcontext(PLLUA_CONTEXT_LUA);

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
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
		Assert(lua_isnil(L, -1));
		lua_pop(L, 1);

		return lua_gettop(L) - (is_xpcall ? 2 : 0);
	}

	/*
	 * Error object is on stack top. But if there's a PG error left in the
	 * registry, then that takes precedence; we have to rethrow it to an outer
	 * level.
	 */
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
	if (!lua_isnil(L, -1))
		pllua_rethrow_from_lua(L, LUA_ERRERR);
	lua_pop(L, 1);

	lua_pushboolean(L, 0);
	lua_insert(L, -2);
	return 2;
}

int
pllua_t_pcall(lua_State *L)
{
	return pllua_t_pcall_guts(L, false);
}

int
pllua_t_xpcall(lua_State *L)
{
	return pllua_t_pcall_guts(L, true);
}


/*
 * module init
 */
static struct luaL_Reg errobj_mt[] = {
	{ "__gc", pllua_errobject_gc },
	{ NULL, NULL }
};

static struct luaL_Reg errfuncs[] = {
	{ "assert", pllua_t_assert },
	{ "error", pllua_t_error },
	{ "pcall", pllua_t_pcall },
	{ "xpcall", pllua_t_xpcall },
	{ "lpcall", pllua_t_lpcall },
	{ "lxpcall", pllua_t_lxpcall },
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
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);

	lua_pushglobaltable(L);
	luaL_setfuncs(L, errfuncs, 0);
	lua_pop(L,1);
}
