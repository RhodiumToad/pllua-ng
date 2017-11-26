
#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"

/*
 * Given a comp_info containing the info we need, compile a function and make
 * an object for it. However, we don't actually store the func_info into the
 * object; caller does that, after reparenting the memory context.
 *
 * Note that "compiling" a function in the current setup may execute some code.
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

	/*
	 * caller fills in pointer value, but it's our job to put in the lua
	 * function object
	 */
	pllua_newrefobject(L, PLLUA_FUNCTION_OBJECT, NULL);

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
	luaL_addstring(&b, "=function(...) ");
	luaL_addlstring(&b, VARDATA_ANY(comp_info->prosrc),
					VARSIZE_ANY_EXHDR(comp_info->prosrc));
	luaL_addstring(&b, " end return ");
	luaL_addstring(&b, fname);
	luaL_pushresult(&b);
	src = lua_tostring(L, -1);

	if (luaL_loadbuffer(L, src, strlen(src), fname))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	lua_remove(L, -2); /* source */
	lua_call(L, 0, 1);
	
	lua_setuservalue(L, -2);
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
 * Returns with a function object on top of the lua stack.
 */
void pllua_validate_and_push(lua_State *L, FmgrInfo *flinfo, bool trusted)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	/*
	 * We need the pg_proc row etc. every time, but we have to avoid throwing
	 * pg errors through lua.
	 */

	pllua_setcontext(PLLUA_CONTEXT_PG);
	PG_TRY();
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
				pllua_getactivation(L, act);
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
					if (act)
					{
						/*
						 * The activation is out of date, but the existing
						 * compiled function is not. Just update the activation.
						 */
						pllua_pushcfunction(L, pllua_setactivation);
						lua_pushlightuserdata(L, act);
						lua_pushvalue(L, -3);
						pllua_pcall(L, 2, 0, 0);
					}
					else
					{
						/*
						 * The activation doesn't exist yet, but the existing
						 * compiled function is up to date.
						 */
						pllua_pushcfunction(L, pllua_newactivation);
						lua_pushvalue(L, -2);
						lua_pushlightuserdata(L, flinfo->fn_mcxt);
						pllua_pcall(L, 2, 1, 0);
						act = lua_touserdata(L, -1);
						lua_pop(L, 1); /* activation is referenced, so won't be GCed */
						if (flinfo && flinfo->fn_extra == NULL)
							flinfo->fn_extra = act;
					}
					lua_remove(L, -2);
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
				lua_pop(L, 2);
			}
			else
				lua_pop(L, 1);

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
			func_info->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
			func_info->fn_tid = procTup->t_self;

			MemoryContextSwitchTo(ccxt);
				
			comp_info = palloc(sizeof(pllua_function_compile_info));
			comp_info->mcxt = ccxt;
			comp_info->func_info = func_info;
			comp_info->prosrc = DatumGetTextPP(psrc);
			
			/*
			 * XXX dig out all the needed info about arg and result types
			 * and stash it in the compile info.
			 *
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
	}
	PG_CATCH();
	{
		pllua_setcontext(PLLUA_CONTEXT_LUA);
		pllua_rethrow_from_pg(L, oldcontext);
	}
	PG_END_TRY();
	pllua_setcontext(PLLUA_CONTEXT_LUA);
	MemoryContextSwitchTo(oldcontext);
}
