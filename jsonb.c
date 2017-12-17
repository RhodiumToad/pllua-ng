/* jsonb.c */

#include "pllua.h"

#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/datum.h"
#include "utils/jsonb.h"

#if PG_VERSION_NUM < 110000
#define DatumGetJsonbP(d_) DatumGetJsonb(d_)
#endif


static bool
pllua_jsonb_is_container(lua_State *L, int nd)
{
	if (lua_type(L, nd) == LUA_TTABLE)
		return true;
	if (luaL_getmetafield(L, nd, "__pairs") != LUA_TNIL)
	{
		lua_pop(L, 1);
		return true;
	}
	return false;
}

static int do_next (lua_State *L)
{
	lua_settop(L, 2);
	if (lua_next(L, 1))
		return 2;
	lua_pushnil(L);
    return 1;
}

/*
 * called with the container value on top of the stack
 *
 * Must push keytable, prevkey, index(=1)
 * where prevkey is nil for objects and 0 for arrays
 *
 * For objects, keytable is a sequence of string keys (we must ensure they are
 * LUA_TSTRING values). For arrays, keytable is a sequence of integers in
 * ascending order giving the "present" keys.
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

	lua_newtable(L);
	lua_newtable(L);

	if (luaL_getmetafield(L, -3, "__pairs") == LUA_TNIL)
	{
		lua_pushcfunction(L, do_next);
		lua_pushvalue(L, -4);
		lua_pushnil(L);
	}
	else
	{
		lua_pushvalue(L, -4);
		lua_call(L, 1, 3);
	}
	/* stack: keytable, numkeytab, iter, state, key */

	for (;;)
	{
		lua_pushvalue(L, -3);
		lua_insert(L, -2);    /* ... iter state iter key */
		lua_pushvalue(L, -3);
		lua_insert(L, -2);    /* ... iter state iter state key */
		lua_call(L, 2, 2);    /* keytable numkeytab iter state key val */
		lua_pop(L, 1);
		if (lua_isnil(L, -1))    /* keytable numkeytab iter state key */
			break;
		lua_pushvalue(L, -1);    /* keytable numkeytab iter state key key */
		++numkeys;
		intval = lua_tointegerx(L, -1, &isint);
		if (isint)
		{
			if (intval > max_intkey)
				max_intkey = intval;
			if (intval < min_intkey)
				min_intkey = intval;
			++numintkeys;
			lua_pushvalue(L, -1);
			lua_rawseti(L, -6, numintkeys);
		}

		switch (lua_type(L, -1))
		{
			case LUA_TUSERDATA:
			case LUA_TTABLE:
				if (luaL_getmetafield(L, -1, "__tostring") == LUA_TNIL)
					luaL_error(L, "cannot serialize userdata or table which lacks __tostring as a key");
				lua_insert(L, -2);
				lua_call(L, 1, 1);
				if (lua_type(L, -1) != LUA_TSTRING)
					luaL_error(L, "tostring on table or userdata object did not return a string");
				break;
			case LUA_TSTRING:
				break;
			case LUA_TNUMBER:
				lua_tostring(L, -1);  /* alters stack value as side effect */
				break;
			default:
				luaL_error(L, "cannot serialize scalar value of type %s as key", luaL_typename(L, -1));
		}

		lua_rawseti(L, -6, numkeys);
	}
	lua_pop(L, 3);

	/* stack: keytable numkeytab */

	if ((empty_object && numkeys == 0)
		|| (numkeys != numintkeys)
		|| (min_intkey < 1)
		|| (numintkeys > 0 && (min_intkey > array_thresh))
		|| (numintkeys > 0 && (max_intkey > (array_frac * numkeys))))
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
		lua_pushvalue(L, lua_upvalueindex(3));
		lua_pushvalue(L, -2);
		lua_call(L, 1, 0);
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 1);
		return WJB_BEGIN_ARRAY;
	}
}

/*
 * Called with the scalar value on top of the stack, which it is allowed to
 * change if need be
 *
 * Must fill in the JsonbValue with data allocated in tmpcxt.
 *
 * Upvalue 2 is the typeinfo pgtype.numeric.
 *
 */
static void
pllua_jsonb_toscalar(lua_State *L, JsonbValue *pval, MemoryContext tmpcxt)
{
	pllua_datum *d;

	switch (lua_type(L, -1))
	{
		case LUA_TNIL:
			pval->type = jbvNull;
			return;
		case LUA_TBOOLEAN:
			pval->type = jbvBool;
			pval->val.boolean = lua_toboolean(L, -1);
			return;
		case LUA_TNUMBER:
			/* must convert to numeric */
			lua_pushvalue(L, lua_upvalueindex(2));
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			/* FALLTHROUGH */
		case LUA_TUSERDATA:
			if ((d = pllua_todatum(L, -1, lua_upvalueindex(2))))
			{
				pllua_typeinfo *dt = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
				pval->type = jbvNumeric;
				PLLUA_TRY();
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
					pval->val.numeric = DatumGetNumeric(datumCopy(d->value, dt->typbyval, dt->typlen));
					MemoryContextSwitchTo(oldcontext);
				}
				PLLUA_CATCH_RETHROW();
				return;
			}
			if (luaL_getmetafield(L, -1, "__tostring") == LUA_TNIL)
				luaL_error(L, "cannot serialize userdata which lacks both __pairs and __tostring");
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			if (lua_type(L, -1) != LUA_TSTRING)
				luaL_error(L, "tostring on userdata object did not return a string");
			/* FALLTHROUGH */
		case LUA_TSTRING:
			PLLUA_TRY();
			{
				size_t len = 0;
				const char *ptr = lua_tolstring(L, -1, &len);
				MemoryContext oldcontext = tmpcxt;
				char *newstr = palloc(len);
				memcpy(newstr, ptr, len);
				pg_verifymbstr(newstr, len, false);
				pval->type = jbvString;
				pval->val.string.val = newstr;
				pval->val.string.len = len;
				MemoryContextSwitchTo(oldcontext);
			}
			PLLUA_CATCH_RETHROW();
			return;
		default:
			luaL_error(L, "cannot serialize scalar value of type %s", luaL_typename(L, -1));
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
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
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
	JsonbValue *result;
	Datum datum;
	pllua_datum *nd;

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

	if (!pllua_jsonb_is_container(L, 1))
	{
		JsonbValue sval;

		lua_pushvalue(L, 1);

		pllua_jsonb_toscalar(L, &sval, tmpcxt);

		PLLUA_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(tmpcxt);
			datum = PointerGetDatum(JsonbValueToJsonb(&sval));
			MemoryContextSwitchTo(oldcontext);
		}
		PLLUA_CATCH_RETHROW();
	}
	else
	{
		JsonbIteratorToken tok;
		int depth = 1;

		lua_pushvalue(L, 1);

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
						Assert(lua_type(L, -2) == LUA_TSTRING);
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

				if (pllua_jsonb_is_container(L, -1))
				{
					tok = pllua_jsonb_pushkeys(L, empty_object, array_thresh, array_frac);
					/* stack: ... value=newcontainer newkeylist newprevkey newindex */
					luaL_checkstack(L, 20, NULL);
					++depth;
				}
				else
				{
					pval = &curval;
					pllua_jsonb_toscalar(L, pval, tmpcxt);
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

	lua_pushvalue(L, lua_upvalueindex(1));
	nd = pllua_newdatum(L);

	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		nd->value = datum;
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
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = *pllua_torefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *numt = *pllua_torefobject(L, lua_upvalueindex(2), PLLUA_TYPEINFO_OBJECT);
	int funcidx = 0;
	int nullvalue;
	bool keep_numeric = false;
	bool noresult = false;
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbIteratorToken r;

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
			if (lua_getfield(L, 2, "pg_numeric") &&
				lua_toboolean(L, -1))
				keep_numeric = true;
			lua_pop(L, 1);
			lua_getfield(L, 2, "null");
			nullvalue = lua_absindex(L, -1);
			break;
		case LUA_TFUNCTION:
			funcidx = 2;
			break;
		case LUA_TNIL:
		default:
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
		bool is_scalar = JB_ROOT_IS_SCALAR(jb);

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

			luaL_checkstack(L, patht_len + 10, NULL);

			PLLUA_TRY();
			{
				r = JsonbIteratorNext(&it, &v, false);
			}
			PLLUA_CATCH_RETHROW();

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
							lua_newtable(L);
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
						lua_newtable(L);
					break;
				case WJB_KEY:
					if (v.type != jbvString)
						luaL_error(L, "unexpected type for jsonb key");
					/* fallthrough */
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
						pllua_datum_single(L, NumericGetDatum(v.val.numeric), false, lua_upvalueindex(2), numt);
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
					if (r == WJB_KEY)
					{
						/* leave on stack */;
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
							lua_settable(L, -3);
						if (!is_scalar)
							lua_pushinteger(L, idx+1);
					}
					break;
				case WJB_END_ARRAY:
					if (is_scalar)
						break;
					lua_pop(L, 1);
					/* FALLTHROUGH */
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
								int isint = 0;
								int idx = lua_tointegerx(L, -2, &isint);
								lua_settable(L, -3);
								/* if it was an integer key, we must be doing a table */
								if (isint)
									lua_pushinteger(L, idx+1);
							}
						}
					}
					break;
				default:
					luaL_error(L, "unexpected return from jsonb iterator");
			}
		}
	}

	PLLUA_TRY();
	{
		if ((Pointer)jb != DatumGetPointer(d->value))
			pfree(jb);
	}
	PLLUA_CATCH_RETHROW();

	return noresult ? 0 : 1;
}

static luaL_Reg jsonb_meta[] = {
	{ "__call", pllua_jsonb_map },
	{ "tosql", pllua_jsonb_tosql },
	{ NULL, NULL }
};

static luaL_Reg jsonb_funcs[] = {
	{ NULL, NULL }
};

int pllua_open_jsonb(lua_State *L)
{
	lua_settop(L, 0);
	lua_newtable(L);  /* module table at index 1 */
	luaL_setfuncs(L, jsonb_funcs, 0);

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, JSONBOID);
	lua_call(L, 1, 1); /* typeinfo at index 2 */

	lua_getuservalue(L, 2);  /* datum metatable at index 3 */

	lua_pushvalue(L, 2);  /* first upvalue for jsonb metamethods */

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, NUMERICOID);
	lua_call(L, 1, 1); /* second upvalue is numeric's typeinfo */

	luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
	if (lua_getfield(L, -1, "table") != LUA_TTABLE)
		luaL_error(L, "table package is not loaded");
	if (lua_getfield(L, -1, "sort") != LUA_TFUNCTION)
		luaL_error(L, "table.sort function not found");
	lua_remove(L, -2);
	lua_remove(L, -2);  /* third upvalue is sort function */

	luaL_setfuncs(L, jsonb_meta, 3);

	/* override normal datum __index entry with our method table */
	lua_pushvalue(L, 1);
	lua_setfield(L, 3, "__index");

	lua_pushvalue(L, 1);
	return 1;
}
