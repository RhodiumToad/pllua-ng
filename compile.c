
#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"


/*
 * Do fairly minimalist validation on the procTup to ensure that we're not
 * going to do something dangerous or security-violating. More detailed checks
 * can be left to the validator func. Throws a pg error on failure.
 */

static void pllua_validate_proctup(lua_State *L, Oid fn_oid,
								   HeapTuple procTup, bool trusted)
{
	HeapTuple lanTup;
	Form_pg_language lanStruct;
	Form_pg_proc procStruct;
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	ASSERT_PG_CONTEXT;

	lanTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
	if (!HeapTupleIsValid(lanTup))
		elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
	lanStruct = (Form_pg_language) GETSTRUCT(lanTup);

	if ((trusted && !lanStruct->lanpltrusted)
		|| (!trusted && lanStruct->lanpltrusted))
	{
		elog(ERROR, "trusted state mismatch for function %u in language %u",
			 fn_oid, procStruct->prolang);
	}
	ReleaseSysCache(lanTup);
}

/*
 * Given a comp_info containing the info we need, compile a function and make
 * an object for it. However, we don't actually store the func_info into the
 * object; caller does that, after reparenting the memory context.
 *
 * Note that "compiling" a function in the current setup may execute some user
 * code.
 *
 * Returns the object on the stack.
 */
static int pllua_compile(lua_State *L)
{
	pllua_function_compile_info *comp_info = lua_touserdata(L, 1);
	pllua_function_info *func_info = comp_info->func_info;
	const char *fname = func_info->name;
	const char *src;
	luaL_Buffer b;

	/* caller fills in pointer */
	pllua_newrefobject(L, PLLUA_FUNCTION_OBJECT, NULL, true);

	luaL_buffinit(L, &b);

	/*
	 * The string we construct is:
	 *   local upvalue,f f=function(args) body end return f
	 * which we then execute and expect it to return the
	 * function object.
	 */
	luaL_addstring(&b, "local " PLLUA_LOCALVAR ",");
	luaL_addstring(&b, fname);
	luaL_addchar(&b, ' ');
	luaL_addstring(&b, fname);
	luaL_addstring(&b, "=function(");
	if (comp_info->nargs > 0)
	{
		int n = 0;
		int i;
		if (comp_info->argnames && comp_info->argnames[0])
		{
			for(i = 0; i < comp_info->nallargs; ++i)
			{
				if (!comp_info->argmodes || comp_info->argmodes[i] != 'o')
				{
					if (comp_info->argnames[i] && comp_info->argnames[i][0])
					{
						if (n > 0)
							luaL_addchar(&b, ',');
						luaL_addstring(&b, comp_info->argnames[i]);
						++n;
					}
					else
						break;
				}
			}
		}
		if (n < comp_info->nargs)
		{
			if (n > 0)
				luaL_addchar(&b, ',');
			luaL_addstring(&b, "...");
		}
	}
	luaL_addstring(&b, ") ");
	luaL_addlstring(&b, VARDATA_ANY(comp_info->prosrc),
					VARSIZE_ANY_EXHDR(comp_info->prosrc));
	luaL_addstring(&b, " end return ");
	luaL_addstring(&b, fname);
	luaL_pushresult(&b);
	src = lua_tostring(L, -1);

	pllua_debug(L, "compiling: %s", src);

	if (luaL_loadbuffer(L, src, strlen(src), fname))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	lua_remove(L, -2); /* source */
	lua_call(L, 0, 1);

	lua_getuservalue(L, -2);
	lua_insert(L, -2);
	lua_rawsetp(L, -2, PLLUA_FUNCTION_MEMBER);
	lua_pop(L, 1);

	return 1;
}

/*
 * Intern a function into the functions table, indexed by numeric oid.
 *
 * This will NOT replace an existing entry, unless the function passed in is
 * nil, which signifies uninterning an existing function.
 */
static int pllua_intern_function(lua_State *L)
{
	lua_Integer oid = luaL_checkinteger(L, 2);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);

	if (!lua_isnil(L, 1))
	{
		pllua_checkrefobject(L, 1, PLLUA_FUNCTION_OBJECT);
		lua_rawgeti(L, -1, oid);
		if (!lua_isnil(L, -1))
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_pop(L, 1);
	}

	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, oid);
	lua_pushboolean(L, 1);

	return 1;
}

/*
 * Call this to resolve an activation before use
 *
 * The act->func_info must have been set up as far as the pg state goes, but
 * the actual lua function may not have been compiled yet (since we want to
 * ensure that this is fully valid before running any user-supplied code).
 * This handles polymorphism and so on.
 *
 * Note that act->func_info may not have been filled in yet, and we don't do
 * that. In error cases we ensure the activation is de-resolved, which should
 * prevent anything accessing any dangling fields.
 */
static void pllua_resolve_activation(lua_State *L,
									 pllua_func_activation *act,
									 pllua_function_info *func_info,
									 FunctionCallInfo fcinfo)
{
	MemoryContext oldcontext;
	FmgrInfo *flinfo = fcinfo->flinfo;
	Oid rettype = func_info->rettype;

	if (act->resolved)
		return;

	ASSERT_PG_CONTEXT;

	oldcontext = MemoryContextSwitchTo(flinfo->fn_mcxt);

	if (IsPolymorphicType(rettype)
		|| type_is_rowtype(rettype))
	{
		act->typefuncclass = get_call_result_type(fcinfo,
												  &act->rettype,
												  &act->tupdesc);
		if (act->tupdesc && act->tupdesc->tdrefcount != -1)
		{
			/*
			 * This is a ref-counted tupdesc, but we can't pin it
			 * because any such pin would belong to a resource owner,
			 * and we can't guarantee that our required data lifetimes
			 * nest properly with the resource owners. So make a copy
			 * instead. (If it's not refcounted, then it'll already be
			 * in fn_mcxt.)
			 */
			act->tupdesc = CreateTupleDescCopy(act->tupdesc);
		}
	}
	else
	{
		act->rettype = rettype;
		act->typefuncclass = TYPEFUNC_SCALAR;
	}

	act->polymorphic = func_info->polymorphic;
	act->variadic_call = get_fn_expr_variadic(fcinfo->flinfo);
	act->nargs = func_info->nargs;
	act->retset = func_info->retset;
	act->readonly = func_info->readonly;

	if (act->polymorphic)
	{
		act->argtypes = palloc(act->nargs * sizeof(Oid));
		memcpy(act->argtypes, func_info->argtypes, act->nargs * sizeof(Oid));
		if (!resolve_polymorphic_argtypes(act->nargs, act->argtypes,
										  NULL, flinfo->fn_expr))
			elog(ERROR,"failed to resolve polymorphic argtypes");
	}
	else
		act->argtypes = func_info->argtypes;

	MemoryContextSwitchTo(oldcontext);
	act->resolved = true;
}

/*
 * Return true if func_info is an up to date compile of procTup.
 */
static bool pllua_function_valid(pllua_function_info *func_info,
								 HeapTuple procTup)
{
	return (func_info &&
			func_info->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
			ItemPointerEquals(&func_info->fn_tid, &procTup->t_self));
}

/*
 * Returns with a function activation object on top of the lua stack.
 *
 * Also returns a pointer to the activation as a convenience to the caller.
 */
pllua_func_activation *
pllua_validate_and_push(lua_State *L,
						FunctionCallInfo fcinfo,
						bool trusted)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	pllua_func_activation *volatile retval = NULL;
	FmgrInfo *flinfo = fcinfo->flinfo;
	ReturnSetInfo *rsi = ((fcinfo->resultinfo && IsA(fcinfo->resultinfo, ReturnSetInfo))
						  ?	(ReturnSetInfo *)(fcinfo->resultinfo)
						  : NULL);

	ASSERT_LUA_CONTEXT;

	/*
	 * We need the pg_proc row etc. every time, but we have to avoid throwing
	 * pg errors through lua.
	 */

	PLLUA_TRY();
	{
		pllua_func_activation *act = flinfo->fn_extra;
		Oid			fn_oid = flinfo->fn_oid;
		HeapTuple	procTup;
		Form_pg_proc procStruct;
		pllua_function_info *func_info;
		pllua_function_compile_info *comp_info;
		bool isnull;
		Datum psrc;
		MemoryContext fcxt;
		MemoryContext ccxt;
		int rc;
		int i;

		/*
		 * If we don't have an activation yet, make one (it'll initially be
		 * invalid). We have to ensure that it's safe to leave a
		 * not-yet-filled-in activation attached to flinfo.
		 *
		 * If we do have one already, find its lua object.
		 *
		 * The activation is left on the lua stack.
		 */
		if (!act)
		{
			pllua_pushcfunction(L, pllua_newactivation);
			lua_pushlightuserdata(L, flinfo->fn_mcxt);
			pllua_pcall(L, 1, 1, 0);
			act = lua_touserdata(L, -1);
			if (flinfo && flinfo->fn_extra == NULL)
				flinfo->fn_extra = act;
		}
		else
			pllua_getactivation(L, act);

		/*
		 * This part may have to be repeated in some rare recursion scenarios.
		 */
		for (;;)
		{
			/* We'll need the pg_proc tuple in any case... */
			procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
			if (!HeapTupleIsValid(procTup))
				elog(ERROR, "cache lookup failed for function %u", fn_oid);
			procStruct = (Form_pg_proc) GETSTRUCT(procTup);

			if (act && pllua_function_valid(act->func_info, procTup))
			{
				/* fastpath out when data is already valid. */
				ReleaseSysCache(procTup);
				break;
			}

			/*
			 * Lookup function by oid in our lua table (this can't throw)
			 */

			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
			if (lua_rawgeti(L, -1, (lua_Integer) fn_oid) == LUA_TUSERDATA)
			{
				void **p = pllua_torefobject(L, -1, PLLUA_FUNCTION_OBJECT);
				func_info = p ? *p : NULL;
				/* might be out of date. */
				if (pllua_function_valid(func_info, procTup))
				{
					/*
					 * The activation is out of date, but the existing
					 * compiled function is not. Just update the activation.
					 */
					/* stack: activation funcs_table funcobject */
					pllua_pushcfunction(L, pllua_setactivation);
					lua_pushlightuserdata(L, act);
					lua_pushvalue(L, -3);
					pllua_pcall(L, 2, 0, 0);
					/* stack: activation funcs_table funcobject */
					lua_pop(L, 2);
					ReleaseSysCache(procTup);
					break;
				}

				/*
				 * Compiled function in cache is out of date. Unintern it
				 * before proceeding (recursion worries). This might lead to it
				 * being GC'd, so forget about the pointer too.
				 */
				func_info = NULL;
				pllua_pushcfunction(L, pllua_intern_function);
				lua_pushnil(L);
				lua_pushinteger(L, (lua_Integer) fn_oid);
				pllua_pcall(L, 2, 0, 0);
			}
			/* stack: activation funcs_table funcobject */
			lua_pop(L, 2);

			act->resolved = false;
			act->func_info = NULL;

			/*
			 * If we get this far, we need to compile up the function from
			 * scratch.  Create the func_info, compile_info and
			 * contexts. Note that the compile context is always transient,
			 * but the function context is reparented to the long-lived lua
			 * context on success.
			 */

			psrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
			if (isnull)
				elog(ERROR, "null prosrc");

			fcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua function object",
										 ALLOCSET_SMALL_SIZES);
			ccxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua compile context",
										 ALLOCSET_SMALL_SIZES);

			MemoryContextSwitchTo(fcxt);

			func_info = palloc(sizeof(pllua_function_info));
			func_info->mcxt = fcxt;
			func_info->name = pstrdup(NameStr(procStruct->proname));
			func_info->fn_oid = fn_oid;
			func_info->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
			func_info->fn_tid = procTup->t_self;
			func_info->rettype = procStruct->prorettype;
			func_info->retset = procStruct->proretset;
			func_info->language_oid = procStruct->prolang;
			func_info->trusted = trusted;
			func_info->nargs = procStruct->pronargs;
			func_info->variadic = procStruct->provariadic != InvalidOid;
			func_info->variadic_any = procStruct->provariadic == ANYOID;
			func_info->polymorphic = false;
			func_info->readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);

			Assert(func_info->nargs == procStruct->proargtypes.dim1);
			func_info->argtypes = (Oid *) palloc(func_info->nargs * sizeof(Oid));
			memcpy(func_info->argtypes,
				   procStruct->proargtypes.values,
				   func_info->nargs * sizeof(Oid));

			for (i = 0; i < func_info->nargs; ++i)
			{
				if (IsPolymorphicType(func_info->argtypes[i])
					|| func_info->argtypes[i] == ANYOID)
				{
					func_info->polymorphic = true;
					break;
				}
			}
			/*
			 * Redo the most essential validation steps out of sheer paranoia
			 */
			pllua_validate_proctup(L, fn_oid, procTup, trusted);

			MemoryContextSwitchTo(ccxt);

			comp_info = palloc(sizeof(pllua_function_compile_info));
			comp_info->mcxt = ccxt;
			comp_info->func_info = func_info;
			comp_info->prosrc = DatumGetTextPP(psrc);

			/*
			 * XXX dig out all the needed info about arg and result types
			 * and stash it in the compile info.
			 */

			comp_info->nargs = procStruct->pronargs;
			comp_info->nallargs = get_func_arg_info(procTup,
													&comp_info->allargtypes,
													&comp_info->argnames,
													&comp_info->argmodes);
			comp_info->variadic = procStruct->provariadic;

			/*
			 * Resolve the activation before compiling in case the user code
			 * tries to access it.
			 */
			pllua_resolve_activation(L, act, func_info, fcinfo);

			/*
			 * Beware, compiling can invoke user-supplied code, which might
			 * in turn recurse here. We trust that stack depth checks will
			 * break any such loop.
			 */
			pllua_pushcfunction(L, pllua_compile);
			lua_pushlightuserdata(L, comp_info);
			rc = pllua_pcall_nothrow(L, 1, 1, 0);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(ccxt);

			if (rc)
			{
				act->resolved = false;
				MemoryContextDelete(fcxt);
				pllua_rethrow_from_lua(L, rc);
			}

			MemoryContextSetParent(fcxt, pllua_get_memory_cxt(L));

			{
				void *p = lua_touserdata(L, -1);
				*(void **)p = func_info;
			}

			/*
			 * Try and intern the function. Since we uninterned it earlier, we
			 * expect this to succeed, but a recursive call could have interned
			 * a new version already (which will be at least as new as
			 * ours). Worse, if so, that new version could already be out of
			 * date, meaning that we have to loop back to check the pg_proc row
			 * again.
			 */

			pllua_pushcfunction(L, pllua_intern_function);
			lua_insert(L, -2);
			lua_pushinteger(L, (lua_Integer) fn_oid);
			pllua_pcall(L, 2, 0, 0);
			func_info = NULL;
			ReleaseSysCache(procTup);
		}

		/*
		 * Post-compile per-call validation (mostly here to avoid more catch
		 * blocks)
		 */
		if (func_info->retset)
		{
			if (!rsi ||
				!IsA(rsi, ReturnSetInfo) ||
				!(rsi->allowedModes & SFRM_ValuePerCall))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("set-valued function called in context that cannot accept a set")));
		}

		if (!act->resolved)
			pllua_resolve_activation(L, act, act->func_info, fcinfo);

		retval = act;
	}
	PLLUA_CATCH_RETHROW();

	MemoryContextSwitchTo(oldcontext);

	return retval;
}
