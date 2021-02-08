/* jsonb.c */

#include "pllua.h"

#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "utils/datum.h"
#include "utils/builtins.h"
#if PG_VERSION_NUM >= 100000
#include "utils/fmgrprotos.h"
#endif
#include "utils/jsonb.h"

#if PG_VERSION_NUM < 110000
#define DatumGetJsonbP(d_) DatumGetJsonb(d_)
#endif

/*
 * called with the container value on top of the stack
 *
 * Must push keytable, prevkey, index(=1)
 * where prevkey is nil for objects and 0 for arrays
 *
 * For objects, keytable is a sequence of string or number keys. For arrays,
 * keytable is a sequence of integers in ascending order giving the "present"
 * keys.
 *
 * We already checked that this is a container (defined as a Lua table or a
 * value with a __pairs metamethod).
 *
 */
static JsonbIteratorToken
pllua_jsonb_pushkeys(lua_State *L, bool empty_object, int array_thresh, int array_frac)
{
	lua_Integer min_intkey = LUA_MAXINTEGER;
	lua_Integer max_intkey = 0;
	int numintkeys = 0;
	int numkeys = 0;
	int isint;
	lua_Integer intval;
	bool metaloop;
	int tabidx = lua_absindex(L, -1);
	int keytabidx;
	int numkeytabidx;
	bool known_object = false;
	bool known_array = false;

	switch (luaL_getmetafield(L, -1, "__jsonb_object"))
	{
		case LUA_TBOOLEAN:
			if (lua_toboolean(L, -1))
				known_object = true;
			else
				known_array = true;
			FALLTHROUGH; /* FALLTHROUGH */
		default:
			lua_pop(L, 1);
			break;
		case LUA_TNIL:
			break;
	}

	lua_newtable(L);
	keytabidx = lua_absindex(L, -1);

	lua_newtable(L);
	numkeytabidx = lua_absindex(L, -1);

	metaloop = pllua_pairs_start(L, tabidx, true);

	/* stack: keytable, numkeytab, [iter, state,] key */

	while (metaloop ? pllua_pairs_next(L) : lua_next(L, tabidx))
	{
		lua_pop(L, 1);			/* don't need the value */
		lua_pushvalue(L, -1);   /* keytable numkeytab [iter state] key key */
		++numkeys;
		/*
		 * this is the input table's key: here, we accept strings containing
		 * integer values as integers
		 */
		intval = lua_tointegerx(L, -1, &isint);
		if (isint)
		{
			if (intval > max_intkey)
				max_intkey = intval;
			if (intval < min_intkey)
				min_intkey = intval;
			++numintkeys;
			lua_pushvalue(L, -1);
			lua_rawseti(L, numkeytabidx, numintkeys);
		}

		switch (lua_type(L, -1))
		{
			case LUA_TUSERDATA:
			case LUA_TTABLE:
				/*
				 * Don't try conversions that might fail if this is an array,
				 * since we're going to ignore non-integer keys if so
				 */
				if (!known_array)
				{
					if (luaL_getmetafield(L, -1, "__tostring") == LUA_TNIL)
						luaL_error(L, "cannot serialize userdata or table which lacks __tostring as a key");
					lua_insert(L, -2);
					lua_call(L, 1, 1);
					if (lua_type(L, -1) != LUA_TSTRING)
						luaL_error(L, "tostring on table or userdata object did not return a string");
				}
				break;
			case LUA_TSTRING:
			case LUA_TNUMBER:
				break;
			default:
				luaL_error(L, "cannot serialize scalar value of type %s as key", luaL_typename(L, -1));
		}

		lua_rawseti(L, keytabidx, numkeys);
	}

	/* stack: keytable numkeytab */

	if (known_object
		|| (!known_array
			&& ((empty_object && numkeys == 0)
				|| (numkeys != numintkeys)
				|| (min_intkey < 1)
				|| (numintkeys > 0 && (min_intkey > array_thresh))
				|| (numintkeys > 0 && (max_intkey > (array_frac * numkeys))))))
	{
		/* it's an object. Use the string key table */
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushinteger(L, 1);
		return WJB_BEGIN_OBJECT;
	}
	else
	{
		/* it's an array */
		lua_remove(L, -2);
		/* need to sort the array */
		lua_getfield(L, lua_upvalueindex(1), "sort");
		lua_pushvalue(L, -2);
		lua_call(L, 1, 0);
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 1);
		return WJB_BEGIN_ARRAY;
	}
}

/*
 * Given a datum input, which might be json or jsonb or have a cast, figure out
 * what to put into JsonbValue. We're already in pg context in the temporary
 * memory context, and the value at -1 on the lua stack is the .f_to_jsonb
 * pgfunc object from the typeinfo.
 */
static void
pllua_jsonb_from_datum(lua_State *L, JsonbValue *pval,
					   pllua_datum *d, pllua_typeinfo *dt)
{
	LOCAL_FCINFO(fcinfo, 1);
	FmgrInfo *fn = *(void **) lua_touserdata(L, -1);
	Datum res;

	if (!fn || !OidIsValid(fn->fn_oid))
	{
		Oid		fnoid = DatumGetObjectId(
							DirectFunctionCall1(regprocedurein,
												CStringGetDatum("pg_catalog.to_jsonb(anyelement)")));
		fn = pllua_pgfunc_init(L, -1, fnoid, 1, &dt->typeoid, JSONBOID);
	}

	InitFunctionCallInfoData(*fcinfo, fn, 1, InvalidOid, NULL, NULL);
	LFCI_ARG_VALUE(fcinfo,0) = d->value;
	LFCI_ARGISNULL(fcinfo,0) = false;
	res = FunctionCallInvoke(fcinfo);

	if (fcinfo->isnull)
		pval->type = jbvNull;
	else
	{
		Jsonb *jb = DatumGetJsonbP(res);
		if (JB_ROOT_IS_SCALAR(jb))
		{
			JsonbValue dummy;
			JsonbIterator *it = JsonbIteratorInit(&jb->root);
			if (JsonbIteratorNext(&it, &dummy, false) != WJB_BEGIN_ARRAY ||
				JsonbIteratorNext(&it, pval, false) != WJB_ELEM ||
				JsonbIteratorNext(&it, &dummy, false) != WJB_END_ARRAY ||
				JsonbIteratorNext(&it, &dummy, false) != WJB_DONE)
				elog(ERROR, "unexpected return from jsonb iterator");
		}
		else
		{
			pval->type = jbvBinary;
			pval->val.binary.len = VARSIZE(jb);
			pval->val.binary.data = &jb->root;
		}
	}
}

/*
 * Called with the scalar value on top of the stack, which it is allowed to
 * change if need be.
 *
 * Must fill in the JsonbValue with data allocated in tmpcxt, or return false
 * to treat the value as a container instead.
 *
 * Upvalue 2 is the typeinfo pgtype.numeric.
 *
 */
static bool
pllua_jsonb_toscalar(lua_State *L, JsonbValue *pval, MemoryContext tmpcxt)
{
	pllua_typeinfo *dt;
	pllua_datum *d;

	switch (lua_type(L, -1))
	{
		case LUA_TNIL:
			pval->type = jbvNull;
			return true;
		case LUA_TBOOLEAN:
			pval->type = jbvBool;
			pval->val.boolean = lua_toboolean(L, -1);
			return true;
		case LUA_TNUMBER:
			/* must convert to numeric */
			lua_pushvalue(L, lua_upvalueindex(3));
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			FALLTHROUGH; /* FALLTHROUGH */
		case LUA_TUSERDATA:
			if ((d = pllua_todatum(L, -1, lua_upvalueindex(3))))
			{
				pllua_typeinfo *ndt = *pllua_torefobject(L, lua_upvalueindex(3), PLLUA_TYPEINFO_OBJECT);
				pval->type = jbvNumeric;
				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
					pval->val.numeric = DatumGetNumeric(datumCopy(d->value, ndt->typbyval, ndt->typlen));
					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();
				return true;
			}
			else if ((d = pllua_toanydatum(L, -1, &dt)))
			{
				pllua_get_user_subfield(L, -1, ".funcs", "to_jsonb");
				Assert(lua_type(L,-1) == LUA_TUSERDATA);
				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
					pllua_jsonb_from_datum(L, pval, d, dt);
					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();
				lua_pop(L, 2);
				return true;
			}
			if (pllua_is_container(L, -1))
				return false;
			if (luaL_getmetafield(L, -1, "__tostring") == LUA_TNIL)
				luaL_error(L, "cannot serialize userdata which lacks both __pairs and __tostring");
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			if (lua_type(L, -1) != LUA_TSTRING)
				luaL_error(L, "tostring on userdata object did not return a string");
			FALLTHROUGH; /* FALLTHROUGH */
		case LUA_TSTRING:
			PLLUA_TRY();
			{
				size_t len = 0;
				const char *ptr = lua_tolstring(L, -1, &len);
				MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
				char *newstr = palloc(len);
				memcpy(newstr, ptr, len);
				pg_verifymbstr(newstr, len, false);
				pval->type = jbvString;
				pval->val.string.val = newstr;
				pval->val.string.len = len;
				MemoryContextSwitchTo(oldcontext);
			}
			PLLUA_CATCH_RETHROW();
			return true;
		case LUA_TTABLE:
			return false;
		default:
			luaL_error(L, "cannot serialize scalar value of type %s", luaL_typename(L, -1));
			return true;
	}
}

/*
 * Called as tosql(table, config)
 *
 * config keys:
 *  - mapfunc
 *  - empty_object = (boolean)
 *  - nullvalue = (any value)
 *
 * Anything raw-equal to the nullvalue is taken as being a json null.
 */
static int
pllua_jsonb_tosql(lua_State *L)
{
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
	int nargs = lua_gettop(L);
	bool empty_object = false;  /* by default assume {} is an array */
	int nullvalue = 2;
	int funcidx = 0;
	int array_thresh = 1000;
	int array_frac = 1000;
	JsonbParseState *pstate = NULL;
	JsonbValue nullval;
	JsonbValue curval;
	MemoryContext tmpcxt;
	JsonbValue *volatile result;
	volatile Datum datum;
	pllua_datum *nd;

	PLLUA_CHECK_PG_STACK_DEPTH();

	nullval.type = jbvNull;

	/*
	 * If we only have one arg and it's not a table or userdata, decline and go
	 * back to the normal main line. We only construct jsonb values with
	 * top-level scalars if called with an explicit second arg. Note that we
	 * don't reach this code if the original __call arg was a single Datum, so
	 * we assume that a passed-in userdata is something we can index into (it
	 * must support __pairs to work).
	 */
	if (nargs < 2 &&
		lua_type(L, 1) != LUA_TTABLE &&
		lua_type(L, 1) != LUA_TUSERDATA)
		return 0;

	/* if there's a second arg, it must be a config table. */
	lua_settop(L, 2);

	if (!lua_isnil(L, 2))
	{
		if (lua_getfield(L, 2, "map") == LUA_TFUNCTION)
		{
			funcidx = lua_absindex(L, -1);
			/* leave on stack */
		}
		else
			lua_pop(L, 1);
		if (lua_getfield(L, 2, "empty_object") &&
			lua_toboolean(L, -1))
			empty_object = true;
		lua_pop(L, 1);
		lua_getfield(L, 2, "array_thresh");
		if (lua_isinteger(L, -1))
			array_thresh = lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 2, "array_frac");
		if (lua_isinteger(L, -1))
			array_frac = lua_tointeger(L, -1);
		lua_pop(L, 1);
		lua_getfield(L, 2, "null");
		nullvalue = lua_absindex(L, -1);
	}

	tmpcxt = pllua_newmemcontext(L, "pllua jsonb temp context",
								 ALLOCSET_START_SMALL_SIZES);

	if (lua_rawequal(L, 1, nullvalue))
	{
		lua_pushnil(L);
		lua_replace(L, 1);
	}
	if (funcidx)
	{
		lua_pushvalue(L, funcidx);
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		lua_replace(L, 1);
	}

	/* note that here, we don't want to treat a jsonb value as a container,
	 * even though it is */

	lua_pushvalue(L, 1);

	if (pllua_jsonb_toscalar(L, &curval, tmpcxt))
	{
		PLLUA_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
			datum = PointerGetDatum(JsonbValueToJsonb(&curval));
			MemoryContextSwitchTo(oldcontext);
		}
		PLLUA_CATCH_RETHROW();
	}
	else
	{
		JsonbIteratorToken tok;
		int depth = 1;

		tok = pllua_jsonb_pushkeys(L, empty_object, array_thresh, array_frac);
		/* stack: ... value=newcontainer newkeylist newprevkey newindex */
		luaL_checkstack(L, 20, NULL);

		PLLUA_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
			pushJsonbValue(&pstate, tok, NULL);
			MemoryContextSwitchTo(oldcontext);
		}
		PLLUA_CATCH_RETHROW();

		/*
		 * stack at loop top:
		 *   [container keylist prevkey index]...
		 * (prevkey is nil for objects)
		 *
		 * do while depth:
		 *   - if index beyond end of keylist:
		 *     - push array/object end into value
		 *     - pop stack
		 *   - else
		 *     - push container[keylist[index]] on stack
		 *     - if isobj, push keylist[key] into value
		 *       else if keylist[key] != prevkey+1
		 *       - push as many nulls as needed into value
		 *     - increment index
		 *     - if scalar
		 *       - convert and push into value
		 *     - else
		 *       - push keylist, prevkey, index
		 *       - increment depth
		 *       - push new container start into value
		 */
		while (depth > 0)
		{
			int idx = lua_tointeger(L, -1);
			lua_pushinteger(L, idx+1);
			lua_replace(L, -2);
			if (lua_rawgeti(L, -3, idx) == LUA_TNIL)
			{
				lua_pop(L, 1);

				tok = lua_isnil(L, -2) ? WJB_END_OBJECT : WJB_END_ARRAY;

				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
					result = pushJsonbValue(&pstate, tok, NULL);
					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();

				lua_pop(L, 4);
				--depth;
			}
			else
			{
				JsonbValue *pval = NULL;

				lua_pushvalue(L, -1);
				lua_gettable(L, -6);
				/* stack: container keylist prevkey index key value */
				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);

					if (!lua_isnil(L, -4))
					{
						int key = lua_tointeger(L, -2);
						int prevkey = lua_tointeger(L, -4);
						while (++prevkey != key)
						{
							pushJsonbValue(&pstate, WJB_ELEM, &nullval);
						}
						lua_pushinteger(L, key);
						lua_replace(L, -5);
						tok = WJB_ELEM;
					}
					else
					{
						size_t len = 0;
						const char *ptr = lua_tolstring(L, -2, &len);
						curval.type = jbvString;
						curval.val.string.val = palloc(len);
						curval.val.string.len = len;
						memcpy(curval.val.string.val, ptr, len);
						pg_verifymbstr(curval.val.string.val, len, false);
						pushJsonbValue(&pstate, WJB_KEY, &curval);
						tok = WJB_VALUE;
					}

					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();

				lua_remove(L, -2);
				/* stack: container keylist prevkey index value */
				if (lua_rawequal(L, -1, nullvalue))
				{
					lua_pushnil(L);
					lua_replace(L, -2);
				}
				if (funcidx)
				{
					lua_pushvalue(L, funcidx);
					lua_insert(L, -2);
					lua_call(L, 1, 1);
				}

				if (pllua_jsonb_toscalar(L, &curval, tmpcxt))
				{
					pval = &curval;
				}
				else
				{
					tok = pllua_jsonb_pushkeys(L, empty_object, array_thresh, array_frac);
					/* stack: ... value=newcontainer newkeylist newprevkey newindex */
					luaL_checkstack(L, 20, NULL);
					++depth;
				}

				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
					pushJsonbValue(&pstate, tok, pval);
					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();

				if (tok != WJB_BEGIN_OBJECT && tok != WJB_BEGIN_ARRAY)
					lua_pop(L, 1);
			}
		}

		PLLUA_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
			datum = PointerGetDatum(JsonbValueToJsonb(result));
			MemoryContextSwitchTo(oldcontext);
		}
		PLLUA_CATCH_RETHROW();
	}

	nd = pllua_newdatum(L, lua_upvalueindex(2), datum);

	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		pllua_savedatum(L, nd, t);
		MemoryContextReset(tmpcxt);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static int
pllua_jsonb_map(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(2));
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *numt = *pllua_torefobject(L, lua_upvalueindex(3), PLLUA_TYPEINFO_OBJECT);
	int funcidx = 0;
	int nullvalue;
	bool keep_numeric = false;
	bool noresult = false;
	bool norecurse = false;
	Jsonb	   *volatile jb;
	JsonbIterator *it;
	JsonbIteratorToken r;

	PLLUA_CHECK_PG_STACK_DEPTH();

	lua_settop(L, 2);

	if (t->typeoid != JSONBOID)
		luaL_error(L, "datum is not of type jsonb");

	switch (lua_type(L, 2))
	{
		case LUA_TTABLE:
			if (lua_getfield(L, 2, "map") == LUA_TFUNCTION)
			{
				funcidx = lua_absindex(L, -1);
				/* leave on stack */
			}
			else
				lua_pop(L, 1);
			if (lua_getfield(L, 2, "discard") &&
				lua_toboolean(L, -1))
				noresult = true;
			lua_pop(L, 1);
			if (lua_getfield(L, 2, "norecurse") &&
				lua_toboolean(L, -1))
				norecurse = true;
			lua_pop(L, 1);
			if (lua_getfield(L, 2, "pg_numeric") &&
				lua_toboolean(L, -1))
				keep_numeric = true;
			lua_pop(L, 1);
			lua_getfield(L, 2, "null");
			nullvalue = lua_absindex(L, -1);
			break;
		case LUA_TFUNCTION:
			lua_pushnil(L);
			nullvalue = lua_absindex(L, -1);
			funcidx = 2;
			break;
		case LUA_TNIL:
		default:
			/* if it's not a table or function, then it's the nullval. */
			nullvalue = 2;
			break;
	}

	PLLUA_TRY();
	{
		/*
		 * This can detoast, but only will for a value coming from a row (hence
		 * a child datum) that has a short header or is compressed.
		 */
		jb = DatumGetJsonbP(d->value);
	}
	PLLUA_CATCH_RETHROW();

	if (JB_ROOT_COUNT(jb) == 0)
	{
		if (!noresult)
			lua_newtable(L);
	}
	else
	{
		int patht;
		int patht_len = 0;
		int i = 0;
		bool is_scalar = (JB_ROOT_IS_SCALAR(jb)) ? true : false;
		bool notfirst = false;
		MemoryContext tmpcxt;
		int tmpcxt_idx;

		if (norecurse)
		{
			tmpcxt = pllua_newmemcontext(L, "jsonb map temp context",
										 ALLOCSET_START_SMALL_SIZES);
			tmpcxt_idx = lua_absindex(L, -1);
			lua_toclose(L, tmpcxt_idx);
		}

		PLLUA_TRY();
		{
			it = JsonbIteratorInit(&jb->root);
		}
		PLLUA_CATCH_RETHROW();

		lua_newtable(L);
		patht = lua_absindex(L, -1);
		lua_pushnil(L);

		for (;;)
		{
			JsonbValue	v;
			volatile JsonbIteratorToken vr;

			luaL_checkstack(L, patht_len + 10, NULL);

			PLLUA_TRY();
			{
				/* skip subobjects if not recursing, except on first call */
				vr = JsonbIteratorNext(&it, &v, notfirst && norecurse);
			}
			PLLUA_CATCH_RETHROW();

			r = vr;
			notfirst = true;

			if (r == WJB_DONE)
				break;

			switch (r)
			{
				case WJB_BEGIN_ARRAY:
					/* iterator puts a dummy array around scalars */
					if (!is_scalar)
					{
						if (!lua_isnil(L, -1))
						{
							lua_pushvalue(L, -1);
							lua_rawseti(L, patht, ++patht_len);
						}
						if (!noresult)
						{
							lua_newtable(L);
							lua_getfield(L, lua_upvalueindex(1), "array_mt");
							lua_setmetatable(L, -2);
						}
						i = 0;
						lua_pushinteger(L, i);
					}
					break;
				case WJB_BEGIN_OBJECT:
					if (!lua_isnil(L, -1))
					{
						lua_pushvalue(L, -1);
						lua_rawseti(L, patht, ++patht_len);
					}
					if (!noresult)
					{
						lua_newtable(L);
						lua_getfield(L, lua_upvalueindex(1), "object_mt");
						lua_setmetatable(L, -2);
					}
					break;
				case WJB_KEY:
					if (v.type != jbvString)
						luaL_error(L, "unexpected type for jsonb key");
					FALLTHROUGH; /* FALLTHROUGH */
				case WJB_VALUE:
				case WJB_ELEM:
					if (v.type == jbvNull)
					{
						lua_pushvalue(L, nullvalue);
					}
					else if (v.type == jbvBool)
					{
						lua_pushboolean(L, v.val.boolean);
					}
					else if (v.type == jbvNumeric)
					{
						pllua_datum_single(L, NumericGetDatum(v.val.numeric), false, lua_upvalueindex(3), numt);
						if (!keep_numeric)
						{
							lua_getfield(L, -1, "tonumber");
							lua_insert(L, -2);
							lua_call(L, 1, 1);
						}
					}
					else if (v.type == jbvString)
					{
						lua_pushlstring(L, v.val.string.val, v.val.string.len);
					}
					else
					{
						volatile Datum vd;

						PLLUA_TRY();
						{
							MemoryContext oldcxt = MemoryContextSwitchTo(tmpcxt);
							Assert(norecurse);
							MemoryContextReset(tmpcxt);
							vd = PointerGetDatum(JsonbValueToJsonb(&v));
							MemoryContextSwitchTo(oldcxt);
						}
						PLLUA_CATCH_RETHROW();

						pllua_datum_single(L, vd, false, lua_upvalueindex(2), t);
					}
					if (r == WJB_KEY)
					{
						/* leave on stack */
					}
					else if (r == WJB_VALUE)
					{
						/* we must have stack: ... [table] key value */
						/* and patht contains the path to reach table */
						/* we do  key,val = mapfunc(key,value,path...) */
						if (funcidx)
						{
							lua_pushvalue(L, funcidx);
							lua_insert(L, -3);
							for (i = 1; i <= patht_len; ++i)
								lua_rawgeti(L, patht, i);
							lua_call(L, 2 + patht_len, 2);
						}
						if (!noresult)
							lua_settable(L, -3);
					}
					else if (r == WJB_ELEM)
					{
						int idx = lua_tointeger(L, -2);
						/* stack: nil elem   or  ... table idx elem */
						if (funcidx)
						{
							lua_pushvalue(L, funcidx);
							lua_insert(L, -3);
							for (i = 1; i <= patht_len; ++i)
								lua_rawgeti(L, patht, i);
							lua_call(L, 2 + patht_len, 2);
						}
						if (!is_scalar && !noresult)
							lua_seti(L, -3, idx+1);
						if (!is_scalar)
						{
							lua_pop(L, 1);
							lua_pushinteger(L, idx+1);
						}
					}
					break;
				case WJB_END_ARRAY:
					if (is_scalar)
						break;
					lua_pop(L, 1);
					FALLTHROUGH; /* FALLTHROUGH */
				case WJB_END_OBJECT:
					/* we have stack: nil arrayval  or  ... [table] key arrayval */
					{
						bool is_toplevel = lua_isnil(L, -2);

						if (!is_toplevel)
							--patht_len;

						if (!noresult)
						{
							if (funcidx)
							{
								lua_pushvalue(L, funcidx);
								lua_insert(L, -3);
								for (i = 1; i <= patht_len; ++i)
									lua_rawgeti(L, patht, i);
								lua_call(L, 2 + patht_len, 2);
							}
							if (!is_toplevel)
							{
								int isint = lua_isinteger(L, -2);  /* NOT tointegerx */
								if (isint)
								{
									int idx = lua_tointeger(L, -2);
									/* if it was an integer key, we must be doing a table */
									lua_seti(L, -3, idx+1);
									lua_pop(L, 1);
									lua_pushinteger(L, idx+1);
								}
								else
									lua_settable(L, -3);
							}
						}
					}
					break;
				default:
					luaL_error(L, "unexpected return from jsonb iterator");
			}
		}

		if (norecurse)
			pllua_closevar(L, tmpcxt_idx);
	}

	PLLUA_TRY();
	{
		if ((Pointer)jb != DatumGetPointer(d->value))
			pfree(jb);
	}
	PLLUA_CATCH_RETHROW();

	return noresult ? 0 : 1;
}


struct jsonb_pairs_state
{
	JsonbIterator *it;
	Jsonb *jb;
	lua_Integer index;
	bool is_ipairs;
	MemoryContext mcxt;
	MemoryContext tmpcxt;
};

/*
 * upvalues:
 *   1 = lightudata ptr to state
 *   2 = jsonb typeinfo
 *   3 = numeric typeinfo
 *   4 = original datum
 */
static int
pllua_jsonb_pairs_next(lua_State *L)
{
	pllua_typeinfo *numt = *pllua_torefobject(L, lua_upvalueindex(3), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
	struct jsonb_pairs_state *statep = lua_touserdata(L, lua_upvalueindex(1));
	volatile JsonbIteratorToken vr;
	volatile Datum d;
	Jsonb *jb = statep->jb;
	JsonbValue vk;
	JsonbValue vv;
	bool root_scalar = false;

	PLLUA_CHECK_PG_STACK_DEPTH();

	/* initial call? */
	if (statep->it == NULL)
	{
		if (JB_ROOT_COUNT(jb) == 0)
			goto end;

		if (statep->is_ipairs && (!JB_ROOT_IS_ARRAY(jb) || JB_ROOT_IS_SCALAR(jb)))
			luaL_error(L, "argument of jsonb ipairs must be a jsonb array");

		if (JB_ROOT_IS_SCALAR(jb))
			root_scalar = true;

		PLLUA_TRY();
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(statep->mcxt);
			statep->it = JsonbIteratorInit(&jb->root);
			vr = JsonbIteratorNext(&statep->it, &vv, false);
			Assert(vr != WJB_VALUE);
			MemoryContextSwitchTo(oldcxt);
		}
		PLLUA_CATCH_RETHROW();
	}
	else
	{
		PLLUA_TRY();
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(statep->mcxt);
			vr = JsonbIteratorNext(&statep->it, &vv, true);
			Assert(vr != WJB_VALUE);
			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(statep->tmpcxt);
		}
		PLLUA_CATCH_RETHROW();
	}

	for (;;)
	{
		switch (vr)
		{
			case WJB_DONE:
				goto end;

			case WJB_BEGIN_ARRAY:
			case WJB_BEGIN_OBJECT:
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				PLLUA_TRY();
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(statep->mcxt);
					vr = JsonbIteratorNext(&statep->it, &vv, true);
					Assert(vr != WJB_VALUE);
					MemoryContextSwitchTo(oldcxt);
				}
				PLLUA_CATCH_RETHROW();
				continue;

			case WJB_VALUE:
				/* shouldn't happen */
				goto end;

			case WJB_KEY:
				if (vv.type != jbvString)
					luaL_error(L, "unexpected type for jsonb key");
				vk = vv;
				PLLUA_TRY();
				{
					MemoryContext oldcxt = MemoryContextSwitchTo(statep->mcxt);
					vr = JsonbIteratorNext(&statep->it, &vv, true);
					Assert(vr == WJB_VALUE);
					MemoryContextSwitchTo(oldcxt);
				}
				PLLUA_CATCH_RETHROW();
				FALLTHROUGH; /* FALLTHROUGH */
			case WJB_ELEM:
				if (vr == WJB_VALUE)
					lua_pushlstring(L, vk.val.string.val, vk.val.string.len);
				else if (root_scalar)
					lua_pushboolean(L, true);
				else
					lua_pushinteger(L, statep->index++);

				if (vv.type == jbvNull)
				{
					lua_pushnil(L);
				}
				else if (vv.type == jbvBool)
				{
					lua_pushboolean(L, vv.val.boolean);
				}
				else if (vv.type == jbvNumeric)
				{
					pllua_datum_single(L, NumericGetDatum(vv.val.numeric), false, lua_upvalueindex(3), numt);
				}
				else if (vv.type == jbvString)
				{
					lua_pushlstring(L, vv.val.string.val, vv.val.string.len);
				}
				else
				{
					PLLUA_TRY();
					{
						MemoryContext oldcxt = MemoryContextSwitchTo(statep->tmpcxt);
						d = PointerGetDatum(JsonbValueToJsonb(&vv));
						MemoryContextSwitchTo(oldcxt);
					}
					PLLUA_CATCH_RETHROW();
					pllua_datum_single(L, d, false, lua_upvalueindex(2), t);
				}

				return 2;

			default:
				luaL_error(L, "unexpected return from jsonb iterator");
		}
	}

end:
	PLLUA_TRY();
	{
		MemoryContextReset(statep->mcxt);
	}
	PLLUA_CATCH_RETHROW();
	return 0;
}

static int
pllua_jsonb_pairs_common(lua_State *L, bool is_ipairs)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(2));
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
	struct jsonb_pairs_state *volatile statep = NULL;
	MemoryContext mcxt;

	PLLUA_CHECK_PG_STACK_DEPTH();

	lua_settop(L, 1);

	if (t->typeoid != JSONBOID)
		luaL_error(L, "datum is not of type jsonb");

	/* loop context object at index 2 */
	mcxt = pllua_newmemcontext(L, "jsonb pairs loop context",
							   ALLOCSET_START_SMALL_SIZES);

	PLLUA_TRY();
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(mcxt);
		struct jsonb_pairs_state *p = palloc(sizeof(struct jsonb_pairs_state));
		p->mcxt = mcxt;
		p->tmpcxt = AllocSetContextCreate(mcxt, "jsonb pairs temp context",
										  ALLOCSET_START_SMALL_SIZES);
		p->it = NULL;
		p->index = 0;
		p->is_ipairs = is_ipairs;
		/*
		 * This can detoast, but only will for a value coming from a row (hence
		 * a child datum) that has a short header or is compressed.
		 */
		p->jb = DatumGetJsonbP(d->value);
		statep = p;
		MemoryContextSwitchTo(oldcxt);
	}
	PLLUA_CATCH_RETHROW();

	lua_pushlightuserdata(L, statep);
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushvalue(L, lua_upvalueindex(3));
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, pllua_jsonb_pairs_next, 4);
	lua_pushnil(L);
	lua_pushnil(L);
	lua_pushvalue(L, 2);  /* put the loop mcxt in the close slot */
	return 4;
}

static int
pllua_jsonb_pairs(lua_State *L)
{
	return pllua_jsonb_pairs_common(L, false);
}

static int
pllua_jsonb_ipairs(lua_State *L)
{
	return pllua_jsonb_pairs_common(L, true);
}

static int
pllua_jsonb_type(lua_State *L)
{
	pllua_datum *d = pllua_todatum(L, 1, lua_upvalueindex(2));
	bool lax = lua_toboolean(L, 2);
	const char *typ = NULL;

	luaL_checkany(L, 1);

	if (d)
	{
		PLLUA_TRY();
		{
			Jsonb *jb;

			/*
			 * This can detoast, but only will for a value coming from a row (hence
			 * a child datum) that has a short header or is compressed.
			 */
			jb = DatumGetJsonbP(d->value);

			/*
			 * this code works around missing backend functions in older PG
			 * versions, consider removing it when support for those is
			 * removed
			 */
			if (JB_ROOT_IS_SCALAR(jb))
			{
				JsonbValue	scalar;
				JsonbIterator *it;
				JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;

				it = JsonbIteratorInit(&jb->root);

				tok = JsonbIteratorNext(&it, &scalar, true);
				Assert(tok == WJB_BEGIN_ARRAY);
				Assert(scalar.val.array.nElems == 1 && scalar.val.array.rawScalar);

				tok = JsonbIteratorNext(&it, &scalar, true);
				Assert(tok == WJB_ELEM);

				switch (scalar.type)
				{
					case jbvNumeric:	typ = "number";		break;
					case jbvString:		typ = "string";		break;
					case jbvBool:		typ = "boolean";	break;
					case jbvNull:		typ = "null";		break;
					default:
						elog(ERROR, "unrecognized jsonb value type: %d", scalar.type);
				}

				tok = JsonbIteratorNext(&it, &scalar, true);
				Assert(tok == WJB_END_ARRAY);

				tok = JsonbIteratorNext(&it, &scalar, true);
				Assert(tok == WJB_DONE);
			}
			else if (JB_ROOT_IS_ARRAY(jb))
				typ = "array";
			else if (JB_ROOT_IS_OBJECT(jb))
				typ = "object";
			else
				elog(ERROR, "invalid jsonb container type: 0x%08x", *(uint32 *) VARDATA(jb));

			if ((Pointer)jb != DatumGetPointer(d->value))
				pfree(jb);
		}
		PLLUA_CATCH_RETHROW();
	}
	else if (lax)
	{
		switch (lua_type(L, 1))
		{
			case LUA_TNIL:		typ = "null";		break;
			case LUA_TBOOLEAN:	typ = "boolean";	break;
			case LUA_TNUMBER:	typ = "number";		break;
			case LUA_TSTRING:	typ = "string";		break;
			case LUA_TUSERDATA:
				if (pllua_todatum(L, 1, lua_upvalueindex(3)))
					typ = "number";
				break;
			default:
				break;
		}
	}

	lua_pushstring(L, typ);
	return 1;
}


static luaL_Reg jsonb_meta[] = {
	{ "__call", pllua_jsonb_map },
	{ "__pairs", pllua_jsonb_pairs },
	{ "tosql", pllua_jsonb_tosql },
	{ NULL, NULL }
};

/*
 * Test whether a table returned from jsonb_map was originally an object or
 * array.
 */
static int
pllua_jsonb_table_is_object(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_getmetafield(L, 1, "__jsonb_object") != LUA_TBOOLEAN)
		return 0;
	return 1;
}

static int
pllua_jsonb_table_is_array(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	if (luaL_getmetafield(L, 1, "__jsonb_object") != LUA_TBOOLEAN)
		return 0;
	lua_pushboolean(L, !lua_toboolean(L, -1));
	return 1;
}

static int
pllua_jsonb_table_set_table_mt(lua_State *L, const char *mtname)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_getmetatable(L, 1))
	{
		lua_getfield(L, lua_upvalueindex(1), "object_mt");
		if (!lua_rawequal(L, -1, -2))
		{
			lua_getfield(L, lua_upvalueindex(1), "array_mt");
			if (!lua_rawequal(L, -1, -3))
				luaL_argerror(L, 1, "cannot replace existing metatable");
		}
	}
	if (mtname)
		lua_getfield(L, lua_upvalueindex(1), mtname);
	else
		lua_pushnil(L);
	lua_setmetatable(L, 1);
	lua_settop(L, 1);
	return 1;
}

static int
pllua_jsonb_table_set_object(lua_State *L)
{
	return pllua_jsonb_table_set_table_mt(L, "object_mt");
}

static int
pllua_jsonb_table_set_array(lua_State *L)
{
	return pllua_jsonb_table_set_table_mt(L, "array_mt");
}

static int
pllua_jsonb_table_set_unknown(lua_State *L)
{
	return pllua_jsonb_table_set_table_mt(L, NULL);
}

static luaL_Reg jsonb_funcs[] = {
	{ "is_object", pllua_jsonb_table_is_object },
	{ "is_array", pllua_jsonb_table_is_array },
	{ "set_as_object", pllua_jsonb_table_set_object },
	{ "set_as_array", pllua_jsonb_table_set_array },
	{ "set_as_unknown", pllua_jsonb_table_set_unknown },
	{ "pairs", pllua_jsonb_pairs },
	{ "ipairs", pllua_jsonb_ipairs },
	{ "type", pllua_jsonb_type },
	{ NULL, NULL }
};

int pllua_open_jsonb(lua_State *L)
{
	lua_settop(L, 0);

	lua_newtable(L);	/* module private data table at index 1 */

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, JSONBOID);
	lua_call(L, 1, 1);
	lua_setfield(L, 1, "jsonb_type");

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, NUMERICOID);
	lua_call(L, 1, 1);
	lua_setfield(L, 1, "numeric_type");

	luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
	if (lua_getfield(L, -1, "table") != LUA_TTABLE)
		luaL_error(L, "table package is not loaded");
	if (lua_getfield(L, -1, "sort") != LUA_TFUNCTION)
		luaL_error(L, "table.sort function not found");
	lua_remove(L, -2);
	lua_remove(L, -2);
	lua_setfield(L, 1, "sort");

	lua_newtable(L);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__jsonb_object");
	lua_setfield(L, 1, "array_mt");

	lua_newtable(L);
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__jsonb_object");
	lua_setfield(L, 1, "object_mt");

	lua_newtable(L);  /* module table at index 2 */
	lua_getfield(L, 1, "jsonb_type");	/* jsonb typeinfo at index 3 */
	lua_getfield(L, 1, "numeric_type");  /* numeric's typeinfo at index 4 */

	lua_pushvalue(L, 2);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 3);
	lua_pushvalue(L, 4);
	luaL_setfuncs(L, jsonb_funcs, 3);

	lua_getuservalue(L, 3);  /* datum metatable */

	lua_pushvalue(L, 1);  /* first upvalue for jsonb metamethods */
	lua_pushvalue(L, 3);  /* second upvalue for jsonb metamethods */
	lua_pushvalue(L, 4);  /* third upvalue for jsonb metamethods */
	luaL_setfuncs(L, jsonb_meta, 3);

	lua_pushvalue(L, 2);
	return 1;
}
