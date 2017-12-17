/* jsonb.c */

#include "pllua.h"

#include "catalog/pg_type.h"
#include "utils/jsonb.h"

#if PG_VERSION_NUM < 110000
#define DatumGetJsonbP(d_) DatumGetJsonb(d_)
#endif


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
			if (lua_getfield(L, 2, "mapfunc") == LUA_TFUNCTION)
			{
				funcidx = lua_absindex(L, -1);
				/* leave on stack */
			}
			else
				lua_pop(L, 1);
			if (lua_getfield(L, 2, "noresult") &&
				lua_toboolean(L, -1))
				noresult = true;
			lua_pop(L, 1);
			if (lua_getfield(L, 2, "keep_numeric") &&
				lua_toboolean(L, -1))
				keep_numeric = true;
			lua_pop(L, 1);
			lua_getfield(L, 2, "nullvalue");
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
								lua_settable(L, -3);
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

	luaL_setfuncs(L, jsonb_meta, 2);

	/* override normal datum __index entry with our method table */
	lua_pushvalue(L, 1);
	lua_setfield(L, 3, "__index");

	lua_pushvalue(L, 1);
	return 1;
}
