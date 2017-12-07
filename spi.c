/* spi.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "commands/trigger.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "parser/analyze.h"
#include "parser/parse_param.h"

typedef struct pllua_spi_statement {
	SPIPlanPtr plan;
	bool kept;
	int nparams;
	int param_types_len;
	Oid *param_types;
	MemoryContext mcxt;
} pllua_spi_statement;

typedef struct pllua_spi_cursor {
	Portal portal;  /* or null */
	bool is_ours;   /* we created (and will close) it? */
} pllua_spi_cursor;

/*
 * Pushes up to four entries on the stack - beware!
 */
static void pllua_spi_alloc_argspace(lua_State *L,
									 int nargs,
									 Datum **values,
									 bool **isnull,
									 Oid **argtypes,
									 pllua_typeinfo ***typeinfos)
{
	if (values)
		*values = lua_newuserdata(L, nargs * sizeof(Datum));
	if (isnull)
		*isnull = lua_newuserdata(L, nargs * sizeof(bool));
	if (argtypes)
		*argtypes = lua_newuserdata(L, nargs * sizeof(Oid));
	if (typeinfos)
		*typeinfos = lua_newuserdata(L, nargs * sizeof(pllua_typeinfo *));
}

static bool pllua_spi_enter(lua_State *L)
{
	bool readonly = pllua_get_cur_act_readonly(L);
	ASSERT_PG_CONTEXT;
	SPI_connect();
#if PG_VERSION_NUM >= 100000
	{
		pllua_activation_record *pact = pllua_getinterpreter(L)->cur_activation;
		if (pact && pact->fcinfo && CALLED_AS_TRIGGER(pact->fcinfo))
			SPI_register_trigger_data((TriggerData *) pact->fcinfo->context);
	}
#endif
	return readonly;
}

static int pllua_spi_is_readonly(lua_State *L)
{
	bool readonly = pllua_get_cur_act_readonly(L);
	lua_pushboolean(L, readonly);
	return 1;
}

static void pllua_spi_exit(lua_State *L)
{
	SPI_finish();
}

/*
 * This creates the result but does not copy the data into the proper memory
 * context; see pllua_spi_save_result for that.
 *
 * args: light[tuptab] nrows [table baseidx]
 * returns: typeinfo table baseidx
 */
int pllua_spi_prepare_result(lua_State *L)
{
	SPITupleTable *tuptab = lua_touserdata(L, 1);
	lua_Integer nrows = lua_tointeger(L, 2);
	TupleDesc tupdesc = tuptab->tupdesc;
	lua_Integer base = 1;
	lua_Integer i;

	if (!lua_istable(L, 3))
	{
		lua_settop(L, 3);
		lua_createtable(L, nrows, 0);
		lua_replace(L, 3);
	}
	else
		base = 1 + lua_tointeger(L, 4);

	pllua_newtypeinfo_raw(L, tupdesc->tdtypeid, tupdesc->tdtypmod, tupdesc);
	for (i = 0; i < nrows; ++i)
	{
		HeapTuple htup = tuptab->vals[i];
		HeapTupleHeader h = htup->t_data;
		pllua_datum *d;

		/* htup might be in on-disk format or datum format. Force datum format. */
		HeapTupleHeaderSetDatumLength(h, htup->t_len);
		HeapTupleHeaderSetTypeId(h, tupdesc->tdtypeid);
		HeapTupleHeaderSetTypMod(h, tupdesc->tdtypmod);

		d = pllua_newdatum(L);
		/* we intentionally do not detoast anything here, see savedatum */
		d->value = PointerGetDatum(h);
		lua_rawseti(L, 3, i+base);
	}

	lua_pushvalue(L, 3);
	lua_pushinteger(L, base+nrows);
	lua_setfield(L, -2, "n");

	lua_pushinteger(L, base);

	return 3;
}

/*
 * stack: ... typeinfo table base
 */
static void pllua_spi_save_result(lua_State *L, lua_Integer nrows)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
	pllua_typeinfo *t = *(void **)lua_touserdata(L, -3);
	lua_Integer base = lua_tointeger(L, -1);
	lua_Integer i;

	/* we rely on the fact that rawgeti won't throw */

	for (i = 0; i < nrows; ++i)
	{
		pllua_datum *d;
		lua_rawgeti(L, -2, i+base);
		d = lua_touserdata(L, -1);
		pllua_savedatum(L, d, t);
		lua_pop(L,1);
	}

	MemoryContextSwitchTo(oldcontext);
}



static int pllua_cursor_options(lua_State *L, int nd)
{
	int flag = 0;
	if (lua_isnoneornil(L, nd))
		return 0;
	luaL_checktype(L, nd, LUA_TTABLE);
	lua_getfield(L, nd, "scroll");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_SCROLL : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "no_scroll");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_NO_SCROLL : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "fast_start");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_FAST_PLAN : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "custom_plan");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_CUSTOM_PLAN : 0;
	lua_pop(L, 1);
	lua_getfield(L, nd, "generic_plan");
	flag |= (lua_toboolean(L, -1)) ? CURSOR_OPT_GENERIC_PLAN : 0;
	lua_pop(L, 1);
	return flag;
}

static int pllua_spi_prepare_recursion = -1;
static post_parse_analyze_hook_type pllua_spi_prev_parse_hook = NULL;


static void pllua_spi_prepare_checkparam_hook(ParseState *pstate,
											  Query *query)
{
	check_variable_parameters(pstate, query);
	if (pllua_spi_prev_parse_hook)
		pllua_spi_prev_parse_hook(pstate, query);
}

static void pllua_spi_prepare_parser_setup_hook(ParseState *pstate, void *arg)
{
	pllua_spi_statement *stmt = arg;
	parse_variable_parameters(pstate, &stmt->param_types, &stmt->param_types_len);
}

static pllua_spi_statement *pllua_spi_make_statement(lua_State *L,
													 const char *str,
													 int nargs_known,
													 Oid *argtypes,
													 int opts)
{
	MemoryContext mcxt = AllocSetContextCreate(CurrentMemoryContext,
											   "PL/Lua SPI statement object",
											   ALLOCSET_SMALL_SIZES);
	MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
	pllua_spi_statement *stmt = palloc0(sizeof(pllua_spi_statement));
	int i;

	ASSERT_PG_CONTEXT;

	stmt->mcxt = mcxt;
	stmt->nparams = 0;

	if (nargs_known > 0)
	{
		stmt->param_types_len = nargs_known;
		stmt->param_types = palloc(nargs_known * sizeof(Oid));
		memcpy(stmt->param_types, argtypes, nargs_known * sizeof(Oid));
	}
	else
	{
		/* we have to preallocate this to get it in the right context */
		stmt->param_types_len = 16;  /* wholly arbitrary */
		stmt->param_types = palloc0(16 * sizeof(Oid));
	}

	/*
	 * GAH. To do parameter type checking properly, we have to install our
	 * own global post-parse hook transiently.
	 */
	if (++pllua_spi_prepare_recursion != 0)
		elog(ERROR, "pllua: recursive entry into prepare!"); /* paranoia */
	PG_TRY();
	{
		pllua_spi_prev_parse_hook = post_parse_analyze_hook;
		post_parse_analyze_hook = pllua_spi_prepare_checkparam_hook;

		stmt->plan = SPI_prepare_params(str,
										pllua_spi_prepare_parser_setup_hook,
										stmt,
										opts);

		post_parse_analyze_hook = pllua_spi_prev_parse_hook;
		--pllua_spi_prepare_recursion;
	}
	PG_CATCH();
	{
		post_parse_analyze_hook = pllua_spi_prev_parse_hook;
		--pllua_spi_prepare_recursion;
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!stmt->plan)
		elog(ERROR, "spi error: %s", SPI_result_code_string(SPI_result));

	for (i = stmt->param_types_len; i > 0; --i)
	{
		if (stmt->param_types[i - 1] != 0)
		{
			stmt->nparams = i;
			break;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	return stmt;
}

/*
 * prepare(cmd,[{argtypes}, [{flag=true,...}])
 *
 * argtypes are given as text or typeinfo.
 * flags: "scroll","no_scroll","fast_start","custom_plan","generic_plan"
 */
static int pllua_spi_prepare(lua_State *L)
{
	const char *str = lua_tostring(L, 1);
	int opts = pllua_cursor_options(L, 3);
	void **volatile p;
	int i;
	int nargs = 0;
	Oid d_argtypes[100];
	Oid *argtypes = d_argtypes;

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, NULL, NULL, &argtypes, NULL);

	lua_settop(L, 2);

	/* make the plan object - index 3 */
	p = pllua_newrefobject(L, PLLUA_SPI_STMT_OBJECT, NULL, true);

	nargs = 0;
	if (!lua_isnoneornil(L, 2))
	{
		for(i = 1; ; ++i)
		{
			void **pt;
			pllua_typeinfo *t;

			if (lua_geti(L, 2, i) == LUA_TNIL)
				break;

			if (lua_isstring(L, -1))
			{
				lua_pushcfunction(L, pllua_typeinfo_parsetype);
				lua_pushvalue(L, -2);
				lua_call(L, 1, 1);
				if (lua_isnil(L, -1))
					luaL_error(L, "unknown type '%s'", lua_tostring(L, -2));
				lua_remove(L, -2);
			}

			pt = pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
			if (!pt)
				luaL_error(L, "unexpected object type in argtypes list");
			t = *pt;

			argtypes[nargs++] = t->typeoid;
		}
	}

	PLLUA_TRY();
	{
		pllua_spi_enter(L);

		pllua_spi_statement *stmt = pllua_spi_make_statement(L, str, nargs, argtypes, opts);

		/* reparent everything */
		SPI_keepplan(stmt->plan);
		stmt->kept = true;
		MemoryContextSetParent(stmt->mcxt, pllua_get_memory_cxt(L));
		*p = stmt;

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	lua_getuservalue(L, 3);

	{
		pllua_spi_statement *stmt = *p;

		for(i = 0; i < stmt->nparams; ++i)
		{
			void **pt;
			pllua_typeinfo *t;

			lua_pushcfunction(L, pllua_typeinfo_lookup);
			lua_pushinteger(L, (lua_Integer) stmt->param_types[i]);
			lua_call(L, 1, 1);
			pt = pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
			if (!pt)
				luaL_error(L, "unexpected type in paramtypes list: %d", (lua_Integer) stmt->param_types[i]);
			t = *pt;

			lua_rawseti(L, -2, i+1);
		}
	}

	lua_pushvalue(L, 3);
	return 1;
}

/*
 * args: light[values] light[isnull] light[argtypes] arg...
 *
 * argtypes are as determined by the parser, may not match the actual type of
 * arg.
 */
int pllua_spi_convert_args(lua_State *L)
{
	Datum *values = lua_touserdata(L, 1);
	bool *isnull = lua_touserdata(L, 2);
	Oid *argtypes = lua_touserdata(L, 3);
	int nargs = lua_gettop(L) - 3;
	int argbase = 4;
	int i;

	for (i = 0; i < nargs; ++i)
	{
		if (!lua_isnil(L, argbase+i))
		{
			pllua_typeinfo *dt;
			pllua_datum *d;
			lua_pushcfunction(L, pllua_typeinfo_lookup);
			lua_pushinteger(L, (lua_Integer) argtypes[i]);
			lua_call(L, 1, 1);
			lua_pushvalue(L, argbase+i);
			lua_call(L, 1, 1);
			d = pllua_checkanydatum(L, -1, &dt);
			if (dt->typeoid != argtypes[i])
				luaL_error(L, "inconsistent value type in SPI parameter list");
			values[i] = d->value;
			isnull[i] = false;
		}
		else
		{
			values[i] = (Datum)0;
			isnull[i] = true;
		}
	}
	return 0;
}

/*
 * spi.execute(cmd, arg...) returns {rows...}
 * also stmt:execute(arg...)
 *
 */
static int pllua_spi_execute(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	const char *str = lua_tostring(L, 1);
	int nargs = lua_gettop(L) - 1;
	int argbase = 2;
	Datum d_values[100];
	bool d_isnull[100];
	Oid d_argtypes[100];
	Datum *values = d_values;
	bool *isnull = d_isnull;
	Oid *argtypes = d_argtypes;
	volatile lua_Integer nrows = -1;
	int i;

	if (!str && !p)
		luaL_error(L, "incorrect argument type for execute, string or statement expected");

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, &values, &isnull, &argtypes, NULL);

	/* check encoding of query string */
	if (str)
		pllua_verify_encoding(L, str);

	/* we're going to re-push all the args, better have space */
	luaL_checkstack(L, 40+nargs, NULL);

	PLLUA_TRY();
	{
		bool readonly = pllua_spi_enter(L);
		pllua_spi_statement *stmt = p ? *p : NULL;
		ParamListInfo paramLI = NULL;
		int rc;

		if (!stmt)
			stmt = pllua_spi_make_statement(L, str, nargs, argtypes, 0);

		if (stmt->nparams != nargs)
			elog(ERROR, "pllua: wrong number of arguments to SPI query: expected %d got %d", stmt->nparams, nargs);

		pllua_pushcfunction(L, pllua_spi_convert_args);
		lua_pushlightuserdata(L, values);
		lua_pushlightuserdata(L, isnull);
		lua_pushlightuserdata(L, stmt->param_types);
		for (i = 0; i < nargs; ++i)
		{
			lua_pushvalue(L, argbase+i);
		}
		pllua_pcall(L, 3+nargs, 0, 0);

		if (nargs > 0)
		{
			paramLI = (ParamListInfo) palloc(offsetof(ParamListInfoData, params) +
											 nargs * sizeof(ParamExternData));
			/* we have static list of params, so no hooks needed */
			paramLI->paramFetch = NULL;
			paramLI->paramFetchArg = NULL;
			paramLI->parserSetup = NULL;
			paramLI->parserSetupArg = NULL;
			paramLI->numParams = nargs;
			paramLI->paramMask = NULL;

			for (i = 0; i < nargs; i++)
			{
				ParamExternData *prm = &paramLI->params[i];

				prm->value = values[i];
				prm->isnull = isnull[i];
				prm->pflags = PARAM_FLAG_CONST;
				prm->ptype = stmt->param_types[i];
			}
		}

		rc = SPI_execute_plan_with_paramlist(stmt->plan, paramLI,
											 readonly, Min(LUA_MAXINTEGER-1, LONG_MAX-1));
		if (rc >= 0)
		{
			nrows = SPI_processed;
			if (SPI_tuptable)
			{
				BlessTupleDesc(SPI_tuptable->tupdesc);

				pllua_pushcfunction(L, pllua_spi_prepare_result);
				lua_pushlightuserdata(L, SPI_tuptable);
				lua_pushinteger(L, nrows);
				pllua_pcall(L, 2, 3, 0);

				pllua_spi_save_result(L, nrows);
				lua_pop(L, 1);
			}
			else
				lua_pushinteger(L, nrows);
		}
		else
			elog(ERROR, "spi error: %s", SPI_result_code_string(rc));

		/*
		 * If we made our own statement, we didn't save it so it goes away here
		 */

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static int pllua_stmt_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_SPI_STMT_OBJECT);
	pllua_spi_statement *stmt = p ? *p : NULL;

	ASSERT_LUA_CONTEXT;

	if (!p)
		return 0;

	*p = NULL;
	if (!stmt)
		return 0;

	PLLUA_TRY();
	{
		if (stmt->kept && stmt->plan)
			SPI_freeplan(stmt->plan);
		MemoryContextDelete(stmt->mcxt);
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

static struct luaL_Reg spi_funcs[] = {
	{ "execute", pllua_spi_execute },
	{ "prepare", pllua_spi_prepare },
	{ "readonly", pllua_spi_is_readonly },
	/* { "rows", pllua_spi_rows },
	   { "find", pllua_spi_find }, */
	{ NULL, NULL }
};

/*
static struct luaL_Reg spi_cursor_methods[] = {
	{ "fetch", pllua_cursor_fetch },
	{ NULL, NULL }
};
static struct luaL_Reg spi_cursor_mt[] = {
	{ "__gc", pllua_cursor_gc },
	{ NULL, NULL }
};
*/

static int pllua_spi_noop(lua_State *L)
{
	lua_settop(L, 1); return 1;  /* return first arg */
}

static int pllua_spi_noop_true(lua_State *L)
{
	lua_pushboolean(L, 1); return 1; /* return true */
}

static struct luaL_Reg spi_stmt_methods[] = {
	{ "save", pllua_spi_noop },
	{ "issaved", pllua_spi_noop_true },
	{ "execute", pllua_spi_execute },
	/* { "rows", pllua_spi_stmt_rows }, */
	{ NULL, NULL }
};
static struct luaL_Reg spi_stmt_mt[] = {
	{ "__gc", pllua_stmt_gc },
	{ NULL, NULL }
};

int pllua_open_spi(lua_State *L)
{
	pllua_newmetatable(L, PLLUA_SPI_STMT_OBJECT, spi_stmt_mt);
	luaL_newlib(L, spi_stmt_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushvalue(L, -1);
	luaL_setfuncs(L, spi_funcs, 0);
	return 1;
}
