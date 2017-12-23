/* elog.c */

#include "pllua.h"

/*
 * pllua_elog
 *
 * Calling ereport with dynamic information is ugly. This needs a catch block
 * even for severity less than ERROR, because it could throw a memory error
 * while building the error object.
 */
static void
pllua_elog(lua_State *L,
		   int elevel,
		   bool hidecontext,
		   int e_code,
		   const char *e_message,
		   const char *e_detail,
		   const char *e_hint,
		   const char *e_column,
		   const char *e_constraint,
		   const char *e_datatype,
		   const char *e_table,
		   const char *e_schema)
{
	PLLUA_TRY();
	{
		ereport(elevel,
				(e_code ? errcode(e_code) : 0,
				 (hidecontext ? errhidecontext(true) : 0),
				 errmsg_internal("%s", e_message),
				 (e_detail != NULL) ? errdetail_internal("%s", e_detail) : 0,
				 (e_hint != NULL) ? errhint("%s", e_hint) : 0,
				 (e_column != NULL) ?
				 err_generic_string(PG_DIAG_COLUMN_NAME, e_column) : 0,
				 (e_constraint != NULL) ?
				 err_generic_string(PG_DIAG_CONSTRAINT_NAME, e_constraint) : 0,
				 (e_datatype != NULL) ?
				 err_generic_string(PG_DIAG_DATATYPE_NAME, e_datatype) : 0,
				 (e_table != NULL) ?
				 err_generic_string(PG_DIAG_TABLE_NAME, e_table) : 0,
				 (e_schema != NULL) ?
				 err_generic_string(PG_DIAG_SCHEMA_NAME, e_schema) : 0));
	}
	PLLUA_CATCH_RETHROW();
}

/*
 * Internal support for pllua_debug.
 */
void
pllua_debug_lua(lua_State *L, const char *msg, ...)
{
	luaL_Buffer b;
	char *buf;
	va_list va;

	luaL_buffinit(L, &b);
	buf = luaL_prepbuffer(&b);
	va_start(va, msg);
	vsnprintf(buf, LUAL_BUFFERSIZE, msg, va);
	va_end(va);
	luaL_addsize(&b, strlen(buf));
	luaL_pushresult(&b);
	msg = lua_tostring(L, -1);
	pllua_elog(L, DEBUG1, true, 0, msg, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	lua_pop(L, 1);
}

void
pllua_warning(lua_State *L, const char *msg, ...)
{
	luaL_Buffer b;
	char *buf;
	va_list va;

	luaL_buffinit(L, &b);
	buf = luaL_prepbuffer(&b);
	va_start(va, msg);
	vsnprintf(buf, LUAL_BUFFERSIZE, msg, va);
	va_end(va);
	luaL_addsize(&b, strlen(buf));
	luaL_pushresult(&b);
	msg = lua_tostring(L, -1);
	pllua_elog(L, WARNING, true, 0, msg, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	lua_pop(L, 1);
}

static void pllua_where(lua_State *L, int level)
{
	lua_Debug ar;
	lua_CFunction fn;
	luaL_checkstack(L, 3, NULL);
	while (lua_getstack(L, level, &ar))
	{
		lua_getinfo(L, "Slf", &ar);
		fn = lua_tocfunction(L, -1);
		lua_pop(L, 1);
		/*
		 * Treat these functions (all the possible args of
		 * pllua_initial_protected_call) as being barriers to error traceback.
		 */
		if (fn == pllua_resume_function ||
			fn == pllua_call_function ||
			fn == pllua_call_trigger ||
			fn == pllua_call_event_trigger ||
			fn == pllua_validate ||
			fn == pllua_call_inline)
			break;
		if (ar.currentline > 0)
		{
			lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
			return;
		}
		++level;
    }
	lua_pushfstring(L, "");  /* else, no information available... */
}

void pllua_error (lua_State *L, const char *fmt, ...)
{
	va_list argp;
	va_start(argp, fmt);
	pllua_where(L, 1);
	lua_pushvfstring(L, fmt, argp);
	va_end(argp);
	lua_concat(L, 2);
	lua_error(L);
	pg_unreachable();
}

/*
 * The use of errdepth is a bit subtle. The main lua stack is shared between
 * all non-SRF function calls, so if a lua function calls another via SPI, then
 * we want to scan only a small part of the stack each time.
 *
 * So errdepth starts out at 0 (it can only be left non-zero if something
 * errors out of the error context stack, and in that case the topmost catch
 * block will clean it up), and each traverse of the main stack stops when it
 * hits a top-level call and stores the current level back in errdepth for the
 * next context callback to use. However, if the activation we're looking at is
 * for a running SRF, then it's on its own private stack, so we ignore all this
 * and start at the stack top (the update_errdepth=false case).
 *
 * This does mean that the scan of the stack must not stop until it reaches a
 * barrier or runs out of stack; stopping when we find the frame of interest
 * for the error wouldn't leave the right errdepth for the next call.
 */
int
pllua_error_callback_location(lua_State *L)
{
	pllua_interpreter *interp = lua_touserdata(L, 1);
	lua_Debug *ar = &interp->ar;
	lua_CFunction fn;
	int level = interp->update_errdepth ? interp->errdepth : 1;
	bool found = false;

	while (lua_getstack(L, level, ar))
	{
		lua_getinfo(L, found ? "f" : "Slf", ar);
		fn = lua_tocfunction(L, -1);
		lua_pop(L, 1);
		/*
		 * Treat these functions (all the possible args of
		 * pllua_initial_protected_call) as being barriers to error traceback.
		 */
		if (fn == pllua_resume_function ||
			fn == pllua_call_function ||
			fn == pllua_call_trigger ||
			fn == pllua_call_event_trigger ||
			fn == pllua_validate ||
			fn == pllua_call_inline)
		{
			if (interp->update_errdepth)
			{
				/*
				 * If the barrier is actually the bottom frame on the stack,
				 * then we completed the traverse. Otherwise the next scan will
				 * start just below it.
				 */
				++level;
				if (!lua_getstack(L, level, ar))
					interp->errdepth = 0;
				else
					interp->errdepth = level;
			}
			return 0;
		}
		if (!found && ar->currentline > 0)
		{
			found = true;
		}
		++level;
    }
	if (!found)
		ar->currentline = 0;
	if (interp->update_errdepth)
		interp->errdepth = 0;
	return 0;
}

void
pllua_error_callback(void *arg)
{
	pllua_activation_record *act = arg;
	lua_State *thr = NULL;
	FunctionCallInfo fcinfo;
	int rc;

	if (!act)
		return;

	if (!act->interp)
	{
		errcontext("during PL/Lua interpreter setup");
		return;
	}
	if (pllua_context != PLLUA_CONTEXT_PG)
		return;  /* dangerous elog call! */
	thr = act->interp->L;
	fcinfo = act->fcinfo;
	if (fcinfo && fcinfo->flinfo && fcinfo->flinfo->fn_extra &&
		((pllua_func_activation *)(fcinfo->flinfo->fn_extra))->onstack)
	{
		thr = ((pllua_func_activation *)(fcinfo->flinfo->fn_extra))->thread;
		act->interp->update_errdepth = false;
	}
	else
		act->interp->update_errdepth = true;
	rc = pllua_cpcall(thr, pllua_error_callback_location, act->interp);
	if (rc == 0 && act->interp->ar.currentline > 0)
		errcontext("Lua function %s at line %d",
				   act->interp->ar.short_src, act->interp->ar.currentline);
}

static struct { const char *str; int val; } ecodes[] = {
#include "plerrcodes.h"
	{ NULL, 0 }
};

void
pllua_get_errcodes(lua_State *L, int nidx)
{
	int ncodes = sizeof(ecodes)/sizeof(ecodes[0]) - 1;
	int i;

	nidx = lua_absindex(L, nidx);

	for (i = 0; i < ncodes; ++i)
	{
		lua_pushstring(L, ecodes[i].str);
		lua_seti(L, nidx, ecodes[i].val);
	}
}

static int
pllua_get_sqlstate(lua_State *L, int tidx, const char *str)
{
	if (strlen(str) == 5
		&& strspn(str, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 5)
	{
		return MAKE_SQLSTATE(str[0], str[1], str[2], str[3], str[4]);
	}
	else
	{
		int code;

		lua_getfield(L, tidx, str);
		if (lua_isnil(L, -1))
		{
			if (lua_next(L, tidx)) /* already a nil on the stack */
			{
				lua_pop(L,2);
				return 0;		/* not found, table not empty */
			}
			else				/* table is empty so populate it */
			{
				int ncodes = sizeof(ecodes)/sizeof(ecodes[0]) - 1;
				int i;

				for (i = 0; i < ncodes; ++i)
				{
					lua_pushinteger(L, ecodes[i].val);
					lua_setfield(L, tidx, ecodes[i].str);
				}
			}

			lua_getfield(L, tidx, str);
		}

		code = lua_tointeger(L, -1);
		lua_pop(L, 1);
		return code;
	}
}

#if LUA_VERSION_NUM == 501
const char *luaL_tolstring(lua_State *L, int idx, size_t *len)
{
  if (!luaL_callmeta(L, idx, "__tostring")) {
    int t = lua_type(L, idx), tt = 0;
    char const* name = NULL;
    switch (t) {
      case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
      case LUA_TSTRING:
      case LUA_TNUMBER:
        lua_pushvalue(L, idx);
        break;
      case LUA_TBOOLEAN:
        if (lua_toboolean(L, idx))
          lua_pushliteral(L, "true");
        else
          lua_pushliteral(L, "false");
        break;
      default:
        tt = luaL_getmetafield(L, idx, "__name");
        name = (tt == LUA_TSTRING) ? lua_tostring(L, -1) : lua_typename(L, t);
        lua_pushfstring(L, "%s: %p", name, lua_topointer(L, idx));
        if (tt != LUA_TNIL)
          lua_replace(L, -2);
        break;
    }
  } else {
    if (!lua_isstring(L, -1))
      luaL_error(L, "'__tostring' must return a string");
  }
  return lua_tolstring(L, -1, len);
}
#endif

/*
 * User-visible global "print" function.
 *
 * This is installed in place of _G.print
 */
int
pllua_p_print(lua_State *L)
{
	int			nargs = lua_gettop(L); /* nargs */
	int			elevel = IsUnderPostmaster ? INFO : LOG;
	const char *s;
	luaL_Buffer b;
	int			i;

	luaL_buffinit(L, &b);

	for (i = 1; i <= nargs; i++)
	{
		if (i > 1) luaL_addchar(&b, '\t');
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	s = lua_tostring(L, -1);
	pllua_elog(L, elevel, true, 0, s,
			   NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	return 0;
}

/*
 * we accept:
 *
 *   error("message")
 *   error("sqlstate", "message", ["detail", ["hint"]])
 *   error{ sqlstate = ?, message = ?, detail = ?, hint = ?,
 *          column = ?, constraint = ?, datatype = ?,
 *          table = ?, schema = ? }
 *
 *   elog("error", ...)
 *
 * pllua_p_elog exists under multiple closures, with upvalues:
 *
 *  1: elevel integer, or nil for the elog() version
 *  2: table mapping "error", "notice" etc. to elevels
 *  3: table mapping error names to sqlstates (lazy init)
 */
static int
pllua_p_elog(lua_State *L)
{
	bool		is_elog = lua_isnil(L, lua_upvalueindex(1));
	int			elevel;
	int			e_code = 0;
	const char *e_message = NULL;
	const char *e_detail = NULL;
	const char *e_hint = NULL;
	const char *e_column = NULL;
	const char *e_constraint = NULL;
	const char *e_datatype = NULL;
	const char *e_table = NULL;
	const char *e_schema = NULL;

	if (is_elog)
	{
		lua_getfield(L, lua_upvalueindex(2), luaL_tolstring(L, 1, NULL));
		if (!lua_isinteger(L, -1))
			luaL_error(L, "unknown elevel for elog()");
		elevel = lua_tointeger(L, -1);
		lua_pop(L, 2);
		lua_remove(L, 1);
	}
	else
		elevel = lua_tointeger(L, lua_upvalueindex(1));

	if (lua_gettop(L) == 1
		&& lua_istable(L, 1))
	{
		int ss = lua_gettop(L);
		luaL_checkstack(L, 30, NULL);

		lua_getfield(L, 1, "sqlstate");
		if (!lua_isnil(L, -1))
			e_code = pllua_get_sqlstate(L, lua_upvalueindex(3), luaL_tolstring(L, -1, NULL));
		lua_getfield(L, 1, "message");
		if (!lua_isnil(L, -1))
			e_message = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "detail");
		if (!lua_isnil(L, -1))
			e_detail = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "hint");
		if (!lua_isnil(L, -1))
			e_hint = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "column");
		if (!lua_isnil(L, -1))
			e_column = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "constraint");
		if (!lua_isnil(L, -1))
			e_constraint = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "datatype");
		if (!lua_isnil(L, -1))
			e_datatype = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "table");
		if (!lua_isnil(L, -1))
			e_table = luaL_tolstring(L, -1, NULL);
		lua_getfield(L, 1, "schema");
		if (!lua_isnil(L, -1))
			e_schema = luaL_tolstring(L, -1, NULL);

		lua_settop(L, ss);
	}
	else
	{
		switch (lua_gettop(L))
		{
			case 1:
				e_message = luaL_tolstring(L, 1, NULL);
				break;
			case 4:
				e_hint = luaL_tolstring(L, 4, NULL);
				/*FALLTHROUGH*/
			case 3:
				e_detail = luaL_tolstring(L, 3, NULL);
				/*FALLTHROUGH*/
			case 2:
				e_message = luaL_tolstring(L, 2, NULL);
				e_code = pllua_get_sqlstate(L, lua_upvalueindex(3),
											luaL_tolstring(L, 1, NULL));
				break;

			default:
				luaL_error(L, "wrong number of parameters to elog");
		}
	}

	if (!e_message)
		e_message = "(no message given)";

	/*
	 * Demand consistency between elevel and sqlstate (ignore the sqlstate if
	 * mismatch). Categories 00, 01 and 02 are not errors, anything else is an
	 * error.
	 */
	switch (ERRCODE_TO_CATEGORY(e_code))
	{
		case MAKE_SQLSTATE('0','0','0','0','0'):
		case MAKE_SQLSTATE('0','1','0','0','0'):
		case MAKE_SQLSTATE('0','2','0','0','0'):
			if (elevel >= ERROR)
				e_code = 0;
			break;
		default:
			if (elevel < ERROR)
				e_code = 0;
			break;
	}

	pllua_elog(L, elevel, false, e_code, e_message,
			   e_detail, e_hint, e_column, e_constraint,
			   e_datatype, e_table, e_schema);
	return 0;
}


static struct { const char *str; int val; } elevels[] = {
	{ "debug", DEBUG1 },
	{ "log", LOG },
	{ "info", INFO },
	{ "notice", NOTICE },
	{ "warning", WARNING },
	{ "error", ERROR }
};

int
pllua_open_elog(lua_State *L)
{
	int i;
	int nlevels = sizeof(elevels)/sizeof(elevels[0]);
	int ncodes = sizeof(ecodes)/sizeof(ecodes[0]) - 1;

	lua_newtable(L);

	lua_pushnil(L);
	lua_createtable(L, 0, nlevels);
	for (i = 0; i < nlevels; ++i)
	{
		lua_pushinteger(L, elevels[i].val);
		lua_setfield(L, -2, elevels[i].str);
	}
	lua_createtable(L, 0, ncodes);

	for (i = 0; i < nlevels; ++i)
	{
		lua_pushinteger(L, elevels[i].val);
		lua_pushvalue(L, -3);
		lua_pushvalue(L, -3);
		lua_pushcclosure(L, pllua_p_elog, 3);
		lua_setfield(L, -5, elevels[i].str);
	}
	lua_pushcclosure(L, pllua_p_elog, 3);
	lua_pushvalue(L, -1);
	lua_setfield(L, -3, "elog");

	/*
	 * if we're in the postmaster, then preload the error table, by
	 * outputting a log message.
	 */
	if (!IsUnderPostmaster)
	{
		const char *ident = NULL;
		lua_pushstring(L, "log");
		lua_pushstring(L, "successful_completion");
		lua_pushstring(L, "PL/Lua preloaded in postmaster");
		lua_getglobal(L, "_PL_IDENT");
		ident = lua_tostring(L, -1);
		lua_pushfstring(L, "_PL_IDENT value is %s", (ident && *ident) ? ident : "empty");
		lua_remove(L, -2);
		lua_call(L, 4, 0);
	}
	else
		lua_pop(L, 1);

	return 1;
}
