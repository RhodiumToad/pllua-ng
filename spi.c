/* spi.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"

/*
 * Pushes three entries on the stack - beware!
 */
static void pllua_spi_alloc_argspace(lua_State *L, int nargs, Datum **values, char **isnull, Oid **argtypes)
{
	*values = lua_newuserdata(L, nargs * sizeof(Datum));
	*isnull = lua_newuserdata(L, nargs * sizeof(char));
	*argtypes = lua_newuserdata(L, nargs * sizeof(Oid));
}

static bool pllua_spi_enter(lua_State *L)
{
	bool readonly = pllua_get_cur_act_readonly(L);
	SPI_connect();
	return readonly;
}

static void pllua_spi_exit(lua_State *L)
{
	SPI_finish();
}

/*
 * This creates the result but does not copy the data into the proper memory
 * context; see pllua_spi_save_result for that.
 */
int pllua_spi_prepare_result(lua_State *L)
{
	SPITupleTable *tuptab = lua_touserdata(L, 1);
	lua_Integer nrows = lua_tointeger(L, 2);
	TupleDesc tupdesc = tuptab->tupdesc;
	lua_Integer i;

	pllua_newtypeinfo_raw(L, tupdesc->tdtypeid, tupdesc->tdtypmod, tupdesc);
	lua_createtable(L, nrows, 0);
	for (i = 0; i < nrows; ++i)
	{
		HeapTuple htup = tuptab->vals[i];
		HeapTupleHeader h = htup->t_data;
		pllua_datum *d;

		/* htup might be in on-disk format or datum format. Force datum format. */
		HeapTupleHeaderSetDatumLength(h, htup->t_len);
		HeapTupleHeaderSetTypeId(h, tupdesc->tdtypeid);
		HeapTupleHeaderSetTypMod(h, tupdesc->tdtypmod);

		lua_pushvalue(L, -2);
		d = pllua_newdatum(L);
		/* we intentionally do not detoast anything here, see savedatum */
		d->value = PointerGetDatum(h);
		/* stack: ... typeinfo table typeinfo datum */
		lua_rawseti(L, -3, i+1);
		lua_pop(L, 1);
	}

	return 2;
}

static void pllua_spi_save_result(lua_State *L, lua_Integer nrows)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
	pllua_typeinfo *t = *(void **)lua_touserdata(L, -2);
	lua_Integer i;

	/* we rely on the fact that rawgeti won't throw */

	for (i = 0; i < nrows; ++i)
	{
		pllua_datum *d;
		lua_rawgeti(L, -1, i+1);
		d = lua_touserdata(L, -1);
		pllua_savedatum(L, d, t);
		lua_pop(L,1);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * spi.execute(cmd, arg...)
 *
 */

static int pllua_spi_execute(lua_State *L)
{
	const char *str = lua_tostring(L, 1);
	int nargs = lua_gettop(L) - 1;
	int argbase = 2;
	Datum d_values[100];
	char d_isnull[100];
	Oid d_argtypes[100];
	Datum *values = d_values;
	char *isnull = d_isnull;
	Oid *argtypes = d_argtypes;
	volatile lua_Integer nrows = -1;
	int i;

	if (pllua_ending)
		luaL_error(L, "cannot call SPI during shutdown");

	if (nargs > 99)
		pllua_spi_alloc_argspace(L, nargs, &values, &isnull, &argtypes);

	for (i = 0; i < nargs; ++i)
	{
		switch (lua_type(L, argbase+i))
		{
			case LUA_TUSERDATA:
				{
					pllua_typeinfo *dt;
					pllua_datum *d = pllua_toanydatum(L, argbase+i, &dt);
					if (d)
					{
						argtypes[i] = dt->typeoid;
						isnull[i] = ' ';  /* ugh ugh ugh */
						values[i] = d->value;
						lua_pop(L, 1);
					}
					else
						luaL_error(L, "userdata parameter not accepted");
				}
				break;
			case LUA_TNUMBER:
				{
					int isint = 0;
					lua_Integer ival = lua_tointegerx(L, argbase+i, &isint);
					lua_Number fval = lua_tonumber(L, argbase+i);
					if (!isint)
					{
						argtypes[i] = FLOAT8OID;
						values[i] = Float8GetDatum(fval);
					}
					else if (ival >= PG_INT32_MIN && ival <= PG_INT32_MAX)
					{
						argtypes[i] = INT4OID;
						values[i] = Int32GetDatum(ival);
					}
					else
					{
						argtypes[i] = INT8OID;
						values[i] = Int64GetDatum(ival);
					}
					isnull[i] = ' ';
				}
				break;
			case LUA_TSTRING:
				{
					const char *str = lua_tostring(L, argbase+i);
					pllua_verify_encoding(L, str);
					argtypes[i] = UNKNOWNOID;
					values[i] = CStringGetDatum(str);
					isnull[i] = ' ';
				}
				break;
			case LUA_TBOOLEAN:
				{
					argtypes[i] = BOOLOID;
					values[i] = BoolGetDatum(lua_toboolean(L, argbase+i) != 0);
					isnull[i] = ' ';
				}
				break;
			case LUA_TNIL:
				{
					argtypes[i] = UNKNOWNOID;
					values[i] = (Datum)0;
					isnull[i] = 'n';
				}
				break;
			default:
				luaL_error(L, "spi: unsupported parameter type");
		}
	}

	PLLUA_TRY();
	{
		bool readonly = pllua_spi_enter(L);
		int rc;

		rc = SPI_execute_with_args(str, nargs, argtypes, values, isnull,
								   readonly, Min(LUA_MAXINTEGER, LONG_MAX) - 1);
		if (rc >= 0)
		{
			nrows = SPI_processed;
			if (SPI_tuptable)
			{
				BlessTupleDesc(SPI_tuptable->tupdesc);

				pllua_pushcfunction(L, pllua_spi_prepare_result);
				lua_pushlightuserdata(L, SPI_tuptable);
				lua_pushinteger(L, nrows);
				pllua_pcall(L, 2, 2, 0);

				pllua_spi_save_result(L, nrows);
			}
			else
				lua_pushinteger(L, nrows);
		}
		else
			elog(ERROR, "spi error: %s", SPI_result_code_string(rc));

		pllua_spi_exit(L);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}


static struct luaL_Reg spi_funcs[] = {
	{ "execute", pllua_spi_execute },
	{ NULL, NULL }
};

int pllua_open_spi(lua_State *L)
{
	lua_newtable(L);
	lua_pushvalue(L, -1);
	luaL_setfuncs(L, spi_funcs, 0);
	return 1;
}
