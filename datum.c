/* datum.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * Basic plan of attack:
 *
 * A Datum object has this directly in its body:
 *
 *   Datum value;
 *   bool  need_gc;
 *
 * We create the object initially with just the value, and need_gc false.
 * However, we then have to (more or less immediately) copy the value if it's a
 * byref type, since we have no control over its lifetime inside lua (it may
 * even need to survive across transactions, so we have to detoast it too).
 * The code that does the copying is separated from the initial creation for
 * reasons of error handling.
 *
 * An exception is made for datums extracted from a row or array which is
 * itself already a datum. For this case we leave need_gc false, and put a
 * reference to the parent value in the uservalue slot.
 *
 * Information about the object type is contained in a typeinfo object. We keep
 * a cache of type info (by oid) and tupdesc info (by typmod for RECORD
 * tupdescs only - for composite types we rely on the typcache instead, in
 * order to avoid dealing with invalidations). Because our cache is decoupled
 * from the syscache and very long-lived, we register for invalidations even
 * though they should be rare.
 *
 * The uservalue of the typeinfo contains the metatable to be used for datum
 * objects of this type. In addition we cache stuff there.
 *
 * Global caches:
 *
 *  reg[PLLUA_RECORDS] = { [typmod] = typeobject }
 *  reg[PLLUA_TYPES] = { [oid] = typeobject }
 *
 *
 */


static bool pllua_typeinfo_iofunc(lua_State *L,
								  pllua_typeinfo *t,
								  IOFuncSelector whichfunc);

/*
 * IMPORTANT!!!
 *
 * It is _our_ responsibility to verify encoding correctness when passing any
 * string data from untrusted sources (i.e. the Lua code) into PG server apis.
 */
static void pllua_verify_encoding(lua_State *L, const char *str)
{
	/* XXX improve error message */
	if (str && !pg_verifymbstr(str, strlen(str), true))
	{
		if (pllua_context == PLLUA_CONTEXT_LUA)
			luaL_error(L, "invalid encoding");
		else
			elog(ERROR, "invalid encoding");
	}
}

/*
 * "light" detoast function that does not copy or align values.
 */
static Datum pllua_detoast_light(lua_State *L, Datum d)
{
	volatile Datum nd;

	if (!VARATT_IS_EXTENDED(d)
		|| (VARATT_IS_SHORT(d) && !VARATT_IS_EXTERNAL(d)))
		return d;

	PLLUA_TRY();
	{
		nd = PointerGetDatum(PG_DETOAST_DATUM_COPY(d));
	}
	PLLUA_CATCH_RETHROW();

	return nd;
}

/*
 * If a datum is representable directly as a Lua type, then push it as that
 * type. Otherwise push nothing.
 *
 * Returns the Lua type or LUA_TNONE
 */
int pllua_value_from_datum(lua_State *L,
						   Datum value,
						   Oid typeid)
{
	ASSERT_LUA_CONTEXT;

	switch (typeid)
	{
		case TEXTOID:
		case BYTEAOID:
			{
				Datum v = pllua_detoast_light(L, value);
				lua_pushlstring(L, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
			}
			return LUA_TSTRING;

		case CSTRINGOID:
		case NAMEOID:
			{
				const char *str = DatumGetPointer(value);
				lua_pushlstring(L, str, strlen(str));
			}
			return LUA_TSTRING;

		case FLOAT4OID:
			lua_pushnumber(L, DatumGetFloat4(value));
			return LUA_TNUMBER;

		case FLOAT8OID:
			lua_pushnumber(L, DatumGetFloat8(value));
			return LUA_TNUMBER;

		case BOOLOID:
			lua_pushboolean(L, DatumGetBool(value) ? 1 : 0);
			return LUA_TBOOLEAN;

		case OIDOID:
			lua_pushinteger(L, (lua_Integer) DatumGetObjectId(value));
			return LUA_TNUMBER;

		case INT2OID:
			lua_pushinteger(L, (lua_Integer) DatumGetInt16(value));
			return LUA_TNUMBER;

		case INT4OID:
			lua_pushinteger(L, (lua_Integer) DatumGetInt32(value));
			return LUA_TNUMBER;

		case INT8OID:
			lua_pushinteger(L, (lua_Integer) DatumGetInt64(value));
			return LUA_TNUMBER;

		default:
			return LUA_TNONE;
	}
}

/*
 * If a datum type corresponds to a simple Lua type, then take a value of that
 * type and return as Datum/isnull. May copy the data into the current memory
 * context (and hence requires PG context for call).
 *
 * nil is accepted as input for any type whatsoever (and treated as NULL).
 *
 * returns true for an acceptable value, false if not.
 */
bool pllua_datum_from_value(lua_State *L, int nd,
							Oid typeid,
							Datum *result,
							bool *isnull)
{
	ASSERT_PG_CONTEXT;

	if (lua_type(L, nd) == LUA_TNIL)
	{
		*isnull = true;
		*result = (Datum)0;
		return true;
	}
	else
		*isnull = false;

	switch (lua_type(L, nd))
	{
		case LUA_TNIL:
		case LUA_TNONE:
			elog(ERROR, "pllua_datum_from_value: missing value");

		case LUA_TBOOLEAN:
			if (typeid == BOOLOID)
			{
				*result = BoolGetDatum( (lua_toboolean(L, nd) != 0) );
				return true;
			}
			return false;

		case LUA_TSTRING:
			{
				size_t len;
				const char *str = lua_tolstring(L, nd, &len);

				switch (typeid)
				{
					case TEXTOID:
					case VARCHAROID:
						{
							if (len != strlen(str))
								elog(ERROR, "null characters not allowed in text values");
							pllua_verify_encoding(L, str);
							*result = CStringGetDatum(cstring_to_text_with_len(str, len));
						}
						return true;

					case BYTEAOID:
						{
							bytea *b = palloc(len + VARHDRSZ);
							memcpy(VARDATA(b), str, len);
							SET_VARSIZE(b, len + VARHDRSZ);
							*result = PointerGetDatum(b);
						}
						return true;

					case NAMEOID:
						{
							Name v;
							/* Truncate oversize input */
							if (len >= NAMEDATALEN)
								len = pg_mbcliplen(str, len, NAMEDATALEN - 1);
							/* We use palloc0 here to ensure result is zero-padded */
							v = (Name) palloc0(NAMEDATALEN);
							memcpy(NameStr(*v), str, len);
							*result = NameGetDatum(v);
						}
						return true;

					case CSTRINGOID:
						{
							if (len != strlen(str))
								elog(ERROR, "null characters not allowed in text values");
							pllua_verify_encoding(L, str);
							*result = CStringGetDatum(str);
						}
						return true;

					case BOOLOID:
						{
							bool v = false;
							if (parse_bool_with_len(str, len, &v))
								*result = BoolGetDatum(v);
							else
								elog(ERROR, "invalid input for bool");
						}
						return true;
				}
			}
			return false;

		case LUA_TNUMBER:
			{
				int isint = 0;
				lua_Integer intval = lua_tointegerx(L, nd, &isint);
				lua_Number floatval = lua_tonumber(L, nd);

				switch (typeid)
				{
					case FLOAT4OID:
						*result = Float4GetDatum((float4) floatval);
						return true;

					case FLOAT8OID:
						*result = Float8GetDatum((float4) floatval);
						return true;

					case BOOLOID:
						if (isint && (intval == 0 || intval == 1))
							*result = BoolGetDatum( (intval == 1) );
						else
							elog(ERROR, "invalid input for bool");
						return true;

					case OIDOID:
						if (isint && intval == (lua_Integer)(Oid)intval)
							*result = ObjectIdGetDatum( (Oid)intval );
						else
							elog(ERROR, "oid out of range");
						return true;

					case INT2OID:
						if (isint && intval >= PG_INT16_MIN && intval <= PG_INT16_MAX)
							*result = Int16GetDatum(intval);
						else
							elog(ERROR, "smallint out of range");
						return true;

					case INT4OID:
						if (isint && intval >= PG_INT32_MIN && intval <= PG_INT32_MAX)
							*result = Int32GetDatum(intval);
						else
							elog(ERROR, "integer out of range");
						return true;

					case INT8OID:
						if (isint)
							*result = Int64GetDatum(intval);
						else
							elog(ERROR, "bigint out of range");
						return true;
				}
			}
			return false;

		default:
			return LUA_TNONE;
	}
}

/*
 * This one always makes a datum object, even for types we don't normally do
 * that for. It also doesn't do savedatum: caller must do that if need be.
 *
 * Value is left on top of the stack.
 */
static void pllua_make_datum(lua_State *L, Datum value, Oid typeid, int32 typmod)
{
	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, (lua_Integer) typeid);
	lua_pushinteger(L, (lua_Integer) typmod);
	lua_call(L, 2, 1);

	if (lua_isnil(L, -1))
		luaL_error(L, "failed to find typeinfo");

	{
		pllua_datum *d;
		pllua_checkrefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
		d = pllua_newdatum(L);
		d->value = value;
		d->need_gc = false;
		lua_remove(L, -2);
	}
}


static int pllua_datum_gc(lua_State *L)
{
	pllua_datum *p = lua_touserdata(L, 1);

	ASSERT_LUA_CONTEXT;

	if (!p->need_gc || !DatumGetPointer(p->value))
		return 0;

	/*
	 * Don't retry if something goes south.
	 */
	p->need_gc = false;

	PLLUA_TRY();
	{
		pfree(DatumGetPointer(p->value));
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

/*
 * check that the item at "nd" is a datum whose typeinfo is "td"
 * (caller must have already checked that it really is a typeinfo)
 */
static pllua_datum *pllua_todatum(lua_State *L, int nd, int td)
{
	void *p = lua_touserdata(L, nd);
	if (p != NULL)
	{
		if (lua_getmetatable(L, nd))
		{
			lua_getuservalue(L, td);
			if (!lua_rawequal(L, -1, -2))
				p = NULL;
			lua_pop(L, 2);
			return p;
		}
	}
	return NULL;
}

static pllua_datum *pllua_checkdatum(lua_State *L, int nd, int td)
{
	pllua_datum *p = pllua_todatum(L, nd, td);
	if (!p)
		luaL_argerror(L, nd, "datum");
	return p;
}

/*
 * check that the item at "nd" is a datum, and also (if it is) push its
 * typeinfo and return it (else push nothing)
 */
static pllua_datum *pllua_toanydatum(lua_State *L, int nd, pllua_typeinfo **ti)
{
	void *p = lua_touserdata(L,nd);
	nd = lua_absindex(L,nd);
	if (p)
	{
		if (lua_getmetatable(L, nd))
		{
			if (lua_getfield(L, -1, "typeinfo") != LUA_TUSERDATA)
			{
				lua_pop(L, 2);
				return NULL;
			}
			*ti = *pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
			if (!*ti)
			{
				lua_pop(L, 2);
				return NULL;
			}
			lua_insert(L, -2);
			lua_getuservalue(L, -2);
			if (!lua_rawequal(L, -1, -2))
			{
				lua_pop(L, 3);
				return NULL;
			}
			lua_pop(L, 2);
			return p;
		}
	}
	return NULL;
}

static pllua_datum *pllua_checkanydatum(lua_State *L, int nd, pllua_typeinfo **ti)
{
	pllua_datum *p = pllua_toanydatum(L, nd, ti);
	if (!p)
		luaL_argerror(L, nd, "datum");
	return p;
}

pllua_datum *pllua_newdatum(lua_State *L)
{
	pllua_datum *d;
	pllua_checkrefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
	d = lua_newuserdata(L, sizeof(pllua_datum));
	d->value = (Datum)0;
	d->need_gc = 0;

	lua_getuservalue(L, -2);
	lua_setmetatable(L, -2);

	return d;
}

/*
 * Caller should have already written the value into d->value.
 */
void pllua_savedatum(lua_State *L,
					 pllua_datum *d,
					 pllua_typeinfo *t)
{
	Datum nv;

	ASSERT_PG_CONTEXT;

	if (t->typbyval)
		return;
	if (t->typlen != -1)
	{
		nv = datumCopy(d->value, false, t->typlen);
		d->value = nv;
		d->need_gc = true;
		return;
	}

	/* Varlena type, which may need detoast. */

	nv = PointerGetDatum(PG_DETOAST_DATUM_COPY(d->value));
	d->value = nv;
	d->need_gc = true;
	return;
}


/*
 * __tostring(d)  returns the string representation of the datum.
 *
 * We get the typeinfo object from the closure.
 */
static int pllua_datum_tostring(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	void **p = pllua_checkrefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	char *volatile str = NULL;

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		if ((OidIsValid(t->outfuncid) && OidIsValid(t->outfunc.fn_oid))
			|| pllua_typeinfo_iofunc(L, t, IOFunc_output))
		{
			str = OutputFunctionCall(&t->outfunc, d->value);
		}
		else
			elog(ERROR, "failed to find output function for type %u", t->typeoid);
	}
	PLLUA_CATCH_RETHROW();

	if (str)
		lua_pushstring(L, str);
	else
		lua_pushnil(L);  /* should never happen? */
	return 1;
}

/*
 * _tobinary(d) returns the binary-protocol representation of the datum.
 *
 * We get the typeinfo object from the closure.
 *
 * CAVEAT: some types will render text parts of the result into the current
 * client encoding.
 */
static int pllua_datum_tobinary(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	void **p = pllua_checkrefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	bytea *volatile res = NULL;
	volatile bool done = false;

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		if ((OidIsValid(t->sendfuncid) && OidIsValid(t->sendfunc.fn_oid))
			|| pllua_typeinfo_iofunc(L, t, IOFunc_send))
		{
			res = SendFunctionCall(&t->infunc, d->value);
			done = true;
		}
	}
	PLLUA_CATCH_RETHROW();

	if (!done)
		luaL_error(L, "failed to find send function for type");

	if (res)
		lua_pushlstring(L, VARDATA_ANY(res), VARSIZE_ANY_EXHDR(res));
	else
		lua_pushnil(L);  /* should never happen? */
	return 1;
}

/*
 * Leaves the table on top of the stack.
 */
static void pllua_datum_deform_tuple(lua_State *L, int nd, pllua_datum *d, pllua_typeinfo *t)
{
	HeapTupleHeader htup = (HeapTupleHeader) DatumGetPointer(d->value);
	Datum values[MaxTupleAttributeNumber + 1];
	bool nulls[MaxTupleAttributeNumber + 1];
	TupleDesc tupdesc = t->tupdesc;
	int i;

	nd = lua_absindex(L, nd);
	lua_getuservalue(L, nd);
	if (!lua_isnoneornil(L, -1))
	{
		lua_getfield(L, -1, ".deformed");
		if (lua_toboolean(L, -1))
		{
			lua_pop(L,1);
			return;
		}
		lua_pop(L,1);
	}
	else
	{
		lua_pop(L,1);
		lua_createtable(L, t->natts, 8);
		lua_pushvalue(L, -1);
		lua_setuservalue(L, nd);
	}
	/* stack: table */

	/* actually do the deform */

	/*
	 * XXX Currently, heap_deform_tuple can't fail or throw error. How thin is
	 * the ice here?  Do a catch-block anyway.
	 */

	PLLUA_TRY();
	{
		HeapTupleData tuple;

		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(htup);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = htup;

		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	PLLUA_CATCH_RETHROW();

	for (i = 0; i < t->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			lua_pushboolean(L, 0);
		else if (nulls[i])
			lua_pushboolean(L, 1);		/* can't use the more natural "nil" */
		else
		{
			pllua_make_datum(L, values[i],
							 tupdesc->attrs[i].atttypid,
							 tupdesc->attrs[i].atttypmod);
			lua_pushvalue(L, nd);
			lua_setuservalue(L, -2);
		}
		lua_seti(L, -2, i+1);
	}
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, ".deformed");
}


static bool pllua_datum_column(lua_State *L, int attno, bool skip_dropped)
{
	switch (lua_geti(L, -1, attno))
	{
		case LUA_TUSERDATA:
			{
				pllua_typeinfo *et;
				pllua_datum *ed = pllua_checkanydatum(L, -1, &et);
				if (pllua_value_from_datum(L, ed->value, et->typeoid) == LUA_TNONE)
					lua_pop(L,1);
				else
				{
					lua_remove(L,-2);
					lua_remove(L,-2);
				}
			}
			break;

		case LUA_TBOOLEAN:
			/* false is a dropped col; true is a present but null col */
			if (skip_dropped && !lua_toboolean(L,-1))
			{
				lua_pop(L,1);
				return false;
			}
			lua_pop(L,1);
			lua_pushnil(L);
			break;

		case LUA_TNIL:
			luaL_error(L, "missing attribute");

		default:
			luaL_error(L, "unexpected type in datum cache");
	}
	return true;
}

static void pllua_datum_getattrs(lua_State *L, int nd, int td)
{
	td = lua_absindex(L, td);
	nd = lua_absindex(L, nd);
	if (luaL_getmetafield(L, nd, "attrs") == LUA_TNIL)
	{
		lua_getfield(L, td, "_attrs");
		lua_pushvalue(L, td);
		lua_call(L, 1, 0);
		if (luaL_getmetafield(L, nd, "attrs") == LUA_TNIL)
			luaL_error(L, "pllua_datum_index: attrs was not populated");
	}
}

/*
 * __index(self,key)
 */
static int pllua_datum_index(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	void **p = pllua_torefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	lua_Integer attno;
	HeapTupleHeader tup;

	if (!d)
		luaL_error(L, "pllua_datum_index: not a datum object");

	if (t->natts < 0)
		luaL_error(L, "datum is not a row type");

	tup = (HeapTupleHeader) DatumGetPointer(d->value);

	switch (lua_type(L, 2))
	{
		default:
			lua_pushnil(L);
			return 1;

		case LUA_TSTRING:
			pllua_datum_getattrs(L, 1, lua_upvalueindex(1));
			/* stack: attrs{ attname = attno } */
			lua_pushvalue(L, 2);
			if (lua_gettable(L, -2) != LUA_TNUMBER)
				luaL_error(L, "datum has no column \"%s\"", lua_tostring(L, 2));
			/*FALLTHROUGH*/

		case LUA_TNUMBER:		/* column number */
			attno = lua_tointeger(L, -1);
			if (attno == ObjectIdAttributeNumber)
				lua_pushinteger(L, (lua_Integer) HeapTupleHeaderGetOid(tup));
			else if (attno < 1 || attno > t->natts)
				luaL_error(L, "datum has no column number %d", attno);
			else
			{
				pllua_datum_deform_tuple(L, 1, d, t);
				pllua_datum_column(L, attno, false);
			}
			return 1;
	}
}

/*
 * Not exposed to the user directly, only as a closure over its index var
 *
 * upvalues:  typeinfo, datum, index, deform, attrs
 */
static int pllua_datum_next(lua_State *L)
{
	void **p = pllua_torefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	int idx = lua_tointeger(L, lua_upvalueindex(3));

	/* don't need the original datum but do this for sanity check */
	pllua_checkdatum(L, lua_upvalueindex(2), lua_upvalueindex(1));

	lua_pushvalue(L, lua_upvalueindex(4));
	for (++idx; idx <= t->natts; ++idx)
	{
		if (pllua_datum_column(L, idx, true))
		{
			lua_pushinteger(L, idx);
			lua_replace(L, lua_upvalueindex(3));
			lua_geti(L, lua_upvalueindex(5), idx);
			lua_insert(L, -2);
			lua_pushinteger(L, idx);
			return 3;
		}
	}
	lua_pushinteger(L, idx);
	lua_replace(L, lua_upvalueindex(3));
	return 0;
}

static int pllua_datum_pairs(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = *pllua_checkrefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);

	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 0);
	pllua_datum_deform_tuple(L, 1, d, t);
	pllua_datum_getattrs(L, 1, lua_upvalueindex(1));
	lua_pushcclosure(L, pllua_datum_next, 5);
	lua_pushnil(L);
	lua_pushnil(L);
	return 3;
}

static struct luaL_Reg datumobj_mt[] = {
	/* __gc entry is handled separately */
	{ "__index", pllua_datum_index },
	{ "__pairs", pllua_datum_pairs },
	{ "__tostring", pllua_datum_tostring },
	{ "_tobinary", pllua_datum_tobinary },
	{ NULL, NULL }
};


/*
 * newtypeinfo(oid,typmod)
 *
 * does not intern the new object.
 */
static int pllua_newtypeinfo(lua_State *L)
{
	Oid oid = luaL_checkinteger(L, 1);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	void **p = pllua_newrefobject(L, PLLUA_TYPEINFO_OBJECT, NULL, true);
	pllua_typeinfo *volatile t;

	if (typmod != -1 && oid != RECORDOID)
		luaL_error(L, "cannot specify typmod for non-RECORD typeinfo");
	if (oid == RECORDOID && typmod == -1)
		luaL_error(L, "must specify typmod for RECORD typeinfo");

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		TupleDesc tupdesc = NULL;
		MemoryContext mcxt = AllocSetContextCreate(CurrentMemoryContext,
												   "pllua type object",
												   ALLOCSET_SMALL_SIZES);
		MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);

		t = palloc0(sizeof(pllua_typeinfo));
		t->mcxt = mcxt;

		t->typeoid = oid;
		t->typmod = typmod;
		t->tupdesc = NULL;
		t->arity = 1;
		t->natts = -1;
		t->hasoid = false;
		t->revalidate = false;
		t->reloid = InvalidOid;

		if (oid == RECORDOID)
		{
			tupdesc = lookup_rowtype_tupdesc_copy(oid, typmod);
			t->tupdesc = tupdesc;
			t->natts = tupdesc->natts;
			t->hasoid = tupdesc->tdhasoid;
		}
		else if ((tupdesc = lookup_rowtype_tupdesc_noerror(getBaseType(oid), typmod, true)))
		{
			t->natts = tupdesc->natts;
			t->hasoid = tupdesc->tdhasoid;
			t->tupdesc = CreateTupleDescCopy(tupdesc);
			t->reloid = get_typ_typrelid(oid);
			ReleaseTupleDesc(tupdesc);
		}

		if (tupdesc)
		{
			int arity = 0;
			int i;
			for (i = 0; i < t->natts; ++i)
				if (!TupleDescAttr(tupdesc,i)->attisdropped)
					++arity;
			t->arity = arity;
		}

		get_type_io_data(oid, IOFunc_output,
						 &t->typlen, &t->typbyval, &t->typalign,
						 &t->typdelim, &t->typioparam, &t->outfuncid);
		t->infuncid = InvalidOid;
		t->sendfuncid = InvalidOid;
		t->recvfuncid = InvalidOid;

		t->outfunc.fn_oid = InvalidOid;
		t->infunc.fn_oid = InvalidOid;
		t->sendfunc.fn_oid = InvalidOid;
		t->recvfunc.fn_oid = InvalidOid;

		MemoryContextSwitchTo(oldcontext);
		MemoryContextSetParent(mcxt, pllua_get_memory_cxt(L));
	}
	PLLUA_CATCH_RETHROW();

	*p = t;

	/*
	 * the table we created for our uservalue is going to be the metatable for
	 * datum objects of this type. We close most of the functions in it over
	 * the typeinfo object itself for easy access.
	 */

	lua_getuservalue(L, -1);
	lua_pushcfunction(L, pllua_datum_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, "typeinfo");
	lua_pushvalue(L, -2);
	luaL_setfuncs(L, datumobj_mt, 1);
	lua_pop(L, 1);

	return 1;
}

static int pllua_typeinfo_eq(lua_State *L)
{
	void **p1 = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	void **p2 = pllua_checkrefobject(L, 2, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *obj1 = *p1;
	pllua_typeinfo *obj2 = *p2;
	if (p1 == p2)
		return 1;

	/*
	 * We don't need to compare everything. If all these fields match, we
	 * assume that existing datums aren't affected by any changes to the
	 * remaining values.
	 */
	if (obj1->typeoid != obj2->typeoid
		|| obj1->typmod != obj2->typmod
		|| obj1->arity != obj2->arity
		|| obj1->natts != obj2->natts
		|| obj1->hasoid != obj2->hasoid
		|| (obj1->tupdesc && !obj2->tupdesc)
		|| (!obj1->tupdesc && obj2->tupdesc)
		|| (obj1->tupdesc && obj2->tupdesc
			&& !equalTupleDescs(obj1->tupdesc, obj2->tupdesc))
		|| obj1->reloid != obj2->reloid
		|| obj1->typlen != obj2->typlen
		|| obj1->typbyval != obj2->typbyval
		|| obj1->typalign != obj2->typalign
		|| obj1->typdelim != obj2->typdelim
		|| obj1->typioparam != obj2->typioparam
		|| obj1->outfuncid != obj2->outfuncid)
	{
		lua_pushboolean(L, false);
		return 1;
	}
	lua_pushboolean(L, true);
	return 1;
}

int pllua_typeinfo_lookup(lua_State *L)
{
	Oid oid = luaL_checkinteger(L, 1);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	void **p = NULL;
	pllua_typeinfo *obj;
	void **np = NULL;
	pllua_typeinfo *nobj;

	if (oid == InvalidOid)
	{
		/* safety check so we never intern an entry for InvalidOid */
		lua_pushnil(L);
		return 1;
	}
	else if (oid == RECORDOID)
	{
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECORDS);
		lua_rawgeti(L, -1, typmod);
	}
	else
	{
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
		lua_rawgeti(L, -1, oid);
	}
	if (!lua_isnil(L, -1))
	{
		p = pllua_checkrefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
		obj = *p;
		if (!obj->revalidate)
			return 1;
	}
	/* stack: oid typmod table oldobject-or-nil */
	/* obj is missing or needs revalidation */
	lua_pushcfunction(L, pllua_newtypeinfo);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_call(L, 2, 1);
	/* stack: oid typmod table oldobject-or-nil newobject */
	np = pllua_checkrefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
	nobj = *np;
	if (p)
	{
		/*
		 * compare old and new object. If they're equal, just drop the new one
		 * and mark the old one valid again. Otherwise we have to intern the
		 * new object in place of the old one.
		 */
		lua_pushcfunction(L, pllua_typeinfo_eq);
		lua_pushvalue(L, -3);
		lua_pushvalue(L, -3);
		lua_call(L, 2, 1);
		if (lua_toboolean(L, -1))
		{
			/* equal. pop the new object */
			obj->revalidate = false;
			lua_pop(L,2);
			return 1;
		}
		/*
		 * We're going to intern the new object in place of the old one
		 */
		lua_pop(L,1);
	}
	/* stack: oid typmod table oldobject-or-nil newobject */
	lua_remove(L, -2);
	lua_pushvalue(L, -1);
	if (oid == RECORDOID)
		lua_rawseti(L, -3, typmod);
	else
		lua_rawseti(L, -3, oid);
	return 1;
}

/*
 * invalidate(typoid,reloid)
 */
int pllua_typeinfo_invalidate(lua_State *L)
{
	if (lua_type(L,1) == LUA_TNUMBER)
	{
		Oid typoid = lua_tointeger(L, 1);
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
		if (OidIsValid(typoid))
		{
			pllua_typeinfo *t;
			if (lua_rawgeti(L, -1, (lua_Integer) typoid) == LUA_TUSERDATA)
			{
				t = *pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
				t->revalidate = true;
			}
		}
		else
		{
			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				pllua_typeinfo *t = *pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
				t->revalidate = true;
				lua_pop(L,1);
			}
		}
	}
	if (lua_type(L,2) == LUA_TNUMBER)
	{
		Oid relid = lua_tointeger(L, 2);
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
		lua_pushnil(L);
		while (lua_next(L, -2))
		{
			pllua_typeinfo *t = *pllua_torefobject(L, -1, PLLUA_TYPEINFO_OBJECT);
			if (t->reloid == relid)
				t->revalidate = true;
			lua_pop(L,1);
		}
	}
	return 0;
}

static int pllua_typeinfo_gc(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *obj = *p;

	*p = NULL;
	if (!obj)
		return 0;

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		/*
		 * typeinfo is allocated in its own memory context (since we expect it
		 * to have stuff dangling off), so free it by destroying that.
		 */
		MemoryContextDelete(obj->mcxt);
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

static int pllua_dump_typeinfo(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *obj = *p;
	luaL_Buffer b;
	char *buf = luaL_buffinitsize(L, &b, 1024);

	if (!obj)
	{
		luaL_addstring(&b, "(null)");
		luaL_pushresult(&b);
		return 1;
	}

	snprintf(buf, 1024,
			 "oid: %u  typmod: %d  natts: %d  hasoid: %c  revalidate: %c  "
			 "tupdesc: %p  reloid: %u  typlen: %d  typbyval: %c  "
			 "typalign: %c  typdelim: %c  typioparam: %u  outfuncid: %u",
			 obj->typeoid, obj->typmod, obj->natts,
			 obj->hasoid ? 't' : 'f',
			 obj->revalidate ? 't' : 'f',
			 obj->tupdesc,
			 obj->reloid,
			 (int) obj->typlen,
			 obj->typbyval ? 't' : 'f',
			 obj->typalign,
			 obj->typdelim,
			 obj->typioparam,
			 obj->outfuncid);
	luaL_addsize(&b, strlen(buf));

	luaL_pushresult(&b);
	return 1;
}

/* given a pg type name, return a typeinfo object */

static int pllua_typeinfo_parsetype(lua_State *L)
{
	const char *str = lua_tostring(L, 1);
	volatile Oid ret_oid = InvalidOid;
	volatile int32 ret_typmod = -1;

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		Oid oid = InvalidOid;
		int32 typmod = -1;

		/*
		 * Don't really want regtypein because it allows things like numeric
		 * oids, '-' and so on. Accept only valid names here.
		 *
		 */
		parseTypeString(str, &oid, &typmod, true);
		ret_oid = oid;
		ret_typmod = typmod;
	}
	PLLUA_CATCH_RETHROW();

	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, (lua_Integer) ret_oid);
	lua_pushinteger(L, (lua_Integer) ret_typmod);
	lua_call(L, 2, 1);
	return 1;
}

static int pllua_typeinfo_attrs(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *obj = *p;
	int i;
	TupleDesc tupdesc = obj->tupdesc;

	if (obj->natts < 0)
		return 0;
	lua_getuservalue(L, 1);
	lua_createtable(L, obj->natts, obj->natts);

	/* stack: typeinfo metatable attrtab */

	for (i = 0; i < obj->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		lua_pushinteger(L, i+1);
		lua_pushstring(L, NameStr(att->attname));
		lua_pushvalue(L, -1);
		lua_pushinteger(L, i+1);
		lua_settable(L, -5);
		lua_settable(L, -3);
	}
	lua_setfield(L, -2, "attrs");
	return 0;
}


/*
 * Main user-visible entry:
 *
 * pgtype(x, [N]) returns the typeinfo object for datum x, or if x is not a
 * datum object and N is an integer, the typeinfo for argument N (counting from
 * 1) of the current function if one exists (throwing a lua error if not), or
 * the return type if N==0. If N is a string, parse it as a pg type string.
 *
 */
static int pllua_typeinfo_package_call(lua_State *L)
{
	if (lua_isuserdata(L, 2)
		&& lua_getmetatable(L, 2))
	{
		if (lua_getfield(L, -1, "_typeinfo") == LUA_TUSERDATA)
			return 1;
		lua_pop(L,2);
	}
	if (lua_isnoneornil(L, 3))
		return 0;
	if (lua_isinteger(L, 3))
	{
		int idx = lua_tointeger(L, 3);
		FmgrInfo *flinfo;
		pllua_func_activation *act;
		Oid oid = InvalidOid;
		int32 typmod = -1;
		pllua_get_cur_act(L); /* throws if not in a function */
		act = pllua_toobject(L, -1, PLLUA_ACTIVATION_OBJECT);
		if (idx == 0)
		{
			oid = act->rettype;
			if (oid == RECORDOID && act->tupdesc)
				typmod = act->tupdesc->tdtypmod;
		}
		else if (idx > 0 && idx <= act->nargs)
		{
			if (act->argtypes[idx - 1] != ANYOID)
				oid = act->argtypes[idx - 1];
			else if ((flinfo = pllua_get_cur_flinfo(L)))
				oid = get_fn_expr_argtype(flinfo, idx-1);
			else
				oid = ANYOID;
		}
		else if (idx > act->nargs
				 && act->func_info->variadic_any
				 && (flinfo = pllua_get_cur_flinfo(L)))
		{
			oid = get_fn_expr_argtype(flinfo, idx-1);
		}

		if (!OidIsValid(oid))
			luaL_error(L, "argument index out of range");

		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) oid);
		lua_pushinteger(L, (lua_Integer) typmod);
		lua_call(L, 2, 1);
		if (lua_isnil(L, -1))
			luaL_error(L, "unknown type");
		return 1;
	}
	if (lua_type(L, 3) == LUA_TSTRING)
	{
		lua_pushcfunction(L, pllua_typeinfo_parsetype);
		lua_pushvalue(L, 3);
		lua_call(L, 1, 1);
		if (lua_isnil(L, -1))
			luaL_error(L, "unknown type");
		return 1;
	}
	return luaL_error(L, "invalid argument type");
}

static int pllua_typeinfo_name(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	bool typmod_given = !lua_isnoneornil(L, 2);
	pllua_typeinfo *obj = *p;
	const char *volatile name = NULL;

	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		if (SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(obj->typeoid)))
		{
			if (typmod_given && obj->typeoid != RECORDOID)
				name = format_type_with_typemod(obj->typeoid, typmod);
			else
				name = format_type_be(obj->typeoid);
		}
	}
	PLLUA_CATCH_RETHROW();

	if (!name)
		luaL_error(L, "type not found when generating name");

	lua_pushstring(L, name);
	return 1;
}

static bool pllua_typeinfo_iofunc(lua_State *L,
								  pllua_typeinfo *t,
								  IOFuncSelector whichfunc)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;
	Oid funcoid;
	FmgrInfo *flinfo;

	ASSERT_PG_CONTEXT;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(t->typeoid));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", t->typeoid);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	switch (whichfunc)
	{
		case IOFunc_input:
			funcoid = pt->typinput;
			t->infuncid = funcoid;
			flinfo = &t->infunc;
			break;
		case IOFunc_output:
			funcoid = pt->typoutput;
			t->outfuncid = funcoid;
			flinfo = &t->outfunc;
			break;
		case IOFunc_receive:
			funcoid = pt->typreceive;
			t->recvfuncid = funcoid;
			flinfo = &t->recvfunc;
			break;
		case IOFunc_send:
			funcoid = pt->typsend;
			t->sendfuncid = funcoid;
			flinfo = &t->sendfunc;
			break;
	}
	ReleaseSysCache(typeTuple);

	if (!OidIsValid(funcoid))
		return false;

	fmgr_info_cxt(funcoid, flinfo, t->mcxt);
	return true;
}

/*
 * t:fromstring('str')  returns a datum object.
 *
 * Given a nil input, it returns nil, but might call the input function anyway
 * (only if it's not strict)
 */
static int pllua_typeinfo_fromstring(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	const char *str = lua_isnil(L, 2) ? NULL : lua_tostring(L, 2);
	MemoryContext mcxt = pllua_get_memory_cxt(L);
	pllua_datum *d = NULL;
	volatile bool done = false;

	if (!str)
	{
		lua_pushnil(L);
		return 1;
	}

	ASSERT_LUA_CONTEXT;

	if (str)
		pllua_verify_encoding(L, str);

	lua_pushvalue(L, 1);
	if (str)
		d = pllua_newdatum(L);
	else
		lua_pushnil(L);

	PLLUA_TRY();
	{
		Datum nv;

		if ((OidIsValid(t->infuncid) && OidIsValid(t->infunc.fn_oid))
			|| pllua_typeinfo_iofunc(L, t, IOFunc_input))
		{
			nv = InputFunctionCall(&t->infunc, (char *) str, t->typioparam, t->typmod);
			if (str)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
				d->value = nv;
				pllua_savedatum(L, d, t);
				MemoryContextSwitchTo(oldcontext);
			}
			done = true;
		}
	}
	PLLUA_CATCH_RETHROW();

	if (!done)
		luaL_error(L, "could not find input function for type");
	return 1;
}

/*
 * t:frombinary('str')  returns a datum object.
 *
 * Given a nil input, it returns nil, but might call the input function anyway
 * (only if it's not strict)
 *
 * CAVEAT: this assumes, for many types, that the binary data is in the
 * current _client_ encoding, not the server encoding.
 */
static int pllua_typeinfo_frombinary(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *t = *p;
	size_t len = 0;
	const char *str = lua_isnil(L, 2) ? NULL : lua_tolstring(L, 2, &len);
	MemoryContext mcxt = pllua_get_memory_cxt(L);
	pllua_datum *d = NULL;
	volatile bool done = false;

	if (!str)
		return 0;

	ASSERT_LUA_CONTEXT;

	lua_pushvalue(L, 1);
	if (str)
		d = pllua_newdatum(L);
	else
		lua_pushnil(L);

	PLLUA_TRY();
	{
		Datum nv;
		StringInfoData buf;
		initStringInfo(&buf);
		if (str)
			appendBinaryStringInfo(&buf, str, len);

		if ((OidIsValid(t->recvfuncid) && OidIsValid(t->recvfunc.fn_oid))
			|| pllua_typeinfo_iofunc(L, t, IOFunc_receive))
		{
			nv = ReceiveFunctionCall(&t->infunc, str ? &buf : NULL, t->typioparam, t->typmod);
			if (str)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
				d->value = nv;
				pllua_savedatum(L, d, t);
				MemoryContextSwitchTo(oldcontext);
			}
			done = true;
		}
		pfree(buf.data);
	}
	PLLUA_CATCH_RETHROW();

	if (!done)
		luaL_error(L, "could not find receive function for type");
	return 1;
}

/*
 * "call" for a typeinfo is to invoke it as a value constructor:
 *
 * t(val,val,val,...)
 *
 * The number of args must match the arity of the type. Each value must either
 * be acceptable simple input for the field type, or a datum of the correct
 * type - probably this needs extending to allow casts.
 *
 */
static int pllua_typeinfo_call(lua_State *L)
{
	pllua_typeinfo *t = *pllua_checkrefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_datum *newd;
	int nargs = lua_gettop(L) - 1;
	Datum values[MaxTupleAttributeNumber + 1];
	bool isnull[MaxTupleAttributeNumber + 1];
	int i;
	int argno = 0;
	int natts = t->natts;

	if (nargs != t->arity)
		luaL_error(L, "incorrect number of arguments for type constructor (expected %d got %d)",
				   t->arity, nargs);

	for (argno = 0, i = 0; i < ((natts < 0) ? 1 : natts); ++i)
	{
		Oid coltype = ((i == 0 && t->natts < 0)
					   ? t->typeoid
					   : TupleDescAttr(t->tupdesc, i)->atttypid);
		if (TupleDescAttr(t->tupdesc, i)->attisdropped)
		{
			values[i] = (Datum)0;
			isnull[i] = true;
			continue;
		}

		++argno;

		if (lua_type(L, 2+argno) == LUA_TUSERDATA)
		{
			pllua_typeinfo *dt;
			pllua_datum *d = pllua_checkanydatum(L, 1+argno, &dt);
			if (dt->typeoid != coltype)
				luaL_error(L, "incorrect argtype");
			values[i] = d->value;
			isnull[i] = false;
			lua_pop(L,1);
		}
	}

	PLLUA_TRY();
	{
		for (argno = 0, i = 0; i < ((natts < 0) ? 1 : natts); ++i)
		{
			Oid coltype = ((i == 0 && t->natts < 0)
						   ? t->typeoid
						   : TupleDescAttr(t->tupdesc, i)->atttypid);
			if (TupleDescAttr(t->tupdesc, i)->attisdropped)
				continue;

			++argno;

			if (lua_type(L, 1+argno) != LUA_TUSERDATA)
			{
				if (!pllua_datum_from_value(L, 1+argno, coltype, &values[i], &isnull[i]))
					elog(ERROR, "incompatible value type");
			}
		}
	}
	PLLUA_CATCH_RETHROW();

	if (natts < 0 && isnull[0])
	{
		lua_pushnil(L);
		return 1;
	}

	lua_pushvalue(L, 1);
	newd = pllua_newdatum(L);
	lua_remove(L, -2);

	PLLUA_TRY();
	{
		MemoryContext mcxt = pllua_get_memory_cxt(L);

		if (natts < 0)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
			newd->value = values[0];
			pllua_savedatum(L, newd, t);
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			HeapTuple htup = heap_form_tuple(t->tupdesc, values, isnull);
			MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
			newd->value = heap_copy_tuple_as_datum(htup, t->tupdesc);
			newd->need_gc = true;
			MemoryContextSwitchTo(oldcontext);
		}
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

static struct luaL_Reg typeinfo_mt[] = {
	{ "__eq", pllua_typeinfo_eq },
	{ "__gc", pllua_typeinfo_gc },
	{ "__tostring", pllua_dump_typeinfo },
	{ "__call", pllua_typeinfo_call },
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_methods[] = {
	{ "fromstring", pllua_typeinfo_fromstring },
	{ "frombinary", pllua_typeinfo_frombinary },
	{ "dump", pllua_dump_typeinfo },
	{ "name", pllua_typeinfo_name },
	{ "_attrs", pllua_typeinfo_attrs },
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_funcs[] = {
	{ "lookup", pllua_typeinfo_lookup },
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_package_mt[] = {
	{ "__call", pllua_typeinfo_package_call },
	{ NULL, NULL }
};

void pllua_init_datum_objects(lua_State *L)
{
	pllua_newmetatable(L, PLLUA_TYPEINFO_OBJECT, typeinfo_mt);
	lua_newtable(L);
	luaL_setfuncs(L, typeinfo_methods, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_newtable(L);
	pllua_newmetatable(L, PLLUA_TYPEINFO_PACKAGE_OBJECT, typeinfo_package_mt);
	lua_setmetatable(L, -2);
	luaL_setfuncs(L, typeinfo_funcs, 0);
	lua_setglobal(L, "pgtype");
}
