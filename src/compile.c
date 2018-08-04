
#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"


/*
 * Do fairly minimalist validation on the procTup to ensure that we're not
 * going to do something dangerous or security-violating. More detailed checks
 * can be left to the validator func. Throws a pg error on failure.
 *
 * We only do this when compiling a new function.
 */
static void
pllua_validate_proctup(lua_State *L, Oid fn_oid,
					   HeapTuple procTup, bool trusted)
{
	HeapTuple		lanTup;
	Form_pg_language lanStruct;
	Form_pg_proc	procStruct;
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	ASSERT_PG_CONTEXT;

	lanTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
	if (!HeapTupleIsValid(lanTup))
		elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
	lanStruct = (Form_pg_language) GETSTRUCT(lanTup);

	if ((trusted && !lanStruct->lanpltrusted)
		|| (!trusted && lanStruct->lanpltrusted))
	{
		/* this can't happen unless someone is monkeying with catalogs. */
		elog(ERROR, "trusted state mismatch for function %u in language %u",
			 fn_oid, procStruct->prolang);
	}

	ReleaseSysCache(lanTup);
}


/*
 * Given a function body chunk or inline chunk on stack top, prepare it for
 * execution. The function is left below its single first arg.
 */
static void
pllua_prepare_function(lua_State *L, bool trusted)
{
	lua_newtable(L);
	if (lua_rawgetp(L, LUA_REGISTRYINDEX,
					trusted ? PLLUA_SANDBOX_META : PLLUA_GLOBAL_META) != LUA_TTABLE)
		luaL_error(L, "missing environment metatable");
	lua_setmetatable(L, -2);
	lua_pushvalue(L, -1);
	pllua_set_environment(L, -3);
}

/*
 * Given the body of a DO-block, compile it. This is here mostly to centralize
 * in this file (pllua_compile_inline and pllua_compile) the environment tweaks
 * that we do.
 */
void
pllua_compile_inline(lua_State *L, const char *str, bool trusted)
{
	if (luaL_loadbufferx(L, str, strlen(str), "DO-block", "t"))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	pllua_prepare_function(L, trusted);
}

/*
 * Given a comp_info containing the info we need, compile a function and make
 * an object for it. However, we don't actually store the func_info into the
 * object; caller does that, after reparenting the memory context.
 *
 * Note that "compiling" a function in the current setup may execute some user
 * code (except in validate_only mode).
 *
 * Returns the object on the stack (except in validate_only mode, which returns
 * nothing)
 */
int
pllua_compile(lua_State *L)
{
	pllua_function_compile_info *comp_info = lua_touserdata(L, 1);
	pllua_function_info *func_info = comp_info->func_info;
	const char	   *fname = func_info->name;
	const char	   *src;
	luaL_Buffer b;

	if (!comp_info->validate_only)
	{
		/* caller fills in pointer */
		pllua_newrefobject(L, PLLUA_FUNCTION_OBJECT, NULL, true);
	}

	luaL_buffinit(L, &b);

	/*
	 * New-style function environment:
	 *
	 * The string we construct is:
	 *   local self = (...) local function f(args) body end return f
	 *
	 * We construct an empty table and set a metatable on it so that it
	 * inherits from _G (the sandbox env or the real env, depending).
	 * We set the environment of the chunk to this table.
	 * Then we pass that table as the first arg when we run the chunk,
	 * so it gets set as the value of the local "self" as well.
	 *
	 * For trigger funcs, the args list is a standardized one.
	 */
	luaL_addstring(&b, "local self = (...) local function ");
	luaL_addstring(&b, fname);
	luaL_addchar(&b, '(');
	if (func_info->is_trigger)
	{
		luaL_addstring(&b, "trigger,old,new,...");
	}
	else if (func_info->is_event_trigger)
	{
		luaL_addstring(&b, "trigger");
	}
	else if (comp_info->nargs > 0)
	{
		int n = 0;
		int i;

		/*
		 * Build up the list from the parameter names, excluding any
		 * OUT parameters; stop when we find an unnamed arg. Note that
		 * argmodes can be null if all params are IN.
		 */
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
		/*
		 * If we didn't get all the args (which includes the VARIADIC "any"
		 * case since we don't let that have a name), append ... to get
		 * the rest.
		 */
		if (n < comp_info->nargs)
		{
			if (n > 0)
				luaL_addchar(&b, ',');
			luaL_addstring(&b, "...");
		}
	}
	luaL_addstring(&b, ") ");
	/*
	 * Actual function body.
	 */
	luaL_addlstring(&b, VARDATA_ANY(comp_info->prosrc),
					VARSIZE_ANY_EXHDR(comp_info->prosrc));
	/*
	 * Terminate string and convert to lua value
	 */
	luaL_addstring(&b, " end return ");
	luaL_addstring(&b, fname);
	luaL_pushresult(&b);
	src = lua_tostring(L, -1);

	/*
	 * Load the code into lua but run nothing. (Syntax errors show up here.)
	 */
	if (luaL_loadbufferx(L, src, strlen(src), fname, "t"))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	lua_remove(L, -2); /* drop source */

	/*
	 * Bail out here if validating.
	 */
	if (comp_info->validate_only)
		return 0;

	pllua_prepare_function(L, func_info->trusted);

	/*
	 * Run the code to obtain the function value as a result.
	 */
	lua_call(L, 1, 1);

	/*
	 * Store in the function object's uservalue table.
	 */
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
 *
 * Returns true if something was stored, false if not.
 */
int
pllua_intern_function(lua_State *L)
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
static void
pllua_resolve_activation(lua_State *L,
						 pllua_func_activation *act,
						 pllua_function_info *func_info,
						 FunctionCallInfo fcinfo)
{
	MemoryContext	oldcontext;
	FmgrInfo	   *flinfo = fcinfo->flinfo;
	Oid				rettype = func_info->rettype;

	if (act->resolved)
		return;

	ASSERT_PG_CONTEXT;

	oldcontext = MemoryContextSwitchTo(flinfo->fn_mcxt);

	if (func_info->polymorphic_ret ||
		func_info->returns_row)
	{
		act->typefuncclass = get_call_result_type(fcinfo,
												  &act->rettype,
												  &act->tupdesc);
		if (act->tupdesc && act->tupdesc->tdrefcount != -1)
		{
			/*
			 * This is a ref-counted tupdesc, but we can't pin it because any
			 * such pin would belong to a resource owner, and we can't
			 * guarantee that our required data lifetimes nest properly with
			 * the resource owners. So make a copy instead. (If it's not
			 * refcounted, then it'll already be in fn_mcxt.)
			 */
			act->tupdesc = CreateTupleDescCopy(act->tupdesc);
		}
	}
	else
	{
		act->rettype = rettype;
		act->typefuncclass = TYPEFUNC_SCALAR;
	}

	act->retdomain = get_typtype(act->rettype) == TYPTYPE_DOMAIN;
	act->polymorphic = func_info->polymorphic;
	act->variadic_call = get_fn_expr_variadic(fcinfo->flinfo);
	act->nargs = func_info->nargs;
	act->retset = func_info->retset;
	act->readonly = func_info->readonly;

	if (act->polymorphic)
	{
		/*
		 * Polymorphic arguments. Copy the argtypes list and resolve the actual
		 * types from the call site.
		 */
		act->argtypes = palloc(act->nargs * sizeof(Oid));
		memcpy(act->argtypes,
			   func_info->argtypes,
			   act->nargs * sizeof(Oid));
		if (!resolve_polymorphic_argtypes(act->nargs,
										  act->argtypes,
										  NULL,  /* these are IN params only */
										  flinfo->fn_expr))
			elog(ERROR,"failed to resolve polymorphic argtypes");
	}
	else
		act->argtypes = func_info->argtypes;

	MemoryContextSwitchTo(oldcontext);
	act->resolved = true;
}

/*
 * Load up our func_info and comp_info structures from the function's catalog
 * entry.
 */
static void
pllua_load_from_proctup(lua_State *L,
						Oid fn_oid,
						pllua_function_info *func_info,
						pllua_function_compile_info *comp_info,
						HeapTuple procTup,
						bool trusted)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(func_info->mcxt);
	Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	bool		isnull;
	Datum		psrc;
	int			i;

	func_info->name = pstrdup(NameStr(procStruct->proname));
	func_info->fn_oid = fn_oid;
	func_info->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
	func_info->fn_tid = procTup->t_self;

	func_info->rettype = procStruct->prorettype;
	func_info->returns_row = type_is_rowtype(func_info->rettype);
	func_info->retset = procStruct->proretset;
	func_info->polymorphic_ret = IsPolymorphicType(func_info->rettype);

	func_info->language_oid = procStruct->prolang;
	func_info->trusted = trusted;

	func_info->nargs = procStruct->pronargs;
	func_info->variadic = procStruct->provariadic != InvalidOid;
	func_info->variadic_any = procStruct->provariadic == ANYOID;
	func_info->readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);
	func_info->is_trigger = (procStruct->prorettype == TRIGGEROID);
	func_info->is_event_trigger = (procStruct->prorettype == EVTTRIGGEROID);
	func_info->polymorphic = false;		/* set below */

	Assert(func_info->nargs == procStruct->proargtypes.dim1);

	func_info->argtypes = (Oid *) palloc(func_info->nargs * sizeof(Oid));
	memcpy(func_info->argtypes,
		   procStruct->proargtypes.values,
		   func_info->nargs * sizeof(Oid));

	/* check for polymorphic arg */
	for (i = 0; i < func_info->nargs; ++i)
	{
		if (IsPolymorphicType(func_info->argtypes[i]) ||
			func_info->argtypes[i] == ANYOID)
		{
			func_info->polymorphic = true;
			break;
		}
	}

	/*
	 * Redo the most essential validation steps out of sheer paranoia
	 */
	pllua_validate_proctup(L, fn_oid, procTup, trusted);

	/*
	 * Stuff below is only needed for compiling
	 */
	MemoryContextSwitchTo(comp_info->mcxt);

	psrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	comp_info->prosrc = DatumGetTextPP(psrc);
	comp_info->validate_only = false;

	/*
	 * Compile needs the allargs list (to get names and modes) as well as the
	 * runtime (IN only) argtypes list set above.
	 */
	comp_info->nargs = procStruct->pronargs;
	comp_info->nallargs = get_func_arg_info(procTup,
											&comp_info->allargtypes,
											&comp_info->argnames,
											&comp_info->argmodes);
	comp_info->variadic = procStruct->provariadic;

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Return true if func_info is an up to date compile of procTup.
 */
static bool
pllua_function_valid(pllua_function_info *func_info,
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
	FmgrInfo	*flinfo = fcinfo->flinfo;
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
		Oid		fn_oid = flinfo->fn_oid;
		int		rc;

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
			flinfo->fn_extra = act;
		}
		else
			pllua_getactivation(L, act);

		/*
		 * This part may have to be repeated in some rare recursion scenarios.
		 */
		for (;;)
		{
			pllua_function_info *func_info;
			pllua_function_compile_info *comp_info;
			MemoryContext fcxt;
			MemoryContext ccxt;
			HeapTuple	procTup;

			/* Get the pg_proc tuple. */
			procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
			if (!HeapTupleIsValid(procTup))
				elog(ERROR, "cache lookup failed for function %u", fn_oid);

			if (pllua_function_valid(act->func_info, procTup))
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
			 * scratch. Create the func_info, compile_info and contexts. Note
			 * that the compile context is always transient, but the function
			 * context is reparented to the long-lived lua context on success.
			 *
			 * (CurrentMemoryContext at this point is still the original
			 * caller's context, assumed transient)
			 */
			fcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua function object",
										 ALLOCSET_SMALL_SIZES);
			ccxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua compile context",
										 ALLOCSET_SMALL_SIZES);

			func_info = MemoryContextAlloc(fcxt, sizeof(pllua_function_info));
			func_info->mcxt = fcxt;

			comp_info = MemoryContextAlloc(ccxt, sizeof(pllua_function_compile_info));
			comp_info->mcxt = ccxt;
			comp_info->func_info = func_info;

			pllua_load_from_proctup(L, fn_oid,
									func_info, comp_info,
									procTup, trusted);

			/*
			 * Resolve the activation before compiling in case the user code
			 * tries to do something that needs access to it.
			 */
			pllua_resolve_activation(L, act, func_info, fcinfo);

			/*
			 * Beware, compiling can invoke user-supplied code, which might
			 * in turn recurse here. We trust that stack depth checks will
			 * break any such loop if need be.
			 */
			pllua_pushcfunction(L, pllua_compile);
			lua_pushlightuserdata(L, comp_info);
			rc = pllua_pcall_nothrow(L, 1, 1, 0);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(ccxt);

			if (rc)
			{
				/* error. bail out */
				act->resolved = false;
				MemoryContextDelete(fcxt);
				pllua_rethrow_from_lua(L, rc);
			}
			else
			{
				void **p = lua_touserdata(L, -1);
				MemoryContextSetParent(fcxt, pllua_get_memory_cxt(L));
				*p = func_info;
			}

			/*
			 * Try and intern the function. Since we uninterned it earlier, we
			 * expect this to succeed, but a recursive call could have interned
			 * a new version already (which will be at least as new as ours).
			 * Worse, if so, that new version could already be out of date,
			 * meaning that we have to loop back to check the pg_proc row
			 * again.
			 */
			/* stack: activation funcinfo */
			pllua_pushcfunction(L, pllua_intern_function);
			lua_insert(L, -2);
			lua_pushinteger(L, (lua_Integer) fn_oid);
			pllua_pcall(L, 2, 0, 0);
			func_info = NULL;
			ReleaseSysCache(procTup);
		}

		/*
		 * Post-compile per-call validation (mostly here to avoid more catch
		 * blocks elsewhere)
		 */
		if (act->func_info->retset)
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


/*
 * Returns true if typeid (a pseudotype) is acceptable for either a result type
 * (if is_result) or a param of mode "argmode".
 */
static bool
pllua_acceptable_pseudotype(lua_State *L,
							Oid typeid,
							bool is_result,
							char argmode)
{
	bool is_input = !is_result;
	bool is_output = is_result;

	if (!is_result)
	{
		switch (argmode)
		{
			case PROARGMODE_VARIADIC:
			case PROARGMODE_IN:
				is_input = true;
				is_output = false;
				break;
			case PROARGMODE_INOUT:
				is_input = true;
				is_output = true;
				break;
			case PROARGMODE_TABLE:
			case PROARGMODE_OUT:
				is_input = false;
				is_output = true;
				break;
		}
	}

	/*
	 * we actually support most pseudotypes, but we whitelist rather
	 * than blacklist to reduce the chance of future breakage.
	 */
	switch (typeid)
	{
		/* only as return types */
		case TRIGGEROID:
		case EVTTRIGGEROID:
		case VOIDOID:
			return !is_input;

		/* only as argument type */
		case ANYOID:
			return !is_output;

		/* ok for either argument or result */
		case RECORDOID:
		case RECORDARRAYOID:
		case CSTRINGOID:
			return true;

		/* core code has the job of validating these are correctly used */
		case ANYARRAYOID:
		case ANYNONARRAYOID:
		case ANYELEMENTOID:
		case ANYENUMOID:
		case ANYRANGEOID:
			return true;

		default:
			return false;
	}
}

static bool
pllua_acceptable_name(lua_State *L, const char *name)
{
	unsigned char *p = (unsigned char *) name;
	unsigned char c;
	if (!name)
		return false;
	c = *p;
	if (!c || (c >= '0' && c <= '9'))
		return false;
	while ((c = *p++))
		if (!((c >= 'A' && c <= 'Z') ||
			  (c >= 'a' && c <= 'z') ||
			  (c >= '0' && c <= '9') ||
			  (c == '_')))
			return false;
	switch (name[0])
	{
		case 'a': return strcmp(name,"and") != 0;
		case 'b': return strcmp(name,"break") != 0;
		case 'd': return strcmp(name,"do") != 0;
		case 'e': return ((strcmp(name,"else") != 0) &&
						  (strcmp(name,"elseif") != 0) &&
						  (strcmp(name,"end") != 0));
		case 'f': return ((strcmp(name,"false") != 0) &&
						  (strcmp(name,"for") != 0) &&
						  (strcmp(name,"function") != 0));
		case 'g': return strcmp(name,"goto") != 0;
		case 'i': return ((strcmp(name,"if") != 0) &&
						  (strcmp(name,"in") != 0));
		case 'l': return strcmp(name,"local") != 0;
		case 'n': return ((strcmp(name,"nil") != 0) &&
						  (strcmp(name,"not") != 0));
		case 'o': return strcmp(name,"or") != 0;
		case 'r': return ((strcmp(name,"repeat") != 0) &&
						  (strcmp(name,"return") != 0));
		case 't': return ((strcmp(name,"then") != 0) &&
						  (strcmp(name,"true") != 0));
		case 'u': return strcmp(name,"until") != 0;
		case 'w': return strcmp(name,"while") != 0;
		default:
			return true;
	}
}

/*
 * This is the guts of the validator function
 */
void
pllua_validate_function(lua_State *L,
						Oid fn_oid,
						bool trusted)
{
	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		HeapTuple	procTup;
		pllua_function_info *func_info;
		pllua_function_compile_info *comp_info;
		int			i;
		bool		nameless = false;

		procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
		if (!HeapTupleIsValid(procTup))
			elog(ERROR, "cache lookup failed for function %u", fn_oid);

		/* don't worry about memory contexts, it's all transient */

		func_info = palloc(sizeof(pllua_function_info));
		func_info->mcxt = CurrentMemoryContext;

		comp_info = palloc(sizeof(pllua_function_compile_info));
		comp_info->func_info = func_info;
		comp_info->mcxt = CurrentMemoryContext;

		pllua_load_from_proctup(L, fn_oid,
								func_info, comp_info,
								procTup, trusted);

		/*
		 * Produce a better error message if the function name itself would
		 * break the syntax.
		 */
		if (!pllua_acceptable_name(L, func_info->name))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Lua function name \"%s\" is not a valid Lua identifier", func_info->name)));

		/* nitpick over the argument and result types. */
		if (get_typtype(func_info->rettype) == TYPTYPE_PSEUDO
			&& !pllua_acceptable_pseudotype(L, func_info->rettype, true, ' '))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("PL/Lua functions cannot return type %s",
								   format_type_be(func_info->rettype))));
		}

		/* check OUT as well as IN args */
		for (i = 0; i < comp_info->nallargs; ++i)
		{
			Oid		argtype = comp_info->allargtypes[i];
			char	argmode = (comp_info->argmodes
							   ? comp_info->argmodes[i]
							   : PROARGMODE_IN);
			const char *argname = (comp_info->argnames
								   ? comp_info->argnames[i]
								   : "");

			if (get_typtype(argtype) == TYPTYPE_PSEUDO
				&& !pllua_acceptable_pseudotype(L, argtype, false, argmode))
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/Lua functions cannot accept type %s",
								format_type_be(argtype))));
			}

			/*
			 * IN or INOUT argument with a name should not follow one without a
			 * name. VARIADIC "any" must not have a name. These restrictions
			 * don't matter at SQL level, but violating them leads to the
			 * possibility of non-obvious errors with variable scopes, so best
			 * to forbid them.
			 */
			switch (argmode)
			{
				case PROARGMODE_IN:
				case PROARGMODE_INOUT:
					if (argname[0])
					{
						if (nameless)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("PL/Lua function arguments with names must not follow arguments without names")));
					}
					else
						nameless = true;
					break;

				case PROARGMODE_TABLE:
				case PROARGMODE_OUT:
					break;

				case PROARGMODE_VARIADIC:
					if (argtype == ANYOID && argname[0])
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("PL/Lua function arguments of type VARIADIC \"any\" must not have names")));
					break;
			}

			/*
			 * Produce a better error message if the argument name would break
			 * the syntax.
			 */
			if (argname && argname[0] && !pllua_acceptable_name(L, argname))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/Lua argument name \"%s\" is not a valid Lua identifier", argname)));
		}

		/*
		 * We really don't want to invoke any user-defined code for this, so we
		 * arrange to load() the function body but execute nothing. This should
		 * catch syntax errors just fine. But disable body checks entirely if
		 * chech_function_bodies is false.
		 */
		comp_info->validate_only = true;

		if (check_function_bodies)
		{
			pllua_pushcfunction(L, pllua_compile);
			lua_pushlightuserdata(L, comp_info);
			pllua_pcall(L, 1, 0, 0);
		}

		ReleaseSysCache(procTup);
	}
	PLLUA_CATCH_RETHROW();
}
