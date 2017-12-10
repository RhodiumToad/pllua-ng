/* exec.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "utils/datum.h"


static void
pllua_common_lua_init(lua_State *L, FunctionCallInfo fcinfo)
{
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	luaL_checkstack(L, 40, NULL);
}

/*
 * Given that the top "nret" items on the stack are the return value, convert
 * to Datum/isnull.
 *
 * Note that this is not used for triggers, which have their own function.
 *
 * nret==0 is taken as returning null for non-SRFs; the case of the initial
 * call of an SRF returning nret==0 without yielding is handled elsewhere.
 *
 * Otherwise, we simply pass the whole list of values to the type constructor
 * for the return type, which does all the work. We then copy the result to the
 * current memory context (presumed to be the caller's), in order to avoid any
 * uncertainty regarding garbage collection.
 */
static Datum
pllua_return_result(lua_State *L,
					int nret,
					pllua_func_activation *act,
					bool *isnull)
{
	pllua_typeinfo *ti;
	pllua_datum *d;

	if (nret == 0)
	{
		*isnull = true;
		return (Datum)0;
	}

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	if (!act->tupdesc)
	{
		lua_pushinteger(L, (lua_Integer)(act->rettype));
		lua_call(L, 1, 1);
	}
	else
	{
		lua_pushinteger(L, (lua_Integer)(act->tupdesc->tdtypeid));
		lua_pushinteger(L, (lua_Integer)(act->tupdesc->tdtypmod));
		lua_call(L, 2, 1);
	}
	lua_insert(L, -(nret+1));
	lua_call(L, nret, 1);

	if (lua_type(L, -1) == LUA_TNIL)
	{
		*isnull = true;
		return (Datum)0;
	}
	else
	{
		d = pllua_checkanydatum(L, -1, &ti);

		*isnull = false;
		return datumCopy(d->value, ti->typbyval, ti->typlen);
	}
}

/*
 * If an argument is a record type with a non-NULL value, get the actual
 * typeid/typmod from the record header.
 */
static void
pllua_get_record_argtype(lua_State *L, Datum *value, Oid *argtype, int32 *argtypmod)
{
	/*
	 * this may detoast, so we need a catch block
	 *
	 * we detoast in the current memory context, assumed to be transient,
	 * because we're going to datumCopy the result after anyway
	 */
	PLLUA_TRY();
	{
		HeapTupleHeader arg = DatumGetHeapTupleHeader(*value);
		*value = PointerGetDatum(arg);
		*argtype = HeapTupleHeaderGetTypeId(arg);
		*argtypmod = HeapTupleHeaderGetTypMod(arg);
	}
	PLLUA_CATCH_RETHROW();
}

/*
 * args are on stack at -nargs .. -1
 *
 * Perform savedatum on the list of args to ensure they are all copied into our
 * memory context.
 */
static void
pllua_save_args(lua_State *L, int nargs, pllua_typeinfo **argtypes)
{
	ASSERT_LUA_CONTEXT;

	if (nargs == 0)
		return;

	PLLUA_TRY();
	{
		int			i;
		int			arg0 = lua_absindex(L, -nargs);
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));

		for (i = 0; i < nargs; ++i)
		{
			if (lua_type(L, arg0+i) == LUA_TUSERDATA
				&& argtypes[i])
			{
				pllua_datum *d = lua_touserdata(L, arg0+i);
				pllua_savedatum(L, d, argtypes[i]);
			}
		}

		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();
}

/*
 * Push all the arguments from fcinfo onto the lua stack with all necessary
 * conversions.
 */
static int
pllua_push_args(lua_State *L,
				FunctionCallInfo fcinfo,
				pllua_func_activation *act)
{
	int			i;
	int			nargs = PG_NARGS();   /* _actual_ args in call */
	pllua_typeinfo *argtinfo[FUNC_MAX_ARGS];

	/*
	 * If we're variadic, pg has collected the variadic args into an array,
	 * _unless_ we're doing variadic_any in which case the extra arguments are
	 * still separate (but there can't be more than FUNC_MAX_ARGS of them).
	 */
	if (nargs != act->nargs && !act->func_info->variadic_any)
		luaL_error(L, "wrong number of args: expected %d got %d", act->nargs, nargs);

	luaL_checkstack(L, 40 + nargs, NULL);

	for (i = 0; i < nargs; ++i)
	{
		Datum	value = PG_GETARG_DATUM(i);
		Oid		argtype = InvalidOid;
		int32	argtypmod = -1;

		if (i < act->nargs
			&& act->argtypes[i] != ANYOID)
		{
			argtype = act->argtypes[i];
		}
		else
		{
			/* arg is ANYOID, so resolve what type the caller thinks it is. */
			/* we rely on this not throwing! */
			argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
			if (!OidIsValid(argtype))
				luaL_error(L, "cannot determine type of argument %d", i);
		}

		if (argtype == RECORDOID && !PG_ARGISNULL(i))
		{
			/*
			 * RECORD type with a non-null value - prefer to take the type
			 * from the real record
			 */
			pllua_get_record_argtype(L, &value, &argtype, &argtypmod);
		}

		/*
		 * Try pushing the value as a simple lua value first, and only push a
		 * datum object if that failed.
		 */
		if (PG_ARGISNULL(i))
		{
			lua_pushnil(L);
		}
		else if (pllua_value_from_datum(L, value, argtype) == LUA_TNONE)
		{
			void **p;
			pllua_datum *d;

			lua_pushcfunction(L, pllua_typeinfo_lookup);
			lua_pushinteger(L, (lua_Integer) argtype);
			lua_pushinteger(L, (lua_Integer) argtypmod);
			lua_call(L, 2, 1);

			if (lua_isnil(L, -1))
				luaL_error(L, "failed to find typeinfo");
			p = pllua_checkrefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
			argtinfo[i] = *p;
			d = pllua_newdatum(L);
			d->value = value;
			lua_remove(L,-2);
		}
		else
			argtinfo[i] = NULL;
	}

	/*
	 * Now, we have the arg datums at index -nargs .. -1, but we need to
	 * run savedatum on all of them to get them copied safely.
	 */
	pllua_save_args(L, nargs, argtinfo);

	return nargs;
}

/*
 * Resume an SRF in value-per-call mode (second and subsequent calls come here)
 */
int
pllua_resume_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	pllua_func_activation *fact = fcinfo->flinfo->fn_extra;
	lua_State  *thr = fact->thread;
	int			rc;

	Assert(thr != NULL);
	Assert(lua_gettop(L) == 1);

	rc = lua_resume(thr, L, 0);

	if (rc == LUA_OK)
	{
		lua_xmove(thr, L, lua_gettop(thr));
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
									  fact,
									  &fcinfo->isnull);
	return 0;
}

/*
 * Main entry point for function calls
 */
int
pllua_call_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	pllua_func_activation *fact;
	int			nstack;
	int			nargs;
	int			rc;

	pllua_common_lua_init(L, fcinfo);

	/* pushes the activation on the stack */
	fact = pllua_validate_and_push(L, fcinfo, act->trusted);

	/* stack mark for result processing */
	nstack = lua_gettop(L);
	Assert(nstack == 2);

	/* get the function object from the activation and push that */
	pllua_activation_getfunc(L);

	/* func should be the only thing on the stack after the act */
	Assert(lua_gettop(L) == nstack + 1);

	nargs = pllua_push_args(L, fcinfo, fact);

	if (fact->retset)
	{
		/*
		 * This is the initial call into a SRF. Activate a new thread (which
		 * also handles registering into the ExprContext), move the func and
		 * parameters over to the new thread and resume it.
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

			luaL_checkstack(L, 10 + nret, NULL);
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
			int nret = lua_gettop(thr);
			luaL_checkstack(L, 10 + nret, NULL);
			lua_xmove(thr, L, nret);
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
		lua_call(L, nargs, LUA_MULTRET);
		luaL_checkstack(L, 10, NULL);
	}

	/*
	 * func and args are popped by the call, so everything left is a function
	 * result. the func_info is not on the stack any more, but we know it must
	 * be referenced from the activation
	 */
	act->retval = pllua_return_result(L, lua_gettop(L) - nstack,
									  fact,
									  &fcinfo->isnull);
	return 0;
}

/*
 * Entry point for trigger invocations
 */
int
pllua_call_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;
	TriggerData *td = (TriggerData *) fcinfo->context;
	pllua_func_activation *fact;
	int			nstack;
	int			nargs;

	pllua_common_lua_init(L, fcinfo);

	/* push a trigger object on the stack */
	pllua_trigger_begin(L, td);

	/* pushes the activation on the stack */
	fact = pllua_validate_and_push(L, fcinfo, act->trusted);

	/* stack mark for result processing */
	nstack = lua_gettop(L);
	Assert(nstack == 3);

	/* get the function object from the activation and push that */
	pllua_activation_getfunc(L);

	/*
	 * Triggers have three fixed args: the trigger object, old and new tuples
	 * plus a variable number of string args from tg_args. These don't
	 * correspond in any way to the arguments declared in the funcinfo (which
	 * will specify that there are no args).
	 */
	lua_pushvalue(L, 2);
	lua_getfield(L, -1, "old");
	lua_getfield(L, -2, "new");
	nargs = 3 + pllua_push_trigger_args(L, td);

	lua_call(L, nargs, LUA_MULTRET);
	luaL_checkstack(L, 10, NULL);

	act->retval = pllua_return_trigger_result(L, lua_gettop(L) - nstack, 2);

	/* mark the trigger object dead */
	pllua_trigger_end(L, 2);

	return 0;
}

/*
 * Entry point for event triggers.
 *
 * TODO
 */
int
pllua_call_event_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L, fcinfo);

	return 0;
}

/*
 * Entry point for inline code blocks (DO).
 *
 * Very little needs doing here.
 */
int
pllua_call_inline(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L, fcinfo);

	if (luaL_loadbuffer(L, act->cblock->source_text, strlen(act->cblock->source_text), "DO-block"))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	if (act->trusted)
	{
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
		lua_setupvalue(L, -2, 1);
	}
	lua_call(L, 0, 0);

	return 0;
}

/*
 * Entry point for function validator. Guts of this are in compile.c
 *
 * No return values; is expected to throw error on failure.
 */
int
pllua_validate(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	Oid func_oid = act->validate_func;

	pllua_common_lua_init(L, NULL);

	pllua_validate_function(L, func_oid, act->trusted);

	return 0;
}
