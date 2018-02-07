/* datum.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#if PG_VERSION_NUM < 110000
#define DatumGetRangeTypeP(d_) DatumGetRangeType(d_)
#define DatumGetAnyArrayP(d_) DatumGetAnyArray(d_)
#endif

/*
 * Basic plan of attack:
 *
 * A Datum object has this directly in its body:
 *
 *   Datum value;
 *   int32 typmod;
 *   bool  need_gc;
 *   bool  modified;
 *
 * We create the object initially with just the value, and need_gc false.
 * However, we then have to (more or less immediately) copy the value if it's a
 * byref type, since we have no control over its lifetime inside lua (it may
 * even need to survive across transactions, so we have to detoast it too).
 * The code that does the copying is separated from the initial creation for
 * reasons of error handling.
 *
 * An exception is made for datums extracted from a row which is itself already
 * a datum. For this case we leave need_gc false, and put a reference to the
 * parent value in the uservalue slot.
 *
 * Typmod is -1 unless we took this datum from a column in which case it's the
 * column atttypmod.
 *
 * Information about the object type is contained in a typeinfo object. We keep
 * a cache of type info (by oid) and tupdesc info (by typmod for RECORD
 * tupdescs). Because our cache is decoupled from the syscache and very
 * long-lived, we register for invalidations.
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
static void pllua_typeconv_register(lua_State *L, int tabidx, int typeidx);
static const char *pllua_typeinfo_raw_output(lua_State *L, Datum value, pllua_typeinfo *t);

#if LUAJIT_VERSION_NUM > 0 && !defined(NO_LUAJIT)

static char PLLUA_INT8HACK_INFUNC[] = "int8hack infunc";
static char PLLUA_INT8HACK_OUTFUNC[] = "int8hack outfunc";

static const char *luajit_lua =
"local ffi = require 'ffi' \n"
"local u64 = ffi.typeof('uint64_t') \n"
"local s64 = ffi.typeof('int64_t') \n"
"local u32 = ffi.typeof('uint32_t') \n"
"local s32 = ffi.typeof('int32_t') \n"
"local u16 = ffi.typeof('uint16_t') \n"
"local s16 = ffi.typeof('int16_t') \n"
"local u8 = ffi.typeof('uint8_t') \n"
"local s8 = ffi.typeof('int8_t') \n"
"local function infunc(lo,hi) \n"
"  return s64(u64(hi) * 4294967296ULL + u64(lo)) \n"
"end \n"
"local function outfunc(v) \n"
"  if ffi.istype(s64,v) then \n"
"    return tonumber(u64(v) / 4294967296ULL), tonumber(u64(v) % 4294967296ULL), true \n"
"  elseif ffi.istype(u64,v) then \n"
"    return tonumber(v / 4294967296ULL), tonumber(v % 4294967296ULL), false \n"
"  elseif ffi.istype(s32,v) \n"
"      or ffi.istype(u32,v) \n"
"      or ffi.istype(s8,v) \n"
"      or ffi.istype(u8,v) \n"
"      or ffi.istype(s16,v) \n"
"      or ffi.istype(u16,v) \n"
"  then \n"
"    return v < 0 and -1 or 0, tonumber(u32(v)), true \n"
"  end \n"
"end \n"
"return infunc,outfunc\n";

#endif

/*
 * IMPORTANT!!!
 *
 * It is _our_ responsibility to verify encoding correctness when passing any
 * string data from untrusted sources (i.e. the Lua code) into PG server apis.
 */
void pllua_verify_encoding(lua_State *L, const char *str)
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

bool pllua_verify_encoding_noerror(lua_State *L, const char *str)
{
	if (!str)
		return true;
	return pg_verifymbstr(str, strlen(str), true);
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

	if (nd != d)
		pllua_record_gc_debt(L, VARSIZE(DatumGetPointer(nd)));

	return nd;
}

void *pllua_palloc(lua_State *L, size_t sz)
{
	void *volatile res = NULL;
	PLLUA_TRY();
	{
		res = palloc(sz);
	}
	PLLUA_CATCH_RETHROW();
	pllua_record_gc_debt(L, sz);
	return res;
}

static Datum
pllua_float4_get_datum(lua_State *L, float4 val)
{
#if USE_FLOAT4_BYVAL
	return Float4GetDatum(val);
#else
	float4 *p = pllua_palloc(L, sizeof(float4));
	*p = val;
	return Float4GetDatumFast(*p);
#endif
}

static Datum
pllua_float8_get_datum(lua_State *L, float8 val)
{
#if USE_FLOAT8_BYVAL
	return Float8GetDatum(val);
#else
	float8 *p = pllua_palloc(L, sizeof(float8));
	*p = val;
	return Float8GetDatumFast(*p);
#endif
}

static Datum
pllua_int64_get_datum(lua_State *L, int64 val)
{
#if USE_FLOAT8_BYVAL  /* yes, this controls int64 too */
	return Int64GetDatum(val);
#else
	int64 *p = pllua_palloc(L, sizeof(int64));
	*p = val;
	return Int64GetDatumFast(*p);
#endif
}

/*
 * Get the typeid/typmod from a datum tuple, regardless of its toast status.
 *
 * This works in either lua or pg context.
 */
static void
pllua_get_tuple_type(lua_State *L, Datum value, Oid *typeid, int32 *typmod)
{
	*typeid = InvalidOid;
	if (typmod)
		*typmod = -1;

	if (VARATT_IS_EXTENDED(value))
	{
		PLLUA_TRY();
		{
			HeapTupleHeader htup = (HeapTupleHeader)
				heap_tuple_untoast_attr_slice(
					(struct varlena *) DatumGetPointer(value),
					0,
					sizeof(HeapTupleHeaderData));
			*typeid = HeapTupleHeaderGetTypeId(htup);
			if (typmod)
				*typmod = HeapTupleHeaderGetTypMod(htup);
			pfree(htup);
		}
		PLLUA_CATCH_RETHROW();
	}
	else
	{
		HeapTupleHeader htup = (HeapTupleHeader) DatumGetPointer(value);
		*typeid = HeapTupleHeaderGetTypeId(htup);
		if (typmod)
			*typmod = HeapTupleHeaderGetTypMod(htup);
	}
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
		/*
		 * Everything has a text representation, but we use this only for those
		 * types where there isn't really any structure _other_ than text.
		 */
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case XMLOID:
		case JSONOID:
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
#if defined(PLLUA_INT8_OK)
		case INT8OID:
			lua_pushinteger(L, (lua_Integer) DatumGetInt64(value));
			return LUA_TNUMBER;
#elif defined(PLLUA_INT8_LUAJIT_HACK)
		case INT8OID:
			{
				int64 v = DatumGetInt64(value);
				lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_INT8HACK_INFUNC);
				lua_pushnumber(L, (lua_Number) (double) (v & 0xFFFFFFFF));
				lua_pushnumber(L, (lua_Number) (double) ((v >> 32) & 0xFFFFFFFF));
				lua_call(L, 2, 1);
			}
			return lua_type(L, -1);
#endif
		case REFCURSOROID:
			lua_pushcfunction(L, pllua_spi_newcursor);
			{
				Datum v = pllua_detoast_light(L, value);
				lua_pushlstring(L, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
			}
			lua_call(L, 1, 1);
			return LUA_TUSERDATA;
		default:
			return LUA_TNONE;
	}
}

/*
 * If a datum type corresponds to a simple Lua type, then take a value of that
 * type and return as Datum/isnull. May copy the data into the current memory
 * context (but uses a catch block for that; requires Lua context).
 *
 * nil is accepted as input for any type whatsoever (and treated as NULL).
 *
 * Throws a lua error only on memory exhaustion.
 *
 * Note: for some value types (notably cstring), does not copy the data. Caller
 * must ensure that savedatum/formtuple/construct_array is done before dropping
 * the reference to the lua value.
 */
bool pllua_datum_from_value(lua_State *L, int nd,
							Oid typeid,
							Datum *result,
							bool *isnull,
							const char **errstr)
{
	ASSERT_LUA_CONTEXT;

	nd = lua_absindex(L, nd);

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
			*errstr = "missing value";
			return true;

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

				/*
				 * Only handle the common cases here, we punt everything else
				 * to the input functions. (The only one that really matters here
				 * is bytea, where the semantics are different.)
				 */
				switch (typeid)
				{
					case TEXTOID:
					case VARCHAROID:
					case REFCURSOROID:
						{
							text *t;
							if (len != strlen(str))
								*errstr = "null characters not allowed in text values";
							else if (!pllua_verify_encoding_noerror(L, str))
								*errstr = "invalid encoding for text value";
							else
							{
								t = pllua_palloc(L, len + VARHDRSZ);
								memcpy(VARDATA(t), str, len);
								SET_VARSIZE(t, len + VARHDRSZ);
								*result = PointerGetDatum(t);
							}
						}
						return true;

					case BYTEAOID:
						{
							bytea *b = pllua_palloc(L, len + VARHDRSZ);
							memcpy(VARDATA(b), str, len);
							SET_VARSIZE(b, len + VARHDRSZ);
							*result = PointerGetDatum(b);
						}
						return true;

					case CSTRINGOID:
						{
							if (len != strlen(str))
								*errstr = "null characters not allowed in cstring values";
							else if (!pllua_verify_encoding_noerror(L, str))
								*errstr = "invalid encoding for cstring value";
							else
								*result = CStringGetDatum(str);
						}
						return true;

					case BOOLOID:
						{
							bool v = false;
							if (parse_bool_with_len(str, len, &v))
								*result = BoolGetDatum(v);
							else
								*errstr = "invalid boolean value";
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
						*result = pllua_float4_get_datum(L, (float4) floatval);
						return true;

					case FLOAT8OID:
						*result = pllua_float8_get_datum(L, (float8) floatval);
						return true;

					case BOOLOID:
						if (isint)
							*result = BoolGetDatum( (intval != 0) );
						else
							*errstr = "invalid boolean value";
						return true;

					case OIDOID:
						if (isint && intval == (lua_Integer)(Oid)intval)
							*result = ObjectIdGetDatum( (Oid)intval );
						else
							*errstr = "oid value out of range";
						return true;

					case INT2OID:
						if (isint && intval >= PG_INT16_MIN && intval <= PG_INT16_MAX)
							*result = Int16GetDatum(intval);
						else
							*errstr = "smallint value out of range";
						return true;

					case INT4OID:
						if (isint && intval >= PG_INT32_MIN && intval <= PG_INT32_MAX)
							*result = Int32GetDatum(intval);
						else
							*errstr = "integer value out of range";
						return true;

					case INT8OID:
						if (isint)
							*result = pllua_int64_get_datum(L, intval);
						else
							*errstr = "bigint out of range";
						return true;

					case NUMERICOID:
						PLLUA_TRY();
						{
							if (isint)
								*result = DirectFunctionCall1(int8_numeric, Int64GetDatumFast(intval));
							else
								*result = DirectFunctionCall1(float8_numeric, Float8GetDatumFast(floatval));
						}
						PLLUA_CATCH_RETHROW();
						return true;
				}
			}
			return false;

		case LUA_TUSERDATA:
			if (typeid == REFCURSOROID &&
				pllua_toobject(L, nd, PLLUA_SPI_CURSOR_OBJECT))
			{
				text *t;
				size_t len;
				const char *str;

				lua_pushcfunction(L, pllua_cursor_name);
				lua_pushvalue(L, nd);
				lua_call(L, 1, 1);

				if (!lua_isnil(L, -1))
				{
					/* cursor name is already checked for encoding correctness */
					str = lua_tolstring(L, -1, &len);
					t = pllua_palloc(L, len + VARHDRSZ);
					memcpy(VARDATA(t), str, len);
					SET_VARSIZE(t, len + VARHDRSZ);
					*result = PointerGetDatum(t);
				}
				else
				{
					*isnull = true;
					*result = (Datum)0;
				}
				return true;
			}
			return false;

/*
 * Enable this even if we don't convert bigint to cdata ourselves, since
 * there's no reason not to allow cdata parameters to constructor calls.
 */
#if LUAJIT_VERSION_NUM > 0 && !defined(NO_LUAJIT)
		case LUA_TCDATA:
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_INT8HACK_OUTFUNC);
			lua_pushvalue(L, nd);
			lua_call(L, 1, 3);
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 3);
				return false;
			}
			else
			{
				bool s64 = lua_toboolean(L, -1);
				uint64 lo = (uint64) lua_tonumber(L, -2);
				uint64 hi = (uint64) lua_tonumber(L, -3);
				uint64 uval = (uint64) ((hi << 32) | lo);
				int64 val = (int64) uval;
				lua_pop(L, 3);
				switch (typeid)
				{
					case FLOAT4OID:
						if (s64)
							*result = pllua_float4_get_datum(L, (float4) val);
						else
							*result = pllua_float4_get_datum(L, (float4) uval);
						return true;

					case FLOAT8OID:
						if (s64)
							*result = pllua_float8_get_datum(L, (float8) val);
						else
							*result = pllua_float8_get_datum(L, (float8) uval);
						return true;

					case BOOLOID:
						*result = BoolGetDatum( (val != 0) );
						return true;

					case OIDOID:
						if (uval == (uint64)(Oid)uval)
							*result = ObjectIdGetDatum( (Oid)uval );
						else
							*errstr = "oid value out of range";
						return true;

					case INT2OID:
						if (s64
							? (val >= PG_INT16_MIN && val <= PG_INT16_MAX)
							: (uval <= PG_INT16_MAX))
							*result = Int16GetDatum(val);
						else
							*errstr = "smallint value out of range";
						return true;

					case INT4OID:
						if (s64
							? (val >= PG_INT32_MIN && val <= PG_INT32_MAX)
							: (uval <= PG_INT32_MAX))
							*result = Int32GetDatum(val);
						else
							*errstr = "integer value out of range";
						return true;

					case INT8OID:
						if (s64 || uval <= PG_INT64_MAX)
							*result = pllua_int64_get_datum(L, val);
						else
							*errstr = "bigint value out of range";
						return true;

					case NUMERICOID:
						PLLUA_TRY();
						{
							if (s64 || uval <= PG_INT64_MAX)
								*result = DirectFunctionCall1(int8_numeric, Int64GetDatumFast(val));
							else
							{
								char str[32];
								snprintf(str, 32, UINT64_FORMAT, uval);
								*result = DirectFunctionCall3(numeric_in,
															  CStringGetDatum(str),
															  ObjectIdGetDatum(InvalidOid),
															  Int32GetDatum(-1));
							}
						}
						PLLUA_CATCH_RETHROW();
						return true;
				}
			}
			return false;
#endif

		default:
			return false;
	}
}


/*
 * Make the datum at "nd" hold a reference to the one on the stack top.
 */
static void pllua_datum_reference(lua_State *L, int nd)
{
	ASSERT_LUA_CONTEXT;

	pllua_set_user_field(L, nd, ".datumref");
}


static int pllua_datum_gc(lua_State *L)
{
	pllua_datum *p = lua_touserdata(L, 1);

	if (!p || !p->need_gc || !DatumGetPointer(p->value))
		return 0;

	ASSERT_LUA_CONTEXT;

	/*
	 * Don't retry if something goes south.
	 */
	p->need_gc = false;

	/*
	 * Remove our metatable. There are ways (using keys of ephemeron tables)
	 * that Lua code can hold on to references to post-finalized objects; this
	 * makes sure that all they end up seeing is an opaque and inert userdata.
	 */
	lua_pushnil(L);
	lua_setmetatable(L, 1);

	PLLUA_TRY();
	{
		if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(p->value)))
		{
			pllua_debug(L, "pllua_datum_gc: expanded object %p", DatumGetPointer(p->value));
			DeleteExpandedObject(p->value);
		}
		else if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(p->value)))
		{
			/* how'd this get here? */
			elog(ERROR, "unexpected expanded datum");
		}
		else
		{
			pllua_debug(L, "pllua_datum_gc: flat object %p", DatumGetPointer(p->value));
			pfree(DatumGetPointer(p->value));
		}
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

pllua_typeinfo *pllua_totypeinfo(lua_State *L, int nd)
{
	void **p = pllua_torefobject(L, nd, PLLUA_TYPEINFO_OBJECT);
	return p ? *p : NULL;
}

pllua_typeinfo *pllua_checktypeinfo(lua_State *L, int nd, bool revalidate)
{
	pllua_typeinfo *t = *pllua_checkrefobject(L, nd, PLLUA_TYPEINFO_OBJECT);
	if (!t)
		luaL_error(L, "invalid typeinfo");
	if (!revalidate || !t->revalidate || t->obsolete || t->modified)
		return t;
	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, (lua_Integer) t->typeoid);
	lua_pushinteger(L, (lua_Integer) t->typmod);
	lua_call(L, 2, 0);  /* discard result, we don't need it */
	/* t->revalidate must have been cleared by cache replacement */
	Assert(!t->revalidate);
	return t;
}

/*
 * check that the item at "nd" is a datum whose typeinfo is "td"
 * (caller must have already checked that it really is a typeinfo)
 */
pllua_datum *pllua_todatum(lua_State *L, int nd, int td)
{
	void *p = lua_touserdata(L, nd);
	td = lua_absindex(L, td);
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

pllua_datum *pllua_checkdatum(lua_State *L, int nd, int td)
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
pllua_datum *pllua_toanydatum(lua_State *L, int nd, pllua_typeinfo **ti)
{
	pllua_typeinfo *t = NULL;
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
			t = pllua_totypeinfo(L, -1);
			if (!t)
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
			if (t->revalidate)
			{
				lua_pushcfunction(L, pllua_typeinfo_lookup);
				lua_pushinteger(L, (lua_Integer) t->typeoid);
				lua_pushinteger(L, (lua_Integer) t->typmod);
				lua_call(L, 2, 0);  /* discard result, we don't need it */
			}
			if (ti)
				*ti = t;
			return p;
		}
	}
	return NULL;
}

pllua_datum *pllua_checkanydatum(lua_State *L, int nd, pllua_typeinfo **ti)
{
	pllua_datum *p = pllua_toanydatum(L, nd, ti);
	if (!p)
		luaL_argerror(L, nd, "datum");
	return p;
}

pllua_datum *pllua_newdatum(lua_State *L, int nt, Datum value)
{
	pllua_datum *d;
	pllua_typeinfo *t =	pllua_checktypeinfo(L, nt, false);

	lua_pushvalue(L, nt);
	d = lua_newuserdata(L, sizeof(pllua_datum));

#if MANDATORY_USERVALUE
	lua_newtable(L);
	lua_setuservalue(L, -2);
#endif

	d->value = value;
	d->typmod = -1;
	d->need_gc = false;
	d->modified = false;

	/*
	 * If this is a record type of unknown structure but known value, see about
	 * replacing the caller-supplied typeinfo with one that reflects the actual
	 * value.
	 *
	 * XXX revisit when certain pg issues are addressed.
	 */
	if (t->is_anonymous_record && value != (Datum)0)
	{
		Oid typeid;
		int32 typmod;

		pllua_get_tuple_type(L, value, &typeid, &typmod);

		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) typeid);
		lua_pushinteger(L, (lua_Integer) typmod);
		lua_call(L, 2, 1);
		if (!lua_isnil(L, -1))
		{
			t = pllua_checktypeinfo(L, -1, false);
			lua_replace(L, -3);
		}
	}

	lua_getuservalue(L, -2);
	lua_setmetatable(L, -2);
	lua_remove(L, -2);

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
		pllua_record_gc_debt(L, t->typlen);
		return;
	}

	/*
	 * Varlena type, which may need detoast. For record types, we may need to
	 * detoast internal fields.
	 */

	if (t->natts >= 0)
	{
		HeapTupleHeader htup = (HeapTupleHeader) DatumGetPointer(d->value);
		HeapTupleData tuple;

		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(htup);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = htup;

		nv = heap_copy_tuple_as_datum(&tuple, t->tupdesc);
		d->value = nv;
	}
	else if (t->is_array)
	{
		if (VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d->value)))
		{
			/*
			 * read/write pointer to an expanded array; we should be safe to
			 * just own it.
			 */
			nv = TransferExpandedObject(d->value, CurrentMemoryContext);
			d->value = nv;
		}
		else
		{
			/*
			 * Otherwise, expand it into the current memory context.
			 */
			nv = expand_array(d->value, CurrentMemoryContext, &t->array_meta);
			d->value = nv;
		}
	}
	else
	{
		nv = PointerGetDatum(PG_DETOAST_DATUM_COPY(d->value));
		d->value = nv;
	}
	pllua_record_gc_debt(L, toast_datum_size(d->value));
	d->need_gc = true;
	return;
}

/*
 * We should avoid saving individual datums retail, but it happens anyway.
 */
void
pllua_save_one_datum(lua_State *L, pllua_datum *d, pllua_typeinfo *t)
{
	ASSERT_LUA_CONTEXT;

	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		pllua_savedatum(L, d, t);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();
}


/*
 * __tostring(d)  returns the string representation of the datum.
 *
 * We get the typeinfo object from the closure.
 */
static int pllua_datum_tostring(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), true);
	const char *volatile str = NULL;

	ASSERT_LUA_CONTEXT;

	if (d->modified)
	{
		/* form a new datum by imploding the arg */
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		d = pllua_checkdatum(L, -1, lua_upvalueindex(1));
	}

	PLLUA_TRY();
	{
		str = pllua_typeinfo_raw_output(L, d->value, t);
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
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), true);
	bytea *volatile res = NULL;
	volatile bool done = false;

	ASSERT_LUA_CONTEXT;

	if (d->modified)
	{
		/* form a new datum by imploding the arg */
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		d = pllua_checkdatum(L, -1, lua_upvalueindex(1));
	}

	PLLUA_TRY();
	{
		if ((OidIsValid(t->sendfuncid) && OidIsValid(t->sendfunc.fn_oid))
			|| pllua_typeinfo_iofunc(L, t, IOFunc_send))
		{
			res = SendFunctionCall(&t->sendfunc, d->value);
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
	bool needsave[MaxTupleAttributeNumber + 1];
	pllua_datum *savedatum[MaxTupleAttributeNumber + 1];
	pllua_typeinfo *saveti[MaxTupleAttributeNumber + 1];
	TupleDesc tupdesc = t->tupdesc;
	MemoryContext mcxt = pllua_get_memory_cxt(L);
	bool anysave = false;
	int i;

	nd = lua_absindex(L, nd);
	if (pllua_get_user_field(L, nd, ".deformed") == LUA_TTABLE)
		return;
	lua_pop(L, 1);

	if (luaL_getmetafield(L, nd, "attrtypes") != LUA_TTABLE)
		luaL_error(L, "mising attrtypes table");

	lua_createtable(L, t->natts, 8);

	/* stack: attrtypes,table */

	/* actually do the deform */

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

		/*
		 * Fields with substructure that we know about, like composites, might
		 * have been converted to short-varlena format. We need to convert them
		 * back if so, since otherwise lots of stuff breaks. Such values can't
		 * be non-copied "child" datums, but at least they must be small.
		 *
		 * On the other hand, we might encounter a compressed value, and we
		 * have to expand that.
		 *
		 * We intentionally *don't* do this for arrays. We point at the original
		 * value as an opaque blob until we need to deform or explode it, and at
		 * that point we convert it to an expanded object.
		 *
		 * We don't look at the substructure of range types ourselves, but we
		 * do allow calls to functions that will detoast a range if it is a
		 * short varlena. So better to expand it once here than risk doing so
		 * many times elsewhere.
		 */
		for (i = 0; i < t->natts; ++i)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			char typtype = (att->attlen == -1) ? get_typtype(getBaseType(att->atttypid)) : '\0';
			if (!nulls[i]
				&& att->attlen == -1
				&& (att->atttypid == RECORDOID ||
					typtype == TYPTYPE_RANGE ||
					typtype == TYPTYPE_COMPOSITE)
				&& VARATT_IS_EXTENDED(DatumGetPointer(values[i])))
			{
				struct varlena *vl = (struct varlena *) DatumGetPointer(values[i]);
				values[i] = PointerGetDatum(heap_tuple_untoast_attr(vl));
				needsave[i] = true;
			}
			else
				needsave[i] = false;
		}
	}
	PLLUA_CATCH_RETHROW();

	/* stack: attrtypes,table */

	for (i = 0; i < t->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		lua_rawgeti(L, -2, i+1);
		/* stack: attrtypes,table,typeinfo */

		if (att->attisdropped)
			lua_pushboolean(L, 0);
		else if (nulls[i])
			lua_pushboolean(L, 1);		/* can't use the more natural "nil" */
		else
		{
			pllua_typeinfo *newt = pllua_checktypeinfo(L, -1, false);
			pllua_datum *newd = pllua_newdatum(L, -1, values[i]);

			if (newt->typeoid != RECORDOID)
				newd->typmod = att->atttypmod;
			newd->need_gc = false;

			/*
			 * the uservalue of the new datum points to the old one in order to
			 * hold a reference, _regardless_ of whether we actually made a
			 * copy. Otherwise, if we modify the copy we made, the parent tuple
			 * is never informed of that fact, and so might get copied without
			 * our changes.
			 */
			lua_pushvalue(L, nd);
			pllua_datum_reference(L, -2);

			if (needsave[i])
			{
				saveti[i] = newt;
				savedatum[i] = newd;
				anysave = true;
			}
		}

		/* stack: attrtypes,table,typeinfo,value */
		lua_rawseti(L, -3, i+1);
		lua_pop(L, 1);
	}

	if (anysave)
	{
		PLLUA_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
			for (i = 0; i < t->natts; ++i)
			{
				if (needsave[i])
				{
					pllua_datum *newd = savedatum[i];
					void *oldp = DatumGetPointer(newd->value);
					pllua_savedatum(L, newd, saveti[i]);
					/*
					 * We don't normally worry about freeing transient data,
					 * but here it's likely to be worthwhile.
					 */
					pfree(oldp);
				}
			}
			MemoryContextSwitchTo(oldcontext);
		}
		PLLUA_CATCH_RETHROW();
	}

	/* stack: attrtypes,table */

	/* handle oid column specially */
	if (t->hasoid)
	{
		Oid oid = HeapTupleHeaderGetOid(htup);
		lua_pushinteger(L, (lua_Integer) oid);
		lua_setfield(L, -2, "oid");
	}

	/* stack: attrtypes,table */

	lua_pushvalue(L, -1);
	pllua_set_user_field(L, nd, ".deformed");
	lua_remove(L, -2);
}

/*
 * Current tuple's deformed table is on top of the stack.
 */
static void pllua_datum_explode_tuple_inner(lua_State *L, int nd, pllua_datum *d, pllua_typeinfo *t);

static void pllua_datum_explode_tuple_recurse(lua_State *L, pllua_datum *d, pllua_typeinfo *t)
{
	int i;
	int natts = t->natts;

	luaL_checkstack(L, 20, NULL);
	/* need to check pg stack here because we recurse in lua context */
	PLLUA_CHECK_PG_STACK_DEPTH();

	for (i = 1; i <= natts; ++i)
	{
		if (lua_rawgeti(L, -1, i) == LUA_TUSERDATA)
		{
			pllua_typeinfo *et;
			pllua_datum *ed = pllua_toanydatum(L, -1, &et);

			/*
			 * Datums at this level are handled by the caller, our job is to
			 * handle datums of deeper levels.
			 *
			 * We don't handle arrays by explosion (instead using the
			 * expanded-object representation) so no need to consider them
			 * here.
			 *
			 * We need to explode nested tuples even when need_gc is true.
			 * It should be impossible to get here if modified is already
			 * true.
			 */
			Assert(!ed->modified);
			if (et->natts >= 0)
			{
				pllua_datum_deform_tuple(L, -2, ed, et);
				pllua_datum_explode_tuple_inner(L, -3, ed, et);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		lua_pop(L,1);
	}
}

/*
 * Deform (if needed) a datum, and then detach the column values from the
 * original record, which is then freed. (This is used when we want to modify
 * the datum.)
 *
 * Expects the result of deform on the stack top and leaves it there.
 */
static void pllua_datum_explode_tuple_inner(lua_State *L, int nd, pllua_datum *d, pllua_typeinfo *t)
{
	int i;
	int natts = t->natts;  /* must include dropped cols */

	if (d->value == (Datum)0)
		return;

	nd = lua_absindex(L, nd);

	ASSERT_LUA_CONTEXT;

	/*
	 * If a composite value is nested inside another, we might have already
	 * deformed the inner value, in which case it has its own set of child
	 * datums that depend on the outer tuple's storage. So recursively explode
	 * all nested values before modifying anything. (Separate loop here to
	 * handle the fact that we want to recurse from lua context, not pg
	 * context.)
	 *
	 * (We can't just un-deform the child values, because
	 * something might be holding references to their values.)
	 */
	pllua_datum_explode_tuple_recurse(L, d, t);

	/*
	 * If this errors partway through, we may have saved some values but not
	 * others, so cope.
	 */
	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));

		for (i = 1; i <= natts; ++i)
		{
			if (lua_rawgeti(L, -1, i) == LUA_TUSERDATA)
			{
				pllua_typeinfo *et;
				pllua_datum *ed = pllua_toanydatum(L, -1, &et);

				if (!ed->need_gc && !ed->modified)
				{
					/*
					 * nested child datums must have already been handled in
					 * recursion above. Can't do the deref here since we're in
					 * pg context; do that below.
					 */
					pllua_savedatum(L, ed, et);
				}
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}

		if (d->need_gc)
		{
			void *oldval = DatumGetPointer(d->value);
			d->modified = true;
			d->need_gc = false;
			d->value = (Datum)0;
			pfree(oldval);
		}
		else
		{
			d->modified = true;
			d->value = (Datum)0;
		}
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	for (i = 1; i <= natts; ++i)
	{
		if (lua_rawgeti(L, -1, i) == LUA_TUSERDATA)
		{
			lua_pushnil(L);
			pllua_datum_reference(L, -2);
		}
		lua_pop(L, 1);
	}

	lua_pushnil(L);
	pllua_datum_reference(L, nd);
}

/*
 * Deform (if needed) a datum, and then detach the column values from the
 * original record, which is then freed. (This is used when we want to modify
 * the datum.)
 *
 * The tricky part of this is that if we're a dependent child tuple, we need to
 * go back and explode our parent, and it's parent, and so on, and recurse into
 * all children. So this function is split into three:
 * pllua_datum_explode_tuple proper handles finding the ultimate parent,
 * pllua_datum_explode_tuple_inner handles exploding one tuple, and
 * pllua_datum_explode_tuple_recurse handles exploding the children of one tuple
 *
 * Leaves the result of deform on the stack.
 */
static void pllua_datum_explode_tuple(lua_State *L, int nd, pllua_datum *d, pllua_typeinfo *t)
{
	if (d->value == (Datum)0)
		return;

	nd = lua_absindex(L, nd);

	ASSERT_LUA_CONTEXT;

	pllua_datum_deform_tuple(L, nd, d, t);

	lua_pushvalue(L, nd);
	for (;;)
	{
		pllua_get_user_field(L, -1, ".datumref");
		if (lua_isnil(L, -1))
			break;
		lua_remove(L, -2);
	}
	lua_pop(L, 1);
	/* stack top is now the ultimate parent */

	/*
	 * If a composite value is nested inside another, we might have already
	 * deformed the inner value, in which case it has its own set of child
	 * datums that depend on the outer tuple's storage. So recursively explode
	 * all nested values before modifying anything. (Separate loop here to
	 * handle the fact that we want to recurse from lua context, not pg
	 * context.)
	 *
	 * Likewise, we might be the child value of a parent, which will also need
	 * exploding in order to inherit any of our changes.
	 *
	 * (We can't just un-deform the child values, because something might be
	 * holding references to their values.)
	 */
	if (!lua_rawequal(L, -1, nd))
	{
		pllua_typeinfo *parent_t;
		pllua_datum *parent_d = pllua_toanydatum(L, -1, &parent_t);
		pllua_datum_deform_tuple(L, -2, parent_d, parent_t);
		pllua_datum_explode_tuple_inner(L, -3, parent_d, parent_t);
		lua_pop(L, 3);  /* pop deform, typeinfo, parent */
	}
	else
	{
		lua_pop(L, 1); /* pop parent, deform is on stack top */
		pllua_datum_explode_tuple_inner(L, nd, d, t);
	}

	/* our own deform is now on stack top */
}


static bool pllua_datum_column(lua_State *L, int attno, bool skip_dropped)
{
	switch (lua_geti(L, -1, attno))
	{
		case LUA_TUSERDATA:
			{
				pllua_typeinfo *et;
				pllua_datum *ed = pllua_checkanydatum(L, -1, &et);
				if (pllua_value_from_datum(L, ed->value, et->basetype) == LUA_TNONE &&
					pllua_datum_transform_fromsql(L, ed->value, -1, et) == LUA_TNONE)
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


/*
 * __tostring(d)  returns the string representation of an unregistered row.
 *
 * We get the typeinfo object from the closure.
 */
static int pllua_datum_row_tostring(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), true);
	int i;
	int natts = t->natts;
	luaL_Buffer b;
	const char *ptr;
	const char *str;
	size_t len;
	bool needquote;

	lua_settop(L, 1);
	pllua_datum_deform_tuple(L, 1, d, t);  /* deform is at index 2 */

	luaL_buffinit(L, &b);
	luaL_addchar(&b, '(');

	for (i = 1; i <= natts; ++i)
	{
		/*
		 * we don't use pllua_datum_column here since we want the raw datum,
		 * otherwise bytea etc. won't come out right.
		 */
		switch (lua_geti(L, 2, i))
		{
			default:
			case LUA_TNIL:
				return luaL_error(L, "unexpected type in datum cache");
			case LUA_TBOOLEAN:
				/* false is a dropped col; true is a present but null col */
				if (!lua_toboolean(L, -1))
				{
					lua_pop(L, 1);
					continue;
				}
				lua_pop(L, 1);
				if (i > 1)
					lua_pushliteral(L, ",");
				else
					lua_pushliteral(L, "");
				break;
			case LUA_TUSERDATA:
				str = luaL_tolstring(L, -1, &len);
				lua_remove(L, -2);
				needquote = false;
				for (ptr = str; *ptr; ++ptr)
				{
					char ch = *ptr;
					if (ch == '"' || ch == '\\' ||
						ch == '(' || ch == ')' || ch == ',' ||
						isspace((unsigned char) ch))
					{
						needquote = true;
						break;
					}
				}
				if (i > 1 || needquote)
				{
					luaL_Buffer nb;
					luaL_buffinit(L, &nb);
					if (i > 1)
						luaL_addchar(&nb, ',');
					if (!needquote)
						luaL_addlstring(&nb, str, len);
					else
					{
						luaL_addchar(&nb, '"');
						for (ptr = str; *ptr; ++ptr)
						{
							char ch = *ptr;
							if (ch == '\\' || ch == '"')
								luaL_addchar(&nb, ch);
							luaL_addchar(&nb, ch);
						}
						luaL_addchar(&nb, '"');
					}
					luaL_pushresult(&nb);
					lua_remove(L, -2);
				}
				break;
		}
		luaL_addvalue(&b);
	}
	luaL_addchar(&b, ')');
	luaL_pushresult(&b);
	return 1;
}



static void pllua_datum_getattrs(lua_State *L, int nd)
{
	if (luaL_getmetafield(L, nd, "attrs") != LUA_TTABLE)
		luaL_error(L, "missing attrs table");
}

/*
 * __index(self,key)
 */
static int pllua_datum_row_index(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	lua_Integer attno;

	if (!d)
		luaL_error(L, "pllua_datum_index: not a datum object");

	if (t->natts < 0)
		luaL_error(L, "datum is not a row type");

	switch (lua_type(L, 2))
	{
		default:
			lua_pushnil(L);
			return 1;

		case LUA_TSTRING:
			pllua_datum_getattrs(L, 1);
			/* stack: attrs{ attname = attno } */
			lua_pushvalue(L, 2);
			if (lua_gettable(L, -2) != LUA_TNUMBER)
				luaL_error(L, "datum has no column \"%s\"", lua_tostring(L, 2));
			/*FALLTHROUGH*/

		case LUA_TNUMBER:		/* column number */
			attno = lua_tointeger(L, -1);
			if (((attno != ObjectIdAttributeNumber || !t->hasoid)
				 && (attno < 1 || attno > t->natts))
				|| TupleDescAttr(t->tupdesc, attno-1)->attisdropped)
				luaL_error(L, "datum has no column number %d", attno);
			pllua_datum_deform_tuple(L, 1, d, t);
			if (attno == ObjectIdAttributeNumber)
				lua_getfield(L, -1, "oid");
			else
				pllua_datum_column(L, attno, false);
			return 1;
	}
}

/*
 * __newindex(self,key,val)   self[key] = val
 */
static int pllua_datum_row_newindex(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	lua_Integer attno;

	if (!d)
		luaL_error(L, "pllua_datum_newindex: not a datum object");

	if (t->natts < 0)
		luaL_error(L, "datum is not a row type");

	switch (lua_type(L, 2))
	{
		default:
			luaL_error(L, "invalid type for key field");
			return 0;

		case LUA_TSTRING:
			pllua_datum_getattrs(L, 1);
			/* stack: attrs{ attname = attno } */
			lua_pushvalue(L, 2);
			if (lua_gettable(L, -2) != LUA_TNUMBER)
				luaL_error(L, "datum has no column \"%s\"", lua_tostring(L, 2));
			lua_replace(L, 2);
			/*FALLTHROUGH*/

		case LUA_TNUMBER:		/* column number */
			attno = lua_tointeger(L, 2);
			if (((attno != ObjectIdAttributeNumber || !t->hasoid)
				 && (attno < 1 || attno > t->natts))
				|| TupleDescAttr(t->tupdesc, attno-1)->attisdropped)
				luaL_error(L, "datum has no column number %d", attno);
			pllua_datum_explode_tuple(L, 1, d, t);
			if (attno == ObjectIdAttributeNumber)
			{
				int isint = 0;
				lua_Integer newoid = lua_tointegerx(L, 3, &isint);
				if (!isint || (newoid != (lua_Integer)(Oid)newoid))
					luaL_error(L, "invalid oid value");
				lua_pushinteger(L, newoid);
				lua_setfield(L, -2, "oid");
			}
			else
			{
				lua_pushcfunction(L, pllua_typeinfo_lookup);
				lua_pushinteger(L, TupleDescAttr(t->tupdesc, attno-1)->atttypid);
				lua_pushinteger(L, TupleDescAttr(t->tupdesc, attno-1)->atttypmod);
				lua_call(L, 2, 1);
				lua_pushvalue(L, 3);
				lua_call(L, 1, 1);
				lua_seti(L, -2, attno);
			}
			return 0;
	}
}

/*
 * Not exposed to the user directly, only as a closure over its index var
 *
 * upvalues:  typeinfo, datum, index, deform, attrs
 */
static int pllua_datum_row_next(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
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

static int pllua_datum_row_pairs(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);

	if (t->natts < 0)
		luaL_error(L, "pairs(): datum is not a rowtype");

	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 0);
	pllua_datum_deform_tuple(L, 1, d, t);
	pllua_datum_getattrs(L, 1);
	lua_pushcclosure(L, pllua_datum_row_next, 5);
	lua_pushnil(L);
	lua_pushnil(L);
	return 3;
}

static int pllua_datum_row_len(lua_State *L)
{
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);

	pllua_checkdatum(L, 1, lua_upvalueindex(1));

	if (t->natts < 0)
		luaL_error(L, "attempt to get length of a non-rowtype datum");

	/* length is the arity, not natts, because we skip dropped columns */
	lua_pushinteger(L, t->arity);
	return 1;
}

/*
 * __call(row)
 * __call(row,func)
 * __call(row,nullvalue)
 * __call(row,configtable)
 *
 * mapfunc is function(k,v,n,d)
 *
 *
 * Apply a mapping to the row and return the result as a Lua table.
 */
static int
pllua_datum_row_map(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	int funcidx = 0;
	int nullvalue = 0;
	bool noresult = false;
	lua_Integer attno = 0;

	lua_settop(L, 2);

	if (t->natts < 0)
		luaL_error(L, "datum is not a row type");

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
			lua_getfield(L, 2, "null");
			nullvalue = lua_absindex(L, -1);
			break;
		case LUA_TFUNCTION:
			funcidx = 2;
			break;
		case LUA_TNIL:
			break;
		default:
			nullvalue = 2;
			break;
	}

	if (!noresult)
		lua_newtable(L);
	pllua_datum_getattrs(L, 1);
	pllua_datum_deform_tuple(L, 1, d, t);
	/* stack: [table] attrs deform */

	for (++attno; attno <= t->natts; ++attno)
	{
		if (pllua_datum_column(L, attno, true))
		{
			/* stack: [table] attrs deform value */
			lua_geti(L, -3, attno);
			lua_insert(L, -2);
			/* stack: [table] attrs deform key value */
			if (nullvalue && lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				lua_pushvalue(L, nullvalue);
			}
			if (funcidx)
			{
				lua_pushvalue(L, funcidx);
				lua_insert(L, -2);
				/* ... deform key func value */
				lua_pushvalue(L, -3);
				/* ... deform key func value key */
				lua_insert(L, -2);
				/* ... deform key func key value */
				lua_pushinteger(L, attno);
				lua_pushvalue(L, 1);
				/* ... deform key func key value attno datum */
				lua_call(L, 4, 1);
				/* ... [table] attrs deform key newvalue */
			}
			if (!noresult)
				lua_settable(L, -5);
			else
				lua_pop(L, 2);
		}
	}
	lua_pop(L, 2);
	return noresult ? 0 : 1;
}


struct idxlist
{
	int ndim;  /* ndim of parent array */
	int cur_dim;  /* dim 1..ndim of max currently specified index */
	int idx[MAXDIM];  /* specified indexes [dim-1] */
};

static struct idxlist *
pllua_datum_array_make_idxlist(lua_State *L, int nd,
							   struct idxlist *idxlist)
{
	struct idxlist *nlist = pllua_newobject(L, PLLUA_IDXLIST_OBJECT, sizeof(struct idxlist), true);

	*nlist = *idxlist;

	lua_pushvalue(L, nd);
	pllua_set_user_field(L, -2, "datum");

	return nlist;
}

static int pllua_datum_idxlist_index(lua_State *L)
{
	struct idxlist *idxlist = pllua_toobject(L, 1, PLLUA_IDXLIST_OBJECT);
	int idx = luaL_checkinteger(L, 2);

	pllua_get_user_field(L, 1, "datum");
	idxlist = pllua_datum_array_make_idxlist(L, lua_absindex(L, -1), idxlist);
	idxlist->idx[idxlist->cur_dim++] = idx;

	if (idxlist->cur_dim >= idxlist->ndim)
		lua_gettable(L, -2);

	return 1;
}

static int pllua_datum_idxlist_newindex(lua_State *L)
{
	struct idxlist *idxlist = pllua_toobject(L, 1, PLLUA_IDXLIST_OBJECT);
	int idx = luaL_checkinteger(L, 2);

	luaL_checkany(L, 3);

	pllua_get_user_field(L, 1, "datum");
	idxlist = pllua_datum_array_make_idxlist(L, lua_absindex(L, -1), idxlist);
	idxlist->idx[idxlist->cur_dim++] = idx;

	if (idxlist->cur_dim != idxlist->ndim)
		luaL_error(L, "incorrect number of dimensions in array assignment (expected %d got %d)",
				   idxlist->ndim, idxlist->cur_dim);

	lua_pushvalue(L, 3);
	lua_settable(L, -2);
	return 0;
}

static int pllua_datum_idxlist_len(lua_State *L)
{
	pllua_checkobject(L, 1, PLLUA_IDXLIST_OBJECT);

	pllua_get_user_field(L, 1, "datum");
	if (luaL_getmetafield(L, -1, "__len") == LUA_TNIL)
		luaL_error(L, "array len error");
	lua_pushvalue(L, -2);
	lua_pushvalue(L, 1);
	lua_call(L, 2, 1);
	return 1;
}

static int pllua_datum_array_next(lua_State *L);

static ExpandedArrayHeader *
pllua_datum_array_value(lua_State *L, pllua_datum *d, pllua_typeinfo *t)
{
	/* Switch to expanded representation if we haven't already. */
	if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d->value)))
	{
		PLLUA_TRY();
		{
			d->value = expand_array(d->value, pllua_get_memory_cxt(L), &t->array_meta);
			pllua_record_gc_debt(L, toast_datum_size(d->value));
			d->need_gc = true;
		}
		PLLUA_CATCH_RETHROW();
	}

	return (ExpandedArrayHeader *) DatumGetEOHP(d->value);
}

static int pllua_datum_idxlist_pairs(lua_State *L)
{
	struct idxlist *idxlist = pllua_toobject(L, 1, PLLUA_IDXLIST_OBJECT);
	pllua_datum *d;
	pllua_typeinfo *t;
	ExpandedArrayHeader *arr;

	pllua_get_user_field(L, 1, "datum");

	d = pllua_checkanydatum(L, -1, &t);
	/* stack: ... datum typeinfo */

	arr = pllua_datum_array_value(L, d, t);

	lua_pushvalue(L, -1);
	lua_pushvalue(L, 1);
	lua_pushinteger(L, arr->lbound[idxlist->cur_dim]);
	lua_pushinteger(L, arr->lbound[idxlist->cur_dim] + arr->dims[idxlist->cur_dim]);
	lua_pushcclosure(L, pllua_datum_array_next, 4);
	lua_pushnil(L);
	lua_pushnil(L);
	return 3;
}

static struct luaL_Reg idxlist_mt[] = {
	{ "__index", pllua_datum_idxlist_index },
	{ "__newindex", pllua_datum_idxlist_newindex },
	{ "__len", pllua_datum_idxlist_len },
	{ "__pairs", pllua_datum_idxlist_pairs },
	{ "__ipairs", pllua_datum_idxlist_pairs },
	{ NULL, NULL }
};

int
pllua_datum_single(lua_State *L, Datum res, bool isnull, int nt, pllua_typeinfo *t)
{
	pllua_datum *newd;

	nt = lua_absindex(L, nt);

	if (isnull)
		lua_pushnil(L);
	else if (pllua_value_from_datum(L, res, t->basetype) == LUA_TNONE &&
			 pllua_datum_transform_fromsql(L, res, nt, t) == LUA_TNONE)
	{
		newd = pllua_newdatum(L, nt, res);
		pllua_save_one_datum(L, newd, t);
	}
	return 1;
}

/*
 * __index(self,key)
 */
static int pllua_datum_array_index(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	pllua_typeinfo *et = pllua_totypeinfo(L, lua_upvalueindex(2));
	struct idxlist d_idxlist;
	struct idxlist *idxlist = NULL;
	ExpandedArrayHeader *arr;
	bool isnull = false;
	const char *str = NULL;
	volatile Datum res;

	if (!t->is_array)
		luaL_error(L, "datum is not an array type");

	if (lua_isinteger(L, 2))
	{
		d_idxlist.idx[0] = lua_tointeger(L, 2);
		d_idxlist.cur_dim = 1;
	}
	else if ((str = lua_tostring(L, 2)) &&
			 luaL_getmetafield(L, 1, "__methods") != LUA_TNIL)
	{
		lua_getfield(L, -1, str);
		return 1;
	}
	else if (!(idxlist = pllua_toobject(L, 2, PLLUA_IDXLIST_OBJECT)))
	{
		luaL_argerror(L, 2, NULL);
	}

	arr = pllua_datum_array_value(L, d, t);

	if (idxlist)
	{
		pllua_get_user_field(L, 2, "datum");

		if (idxlist->ndim != arr->ndims ||
			idxlist->cur_dim != arr->ndims ||
			!lua_rawequal(L, -1, 1))
			luaL_argerror(L, 2, "wrong idxlist");

		lua_pop(L, 1);
	}
	else if (arr->ndims > 1)
	{
		d_idxlist.ndim = arr->ndims;
		pllua_datum_array_make_idxlist(L, 1, &d_idxlist);
		return 1;
	}
	else
		idxlist = &d_idxlist;

	PLLUA_TRY();
	{
		res = array_get_element(d->value,
								idxlist->cur_dim, idxlist->idx,
								t->typlen, t->elemtyplen, t->elemtypbyval, t->elemtypalign,
								&isnull);
	}
	PLLUA_CATCH_RETHROW();

	pllua_datum_single(L, res, isnull, lua_upvalueindex(2), et);

	return 1;
}

/*
 * __newindex(self,key,val)   self[key] = val
 */
static int pllua_datum_array_newindex(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	struct idxlist d_idxlist;
	struct idxlist *idxlist = NULL;
	ExpandedArrayHeader *arr;
	pllua_datum *nd;

	if (!t->is_array)
		luaL_error(L, "datum is not an array type");

	if (lua_isinteger(L, 2))
	{
		d_idxlist.idx[0] = lua_tointeger(L, 2);
		d_idxlist.cur_dim = 1;
		idxlist = &d_idxlist;
	}
	else
	{
		idxlist = pllua_toobject(L, 2, PLLUA_IDXLIST_OBJECT);
		if (!idxlist)
			luaL_argerror(L, 2, "integer");
	}

	/*
	 * If we came from a row object's deform, then explode the source row;
	 * otherwise, it would not pick up our changes and the result of
	 * row.arraycol[i] = n  would not be reflected in "row"
	 */
	if (pllua_get_user_field(L, 1, ".datumref") != LUA_TNIL)
	{
		pllua_typeinfo *parent_t;
		pllua_datum *parent_d = pllua_checkanydatum(L, -1, &parent_t);
		pllua_datum_explode_tuple(L, -2, parent_d, parent_t);
		lua_pop(L, 3);
	}
	else
		lua_pop(L, 1);

	arr = pllua_datum_array_value(L, d, t);

	if (idxlist->cur_dim < arr->ndims)
		luaL_error(L, "not enough subscripts for array");
	else if (idxlist->cur_dim > arr->ndims && arr->ndims > 0)
		luaL_error(L, "too many subscripts for array");

	lua_pushvalue(L, lua_upvalueindex(2));
	lua_pushvalue(L, 3);
	lua_call(L, 1, 1);
	if (!lua_isnil(L, -1))
		nd = pllua_todatum(L, -1, lua_upvalueindex(2));
	else
		nd = NULL;

	PLLUA_TRY();
	{
		bool isnull = (nd == NULL);
		Datum val = nd ? nd->value : (Datum)0;
		Datum res PG_USED_FOR_ASSERTS_ONLY;

		res = array_set_element(d->value,
								idxlist->cur_dim, idxlist->idx,
								val, isnull,
								t->typlen, t->elemtyplen, t->elemtypbyval, t->elemtypalign);
		Assert(res == d->value);
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

/*
 * __len(self[,idxlist])
 */
static int pllua_datum_array_len(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	struct idxlist *idxlist = pllua_toobject(L, 2, PLLUA_IDXLIST_OBJECT);
	int reqdim = (idxlist) ? idxlist->cur_dim + 1 : 1;
	ExpandedArrayHeader *arr;
	int res;

	if (!t->is_array)
		luaL_error(L, "datum is not an array type");

	if (!idxlist && !lua_isnoneornil(L, 2) && !lua_rawequal(L, 1, 2))
		luaL_argerror(L, 2, "incorrect type");

	arr = pllua_datum_array_value(L, d, t);

	if (arr->ndims < 1 || reqdim > arr->ndims)
		res = 0;
	else
		res = arr->lbound[reqdim - 1] + arr->dims[reqdim - 1] - 1;
	lua_pushinteger(L, res);
	return 1;
}


/*
 * Not exposed to the user directly, only as a closure over its index var
 *
 * upvalues:  typeinfo, datum or idxlist, index, ubound
 */
static int pllua_datum_array_next(lua_State *L)
{
	int idx = lua_tointeger(L, lua_upvalueindex(3));
	int ubound = lua_tointeger(L, lua_upvalueindex(4));

	if (idx >= ubound)
		return 0;

	lua_pushinteger(L, idx+1);
	lua_replace(L, lua_upvalueindex(3));

	lua_pushinteger(L, idx);
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_geti(L, -1, idx);
	lua_remove(L, -2);

	return 2;
}

static int pllua_datum_array_pairs(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	ExpandedArrayHeader *arr;

	if (!t->is_array)
		luaL_error(L, "datum is not an array type");

	arr = pllua_datum_array_value(L, d, t);

	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 1);
	if (arr->ndims < 1)
	{
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 0);
	}
	else
	{
		lua_pushinteger(L, arr->lbound[0]);
		lua_pushinteger(L, arr->lbound[0] + arr->dims[0]);
	}
	lua_pushcclosure(L, pllua_datum_array_next, 4);
	lua_pushnil(L);
	lua_pushnil(L);
	return 3;
}

/*
 * __call(array)
 * __call(array,func)
 * __call(array,nullval)
 * __call(array,configtable)
 *
 * configtable:
 *   mapfunc = function(e,a,i,j,k,...)
 *   noresult = boolean, if true the result of map is discarded
 *   nullvalue = any
 *
 * map(array,func)
 *
 * Calls func on every element of array and returns the results as a Lua table
 * (NOT an array).
 *
 * mapnull(array,nullval)
 * table(array)
 *
 * Converts array to a Lua table optionally replacing all null values by
 * "nullval"
 *
 * These are actually all the same function, the presence and argument type of
 * arg 2 determines which.
 */
static int
pllua_datum_array_map(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	pllua_typeinfo *et = pllua_totypeinfo(L, lua_upvalueindex(2));
	struct idxlist idxlist;
	ExpandedArrayHeader *arr;
	array_iter iter;
	int index;
	int nstack;
	int ndim;
	int nelems;
	int i;
	int funcidx = 0;
	int nullvalue = 0;
	bool noresult = false;

	lua_settop(L, 2);

	if (!t->is_array)
		luaL_error(L, "datum is not an array type");

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
			lua_getfield(L, 2, "null");
			nullvalue = lua_absindex(L, -1);
			break;
		case LUA_TFUNCTION:
			funcidx = 2;
			break;
		case LUA_TNIL:
			break;
		default:
			nullvalue = 2;
			break;
	}

	arr = pllua_datum_array_value(L, d, t);
	ndim = arr->ndims;
	nelems = ArrayGetNItems(ndim, arr->dims);

	if (ndim < 1 || nelems < 1)
	{
		if (!noresult)
			lua_newtable(L);
		return noresult ? 0 : 1;
	}

	/*
	 * We create a stack of tables per dimension:
	 *
	 * t1 t2 t3 ...
	 *
	 * At each step, we append the current value to the top table on the stack.
	 * When we reach the end of a dimension, the top table is appended to the
	 * next one down, as needed, and then new tables created until we get back
	 * to the right depth.
	 */

	array_iter_setup(&iter, (AnyArrayType *) arr);

	for (nstack = 0, index = 0; index < nelems; ++index)
	{
		Datum val;
		bool isnull = false;

		/* stack up new tables to the required depth */
		while (nstack < ndim)
		{
			if (!noresult)
				lua_createtable(L, arr->dims[nstack], 0);
			idxlist.idx[nstack] = 0;  /* lbound added later */
			++nstack;
		}

		val = array_iter_next(&iter, &isnull, index,
							  et->typlen, et->typbyval, et->typalign);

		pllua_datum_single(L, val, isnull, lua_upvalueindex(2), et);

		if (nullvalue && lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			lua_pushvalue(L, nullvalue);
		}

		if (funcidx)
		{
			lua_pushvalue(L, funcidx);
			lua_insert(L, -2);
			lua_pushvalue(L, 1);
			for (i = 0; i < ndim; ++i)
				lua_pushinteger(L, idxlist.idx[i] + arr->lbound[i]);
			lua_call(L, 2+ndim, 1);
		}

		if (!noresult)
			lua_seti(L, -2, idxlist.idx[nstack-1] + arr->lbound[nstack-1]);

		for (i = nstack - 1; i >= 0; --i)
		{
			if ((idxlist.idx[i] = (idxlist.idx[i] + 1) % arr->dims[i]))
				break;
			else if (i > 0)
			{
				--nstack;
				if (!noresult)
					lua_seti(L, -2, idxlist.idx[nstack-1] + arr->lbound[nstack-1]);
			}
		}
	}

	return noresult ? 0 : 1;
}

/*
 * deform a range value and cache the details
 */
static void
pllua_datum_range_deform(lua_State *L, int nd, int nte, pllua_datum *d, pllua_typeinfo *t, pllua_typeinfo *et)
{
	RangeType  *r1;
	TypeCacheEntry *typcache;
	RangeBound	lower;
	RangeBound	upper;
	bool		empty;
	pllua_datum *ld = NULL;
	pllua_datum *ud = NULL;

	nd = lua_absindex(L, nd);
	nte = lua_absindex(L, nte);

	PLLUA_TRY();
	{
		r1 = DatumGetRangeTypeP(d->value);
		typcache = lookup_type_cache(t->typeoid, TYPECACHE_RANGE_INFO);
		if (typcache->rngelemtype == NULL)
			elog(ERROR, "type %u is not a range type", t->typeoid);
		range_deserialize(typcache, r1, &lower, &upper, &empty);
	}
	PLLUA_CATCH_RETHROW();

	lua_createtable(L, 0, 8);
	lua_pushboolean(L, empty);
	lua_setfield(L, -2, "isempty");

	if (empty)
	{
		lua_pushlightuserdata(L, (void*)0);
		lua_setfield(L, -2, "lower");
		lua_pushlightuserdata(L, (void*)0);
		lua_setfield(L, -2, "upper");
		lua_pushboolean(L, false);
		lua_setfield(L, -2, "lower_inc");
		lua_pushboolean(L, false);
		lua_setfield(L, -2, "upper_inc");
		lua_pushboolean(L, false);
		lua_setfield(L, -2, "lower_inf");
		lua_pushboolean(L, false);
		lua_setfield(L, -2, "upper_inf");
		return;
	}

	lua_pushboolean(L, lower.inclusive);
	lua_setfield(L, -2, "lower_inc");
	lua_pushboolean(L, lower.infinite);
	lua_setfield(L, -2, "lower_inf");
	if (lower.infinite)
		lua_pushlightuserdata(L, (void*)0);
	else
		ld = pllua_newdatum(L, nte, lower.val);

	lua_pushboolean(L, upper.inclusive);
	lua_setfield(L, -3, "upper_inc");
	lua_pushboolean(L, upper.infinite);
	lua_setfield(L, -3, "upper_inf");
	if (upper.infinite)
		lua_pushlightuserdata(L, (void*)0);
	else
		ud = pllua_newdatum(L, nte, upper.val);

	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		if (ld)
			pllua_savedatum(L, ld, et);
		if (ud)
			pllua_savedatum(L, ud, et);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	lua_setfield(L, -3, "upper");
	lua_setfield(L, -2, "lower");
	lua_pushvalue(L, -1);
	pllua_set_user_field(L, nd, ".deformed");
}
/*
 * __index(range,idx)
 *
 * Provide virtual columns .lower, .upper, .isempty, etc.
 *
 * Upvalue 1 is the typeinfo, 2 the element typeinfo
 */
static int
pllua_datum_range_index(lua_State *L)
{
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	pllua_typeinfo *et = pllua_totypeinfo(L, lua_upvalueindex(2));
	const char *str = luaL_checkstring(L, 2);

	if (pllua_get_user_field(L, 1, ".deformed") != LUA_TTABLE)
	{
		lua_pop(L, 1);
		pllua_datum_range_deform(L, 1, lua_upvalueindex(2), d, t, et);
	}
	switch (lua_getfield(L, -1, str))
	{
		case LUA_TNIL:
			return 1;   /* no such field */
		case LUA_TLIGHTUSERDATA:
			lua_pushnil(L);
			return 1;   /* dummy null */
		default:
			return 1;
	}
}

static int
pllua_datum_noindex(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	if (t->is_anonymous_record)
		return luaL_error(L, "cannot access fields from a record of unknown structure");
	else
		return luaL_error(L, "datum is not an indexable type");
}


static struct luaL_Reg datumobj_base_mt[] = {
	/* __gc entry is handled separately */
	{ "__tostring", pllua_datum_tostring },
	{ "__index", pllua_datum_noindex },
	{ "_tobinary", pllua_datum_tobinary },
	{ NULL, NULL }
};

static struct luaL_Reg datumobj_unreg_row_mt[] = {
	{ "__tostring", pllua_datum_row_tostring },
	{ NULL, NULL }
};

static struct luaL_Reg datumobj_row_mt[] = {
	{ "__len", pllua_datum_row_len },
	{ "__index", pllua_datum_row_index },
	{ "__newindex", pllua_datum_row_newindex },
	{ "__pairs", pllua_datum_row_pairs },
	{ "__call", pllua_datum_row_map },
	{ NULL, NULL }
};

static struct luaL_Reg datumobj_range_mt[] = {
	{ "__index", pllua_datum_range_index },
	{ NULL, NULL }
};

static struct luaL_Reg datumobj_array_methods[] = {
	{ "table", pllua_datum_array_map },
	{ "map", pllua_datum_array_map },
	{ "mapnull", pllua_datum_array_map },
	{ NULL, NULL }
};

static struct luaL_Reg datumobj_array_mt[] = {
	{ "__len", pllua_datum_array_len },
	{ "__pairs", pllua_datum_array_pairs },
	{ "__ipairs", pllua_datum_array_pairs },
	{ "__index", pllua_datum_array_index },
	{ "__newindex", pllua_datum_array_newindex },
	{ "__call", pllua_datum_array_map },
	{ NULL, NULL }
};


/*
 * This entry point allows constructing a typeinfo for an anonymous tupdesc, if
 * that turns out to be useful.
 *
 * If oid==RECORDOID and typmod==-1 and tupdesc==NULL then we need to construct
 * a typeinfo that works for all record types but does not allow them to be
 * indexed into.
 *
 * This returns NULL (with an empty typeinfo on stack) without error if the
 * type doesn't exist; caller beware.
 */
pllua_typeinfo *pllua_newtypeinfo_raw(lua_State *L, Oid oid, int32 typmod, TupleDesc in_tupdesc)
{
	void **p = pllua_newrefobject(L, PLLUA_TYPEINFO_OBJECT, NULL, true);
	pllua_typeinfo *t = NULL;
	pllua_typeinfo *volatile nt;
	Oid langoid = InvalidOid;

	ASSERT_LUA_CONTEXT;

	if (oid != RECORDOID)
	{
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_LANG_OID);
		langoid = (Oid) lua_tointeger(L, -1);
		lua_pop(L, 1);
	}

	PLLUA_TRY();
	{
		MemoryContext mcxt;
		MemoryContext oldcontext;
		Oid basetype;
		int32 basetypmod = -1;		/* XXX revisit */
		Oid elemtype;
		char typtype;
		TupleDesc tupdesc = NULL;

		/*
		 * Break out the RECORDOID case since we can skip a lot of pointless
		 * cache lookups for what needs to be a reasonably fast path (since
		 * we're going to do it for every SPI result set).
		 */
		if (oid == RECORDOID)
		{
			if (typmod >= 0)
			{
				tupdesc = lookup_rowtype_tupdesc_noerror(oid, typmod, true);
				if (!tupdesc)
					goto out;
				/* note: in this case we still hold a pin on tupdesc, see below */
			}
			else
				tupdesc = in_tupdesc;

			basetypmod = typmod;
			basetype = RECORDOID;
			elemtype = InvalidOid;
			typtype = TYPTYPE_PSEUDO;
		}
		else
		{
			if (!SearchSysCacheExists(TYPEOID, ObjectIdGetDatum(oid), 0, 0, 0))
				goto out;

			basetype = getBaseTypeAndTypmod(oid, &basetypmod);
			elemtype = get_element_type(basetype);
			typtype = get_typtype(basetype);
		}

		mcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "pllua type object",
									 ALLOCSET_SMALL_SIZES);
		oldcontext = MemoryContextSwitchTo(mcxt);

		t = palloc0(sizeof(pllua_typeinfo));
		t->mcxt = mcxt;

		t->typeoid = oid;
		t->typmod = typmod;
		t->tupdesc = NULL;
		t->arity = 1;
		t->natts = -1;
		t->hasoid = false;
		t->revalidate = false;
		t->obsolete = false;
		t->modified = false;
		t->reloid = InvalidOid;
		t->basetype = basetype;
		t->basetypmod = basetypmod;
		t->array_meta.element_type = InvalidOid;
		t->coerce_typmod = false;
		t->coerce_typmod_element = false;
		t->typmod_funcid = InvalidOid;
		t->elemtype = elemtype;
		t->rangetype = InvalidOid;
		t->is_enum = (typtype == TYPTYPE_ENUM);
		t->is_anonymous_record = false;
		t->nested_unknowns = false;
		t->nested_composites = false;

		/*
		 * Must look at the base type for typmod coercions
		 */
		if (basetype != RECORDOID)
		{
			switch (find_typmod_coercion_function(basetype, &t->typmod_funcid))
			{
				default:
				case COERCION_PATH_NONE:
					break;
				case COERCION_PATH_ARRAYCOERCE:
					t->coerce_typmod_element = true;
					/*FALLTHROUGH*/
				case COERCION_PATH_FUNC:
					t->coerce_typmod = true;
					break;
			}
		}

		if (oid == RECORDOID && typmod >= 0)
		{
			/* we looked up the tupdesc above but didn't copy or release it */
			t->tupdesc = CreateTupleDescCopy(tupdesc);
			ReleaseTupleDesc(tupdesc);
			tupdesc = t->tupdesc;
			t->natts = tupdesc->natts;
			t->hasoid = tupdesc->tdhasoid;
		}
		else if (oid == RECORDOID && typmod == -1 && tupdesc)
		{
			/* input tupdesc is of uncertain lifetime, so we'd better copy it */
			t->tupdesc = CreateTupleDescCopy(tupdesc);
			t->natts = tupdesc->natts;
			t->hasoid = tupdesc->tdhasoid;
		}
		else if (oid == RECORDOID && typmod == -1)
		{
			t->is_anonymous_record = true;
		}
		else if (typtype == TYPTYPE_COMPOSITE &&
				 (tupdesc = lookup_rowtype_tupdesc_noerror(t->basetype, typmod, true)))
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
			{
				Form_pg_attribute att = TupleDescAttr(tupdesc, i);
				if (att->attisdropped)
					continue;
				++arity;
				/* but see below re. propagation of nested_unknowns */
				if (att->atttypid == RECORDOID && att->atttypmod < 0)
					t->nested_unknowns = true;
			}
			t->arity = arity;
		}

		/*
		 * We intentionally don't look through domains here, so we get
		 * domain_in etc. for a domain type.
		 */
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

		if (OidIsValid(elemtype))
		{
			get_typlenbyvalalign(elemtype,
								 &t->elemtyplen,
								 &t->elemtypbyval,
								 &t->elemtypalign);
			t->is_array = true;
		}
		else
			t->is_array = false;

		if (typtype == TYPTYPE_RANGE)
		{
			TypeCacheEntry *tc = lookup_type_cache(oid, TYPECACHE_RANGE_INFO);
			t->rangetype = tc->rngelemtype->type_id;
			t->is_range = true;
		}

		if (OidIsValid(langoid))
		{
			List *l = list_make1_oid(oid);
			t->fromsql = get_transform_fromsql(basetype, langoid, l);
			t->tosql = get_transform_tosql(basetype, langoid, l);
		}

		MemoryContextSwitchTo(oldcontext);
		MemoryContextSetParent(mcxt, pllua_get_memory_cxt(L));

	out:
		nt = t;
	}
	PLLUA_CATCH_RETHROW();

	*p = t = nt;

	if (!t)
		return t;

	pllua_record_gc_debt(L, 4096);  /* somewhat arbitrary */

	/*
	 * the table we created for our uservalue is going to be the metatable
	 * for datum objects of this type. We close most of the functions in it
	 * over the typeinfo object itself for easy access.
	 */
	lua_getuservalue(L, -1);
	lua_pushcfunction(L, pllua_datum_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, "typeinfo");
	/* stack: self uservalue */
	pllua_pgfunc_table_new(L);
	lua_setfield(L, -2, ".funcs");

	pllua_typeconv_register(L, -1, -2);

	/* stack: self uservalue */
	if (t->basetype != t->typeoid)
	{
		/*
		 * Note, "basetype" is the base type's typeinfo as of now, it won't be
		 * re-pointed as a result of invalidations. This is important because
		 * when we look at a value of the domain type, we need the base type's
		 * typeinfo as of that value's creation, not any more recent value.
		 *
		 * We require that in the event of an incompatible change to the base
		 * type, that both base and domain typeinfos are replaced (leaving the
		 * old domain typeinfo pointing at the old base).
		 */
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) t->basetype);
		lua_call(L, 1, 1);
		lua_setfield(L, -2, "basetype");
	}
	/* stack: self uservalue */
	if (t->tupdesc)
	{
		int i;
		/*
		 * If we're a row type with a tupdesc, then for the same reasons as
		 * given above, we need to look up all our element typeinfos now rather
		 * than deferring it to later (since they may be altered in the
		 * meantime). Note that equalTupleDescs doesn't look into nested
		 * composite values, so it can't detect incompatible changes caused by
		 * altering a column type. Instead, we must revalidate all our column
		 * types as part of our own revalidation.
		 *
		 * We also need to know whether any record type nested inside us has a
		 * column of unknown rowtype, since this may require extra work when we
		 * encounter tuples.
		 *
		 * XXX partial work here needs revisiting once certain issues in PG
		 * proper get fixed.
		 *
		 * Since we're going this far we might as well fill in the attribute
		 * names/positions table at the same time.
		 */
		lua_createtable(L, t->natts + 2, t->natts + 2);
		lua_createtable(L, t->natts, 0);
		/* stack: self uservalue attrnames attrtypes */
		for (i = 0; i < t->natts; ++i)
		{
			Form_pg_attribute att = TupleDescAttr(t->tupdesc, i);
			pllua_typeinfo *et = NULL;

			if (att->attisdropped)
				continue;

			lua_pushinteger(L, i+1);
			lua_pushstring(L, NameStr(att->attname));
			lua_pushvalue(L, -1);
			lua_pushinteger(L, i+1);
			lua_rawset(L, -6);
			lua_rawset(L, -4);
			lua_pushcfunction(L, pllua_typeinfo_lookup);
			lua_pushinteger(L, (lua_Integer) att->atttypid);
			if (att->atttypid != RECORDOID)
				lua_pushnil(L);
			else
				lua_pushinteger(L, (lua_Integer) att->atttypmod);
			lua_call(L, 2, 1);
			if (lua_isnil(L, -1))
				luaL_error(L, "failed to find attribute type info for column");
			et = pllua_checktypeinfo(L, -1, false);
			if (et->nested_unknowns)
				t->nested_unknowns = true;
			if (et->nested_composites
				|| (et->natts >= 0 && et->typeoid != RECORDOID))
				t->nested_composites = true;
			lua_rawseti(L, -2, i+1);
		}
		lua_setfield(L, -3, "attrtypes");
		if (t->hasoid)
		{
			lua_pushinteger(L, ObjectIdAttributeNumber);
			lua_setfield(L, -2, "oid");
			lua_pushstring(L, "oid");
			lua_seti(L, -2, ObjectIdAttributeNumber);
		}
		lua_setfield(L, -2, "attrs");
	}
	/* stack: self uservalue */
	if (t->is_array || t->is_range)
	{
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, t->is_range ? t->rangetype : t->elemtype);
		lua_call(L, 1, 1);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, "elemtypeinfo");
	}
	else
		lua_pushnil(L);
	/* stack: self uservalue elemtype-or-nil */
	lua_insert(L, -2);
	/* stack: self elemtype-or-nil uservalue */
	lua_pushvalue(L, -3);
	luaL_setfuncs(L, datumobj_base_mt, 1);
	if (t->is_array)
	{
		lua_pushvalue(L, -3);
		lua_pushvalue(L, -3);
		luaL_setfuncs(L, datumobj_array_mt, 2);
		lua_newtable(L);
		lua_pushvalue(L, -4);
		lua_pushvalue(L, -4);
		luaL_setfuncs(L, datumobj_array_methods, 2);
		lua_setfield(L, -2, "__methods");
	}
	else if (t->is_range)
	{
		lua_pushvalue(L, -3);
		lua_pushvalue(L, -3);
		luaL_setfuncs(L, datumobj_range_mt, 2);
	}
	else if (t->natts >= 0)
	{
		lua_pushvalue(L, -3);
		luaL_setfuncs(L, datumobj_row_mt, 1);
		/*
		 * if we're an unregistered rowtype with a known tupdesc, record_out
		 * won't work for us leaving no convenient way to print record values
		 * (e.g. for diagnostics). Work around this by replacing the normal
		 * datum __tostring with a specialized one.
		 */
		if (t->typmod < 0 && t->tupdesc)
		{
			lua_pushvalue(L, -3);
			luaL_setfuncs(L, datumobj_unreg_row_mt, 1);
		}
	}
	lua_pop(L, 2);

	return t;
}

/*
 * newtypeinfo(oid,typmod)
 *
 * does not intern the new object.
 *
 * returns nil for nonexistent type
 */
static int pllua_newtypeinfo(lua_State *L)
{
	Oid oid = luaL_checkinteger(L, 1);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	pllua_typeinfo *t;

	t = pllua_newtypeinfo_raw(L, oid, typmod, NULL);
	if (!t)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
	}
	return 1;
}


static int pllua_typeinfo_eq(lua_State *L)
{
	pllua_typeinfo *obj1 = pllua_checktypeinfo(L, 1, false);
	pllua_typeinfo *obj2 = pllua_checktypeinfo(L, 2, false);
	if (obj1 == obj2)
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
		|| obj1->basetype != obj2->basetype
		|| obj1->elemtype != obj2->elemtype
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
	else
	{
		bool match = true;
		int natts = obj1->natts;
		/*
		 * We also need to check that all the element typeinfos are raw-equal
		 * between both.
		 */
		if (natts > 0)
		{
			int i;
			pllua_get_user_field(L, 1, "attrtypes");
			pllua_get_user_field(L, 2, "attrtypes");
			for (i = 1; match && i <= natts; ++i)
			{
				lua_rawgeti(L, -2, i);
				lua_rawgeti(L, -2, i);
				if (!lua_rawequal(L, -1, -2))
					match = false;
				lua_pop(L, 2);
			}
			lua_pop(L, 2);
		}
		lua_pushboolean(L, match);
		return 1;
	}
}

int pllua_typeinfo_lookup(lua_State *L)
{
	Oid oid = luaL_checkinteger(L, 1);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	pllua_typeinfo *obj = NULL;
	pllua_typeinfo *nobj;

	lua_settop(L, 1);
	lua_pushinteger(L, typmod);

	if (!OidIsValid(oid))
	{
		/* safety check so we never intern an entry for InvalidOid */
		lua_pushnil(L);
		return 1;
	}
	else if (oid == RECORDOID && typmod >= 0)
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
		obj = pllua_checktypeinfo(L, -1, false);
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
	/* note, newobject might be nil too */
	if (lua_isnil(L, -1))
		nobj = NULL;
	else
		nobj = pllua_checktypeinfo(L, -1, false);
	if (obj && nobj)
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
			/* equal. pop the new object after updating anything of interest */
			if (obj->fromsql != nobj->fromsql ||
				obj->tosql != nobj->tosql)
			{
				pllua_get_user_field(L, -3, ".funcs");
				lua_pushnil(L);
				lua_setfield(L, -2, ".fromsql");
				lua_pushnil(L);
				lua_setfield(L, -2, ".tosql");
				lua_pop(L, 1);

				obj->fromsql = nobj->fromsql;
				obj->tosql = nobj->tosql;
			}
			obj->revalidate = false;
			lua_pop(L,2);
			return 1;
		}
		/*
		 * We're going to intern the new object in place of the old one
		 */
		obj->modified = true;
		obj->revalidate = false;
		lua_pop(L,1);
	}
	else if (obj)
	{
		/* no new object, must have been dropped. */
		obj->obsolete = true;
		obj->revalidate = false;
		/* new object (nil) is on stack top */
	}
	/* stack: oid typmod table oldobject-or-nil newobject */
	lua_remove(L, -2);
	lua_pushvalue(L, -1);
	if (oid == RECORDOID && typmod >= 0)
		lua_rawseti(L, -3, typmod);
	else
		lua_rawseti(L, -3, oid);
	return 1;
}

/*
 * invalidate(interp)
 */
int pllua_typeinfo_invalidate(lua_State *L)
{
	pllua_interpreter *interp = lua_touserdata(L, 1);
	Oid typoid = interp->inval->inval_typeoid;
	Oid relid = interp->inval->inval_reloid;

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);

	if (interp->inval->inval_type)
	{
		if (OidIsValid(typoid))
		{
			pllua_typeinfo *t;
			if (lua_rawgeti(L, -1, (lua_Integer) typoid) == LUA_TUSERDATA)
			{
				t = pllua_totypeinfo(L, -1);
				t->revalidate = true;
			}
		}
		else
		{
			lua_pushnil(L);
			while (lua_next(L, -2))
			{
				pllua_typeinfo *t = pllua_totypeinfo(L, -1);
				t->revalidate = true;
				lua_pop(L,1);
			}
		}
	}

	if (interp->inval->inval_rel)
	{
		lua_pushnil(L);
		while (lua_next(L, -2))
		{
			pllua_typeinfo *t = pllua_totypeinfo(L, -1);
			if (t->reloid == relid)
				t->revalidate = true;
			lua_pop(L,1);
		}
	}

	return 0;
}

static int pllua_typeinfo_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_TYPEINFO_OBJECT);
	pllua_typeinfo *obj = p ? *p : NULL;

	ASSERT_LUA_CONTEXT;

	if (!p)
		return 0;

	*p = NULL;
	if (!obj)
		return 0;

	PLLUA_TRY();
	{
		/*
		 * typeinfo is allocated in its own memory context (since we expect it
		 * to have stuff dangling off), so free it by destroying that.
		 */
		pllua_debug(L, "pllua_typeinfo_gc: %p", obj->mcxt);
		MemoryContextDelete(obj->mcxt);
	}
	PLLUA_CATCH_RETHROW();

	return 0;
}

static int pllua_dump_typeinfo(lua_State *L)
{
	pllua_typeinfo *obj = pllua_checktypeinfo(L, 1, false);
	luaL_Buffer b;
	char *buf;

	luaL_buffinit(L, &b);

	if (!obj)
	{
		luaL_addstring(&b, "(null)");
		luaL_pushresult(&b);
		return 1;
	}

	buf = luaL_prepbuffer(&b);
	snprintf(buf, LUAL_BUFFERSIZE,
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

int pllua_typeinfo_parsetype(lua_State *L)
{
	const char *str = luaL_checkstring(L, 1);
	volatile Oid ret_oid = InvalidOid;

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
	}
	PLLUA_CATCH_RETHROW();

	/* we intentionally ignore the typmod here */
	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, (lua_Integer) ret_oid);
	lua_call(L, 1, 1);
	return 1;
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
	pllua_datum *d = pllua_toanydatum(L, 2, NULL);
	if (d)
		return 1;
	if (lua_isnoneornil(L, 3))
		return 0;
	if (lua_isinteger(L, 3))
	{
		int idx = lua_tointeger(L, 3);
		FmgrInfo *flinfo;
		pllua_func_activation *act;
		Oid oid = InvalidOid;
		int32 typmod = -1;
		if (!pllua_get_cur_act(L))
			luaL_error(L, "not in a function");
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

static int pllua_typeinfo_package_index(lua_State *L)
{
	if (lua_isinteger(L, 2))
	{
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushvalue(L, 2);
		lua_call(L, 1, 1);
		return 1;
	}
	else if (lua_isstring(L, 2))
	{
		lua_pushcfunction(L, pllua_typeinfo_parsetype);
		lua_pushvalue(L, 2);
		lua_call(L, 1, 1);
		return 1;
	}
	else
		return luaL_error(L, "invalid args for typeinfo lookup");
}

static int pllua_typeinfo_package_array_index(lua_State *L)
{
	pllua_typeinfo *et;
	volatile Oid oid = InvalidOid;

	lua_pushcfunction(L, pllua_typeinfo_package_index);
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	if (lua_isnil(L, -1))
		return 1;

	et = pllua_checktypeinfo(L, -1, false);
	PLLUA_TRY();
	{
		oid = get_array_type(et->typeoid);
	}
	PLLUA_CATCH_RETHROW();

	if (!OidIsValid(oid))
		lua_pushnil(L);
	else
	{
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) oid);
		lua_call(L, 1, 1);
	}

	return 1;
}

static int pllua_typeinfo_name(lua_State *L)
{
	pllua_typeinfo *obj = pllua_checktypeinfo(L, 1, true);
	lua_Integer typmod = luaL_optinteger(L, 2, -1);
	bool typmod_given = !lua_isnoneornil(L, 2);
	const char *volatile name = NULL;

	ASSERT_LUA_CONTEXT;

	if (obj->obsolete)
		luaL_error(L, "type no longer exists");

	PLLUA_TRY();
	{
		if (typmod_given && obj->typeoid != RECORDOID)
			name = format_type_with_typemod(obj->typeoid, typmod);
		else
			name = format_type_be(obj->typeoid);
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
	Oid funcoid = InvalidOid;
	FmgrInfo *flinfo = NULL;

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

static void pllua_typeinfo_raw_input(lua_State *L, Datum *res, pllua_typeinfo *t,
									 const char *str, int32 typmod)
{
	if ((OidIsValid(t->infuncid) && OidIsValid(t->infunc.fn_oid))
		|| pllua_typeinfo_iofunc(L, t, IOFunc_input))
	{
		*res = InputFunctionCall(&t->infunc, (char *) str, t->typioparam, typmod);
	}
	else
		elog(ERROR, "failed to find input function for type %u", t->typeoid);
}

static const char *pllua_typeinfo_raw_output(lua_State *L, Datum value, pllua_typeinfo *t)
{
	const char *volatile res = NULL;

	if ((OidIsValid(t->outfuncid) && OidIsValid(t->outfunc.fn_oid))
		|| pllua_typeinfo_iofunc(L, t, IOFunc_output))
	{
		res = OutputFunctionCall(&t->outfunc, value);
	}
	else
		elog(ERROR, "failed to find output function for type %u", t->typeoid);

	return res;
}

static void pllua_typeinfo_raw_coerce(lua_State *L, Datum *val, bool *isnull,
									  int nf, Oid fnoid, int32 typmod, bool is_explicit)
{
	FunctionCallInfoData fcinfo;
	FmgrInfo *fn = *(FmgrInfo **) lua_touserdata(L, nf);

	Assert(OidIsValid(fnoid));
	if (!fn || !OidIsValid(fn->fn_oid))
		fn = pllua_pgfunc_init(L, nf, fnoid, -1, NULL, InvalidOid);

	if (*isnull && fn->fn_strict)
		return;

	InitFunctionCallInfoData(fcinfo, fn, 3, InvalidOid, NULL, NULL);

	fcinfo.arg[0] = *val;
	fcinfo.argnull[0] = *isnull;
	fcinfo.arg[1] = Int32GetDatum(typmod);
	fcinfo.argnull[1] = false;
	fcinfo.arg[2] = BoolGetDatum(is_explicit);
	fcinfo.argnull[2] = false;

	*val = FunctionCallInvoke(&fcinfo);
	*isnull = fcinfo.isnull;
}

static void pllua_typeinfo_raw_coerce_array(lua_State *L, Datum *val, bool *isnull,
											CoercionPathType elempath,
											int nf, Oid fnoid, int nf2, Oid fnoid2,
											pllua_typeinfo *st,
											pllua_typeinfo *dt,
											int32 typmod,
											bool is_explicit)
{
	if (!*isnull)
	{
		MemoryContext mcxt = AllocSetContextCreate(CurrentMemoryContext,
												   "pllua temporary array context",
												   ALLOCSET_START_SMALL_SIZES);
		MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
		AnyArrayType *arr = DatumGetAnyArrayP(*val);
		ArrayType *newarr;
		int ndim = AARR_NDIM(arr);
		int32 *dims = AARR_DIMS(arr);
		int nitems = ArrayGetNItems(ndim,dims);
		Datum *values = palloc(nitems * sizeof(Datum));
		bool *isnull = palloc(nitems * sizeof(bool));
		FunctionCallInfoData fcinfo;
		FunctionCallInfoData fcinfo2;
		bool separate_typmod = typmod >= 0 && OidIsValid(fnoid2);
		FmgrInfo *fn = elempath == COERCION_PATH_FUNC ? *(FmgrInfo **) lua_touserdata(L, nf) : NULL;
		FmgrInfo *fn2 = separate_typmod ? *(FmgrInfo **) lua_touserdata(L, nf2) : NULL;
		array_iter iter;
		int idx;

		Assert(elempath != COERCION_PATH_NONE);
		Assert(elempath != COERCION_PATH_ARRAYCOERCE);
		Assert(elempath != COERCION_PATH_COERCEVIAIO || !separate_typmod);

		if (OidIsValid(fnoid) && (!fn || !OidIsValid(fn->fn_oid)))
			fn = pllua_pgfunc_init(L, nf, fnoid, -1, NULL, InvalidOid);
		if (OidIsValid(fnoid2) && (!fn2 || !OidIsValid(fn->fn_oid)))
			fn2 = pllua_pgfunc_init(L, nf2, fnoid2, -1, NULL, InvalidOid);

		array_iter_setup(&iter, arr);

		if (elempath == COERCION_PATH_FUNC)
			InitFunctionCallInfoData(fcinfo, fn, 3, InvalidOid, NULL, NULL);
		if (separate_typmod)
			InitFunctionCallInfoData(fcinfo2, fn2, 3, InvalidOid, NULL, NULL);

		for (idx = 0; idx < nitems; ++idx)
		{
			fcinfo.arg[0] = array_iter_next(&iter, &fcinfo.argnull[0], idx,
											st->elemtyplen, st->elemtypbyval, st->elemtypalign);

			if (elempath == COERCION_PATH_RELABELTYPE)
			{
				values[idx] = fcinfo.arg[0];
				isnull[idx] = fcinfo.argnull[0];
			}

			switch (elempath)
			{
				default:
					break;
				case COERCION_PATH_COERCEVIAIO:
					/*
					 * Contra pg proper, we don't need to do separate typmod
					 * conversions when doing IO casts; separate_typmod should
					 * not be true.
					 */
					if (!fcinfo.argnull[0])
					{
						const char *str = pllua_typeinfo_raw_output(L, fcinfo.arg[0], st);
						pllua_typeinfo_raw_input(L, &values[idx], dt, str, typmod);
						isnull[idx] = (str == NULL);
					}
					else
						isnull[idx] = true;
					break;

				case COERCION_PATH_FUNC:
					if (!fcinfo.argnull[0] || !fn->fn_strict)
					{
						fcinfo.arg[1] = Int32GetDatum(separate_typmod ? -1 : typmod);
						fcinfo.argnull[1] = false;
						fcinfo.arg[2] = BoolGetDatum(is_explicit);
						fcinfo.argnull[2] = false;
						fcinfo.isnull = false;

						values[idx] = FunctionCallInvoke(&fcinfo);
						isnull[idx] = fcinfo.isnull;
					}
					else
					{
						values[idx] = (Datum)0;
						isnull[idx] = true;
					}
					/*FALLTHROUGH*/
				case COERCION_PATH_RELABELTYPE:
					if (separate_typmod && (!isnull[idx] || !fn2->fn_strict))
					{
						fcinfo2.arg[0] = values[idx];
						fcinfo2.argnull[0] = isnull[idx];
						fcinfo2.arg[1] = Int32GetDatum(typmod);
						fcinfo2.argnull[1] = false;
						fcinfo2.arg[2] = BoolGetDatum(is_explicit);
						fcinfo2.argnull[2] = false;
						fcinfo2.isnull = false;

						values[idx] = FunctionCallInvoke(&fcinfo2);
						isnull[idx] = fcinfo2.isnull;
					}
					break;
			}
		}

		MemoryContextSwitchTo(oldcontext);

		newarr = construct_md_array(values, isnull, ndim, dims, AARR_LBOUND(arr),
									dt->elemtype, dt->elemtyplen, dt->elemtypbyval, dt->elemtypalign);
		*val = PointerGetDatum(newarr);
		*isnull = false;

		MemoryContextDelete(mcxt);
	}
}

/*
 * "val" is already known to be of t's base type.
 *
 * Note that we might replace "val" with a new datum allocated in the current
 * memory context.
 *
 * "typmod" is "val's" existing typmod if known, or -1.
 */
static void pllua_typeinfo_check_domain(lua_State *L,
										Datum *val, bool *isnull, int32 typmod,
										int nt, pllua_typeinfo *t)
{
	int oldtop = lua_gettop(L);

	ASSERT_LUA_CONTEXT;

	if (t->basetypmod != -1	&& typmod != t->basetypmod && t->coerce_typmod)
		pllua_get_user_subfield(L, nt, ".funcs", ".f_typmod");

	PLLUA_TRY();
	{
		/*
		 * Check if we need to do typmod coercion first. This might alter the
		 * value.
		 */
		if (t->basetypmod != -1	&& typmod != t->basetypmod && t->coerce_typmod)
		{
			if (t->coerce_typmod_element)
				pllua_typeinfo_raw_coerce_array(L, val, isnull, COERCION_PATH_FUNC,
												-1, t->typmod_funcid, 0, InvalidOid,
												t, t, t->basetypmod, false);
			else
				pllua_typeinfo_raw_coerce(L, val, isnull, -1, t->typmod_funcid, t->basetypmod, false);
		}

		domain_check(*val, *isnull, t->typeoid, &t->domain_extra, t->mcxt);
	}
	PLLUA_CATCH_RETHROW();

	lua_settop(L, oldtop);
}

static Datum pllua_typeinfo_raw_tosql(lua_State *L, pllua_typeinfo *t, bool *isnull)
{
	FunctionCallInfoData fcinfo;
	FmgrInfo *fn = *(void **) lua_touserdata(L, lua_upvalueindex(3));
	Datum result;
	pllua_node node;

	ASSERT_PG_CONTEXT;

	if (!fn || !OidIsValid(fn->fn_oid))
		fn = pllua_pgfunc_init(L, lua_upvalueindex(3), t->tosql, -1, NULL, InvalidOid);

	node.type = T_Invalid;
	node.magic = PLLUA_MAGIC;
	node.L = L;

	InitFunctionCallInfoData(fcinfo, fn, 1, InvalidOid, (struct Node *) &node, NULL);

	/* actual arg(s) on top of stack */
	fcinfo.arg[0] = (Datum)0;
	fcinfo.argnull[0] = true;

	result = FunctionCallInvoke(&fcinfo);

	if (isnull)
		*isnull = fcinfo.isnull;

	return result;
}

/*
 * args 1..top are the value to convert
 * upvalue 1 is the typeinfo
 * upvalue 2 is the datum to be filled in
 * upvalue 3 is the pgfunc object
 * returns the datum or nil
 */
static int pllua_typeinfo_tosql(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	pllua_datum *d;
	volatile Datum val;
	bool isnull = false;

	PLLUA_TRY();
	{
		val = pllua_typeinfo_raw_tosql(L, t, &isnull);
	}
	PLLUA_CATCH_RETHROW();

	if (isnull)
		lua_pushnil(L);
	else
	{
		d = pllua_todatum(L, lua_upvalueindex(2), lua_upvalueindex(1));
		d->value = val;
		lua_pushvalue(L, lua_upvalueindex(2));
	}
	return 1;
}

static bool pllua_typeinfo_raw_fromsql(lua_State *L, Datum val, pllua_typeinfo *t)
{
	FunctionCallInfoData fcinfo;
	FmgrInfo *fn = *(void **) lua_touserdata(L, lua_upvalueindex(3));
	pllua_node node;

	ASSERT_PG_CONTEXT;

	if (!OidIsValid(t->fromsql))
		return false;

	if (!fn || !OidIsValid(fn->fn_oid))
		fn = pllua_pgfunc_init(L, lua_upvalueindex(3), t->fromsql, -1, NULL, InvalidOid);

	node.type = T_Invalid;
	node.magic = PLLUA_MAGIC;
	node.L = L;

	InitFunctionCallInfoData(fcinfo, fn, 1, InvalidOid, (struct Node *) &node, NULL);

	fcinfo.arg[0] = val;
	fcinfo.argnull[0] = false;

	FunctionCallInvoke(&fcinfo);

	return !fcinfo.isnull;
}

/*
 * upvalue 1 is the typeinfo
 * upvalue 2 is a userdata with the value to convert
 * upvalue 3 is the pgfunc object
 * returns the value or nothing
 */
static int pllua_typeinfo_fromsql(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, lua_upvalueindex(1));
	Datum d = *(Datum *)lua_touserdata(L, lua_upvalueindex(2));
	volatile bool done;

	Assert(lua_gettop(L) == 0);

	PLLUA_TRY();
	{
		done = pllua_typeinfo_raw_fromsql(L, d, t);
	}
	PLLUA_CATCH_RETHROW();

	Assert(done ? lua_gettop(L) == 1 : lua_gettop(L) == 0);

	return done ? 1 : 0;
}

/*
 * Note that "typmod" here is the _destination_ typmod
 */
static void pllua_typeinfo_coerce_typmod(lua_State *L,
										 Datum *val, bool *isnull,
										 int nt,
										 pllua_typeinfo *t,
										 int32 typmod)
{
	if (!t->coerce_typmod || typmod < 0)
		return;
	nt = lua_absindex(L, nt);
	pllua_get_user_subfield(L, nt, ".funcs", ".f_typmod");

	PLLUA_TRY();
	{
		if (t->coerce_typmod_element)
		{
			Assert(t->is_array);
			pllua_typeinfo_raw_coerce_array(L, val, isnull, COERCION_PATH_FUNC,
											-1, t->typmod_funcid, 0, InvalidOid,
											t, t, typmod, false);
		}
		else
			pllua_typeinfo_raw_coerce(L, val, isnull, -1, t->typmod_funcid, typmod, false);
	}
	PLLUA_CATCH_RETHROW();

	lua_pop(L, 1);
}

/*
 * t:fromstring('str')  returns a datum object.
 *
 * Given a nil input, it returns nil, but might call the input function anyway
 * (only if it's not strict)
 */
static int pllua_typeinfo_fromstring(lua_State *L)
{
	pllua_typeinfo *t = pllua_checktypeinfo(L, 1, true);
	const char *str = lua_isnil(L, 2) ? NULL : luaL_checkstring(L, 2);
	MemoryContext mcxt = pllua_get_memory_cxt(L);
	pllua_datum *d = NULL;

	if (t->obsolete || t->modified)
		luaL_error(L, "cannot create values for a dropped or modified type");

	ASSERT_LUA_CONTEXT;

	if (str)
	{
		pllua_verify_encoding(L, str);
		d = pllua_newdatum(L, 1, (Datum)0);
	}
	else
		lua_pushnil(L);

	PLLUA_TRY();
	{
		Datum nv;

		pllua_typeinfo_raw_input(L, &nv, t, str, t->typmod);
		if (str)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
			d->value = nv;
			pllua_savedatum(L, d, t);
			MemoryContextSwitchTo(oldcontext);
		}
	}
	PLLUA_CATCH_RETHROW();

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
	pllua_typeinfo *t = pllua_checktypeinfo(L, 1, true);
	size_t len = 0;
	const char *str = lua_isnil(L, 2) ? NULL : luaL_checklstring(L, 2, &len);
	MemoryContext mcxt = pllua_get_memory_cxt(L);
	pllua_datum *d = NULL;
	volatile bool done = false;

	if (t->modified || t->obsolete)
		luaL_error(L, "cannot create values for a dropped or modified type");

	ASSERT_LUA_CONTEXT;

	if (str)
		d = pllua_newdatum(L, 1, (Datum)0);
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
			nv = ReceiveFunctionCall(&t->recvfunc, str ? &buf : NULL, t->typioparam, t->typmod);
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
 * "nd" indexes a table (or table-like object)
 * t = target typeinfo
 *
 * Number of values pushed should always equal the target type's arity, we push
 * nils for anything missing
 */
static int pllua_typeinfo_push_from_table(lua_State *L, int nd, pllua_typeinfo *t)
{
	int attno;
	int natts = t->natts;
	int nret = 0;

	nd = lua_absindex(L, nd);

	luaL_checkstack(L, 10 + t->arity, NULL);

	for (attno = 0; attno < natts; ++attno)
	{
		Form_pg_attribute att = TupleDescAttr(t->tupdesc, attno);
		if (att->attisdropped)
			continue;
		lua_getfield(L, nd, NameStr(att->attname));
		++nret;
	}

	return nret;
}

static bool pllua_datum_transform_tosql(lua_State *L, int nargs, int argbase, int nt,
										pllua_typeinfo *t)
{
	int i;
	nt = lua_absindex(L, nt);
	argbase = lua_absindex(L, argbase);
	if (!OidIsValid(t->tosql))
	{
		/* no SQL-level transform? maybe we have a transform in the typeinfo. */
		if (pllua_get_user_field(L, nt, "tosql") == LUA_TFUNCTION)
		{
			int base = lua_gettop(L) - 1;  /* -1 because func is on stack already */
			luaL_checkstack(L, 10+nargs, NULL);
			for (i = 0; i < nargs; ++i)
				lua_pushvalue(L, argbase+i);
			lua_call(L, nargs, LUA_MULTRET);
			/* transform should return 1 value if it liked the input, no value if not */
			if (lua_gettop(L) == base)
				return false;
			lua_settop(L, base+1);  /* clamp to 1 result */
			return true;
		}
		else
			lua_pop(L, 1);
		return false;
	}
	luaL_checkstack(L, 10+nargs, NULL);
	lua_pushvalue(L, nt);
	pllua_newdatum(L, -1, (Datum)0);
	pllua_get_user_subfield(L, nt, ".funcs", ".tosql");
	lua_pushcclosure(L, pllua_typeinfo_tosql, 3);
	for (i = 0; i < nargs; ++i)
		lua_pushvalue(L, argbase+i);
	lua_call(L, nargs, 1);
	return true;
}

int pllua_datum_transform_fromsql(lua_State *L, Datum val, int nidx, pllua_typeinfo *t)
{
	Datum *tmpd;
	int nd;

	/*
	 * This would belong in pllua_value_from_datum except that we don't have
	 * the typeinfo available there.
	 */
	if (t->is_enum)
	{
		const char *volatile str = NULL;

		PLLUA_TRY();
		{
			str = pllua_typeinfo_raw_output(L, val, t);
		}
		PLLUA_CATCH_RETHROW();
		lua_pushstring(L, str);
		return LUA_TSTRING;
	}

	if (!OidIsValid(t->fromsql))
		return LUA_TNONE;

	nd = lua_gettop(L);
	lua_pushvalue(L, nidx);
	tmpd = lua_newuserdata(L, sizeof(Datum));
	*tmpd = val;
	pllua_get_user_subfield(L, nidx, ".funcs", ".fromsql");
	lua_pushcclosure(L, pllua_typeinfo_fromsql, 3);
	lua_call(L, 0, LUA_MULTRET);
	nd = lua_gettop(L) - nd;
	if (nd == 0)
		return LUA_TNONE;
	else if (nd > 1 || nd < 0)
		return luaL_error(L, "invalid return from transform function");
	else
		return lua_type(L, -1);
}

/*
 * "call" for a typeinfo is to invoke it as a value constructor:
 *
 * t(val,val,val,...)
 *
 * The number of args must match the arity of the type. Each value must either
 * be acceptable simple input for the field type, or a string, or a datum of
 * the correct type - probably this needs extending to allow casts.
 *
 * t(table)
 *
 * Constructs a rowtype or array value from fields in a table.
 *
 * t(table, dim1, dim2, ...)
 *
 * Constructs a possibly multi-dimensional array
 *
 * t(val)
 *
 * Constructs a (deep) copy of the passed-in value of the same type.
 *
 * We break this down into several specific cases.
 */

static int pllua_typeinfo_array_call(lua_State *L);
static int pllua_typeinfo_row_call(lua_State *L);
static int pllua_typeinfo_scalar_call(lua_State *L);
static int pllua_typeinfo_range_call(lua_State *L);


/*
 * scalartype(datum)
 * arraytype(datum)
 *
 * Converting a single Datum of one type to the target type.
 *
 * We invoke the typeconv logic to do the conversion, even if source and target
 * types are the same, since that helps keep the code simpler.
 */
static int pllua_typeinfo_call_datum(lua_State *L, int nd, int nt, int ndt, pllua_datum *d, pllua_typeinfo *t)
{
	nd = lua_absindex(L, nd);   /* source datum */
	nt = lua_absindex(L, nt);   /* target typeinfo */

	/*
	 * All the work here is done by the typeconv system; looking up the type
	 * cast generates the function object.
	 */
	pllua_get_user_field(L, ndt, "typeconv");
	lua_pushvalue(L, nt);
	if (lua_gettable(L, -2) != LUA_TFUNCTION)
		luaL_error(L, "cast lookup error");
	lua_pushvalue(L, nd);
	lua_call(L, 1, 1);
	return 1;
}

/*
 * Try and convert an existing datum to an anonymous record.
 *
 * We can do this if:
 *  - the input is already an anonymous record, which we just copy;
 *  - the input is an unmodified row of any type, which we just copy;
 *  - the input is a modified row of any type, which we reform into a
 *    new unmodified row of its own type and then adopt
 *
 * Any other input is an error, since without a tupdesc we can't know anything
 * about the intended structure.
 */
static int pllua_typeinfo_anonrec_call_datum(lua_State *L, int nd, int nt, int ndt,
											 pllua_typeinfo *t, pllua_datum *d, pllua_typeinfo *dt)
{
	nd = lua_absindex(L, nd);   /* source datum */
	nt = lua_absindex(L, nt);   /* target typeinfo */
	ndt = lua_absindex(L, ndt); /* source datum's typeinfo */

	if (dt->natts >= 0)
	{
		pllua_datum *tmpd;
		pllua_datum *newd;
		
		/* Use the source datum's own typeinfo to make a copy of it; this
		 * ensures we have a new unexploded record which we can just steal the
		 * storage for.
		 */
		lua_pushvalue(L, ndt);
		lua_pushvalue(L, nd);
		lua_call(L, 1, 1);
		tmpd = pllua_todatum(L, -1, ndt);
		Assert(tmpd);
		Assert(!tmpd->modified);
		newd = pllua_newdatum(L, nt, tmpd->value);
		/* transfer ownership of storage to new datum */
		tmpd->need_gc = false;
		newd->need_gc = true;
		return 1;
	}
	else if (dt->is_anonymous_record)
	{
		pllua_datum *newd;

		newd = pllua_newdatum(L, nt, (Datum)0);
		newd->value = d->value;
		pllua_save_one_datum(L, newd, t);
		return 1;
	}
	else
		return luaL_error(L, "anonymous record can only accept input of existing row datum");
}

static int pllua_typeinfo_call(lua_State *L)
{
	pllua_typeinfo *t = pllua_checktypeinfo(L, 1, true);
	int nargs = lua_gettop(L) - 1;
	pllua_typeinfo *dt;
	pllua_datum *d = (nargs == 1) ? pllua_toanydatum(L, 2, &dt) : NULL;

	if (t->modified || t->obsolete)
		luaL_error(L, "cannot create values for obsolete or modified type");

	if (d)
	{
		if (t->is_anonymous_record)
			return pllua_typeinfo_anonrec_call_datum(L, 2, 1, -1, t, d, dt);
		/*
		 * The condition here is to exclude this case:
		 *    destination type is a rowtype
		 *      source type is a scalar,
		 *      or: destination is arity 1
		 *          and source is not the same type oid
		 * This is because all of those cases should be treated as
		 * constructing from the first element value
		 */
		if (!(t->natts >= 0
			  && (dt->natts < 0 || (t->arity == 1 && t->typeoid != dt->typeoid))))
			return pllua_typeinfo_call_datum(L, 2, 1, -1, d, t);
		lua_pop(L, 1);
	}

	if (t->is_array)
		lua_pushcfunction(L, pllua_typeinfo_array_call);
	else if (t->is_range)
		lua_pushcfunction(L, pllua_typeinfo_range_call);
	else if (t->natts >= 0)
		lua_pushcfunction(L, pllua_typeinfo_row_call);
	else if (t->is_anonymous_record)
		luaL_error(L, "anonymous record can only accept input of existing row datum");
	else
		lua_pushcfunction(L, pllua_typeinfo_scalar_call);
	lua_insert(L, 1);
	lua_call(L, nargs+1, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * We only get here for non-Datum input
 */
static int pllua_typeinfo_scalar_call(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, 1);
	pllua_datum *newd = NULL;
	int nargs = lua_gettop(L) - 1;
	Datum nvalue = (Datum) 0;
	bool isnull = false;
	const char *err = NULL;
	const char *str = NULL;

	/*
	 * If there's a transform, it might accept multiple args, so try it first,
	 * but only if the input isn't a single string arg.
	 */
	if ((nargs > 1 || lua_type(L,2) != LUA_TSTRING) &&
		pllua_datum_transform_tosql(L, nargs, 2, 1, t))
	{
		Datum *nvaluep = &nvalue;
		if (!lua_isnil(L, -1))
		{
			newd = pllua_todatum(L, -1, 1);
			nvaluep = &newd->value;
		}
		else
			isnull = true;

		/* must check domain constraints before accepting a null */
		/* note this can change the value */
		if (t->typeoid != t->basetype)
			pllua_typeinfo_check_domain(L, nvaluep, &isnull, -1, 1, t);
		if (isnull)
			return 1;
		/* shouldn't happen: */
		if (!newd)
			luaL_error(L, "domain check returned non-null for null input");
	}
	else if (nargs != 1)
	{
		luaL_error(L, "incorrect number of arguments for type constructor (expected 1 got %d)",
				   nargs);
	}
	else if (pllua_datum_from_value(L, 2,
									t->basetype,  /* accept input for the base type of a domain */
									&nvalue,
									&isnull,
									&err))
	{
		if (err)
			luaL_error(L, "could not convert value: %s", err);
		/* must check domain constraints before accepting a null */
		/* note this can change the value */
		if (t->typeoid != t->basetype)
			pllua_typeinfo_check_domain(L, &nvalue, &isnull, -1, 1, t);
		if (isnull)
		{
			lua_pushnil(L);
			return 1;
		}
		newd = pllua_newdatum(L, 1, nvalue);
	}
	else if (lua_type(L, 2) == LUA_TSTRING)
	{
		str = lua_tostring(L, 2);
		pllua_verify_encoding(L, str);
		newd = pllua_newdatum(L, 1, (Datum)0);
	}
	else
		luaL_error(L, "incompatible value type");

	PLLUA_TRY();
	{
		MemoryContext oldcontext;

		if (str)
		{
			/* input func is responsible for typmod handling on this path */
			pllua_typeinfo_raw_input(L, &nvalue, t, str, t->typmod);
			newd->value = nvalue;
		}

		oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		pllua_savedatum(L, newd, t);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


/*
 * rangetype(lo,hi)
 * rangetype(lo,hi,bounds)
 * rangetype()  empty range
 * rangetype(str)  goes to the normal scalar call
 */
static int pllua_typeinfo_range_call(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, 1);
	pllua_typeinfo *et PG_USED_FOR_ASSERTS_ONLY;
	int nargs = lua_gettop(L) - 1;
	RangeBound lo;
	RangeBound hi;
	pllua_datum *d;

	lua_settop(L, 4);

	pllua_get_user_field(L, 1, "elemtypeinfo");

	et = pllua_checktypeinfo(L, -1, false);
	Assert(et && et->typeoid == t->rangetype);

	if (nargs == 1)
	{
		lua_settop(L, 2);
		lua_pushcfunction(L, pllua_typeinfo_scalar_call);
		lua_insert(L, 1);
		lua_call(L, 2, 1);
		return 1;
	}
	else if (nargs > 3)
		luaL_error(L, "incorrect arguments for range constructor");
	if (nargs == 3 && !lua_isstring(L, 4))
		luaL_argerror(L, 3, "string");

	lo.infinite = false;
	lo.inclusive = true;
	lo.lower = true;
	hi.infinite = false;
	hi.inclusive = false;
	hi.lower = false;

	if (nargs >= 2)
	{
		if (lua_isnil(L, 2))
			lo.infinite = true;
		else
		{
			lua_pushvalue(L, -1);
			lua_pushvalue(L, 2);
			lua_call(L, 1, 1);
			lua_replace(L, 2);
			d = pllua_checkdatum(L, 2, 5);
			lo.val = d->value;
		}
		if (lua_isnil(L, 3))
			hi.infinite = true;
		else
		{
			lua_pushvalue(L, -1);
			lua_pushvalue(L, 3);
			lua_call(L, 1, 1);
			lua_replace(L, 3);
			d = pllua_checkdatum(L, 3, 5);
			hi.val = d->value;
		}
	}

	if (nargs == 3)
	{
		const char *str = lua_tostring(L, 4);
		if (!str
			|| (str[0] != '[' && str[0] != '(')
			|| (str[1] != ']' && str[1] != ')')
			|| str[2])
			luaL_error(L, "invalid range bounds specifier");
		lo.inclusive = (str[0] == '[');
		hi.inclusive = (str[1] == ']');
	}

	d = pllua_newdatum(L, 1, (Datum)0);

	PLLUA_TRY();
	{
		TypeCacheEntry *tc = lookup_type_cache(t->typeoid, TYPECACHE_RANGE_INFO);
		Datum val = PointerGetDatum(make_range(tc, &lo, &hi, (nargs == 0)));
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		d->value = val;
		pllua_savedatum(L, d, t);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static int pllua_typeinfo_array_fromtable(lua_State *L, int nt, int nte, int nd, int ndim, int *dims,
										  pllua_typeinfo *t, pllua_typeinfo *et);

/*
 * arraytype(val,val,val,...)
 * arraytype()  empty array
 * arraytype(table, dim1, dim2, ...)
 *
 * idiom:  arraytype(table, #table)
 * or  arraytype(table, (table.n or #table))
 *
 * no support for lower bounds yet.
 *
 * Note that arraytype(a) where a is already of the array type never gets here.
 */
static int pllua_typeinfo_array_call(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, 1);
	pllua_typeinfo *et;
	int nargs = lua_gettop(L) - 1;
	int ndim = 0;
	int dims[MAXDIM];
	int i;

	pllua_get_user_field(L, 1, "elemtypeinfo");

	et = pllua_checktypeinfo(L, -1, false);
	Assert(et && !et->is_array && et->typeoid == t->elemtype);

	if (nargs > 0)
	{
		int typ1 = lua_type(L, 2);
		if (nargs > 1
			&& (typ1 == LUA_TTABLE || typ1 == LUA_TUSERDATA)
			&& lua_isinteger(L, 3))
		{
			if (nargs > MAXDIM+1)
				luaL_error(L, "too many dimensions for array (max %d)", MAXDIM);
			ndim = nargs - 1;
			for (i = 0; i < ndim; ++i)
			{
				dims[i] = lua_tointeger(L, 3+i);
				if (dims[i] < 0
					|| (dims[i] == 0 && ndim > 1))
					luaL_error(L, "invalid dimension %d (%d) for array", i, dims[i]);
			}
			return pllua_typeinfo_array_fromtable(L, 1, -1, 2, ndim, dims, t, et);
		}
	}

	lua_createtable(L, nargs, 0);
	for (i = 1; i <= nargs; ++i)
	{
		lua_pushvalue(L, 1+i);
		lua_seti(L, -2, i);
	}

	return pllua_typeinfo_array_fromtable(L, 1, -2, -1, 1, &nargs, t, et);
}

static int pllua_typeinfo_array_fromtable(lua_State *L, int nt, int nte, int nd, int ndim, int *dims,
										  pllua_typeinfo *t, pllua_typeinfo *et)
{
	pllua_datum *newd = NULL;
	Datum *values;
	bool *isnull;
	int i;
	int nelems = 0;
	int lbs[MAXDIM];

	nt = lua_absindex(L, nt);
	nte = lua_absindex(L, nte);
	nd = lua_absindex(L, nd);

	if (ndim > 0)
	{
		int64 maxelem = (int64) ((Size) MaxAllocSize / sizeof(Datum));
		int64 tnelems = dims[0];
		lbs[0] = 1;
		for (i = 1; i < ndim; ++i)
		{
			if (dims[i] > maxelem / tnelems)
				luaL_error(L, "number of elements in array exceeds limit");
			tnelems *= dims[i];
			lbs[i] = 1;
		}
		if (tnelems > INT_MAX || tnelems > LUA_MAXINTEGER)
			luaL_error(L, "number of elements in array exceeds limit");
		nelems = tnelems;
	}

	if (nelems)
	{
		int ct;
		int curidx[MAXDIM];
		int topidx;
		/* construct a flat array of datum objects */
		lua_createtable(L, nelems, 0);
		ct = lua_gettop(L);
		/*
		 * stack looks like:
		 *  ct data data[i] data[i][j] ...
		 * beware that the data elements may be nil!
		 *
		 * topidx is the 0..(ndim-1) index of the topmost item on the stack,
		 * i.e. curidx[topidx] is the current index variable. (Or looked at
		 * another way, topidx = depth - 1)
		 */
		lua_pushvalue(L, nd);
		curidx[0] = 1;
		/*
		 * The topidx check in the loop condition serves no purpose except
		 * to silence a really annoying gcc warning
		 */
		for (topidx = 0, i = 1; i <= nelems && topidx >= 0; ++i)
		{
			while (topidx < ndim - 1)
			{
				if (!lua_isnil(L, -1))
					lua_geti(L, -1, curidx[topidx]);
				else
					lua_pushnil(L);
				curidx[++topidx] = 1;
			}

			if (!lua_isnil(L, -1))
				lua_geti(L, -1, curidx[topidx]);
			else
				lua_pushnil(L);
			lua_pushvalue(L, nte);
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			lua_seti(L, ct, i);

			while (topidx >= 0 && (++(curidx[topidx])) > dims[topidx])
			{
				--topidx;
				lua_pop(L, 1);
			}
		}
		lua_settop(L, ct);
	}

	newd = pllua_newdatum(L, nt, (Datum)0);

	PLLUA_TRY();
	{
		MemoryContext oldcontext;

		if (nelems == 0)
		{
			newd->value = PointerGetDatum(construct_empty_array(t->elemtype));
		}
		else
		{
			values = palloc(nelems * sizeof(Datum));
			isnull = palloc(nelems * sizeof(bool));
			for (i = 0; i < nelems; ++i)
			{
				pllua_datum *ed;
				lua_rawgeti(L, -2, i+1);
				if (lua_isnil(L, -1))
					isnull[i] = true;
				else
				{
					ed = (pllua_datum *) lua_touserdata(L, -1);
					Assert(ed);
					values[i] = ed->value;
					isnull[i] = false;
				}
				lua_pop(L, 1);
			}
			newd->value = PointerGetDatum(construct_md_array(values, isnull,
															 ndim, dims, lbs,
															 t->elemtype,
															 t->elemtyplen,
															 t->elemtypbyval,
															 t->elemtypalign));
			pfree(values);
			pfree(isnull);
		}

		oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		pllua_savedatum(L, newd, t);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

/*
 *
 */

static int pllua_typeinfo_row_call(lua_State *L)
{
	pllua_typeinfo *t = pllua_totypeinfo(L, 1);
	pllua_datum *newd;
	int nargs = lua_gettop(L) - 1;
	int argbase = 1;
	/*
	 * this is about 30kbytes of stack space on 64bit, but it's still much
	 * cleaner than messing with dynamic allocations.
	 */
	Datum values[MaxTupleAttributeNumber + 1];
	bool isnull[MaxTupleAttributeNumber + 1];
	int i;
	int argno = 0;
	TupleDesc tupdesc = t->tupdesc;
	Oid newoid = InvalidOid;

	PLLUA_CHECK_PG_STACK_DEPTH();

	if (nargs == 1)
	{
		if (lua_type(L, 2) == LUA_TTABLE
			|| lua_type(L, 2) == LUA_TUSERDATA)
		{
			if (!pllua_toanydatum(L, 2, NULL))
			{
				/*
				 * If it's not a datum, but it is a table or object, we assume it's
				 * something we can index by field name. (If the caller wants
				 * matching by number, they can do t(table.unpack(val)) instead.)
				 *
				 * We push the source values on the stack in the correct order and
				 * fall out to handle it below. typeinfo_push_from_table checks the
				 * stack depth.
				 */
				argbase = lua_gettop(L);
				nargs = pllua_typeinfo_push_from_table(L, 2, t);
			}
			else
				lua_pop(L, 1);
		}
	}

	if (nargs != t->arity)
		luaL_error(L, "incorrect number of arguments for type constructor (expected %d got %d)",
				   t->arity, nargs);

	for (argno = argbase, i = 0; i < nargs; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid coltype = att->atttypid;
		int32 coltypmod = att->atttypmod;
		pllua_datum *d = NULL;
		pllua_typeinfo *argt;

		values[i] = (Datum)-1;

		if (TupleDescAttr(t->tupdesc, i)->attisdropped)
		{
			isnull[i] = true;
			continue;
		}

		++argno;

		/* look up the element typeinfo in case we need it below */
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) coltype);
		if (coltype == RECORDOID)
			lua_pushinteger(L, (lua_Integer) coltypmod);
		else
			lua_pushnil(L);
		lua_call(L, 2, 1);
		argt = pllua_checktypeinfo(L, -1, false);

		/* nil? */
		if (lua_isnil(L, argno))
		{
			isnull[i] = true;
		}
		else
		{
			/* is it already a datum of the correct type? */
			d = pllua_todatum(L, argno, -1);
			if (!d || d->modified)
			{
				/* recursively construct an element datum */
				/* note that here is where most of the work happens */
				lua_pushvalue(L, -1);
				lua_pushvalue(L, argno);
				lua_call(L, 1, 1);
				/* replace result in stack and proceed */
				lua_replace(L, argno);
				d = pllua_todatum(L, argno, -1);
			}
			if (!d || d->modified)
				luaL_error(L, "inconsistency");
			values[i] = d->value;
			isnull[i] = false;
		}
		if (coltype != RECORDOID && coltypmod >= 0 && (!d || coltypmod != d->typmod))
			pllua_typeinfo_coerce_typmod(L, &values[i], &isnull[i], -1, argt, coltypmod);
		lua_pop(L,1);
	}

	newd = pllua_newdatum(L, 1, (Datum)0);

	PLLUA_TRY();
	{
		HeapTuple tuple = heap_form_tuple(t->tupdesc, values, isnull);
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		if (t->hasoid)
			HeapTupleSetOid(tuple, newoid);
		newd->value = heap_copy_tuple_as_datum(tuple, t->tupdesc);
		newd->need_gc = true;
		pfree(tuple);
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


/*
 * Type casting stuff
 *
 * We want to support these cases:
 *
 * scalar -> scalar via assignment cast
 * array -> array via assignment cast of elements
 * row -> row with element-wise assignment casts
 * row -> row with dropped columns or added cols
 *
 * The row cases are best handled by having a function to push the deform
 * elements compensating for added/dropped cols, and then leaving the rest to
 * the non-row cast cases.
 *
 * The typeconv table is indexed as { dest_typeinfo = func }, with func being
 * a closure over src and dest typeinfos and typically a pgfunc.
 */

/*
 * newdatum = f(olddatum) by calling a cast function
 *
 * upvalue 1 is src typeinfo
 * upvalue 2 is dst typeinfo
 * upvalue 3 is cast function oid (or InvalidOid for binary-compatible)
 * upvalue 4 is pgfunc
 * upvalue 5 is nil, or a pgfunc for the typmod cast fn
 *
 * If dst type is a domain, then dst typeinfo is the domain type, but the cast
 * function is to the domain's base type. For this case, we have to know
 * whether the cast function actually takes a typmod parameter, because if not,
 * we have to invoke the typmod cast separately.
 *
 * We rely on our setup code to supply a non-nil value for upvalue 5 as a flag
 * to indicate that the extra coercion is needed.
 */
static int
pllua_typeconv_scalar_coerce_func(lua_State *L)
{
	pllua_typeinfo *src_t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	pllua_typeinfo *dst_t = pllua_checktypeinfo(L, lua_upvalueindex(2), true);
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_datum *newd;
	volatile bool isnull_ret = false;
	Oid fnoid = (Oid) lua_tointeger(L, lua_upvalueindex(3));
	bool need_typmod = !lua_isnil(L, lua_upvalueindex(5));
	if (dst_t->modified || dst_t->obsolete)
		luaL_error(L, "cannot cast value to modified or obsolete type");

	newd = pllua_newdatum(L, lua_upvalueindex(2), (Datum)0);

	PLLUA_TRY();
	{
		Datum val = d->value;
		bool isnull = false;

		/*
		 * If it's an RW expanded datum, take the RO value instead to force
		 * making a copy rather than owning the original (which wouldn't help
		 * since we already own it).
		 */
		if (src_t->typlen == -1
			&& VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(val)))
		{
			val = EOHPGetRODatum(DatumGetEOHP(val));
		}

		if (OidIsValid(fnoid))
			pllua_typeinfo_raw_coerce(L, &val, &isnull,
									  lua_upvalueindex(4), fnoid,
									  (need_typmod) ? -1 : dst_t->basetypmod, false);
		if (need_typmod)
			pllua_typeinfo_raw_coerce(L, &val, &isnull,
									  lua_upvalueindex(5), dst_t->typmod_funcid,
									  dst_t->basetypmod, false);
		if (dst_t->basetype != dst_t->typeoid)
			domain_check(val, isnull, dst_t->typeoid, &dst_t->domain_extra, dst_t->mcxt);

		if (!isnull)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
			newd->value = val;
			pllua_savedatum(L, newd, dst_t);
			MemoryContextSwitchTo(oldcontext);
		}
		isnull_ret = isnull;
	}
	PLLUA_CATCH_RETHROW();

	if (isnull_ret)
		lua_pushnil(L);
	return 1;
}

/*
 * newdatum = f(olddatum) by IO conversions
 *
 * upvalue 1 is src typeinfo
 * upvalue 2 is dst typeinfo
 * upvalue 3 is dst base typeinfo
 */
static int
pllua_typeconv_scalar_coerce_via_io(lua_State *L)
{
	pllua_typeinfo *src_t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	pllua_typeinfo *dst_t = pllua_checktypeinfo(L, lua_upvalueindex(2), true);
	pllua_typeinfo *base_t = pllua_checktypeinfo(L, lua_upvalueindex(3), true);
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_datum *newd;
	volatile bool isnull_ret = false;
	if (dst_t->modified || dst_t->obsolete || base_t->modified || base_t->obsolete)
		luaL_error(L, "cannot cast value to modified or obsolete type");

	newd = pllua_newdatum(L, lua_upvalueindex(2), (Datum)0);

	PLLUA_TRY();
	{
		const char *str = pllua_typeinfo_raw_output(L, d->value, src_t);
		bool isnull = (str == NULL);

		/*
		 * Contra pg proper, we use the domain's typmod (if there is one) for
		 * the input function rather than a separate typmod coercion, because
		 * we're doing only implicit coercions and not explicit ones. This also
		 * means we don't need to special-case the "interval" type (which
		 * always needs its typmod).
		 */
		pllua_typeinfo_raw_input(L, &newd->value, base_t, str, dst_t->basetypmod);

		if (dst_t->basetype != dst_t->typeoid)
			domain_check(newd->value, isnull, dst_t->typeoid, &dst_t->domain_extra, dst_t->mcxt);

		if (str && !isnull)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
			pllua_savedatum(L, newd, dst_t);
			MemoryContextSwitchTo(oldcontext);
		}
		else
			isnull_ret = true;
	}
	PLLUA_CATCH_RETHROW();

	if (isnull_ret)
		lua_pushnil(L);
	return 1;
}

/*
 * newarraydatum = f(oldarraydatum) by calling a cast function or i/o conv
 *
 * upvalue 1 is src typeinfo
 * upvalue 2 is dst typeinfo
 * upvalue 3 is cast function oid, InvalidOid for i/o cast, or nil for no cast
 * upvalue 4 is pgfunc or nil
 * upvalue 5 is second pgfunc or nil
 */
static int
pllua_typeconv_array_coerce(lua_State *L)
{
	pllua_typeinfo *src_t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	pllua_typeinfo *dst_t = pllua_checktypeinfo(L, lua_upvalueindex(2), true);
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	pllua_datum *newd;
	bool isnull = false;
	bool binary_compat = lua_isnil(L, lua_upvalueindex(3));
	CoercionPathType elempath;
	Oid fnoid = (Oid) luaL_optinteger(L, lua_upvalueindex(3), InvalidOid);
	Oid fnoid2 = lua_isnil(L, lua_upvalueindex(5)) ? InvalidOid : dst_t->typmod_funcid;
	if (dst_t->modified || dst_t->obsolete)
		luaL_error(L, "cannot cast value to modified or obsolete type");

	Assert(src_t->is_array && dst_t->is_array);

	if (binary_compat)
		elempath = COERCION_PATH_RELABELTYPE;
	else if (!OidIsValid(fnoid))
		elempath = COERCION_PATH_COERCEVIAIO;
	else
		elempath = COERCION_PATH_FUNC;

	newd = pllua_newdatum(L, lua_upvalueindex(2), (Datum)0);

	PLLUA_TRY();
	{
		Datum val = d->value;

		pllua_typeinfo_raw_coerce_array(L, &val, &isnull, elempath,
										lua_upvalueindex(4), fnoid, lua_upvalueindex(5), fnoid2,
										src_t, dst_t, dst_t->basetypmod, false);

		if (dst_t->basetype != dst_t->typeoid)
			domain_check(val, isnull, dst_t->typeoid, &dst_t->domain_extra, dst_t->mcxt);

		if (!isnull)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
			newd->value = val;
			pllua_savedatum(L, newd, dst_t);
			MemoryContextSwitchTo(oldcontext);
		}
	}
	PLLUA_CATCH_RETHROW();

	if (isnull)
		lua_pushnil(L);
	return 1;
}

/*
 * newrowdatum = f(oldrowdatum) by deform/reform
 *
 * upvalue 1 is src typeinfo
 * upvalue 2 is dst typeinfo
 * upvalue 3 is string giving dropped att flags (or nil)
 */
static int
pllua_typeconv_row_coerce(lua_State *L)
{
	pllua_typeinfo *src_t = pllua_checktypeinfo(L, lua_upvalueindex(1), false);
	pllua_typeinfo *dst_t = pllua_checktypeinfo(L, lua_upvalueindex(2), true);
	pllua_datum *d = pllua_checkdatum(L, 1, lua_upvalueindex(1));
	size_t sz;
	const char *droplist = lua_tolstring(L, lua_upvalueindex(3), &sz);
	int nuv;
	int nargs;
	int i;
	Oid rowoid = InvalidOid;
	if (dst_t->modified || dst_t->obsolete)
		luaL_error(L, "cannot cast value to modified or obsolete type");

	/*
	 * Push all the exploded parts of the source tuple onto the stack. Watch
	 * out for booleans subbing for nulls/dropped cols though!
	 */
	luaL_checkstack(L, 10 + dst_t->arity, NULL);
	pllua_datum_deform_tuple(L, 1, d, src_t);
	nuv = lua_absindex(L, -1);

	lua_pushcfunction(L, pllua_typeinfo_row_call);
	lua_pushvalue(L, lua_upvalueindex(2));
	if (dst_t->hasoid && src_t->hasoid)
	{
		lua_getfield(L, nuv, "oid");
		rowoid = (Oid) lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	nargs = 0;
	for (i = 0; i < src_t->natts; ++i)
	{
		if (TupleDescAttr(src_t->tupdesc, i)->attisdropped)
			continue;
		if (droplist && droplist[i])
			continue;
		if (lua_geti(L, nuv, i+1) == LUA_TBOOLEAN)
		{
			/* we already skipped dropped cols so this must be a null */
			lua_pop(L, 1);
			lua_pushnil(L);
		}
		++nargs;
	}
	/* deal with added cols */
	Assert(nargs <= dst_t->arity);
	for (; nargs < dst_t->arity; ++nargs)
		lua_pushnil(L);
	lua_call(L, nargs+1, 1);

	{
		pllua_datum *newd = pllua_checkdatum(L, -1, lua_upvalueindex(2));

		if (dst_t->hasoid && src_t->hasoid)
			HeapTupleHeaderSetOid((HeapTupleHeader) DatumGetPointer(newd->value), rowoid);

		if (dst_t->basetype != dst_t->typeoid)
			domain_check(newd->value, false, dst_t->typeoid, &dst_t->domain_extra, dst_t->mcxt);
	}

	return 1;
}

/*
 * Raise error for unconvertible type
 *
 * upvalue 1 is the name of the source type, 2 the destination type
 */
static int
pllua_typeconv_error(lua_State *L)
{
	const char *srcname = lua_tostring(L, lua_upvalueindex(1));
	const char *dstname = lua_tostring(L, lua_upvalueindex(2));
	luaL_error(L, "cannot cast from type %s to %s",
			   (srcname ? srcname : "(unknown)"),
			   (dstname ? dstname : "(unknown)"));
	return 0;
}

/*
 * func = create(src_t,dst_t)
 *
 * This does the whole work of determining what type of conversion to apply,
 * creating the necessary closure, and entering it into the table.
 *
 * Returns no result if no cast possible
 */
static int
pllua_typeconv_create(lua_State *L)
{
	pllua_typeinfo *src_t = pllua_checktypeinfo(L, 1, false);
	pllua_typeinfo *dst_t = pllua_checktypeinfo(L, 2, true);
	Oid srctype = src_t->basetype;
	Oid dsttype = dst_t->basetype;
	if (dst_t->modified || dst_t->obsolete)
		luaL_error(L, "cannot cast value to modified or obsolete type");

	/*
	 * Don't look for cast functions for record->x or x->record, or for
	 * any source type which is modified or obsolete (since the cast will
	 * expect the newer form)
	 */
	if (src_t->natts < 0 &&
		dst_t->natts < 0 &&
		!src_t->modified &&
		!src_t->obsolete)
	{
		volatile CoercionPathType pathtype;
		volatile CoercionPathType elempathtype = COERCION_PATH_NONE;
		volatile Oid funcid;
		volatile bool typmod_arg = false;

		PLLUA_TRY();
		{
			Oid fnoid = InvalidOid;
			pathtype = find_coercion_pathway(dsttype, srctype, COERCION_ASSIGNMENT,
											 &fnoid);
			if (pathtype == COERCION_PATH_ARRAYCOERCE)
			{
				Assert(dst_t->is_array && src_t->is_array);
				elempathtype = find_coercion_pathway(dst_t->elemtype, src_t->elemtype,
													 COERCION_ASSIGNMENT, &fnoid);
				Assert(elempathtype != COERCION_PATH_NONE);
			}
			funcid = fnoid;

			if (OidIsValid(fnoid) && get_func_nargs(fnoid) > 1)
				typmod_arg = true;
		}
		PLLUA_CATCH_RETHROW();

		switch (pathtype)
		{
			case COERCION_PATH_NONE:
				break;
			case COERCION_PATH_RELABELTYPE:
				funcid = InvalidOid;
				/*FALLTHROUGH*/
			case COERCION_PATH_FUNC:
			case COERCION_PATH_ARRAYCOERCE:
				lua_pushvalue(L, 1);
				lua_pushvalue(L, 2);

				switch (elempathtype)
				{
					default:
						break;
					case COERCION_PATH_NONE:
						/* (non-array case) */
						lua_pushinteger(L, (lua_Integer) funcid);
						break;
					case COERCION_PATH_RELABELTYPE:
						lua_pushnil(L);
						break;
					case COERCION_PATH_FUNC:
						lua_pushinteger(L, (lua_Integer) funcid);
						break;
					case COERCION_PATH_COERCEVIAIO:
						lua_pushinteger(L, (lua_Integer) InvalidOid);
						break;
				}
				if (OidIsValid(funcid))
					pllua_pgfunc_new(L);
				else
					lua_pushnil(L);
				if (!typmod_arg && dst_t->basetypmod >= 0)
					pllua_pgfunc_new(L);
				else
					lua_pushnil(L);
				lua_pushcclosure(L,
								 ((pathtype == COERCION_PATH_ARRAYCOERCE)
								  ? pllua_typeconv_array_coerce
								  : pllua_typeconv_scalar_coerce_func),
								 5);
				return 1;
			case COERCION_PATH_COERCEVIAIO:
				lua_pushvalue(L, 1);
				lua_pushvalue(L, 2);
				if (dst_t->typeoid != dst_t->basetype)
					pllua_get_user_field(L, 2, "basetype");
				else
					lua_pushvalue(L, 2);
				lua_pushcclosure(L, pllua_typeconv_scalar_coerce_via_io, 3);
				return 1;

				lua_pushvalue(L, 1);
				lua_pushvalue(L, 2);
				if (!typmod_arg && dst_t->basetypmod >= 0)
					pllua_pgfunc_new(L);
				else
					lua_pushnil(L);
				lua_pushcclosure(L, pllua_typeconv_array_coerce, 5);
				return 1;
		}
	}
	/*
	 * if not found, try a row cast
	 *
	 * We don't expect all cases to work.
	 */
	if (src_t->natts >= 0 && dst_t->natts >= 0)
	{
		int i,j;
		int arity = 0;
		bool sametype = (src_t->basetype != RECORDOID && src_t->basetype == dst_t->basetype);
		char droplist[MaxTupleAttributeNumber + 1];
		bool need_droplist = false;

		memset(droplist, 0, src_t->natts * sizeof(char));
		for (i = 0, j = 0; i < src_t->natts && j < dst_t->natts; ++i)
		{
			Form_pg_attribute s_att = TupleDescAttr(src_t->tupdesc, i);
			Form_pg_attribute d_att = TupleDescAttr(dst_t->tupdesc, j);
			/*
			 * How we match up columns depends on the relationship between the
			 * two types.
			 *
			 * If both are RECORD, neither will have dropped cols, so we just
			 * line up the fields and expect it to work.
			 *
			 * If both are the same named type, they must be different
			 * versions, and the dest type must be the newer one (since we
			 * don't allow casting to old versions). So we expect dropped cols
			 * to match up except that the newer version might drop more cols,
			 * and it might also have new cols at the end.
			 *
			 * If they are unrelated types, we just line everything up.
			 */
			if (s_att->attisdropped)
			{
				/*
				 * If the source col is dropped, skip the dest col if it's also
				 * dropped, but only in the same-type case.
				 */
				if (sametype && d_att->attisdropped)
					++j;
				continue;
			}
			/*
			 * If the dest col is dropped, skip the source col in the sametype
			 * case; otherwise just skip the desc col (retrying the main loop
			 * with the same source col).
			 */
			if (d_att->attisdropped)
			{
				++j;
				if (sametype)
				{
					need_droplist = true;
					droplist[i] = 1;
					continue;
				}
				else
				{
					--i;
					continue;
				}
			}
			++arity;
		}
		/*
		 * If we didn't exhaust the source cols, then the source has higher
		 * arity, which we don't allow. Otherwise, make the closure.
		 */
		if (i == src_t->natts)
		{
			lua_pushvalue(L, 1);
			lua_pushvalue(L, 2);
			if (need_droplist)
				lua_pushlstring(L, droplist, src_t->natts);
			else
				lua_pushnil(L);
			lua_pushcclosure(L, pllua_typeconv_row_coerce, 3);
			return 1;
		}
	}

	/*
	 * If we didn't construct a cast, then create an error closure as a
	 * negative cache entry. Use the names of the types in the error.
	 */
	lua_getfield(L, 1, "name");
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	lua_getfield(L, 2, "name");
	lua_pushvalue(L, 2);
	lua_call(L, 1, 1);
	lua_pushcclosure(L, pllua_typeconv_error, 2);

	return 1;
}

/*
 * func = __index(tab,key)  where key is dest typeinfo
 *
 * source typeinfo is in upvalue 1
 */
static int
pllua_typeconv_index(lua_State *L)
{
	lua_settop(L, 2);
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushcfunction(L, pllua_typeconv_create);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 2);
	lua_call(L, 2, 1);
	if (!lua_isfunction(L, -1))
		luaL_error(L, "could not construct cast");
	/* stack: tab key val */
	lua_pushvalue(L, -1);
	lua_insert(L, -3);
	/* stack: tab val key val */
	lua_rawset(L, -4);
	return 1;
}

/*
 * Create a typeinfo table and store it in the table at "tabidx"
 * (under the key "typeinfo"). typeidx denotes the typeinfo object
 * over which we will close the index method.
 */
static void
pllua_typeconv_newtable(lua_State *L, int tabidx, int typeidx)
{
	pllua_new_weak_table(L, "k", "typeconv table");
	lua_pushvalue(L, typeidx);
	lua_pushcclosure(L, pllua_typeconv_index, 1);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	lua_setfield(L, tabidx, "typeconv");
}

static void
pllua_typeconv_register(lua_State *L, int tabidx, int typeidx)
{
	tabidx = lua_absindex(L, tabidx);
	typeidx = lua_absindex(L, typeidx);
	pllua_typeconv_newtable(L, tabidx, typeidx);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPECONV_REGISTRY);
	lua_pushvalue(L, tabidx);
	lua_pushvalue(L, typeidx);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

/*
 * invalidate(interp)
 */
int pllua_typeconv_invalidate(lua_State *L)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TYPECONV_REGISTRY);

	lua_pushnil(L);
	while (lua_next(L, -2))
	{
		pllua_typeconv_newtable(L, lua_absindex(L, -2), lua_absindex(L, -1));
		lua_pop(L, 1);
	}

	return 0;
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
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_funcs[] = {
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_package_mt[] = {
	{ "__index", pllua_typeinfo_package_index },
	{ "__call", pllua_typeinfo_package_call },
	{ NULL, NULL }
};

static struct luaL_Reg typeinfo_package_array_mt[] = {
	{ "__index", pllua_typeinfo_package_array_index },
	{ NULL, NULL }
};

int pllua_open_pgtype(lua_State *L)
{
#if LUAJIT_VERSION_NUM > 0 && !defined(NO_LUAJIT)
	if (luaL_loadstring(L, luajit_lua) == LUA_OK)
	{
		lua_call(L, 0, 2);
		lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_INT8HACK_OUTFUNC);
		lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_INT8HACK_INFUNC);
	}
	else
		lua_error(L);
#endif

	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECORDS);

	/*
	 * key-weak table for registering typeconv tables. This actually references
	 * the table _containing_ the typeconv, so that we can just replace the
	 * table with an empty one rather than having to delete its keys.
	 *
	 * The values are the source typeinfos, so those can be weak too.
	 */
	pllua_new_weak_table(L, "kv", "typeconv registry table");
	lua_pop(L, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TYPECONV_REGISTRY);

	pllua_newmetatable(L, PLLUA_IDXLIST_OBJECT, idxlist_mt);
	lua_pop(L, 1);

	pllua_newmetatable(L, PLLUA_TYPEINFO_OBJECT, typeinfo_mt);
	lua_newtable(L);
	luaL_setfuncs(L, typeinfo_methods, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_newtable(L);
	pllua_newmetatable(L, PLLUA_TYPEINFO_PACKAGE_OBJECT, typeinfo_package_mt);
	lua_setmetatable(L, -2);

	lua_newtable(L);
	pllua_newmetatable(L, PLLUA_TYPEINFO_PACKAGE_ARRAY_OBJECT, typeinfo_package_array_mt);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "array");

	luaL_setfuncs(L, typeinfo_funcs, 0);
	return 1;
}
