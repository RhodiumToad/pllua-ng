/* numeric.c */

#include "pllua.h"

#include "catalog/pg_type.h"
#include "utils/numeric.h"
#include "utils/builtins.h"

enum num_method_id {
	PLLUA_NUM_NONE = 0,

	/* dyadic, boolean result */
	PLLUA_NUM_EQ,
	PLLUA_NUM_LT,
	PLLUA_NUM_LE,

	/* dyadic */
	PLLUA_NUM_ADD,
	PLLUA_NUM_SUB,
	PLLUA_NUM_MUL,
	PLLUA_NUM_DIV,
	PLLUA_NUM_DIVT,
	PLLUA_NUM_MOD,
	PLLUA_NUM_POW,

	/* optional second numeric arg */
	PLLUA_NUM_LOG,
	PLLUA_NUM_LN,   /* change _LOG to this when arg omitted */

	/* optional second integer arg */
	PLLUA_NUM_ROUND,
	PLLUA_NUM_TRUNC,

	/* monadic but must ignore a second arg */
	PLLUA_NUM_UNM,

	/* monadic */
	PLLUA_NUM_ABS,
	PLLUA_NUM_CEIL,
	PLLUA_NUM_EXP,
	PLLUA_NUM_FLOOR,
	PLLUA_NUM_SIGN,
	PLLUA_NUM_SQRT,
	PLLUA_NUM_NOOP,

	/* monadic, boolean result */
	PLLUA_NUM_ISNAN
};


static bool pllua_numeric_guts(lua_State *L, pllua_datum *d, pllua_typeinfo *t,
							   Datum val1, Datum val2, int op, lua_Integer i2,
							   bool free_val1, bool free_val2)
{
	volatile Datum bool_res = (Datum)0;

	PLLUA_TRY();
	{
		Datum res;

		switch (op)
		{
			case PLLUA_NUM_ADD:
				res = DirectFunctionCall2(numeric_add, val1, val2);	break;
			case PLLUA_NUM_SUB:
				res = DirectFunctionCall2(numeric_sub, val1, val2);	break;
			case PLLUA_NUM_MUL:
				res = DirectFunctionCall2(numeric_mul, val1, val2);	break;
			case PLLUA_NUM_DIV:
				res = DirectFunctionCall2(numeric_div, val1, val2);	break;
			case PLLUA_NUM_DIVT:
				res = DirectFunctionCall2(numeric_div_trunc, val1, val2);	break;
			case PLLUA_NUM_MOD:
				res = DirectFunctionCall2(numeric_mod, val1, val2);	break;
			case PLLUA_NUM_POW:
				res = DirectFunctionCall2(numeric_power, val1, val2);	break;
			case PLLUA_NUM_UNM:
				res = DirectFunctionCall1(numeric_uminus, val1);	break;
			case PLLUA_NUM_EQ:
				res = DirectFunctionCall2(numeric_eq, val1, val2);	break;
			case PLLUA_NUM_LT:
				res = DirectFunctionCall2(numeric_lt, val1, val2);	break;
			case PLLUA_NUM_LE:
				res = DirectFunctionCall2(numeric_le, val1, val2);	break;
			case PLLUA_NUM_ABS:
				res = DirectFunctionCall1(numeric_abs, val1);		break;
			case PLLUA_NUM_CEIL:
				res = DirectFunctionCall1(numeric_ceil, val1);		break;
			case PLLUA_NUM_EXP:
				res = DirectFunctionCall1(numeric_exp, val1);		break;
			case PLLUA_NUM_FLOOR:
				res = DirectFunctionCall1(numeric_floor, val1);		break;
			case PLLUA_NUM_LOG:
				res = DirectFunctionCall2(numeric_log, val2, val1);	break;  /* yes, backwards */
			case PLLUA_NUM_LN:
				res = DirectFunctionCall1(numeric_ln, val1);		break;
			case PLLUA_NUM_ROUND:
				res = DirectFunctionCall2(numeric_round, val1, Int32GetDatum(i2));	break;
			case PLLUA_NUM_SIGN:
				res = DirectFunctionCall1(numeric_sign, val1);		break;
			case PLLUA_NUM_SQRT:
				res = DirectFunctionCall1(numeric_sqrt, val1);		break;
			case PLLUA_NUM_TRUNC:
				res = DirectFunctionCall2(numeric_trunc, val1, Int32GetDatum(i2));	break;
			case PLLUA_NUM_NOOP:
				res = DirectFunctionCall1(numeric_uplus, val1);		break;
			case PLLUA_NUM_ISNAN:
				res = numeric_is_nan(DatumGetNumeric(val1));	break;
		}

		if (d)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
			d->value = res;
			pllua_savedatum(L, d, t);
			MemoryContextSwitchTo(oldcontext);
		}
		else
			bool_res = res;

		if (free_val1)
			pfree(DatumGetPointer(val1));
		if (free_val2)
			pfree(DatumGetPointer(val2));
	}
	PLLUA_CATCH_RETHROW();

	return DatumGetBool(bool_res);
}

static Datum pllua_numeric_getarg(lua_State *L, int nd, pllua_datum *d,
								  int isint, lua_Integer ival, int isnum, lua_Number fval)
{
	if (d)
		return d->value;
	if (isint)
	{
		int64 i = ival;
		return DirectFunctionCall1(int8_numeric, Int64GetDatumFast(i));
	}
	if (isnum)
	{
		float8 f = fval;
		return DirectFunctionCall1(float8_numeric, Float8GetDatumFast(f));
	}
	luaL_argcheck(L, false, nd, "not convertible to any number");
	return (Datum)0;
}

/*
 * upvalue 1 is the numeric typeinfo object, 2 the opcode
 */
static int pllua_numeric_handler(lua_State *L)
{
	int op = lua_tointeger(L, lua_upvalueindex(2));
	pllua_typeinfo *t = *pllua_checkrefobject(L, lua_upvalueindex(1), PLLUA_TYPEINFO_OBJECT);
	pllua_datum *d;
	pllua_datum *d1 = pllua_todatum(L, 1, lua_upvalueindex(1));
	pllua_datum *d2 = pllua_todatum(L, 2, lua_upvalueindex(1));
	int isint1 = 0;
	int isint2 = 0;
	lua_Integer i1 = lua_tointegerx(L, 1, &isint1);
	lua_Integer i2 = lua_tointegerx(L, 2, &isint2);
	int isnum1 = 0;
	int isnum2 = 0;
	lua_Number n1 = lua_tonumberx(L, 1, &isnum1);
	lua_Number n2 = lua_tonumberx(L, 2, &isnum2);
	Datum val1;
	Datum val2;
	bool free_val1 = !d1;
	bool free_val2 = !d2;

	if (op < PLLUA_NUM_LOG)
	{
		val1 = pllua_numeric_getarg(L, 1, d1, isint1, i1, isnum1, n1);
		val2 = pllua_numeric_getarg(L, 2, d2, isint2, i2, isnum2, n2);
	}
	else if (op == PLLUA_NUM_LOG)
	{
		val1 = pllua_numeric_getarg(L, 1, d1, isint1, i1, isnum1, n1);
		if (lua_isnone(L, 2))
		{
			op = PLLUA_NUM_LN;
			free_val2 = false;
		}
		else
			val2 = pllua_numeric_getarg(L, 2, d2, isint2, i2, isnum2, n2);
	}
	else if (op < PLLUA_NUM_UNM)
	{
		val1 = pllua_numeric_getarg(L, 1, d1, isint1, i1, isnum1, n1);
		luaL_argcheck(L, (lua_isnone(L, 2) || isint2), 2, NULL);
		free_val2 = false;
	}
	else if (op < PLLUA_NUM_ABS)
	{
		val1 = pllua_numeric_getarg(L, 1, d1, isint1, i1, isnum1, n1);
		free_val2 = false;
	}
	else
	{
		val1 = pllua_numeric_getarg(L, 1, d1, isint1, i1, isnum1, n1);
		luaL_argcheck(L, (lua_isnone(L, 2)), 2, "none expected");
		free_val2 = false;
	}

	if (op >= PLLUA_NUM_ADD && op < PLLUA_NUM_ISNAN)
	{
		lua_pushvalue(L, lua_upvalueindex(1));
		d = pllua_newdatum(L);
		pllua_numeric_guts(L, d, t, val1, val2, op, i2, free_val1, free_val2);
	}
	else
	{
		lua_pushboolean(L, pllua_numeric_guts(L, NULL, NULL, val1, val2, op, i2, free_val1, free_val2));
	}
	return 1;
}

/*
 * upvalue 1 is the numeric typeinfo object, 2 is mininteger datum, 3 is maxinteger datum
 */
static int pllua_numeric_tointeger(lua_State *L)
{
	pllua_datum *d1 = pllua_todatum(L, 1, lua_upvalueindex(1));
	pllua_datum *dmin = pllua_todatum(L, lua_upvalueindex(2), lua_upvalueindex(1));
	pllua_datum *dmax = pllua_todatum(L, lua_upvalueindex(3), lua_upvalueindex(1));
	int isint1 = 0;

	lua_tointegerx(L, 1, &isint1);
	if (isint1)
	{
		lua_pushvalue(L, 1);
		return 1;
	}
	if (!d1)
	{
		luaL_argcheck(L, lua_isnumber(L, 1), 1, "number");
		lua_pushnil(L);
		return 1;
	}

	PLLUA_TRY();
	{
		bool res_isnil = true;

		if (!DatumGetBool(DirectFunctionCall2(numeric_lt, d1->value, dmin->value))
			&& !DatumGetBool(DirectFunctionCall2(numeric_gt, d1->value, dmax->value))
			&& !numeric_is_nan(DatumGetNumeric(d1->value)))
		{
			int64 val = DatumGetInt64(DirectFunctionCall1(numeric_int8, d1->value));
			Datum check = DirectFunctionCall1(int8_numeric, Int64GetDatumFast(val));
			if (DatumGetBool(DirectFunctionCall2(numeric_eq, d1->value, check)))
			{
				lua_pushinteger(L, (lua_Integer) val);  /* already range checked */
				res_isnil = false;
			}
			pfree(DatumGetPointer(check));
		}

		if (res_isnil)
			lua_pushnil(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

/*
 * upvalue 1 is the numeric typeinfo object, 2 is mininteger datum, 3 is maxinteger datum
 */
static int pllua_numeric_tonumber(lua_State *L)
{
	pllua_datum *d1 = pllua_todatum(L, 1, lua_upvalueindex(1));
	pllua_datum *dmin = pllua_todatum(L, lua_upvalueindex(2), lua_upvalueindex(1));
	pllua_datum *dmax = pllua_todatum(L, lua_upvalueindex(3), lua_upvalueindex(1));

	if (!d1)
	{
		luaL_argcheck(L, lua_isnumber(L, 1), 1, "number");
		lua_pushvalue(L, 1);
		return 1;
	}

	PLLUA_TRY();
	{
		bool done = false;

		if (!DatumGetBool(DirectFunctionCall2(numeric_lt, d1->value, dmin->value))
			&& !DatumGetBool(DirectFunctionCall2(numeric_gt, d1->value, dmax->value))
			&& !numeric_is_nan(DatumGetNumeric(d1->value)))
		{
			int64 val = DatumGetInt64(DirectFunctionCall1(numeric_int8, d1->value));
			Datum check = DirectFunctionCall1(int8_numeric, Int64GetDatumFast(val));
			if (DatumGetBool(DirectFunctionCall2(numeric_eq, d1->value, check)))
			{
				lua_pushinteger(L, (lua_Integer) val);  /* already range checked */
				done = true;
			}
			pfree(DatumGetPointer(check));
		}
		if (!done)
		{
			float8 val = DatumGetFloat8(DirectFunctionCall1(numeric_float8, d1->value));
			lua_pushnumber(L, (lua_Number) val);
		}
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static struct { const char *name; enum num_method_id id; } numeric_meta[] = {
	{ "__add", PLLUA_NUM_ADD },
	{ "__sub", PLLUA_NUM_SUB },
	{ "__mul", PLLUA_NUM_MUL },
	{ "__div", PLLUA_NUM_DIV },
	{ "__idiv", PLLUA_NUM_DIVT },
	{ "__mod", PLLUA_NUM_MOD },
	{ "__pow", PLLUA_NUM_POW },
	{ "__unm", PLLUA_NUM_UNM },
	{ "__eq", PLLUA_NUM_EQ },
	{ "__lt", PLLUA_NUM_LT },
	{ "__le", PLLUA_NUM_LE },
	{ NULL, PLLUA_NUM_NONE }
};

static struct { const char *name; enum num_method_id id; } numeric_methods[] = {
	{ "abs", PLLUA_NUM_ABS },
	{ "ceil", PLLUA_NUM_CEIL },
	{ "equal", PLLUA_NUM_EQ },
	{ "exp", PLLUA_NUM_EXP },
	{ "floor", PLLUA_NUM_FLOOR },
	{ "isnan", PLLUA_NUM_ISNAN },
	{ "log", PLLUA_NUM_LOG },
	{ "new", PLLUA_NUM_NOOP },
	{ "round", PLLUA_NUM_ROUND },
	{ "sign", PLLUA_NUM_SIGN },
	{ "sqrt", PLLUA_NUM_SQRT },
	{ "trunc", PLLUA_NUM_TRUNC },
	{ "to", PLLUA_NUM_NOOP },
	{ NULL, PLLUA_NUM_NONE }
};

static luaL_Reg numeric_plain_methods[] = {
	{ "tointeger", pllua_numeric_tointeger },
	{ "tonumber", pllua_numeric_tonumber },
	{ NULL, NULL }
};

int pllua_open_numeric(lua_State *L)
{
	int i;

	StaticAssertStmt(LUA_MAXINTEGER <= PG_INT64_MAX, "lua_Integer type is too big");
	StaticAssertStmt(sizeof(lua_Number) <= sizeof(float8), "lua_Number type is too big");

	lua_settop(L, 0);
	lua_newtable(L);  /* module table at index 1 */
	lua_pushcfunction(L, pllua_typeinfo_lookup);
	lua_pushinteger(L, NUMERICOID);
	lua_call(L, 1, 1); /* typeinfo at index 2 */
	lua_getuservalue(L, 2);  /* datum metatable at index 3 */
	for (i = 0; numeric_methods[i].name; ++i)
	{
		lua_pushvalue(L, 2);
		lua_pushinteger(L, numeric_methods[i].id);
		lua_pushcclosure(L, pllua_numeric_handler, 2);
		lua_setfield(L, 1, numeric_methods[i].name);
	}
	for (i = 0; numeric_meta[i].name; ++i)
	{
		lua_pushvalue(L, 2);
		lua_pushinteger(L, numeric_meta[i].id);
		lua_pushcclosure(L, pllua_numeric_handler, 2);
		lua_setfield(L, 3, numeric_meta[i].name);
	}
	/* override normal datum __index entry with our method table */
	lua_pushvalue(L, 1);
	lua_setfield(L, 3, "__index");

	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_getfield(L, 1, "to");
	lua_pushinteger(L, LUA_MININTEGER);
	lua_call(L, 1, 1);
	lua_pushvalue(L, -1);
	lua_setfield(L, 1, "mininteger");
	lua_getfield(L, 1, "to");
	lua_pushinteger(L, LUA_MAXINTEGER);
	lua_call(L, 1, 1);
	lua_pushvalue(L, -1);
	lua_setfield(L, 1, "maxinteger");
	luaL_setfuncs(L, numeric_plain_methods, 3);
	lua_pop(L, 1);

	lua_pushvalue(L, 1);
	return 1;
}
