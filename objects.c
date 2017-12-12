/* objects.c */

/*
 * Misc. lua object handling stuff.
 */

#include "pllua.h"

/*
 * True if object at index "nd" is of "objtype"
 *
 * Unlike luaL_*, this uses lightuserdata keys rather than strings.
 */
bool pllua_isobject(lua_State *L, int nd, char *objtype)
{
	if (lua_type(L, nd) != LUA_TUSERDATA)
		return false;
	if (!lua_getmetatable(L, nd))
		return false;
	lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
	if (!lua_rawequal(L, -1, -2))
	{
		lua_pop(L, 2);
		return false;
	}
	lua_pop(L, 2);
	return true;
}


void pllua_newmetatable(lua_State *L, char *objtype, luaL_Reg *mt)
{
	lua_newtable(L);
	luaL_setfuncs(L, mt, 0);
	lua_pushstring(L, objtype);
	lua_setfield(L, -2, "__name");
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, objtype);
}


/*
 * Reference objects hold only a pointer in the Lua object which points to the
 * real data. The __gc method for each object type is responsible for freeing
 * the real data.
 */

/*
 * pllua_get_memory_cxt
 *
 * Get the memory context associated with the interpreter.
 */
MemoryContext pllua_get_memory_cxt(lua_State *L)
{
	void *p;

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	p = lua_touserdata(L, -1);
	lua_pop(L, 1);

	return (MemoryContext) p;
}

/*
 * Create a refobj of the specified type and value (which may be NULL)
 *
 * Optionally create a table and put it in the uservalue slot.
 *
 * Leaves the object on the stack, and returns the pointer to the pointer slot.
 */
void **pllua_newrefobject(lua_State *L, char *objtype, void *value, bool uservalue)
{
	void **p = lua_newuserdata(L, sizeof(void*));
	*p = value;
	if (objtype)
	{
		int t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
		Assert(t == LUA_TTABLE);
		lua_setmetatable(L, -2);
	}
	if (uservalue || MANDATORY_USERVALUE)
	{
		lua_newtable(L);
		lua_setuservalue(L, -2);
	}
	return p;
}

void **pllua_torefobject(lua_State *L, int nd, char *objtype)
{
	void *p = lua_touserdata(L, nd);
	if (p != NULL)
	{
		if (lua_getmetatable(L, nd))
		{
			lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
			if (!lua_rawequal(L, -1, -2))
				p = NULL;
			lua_pop(L, 2);
			return p;
		}
	}
	return NULL;
}

/*
 * This is a non-ref object, the data is stored directly in lua.
 *
 * Optionally, we create a table and put it in the uservalue slot.
 */
void *pllua_newobject(lua_State *L, char *objtype, size_t sz, bool uservalue)
{
	void *p = lua_newuserdata(L, sz);
	memset(p, 0, sz);
	if (objtype)
	{
		int t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
		Assert(t == LUA_TTABLE);
		lua_setmetatable(L, -2);
	}
	if (uservalue || MANDATORY_USERVALUE)
	{
		lua_newtable(L);
		lua_setuservalue(L, -2);
	}
	return p;
}

void *pllua_toobject(lua_State *L, int nd, char *objtype)
{
	void *p = lua_touserdata(L, nd);
	if (p != NULL)
	{
		if (lua_getmetatable(L, nd))
		{
			lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
			if (!lua_rawequal(L, -1, -2))
				p = NULL;
			lua_pop(L, 2);
			return p;
		}
	}
	return NULL;
}

void pllua_type_error(lua_State *L, char *expected)
{
	luaL_error(L, "wrong parameter type (expected %s)", expected);
}

void **pllua_checkrefobject(lua_State *L, int nd, char *objtype)
{
	void **p = pllua_torefobject(L, nd, objtype);
	if (!p || !*p)
		luaL_argerror(L, nd, objtype);
	return p;
}

void *pllua_checkobject(lua_State *L, int nd, char *objtype)
{
	void *p = pllua_toobject(L, nd, objtype);
	if (!p)
		pllua_type_error(L, objtype);
	return p;
}

/*
 * Activation objects represent a function call site (flinfo).
 *
 * We hang them off flinfo->fn_extra, and ensure that they aren't prematurely
 * freed by keeping a registry of them (in a table in the lua registry).
 *
 * But to ensure they actually do get freed, we use the memory context shutdown
 * callback of the memory context that the flinfo itself is in.
 *
 * Activations can also reference the thread of a set-returning function. In
 * this case, we need to reset things in the event of a rescan of the context.
 */
int pllua_freeactivation(lua_State *L)
{
	pllua_func_activation *act = lua_touserdata(L, 1);

	act->dead = true;
	/*
	 * These are allocated in the memory context that's going away, so forget
	 * they exist
	 */
	act->argtypes = NULL;
	act->tupdesc = NULL;

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	lua_pushnil(L);
	lua_rawsetp(L, -2, act);
	lua_pop(L, 1);

	return 0;
}

static void pllua_freeactivation_cb(void *arg)
{
	pllua_func_activation *act = arg;
	lua_State *L = act->L;

	/*
	 * we got here from pg, in a memory context reset. Since we shouldn't ever
	 * have allowed ourselves far enough into pg for that to happen while in
	 * lua context, assert that fact.
	 */
	Assert(pllua_context == PLLUA_CONTEXT_PG);
	/*
	 * we'd better ignore any (unlikely) lua error here, since that's safer
	 * than raising an error into pg
	 */
	if (pllua_cpcall(L, pllua_freeactivation, act))
		pllua_poperror(L);
}

int pllua_resetactivation(lua_State *L)
{
	int opos = lua_gettop(L) - 1;
	pllua_func_activation *act = lua_touserdata(L, -1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
	{
		elog(WARNING, "failed to find an activation: %p", act);
		return 0;
	}

	pllua_checkobject(L, -1, PLLUA_ACTIVATION_OBJECT);

	act->thread = NULL;
	lua_getuservalue(L, -1);
	lua_pushnil(L);
	lua_rawsetp(L, -2, PLLUA_THREAD_MEMBER);
	lua_settop(L, opos);

	return 0;
}

static void pllua_resetactivation_cb(Datum arg)
{
	pllua_func_activation *act = (void*) DatumGetPointer(arg);
	lua_State *L = act->L;
	int rc;

	/*
	 * we got here from pg, in an expr context reset. Since we shouldn't ever
	 * have allowed ourselves far enough into pg for that to happen while in
	 * lua context, assert that fact.
	 */
	Assert(pllua_context == PLLUA_CONTEXT_PG);

	rc = pllua_cpcall(L, pllua_resetactivation, act);
	if (rc)
		pllua_rethrow_from_lua(L, rc);
}

/*
 * Create a new activation object with the specified memory context. The
 * function is not filled in; a later setactivation must do that. The
 * activation is interned as valid.
 */
int pllua_newactivation(lua_State *L)
{
	MemoryContext mcxt = lua_touserdata(L, 1);
	pllua_func_activation *act = pllua_newobject(L, PLLUA_ACTIVATION_OBJECT,
												 sizeof(pllua_func_activation), true);

	act->func_info = NULL;
	act->thread = NULL;
	act->resolved = false;
	act->rettype = InvalidOid;
	act->tupdesc = NULL;

	act->interp = pllua_getinterpreter(L);
	act->L = L;
	act->cb.func = pllua_freeactivation_cb;
	act->cb.arg = act;
	act->dead = false;

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	lua_pushvalue(L, -2);
	lua_rawsetp(L, -2, act);
	lua_pop(L, 1);

	/* this can't throw a pg error, thankfully */
	MemoryContextRegisterResetCallback(mcxt, &act->cb);

	return 1;
}

/*
 * Update the activation to point to a new function (e.g. after a recompile).
 * Returns nothing.
 */
int pllua_setactivation(lua_State *L)
{
	pllua_func_activation *act = lua_touserdata(L, 1);
	void **p = pllua_checkrefobject(L, 2, PLLUA_FUNCTION_OBJECT);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
	{
		elog(WARNING, "failed to find an activation: %p", act);
		return 0;
	}

	pllua_checkobject(L, -1, PLLUA_ACTIVATION_OBJECT);

	Assert(act->thread == NULL);
	act->func_info = (*p);
	act->resolved = false; /* need to re-resolve types */

	lua_getuservalue(L, -1);
	lua_pushvalue(L, 2);
	lua_rawsetp(L, -2, PLLUA_FUNCTION_MEMBER);

	return 0;
}

/*
 * Get an activation object given its address.
 */
void pllua_getactivation(lua_State *L, pllua_func_activation *act)
{
	ASSERT_PG_CONTEXT;
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
		elog(ERROR, "failed to find an activation: %p", act);
	lua_remove(L, -2);
}

int pllua_activation_getfunc(lua_State *L)
{
	lua_getuservalue(L, -1);
	lua_rawgetp(L, -1, PLLUA_FUNCTION_MEMBER);
	lua_getuservalue(L, -1);
	lua_rawgetp(L, -1, PLLUA_FUNCTION_MEMBER);
	lua_insert(L, -4);
	lua_pop(L, 3);
	return 1;
}

FmgrInfo *pllua_get_cur_flinfo(lua_State *L)
{
	pllua_activation_record *pact = &(pllua_getinterpreter(L)->cur_activation);
	return pact->fcinfo ? pact->fcinfo->flinfo : NULL;
}

int pllua_get_cur_act(lua_State *L)
{
	FmgrInfo *flinfo = pllua_get_cur_flinfo(L);
	pllua_func_activation *act;

	act = (flinfo) ? flinfo->fn_extra : NULL;
	if (!act)
		return 0;
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
		luaL_error(L, "activation not found: %p", act);
	lua_remove(L, -2);
	return 1;
}

/*
 * This one doesn't throw unless the activation is actually invalid; if we're
 * in DO-block context, we're not readonly.
 */
bool pllua_get_cur_act_readonly(lua_State *L)
{
	FmgrInfo *flinfo = pllua_get_cur_flinfo(L);
	pllua_func_activation *act;

	act = (flinfo) ? flinfo->fn_extra : NULL;
	if (!act)
		return false;
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
		luaL_error(L, "activation not found: %p", act);
	lua_pop(L, 2);
	return act->readonly;
}

static int pllua_dump_activation(lua_State *L)
{
	pllua_func_activation *act = pllua_checkobject(L, 1, PLLUA_ACTIVATION_OBJECT);
	luaL_Buffer b;
	char *buf;
	int i;

	luaL_buffinit(L, &b);
	buf = luaL_prepbuffer(&b);
	snprintf(buf, LUAL_BUFFERSIZE,
			 "%s"
			 "func_info: %p  thread: %p  "
			 "resolved: %d  polymorphic: %d  variadic_call: %d  retset: %d  "
			 "rettype: %u  tupdesc: %p  typefuncclass: %d  "
			 "nargs: %d  argtypes:",
			 (act->dead) ? "DEAD " : "",
			 act->func_info, act->thread,
			 (int) act->resolved, (int) act->polymorphic, (int) act->variadic_call,
			 (int) act->retset,
			 (unsigned) act->rettype, act->tupdesc, (int) act->typefuncclass,
			 act->nargs);
	luaL_addsize(&b, strlen(buf));

	if (!act->dead && act->argtypes)
	{
		for (i = 0; i < act->nargs; ++i)
		{
			buf = luaL_prepbuffer(&b);
			snprintf(buf, LUAL_BUFFERSIZE, " %u", (unsigned) act->argtypes[i]);
			luaL_addsize(&b, strlen(buf));
		}
	}
	else if (!act->dead)
		luaL_addstring(&b, " (null)");

	luaL_pushresult(&b);
	return 1;
}

/*
 * nd is the stack index of an activation object, which should not already have
 * a thread, which needs to be registered in the econtext and have a thread
 * allocated to it.
 */
lua_State *pllua_activate_thread(lua_State *L, int nd, ExprContext *econtext)
{
	pllua_func_activation *act = pllua_toobject(L, nd, PLLUA_ACTIVATION_OBJECT);
	lua_State *newthread = NULL;

	ASSERT_LUA_CONTEXT;

	Assert(act->thread == NULL);

	PLLUA_TRY();
	{
		RegisterExprContextCallback(econtext,
									pllua_resetactivation_cb,
									PointerGetDatum(act));
	}
	PLLUA_CATCH_RETHROW();

	lua_getuservalue(L, nd);
	newthread = lua_newthread(L);
	act->thread = newthread;
	lua_rawsetp(L, -2, PLLUA_THREAD_MEMBER);
	lua_pop(L, 1);

	return newthread;
}

/*
 * act is an activation object which needs to be deregistered
 * in the econtext and have its thread released
 */
void pllua_deactivate_thread(lua_State *L, pllua_func_activation *act, ExprContext *econtext)
{
	Assert(act->thread != NULL);

	PLLUA_TRY();
	{
		UnregisterExprContextCallback(econtext,
									  pllua_resetactivation_cb,
									  PointerGetDatum(act));
	}
	PLLUA_CATCH_RETHROW();

	lua_pushlightuserdata(L, act);
	pllua_resetactivation(L);
}

/*
 * Function objects are refobjects containing cached function info.
 *
 * The uservalue slot of the object contains the actual Lua function.
 */
static void pllua_destroy_funcinfo(lua_State *L, pllua_function_info *obj)
{
	PLLUA_TRY();
	{
		/*
		 * funcinfo is allocated in its own memory context (since we expect it
		 * to have stuff dangling off), so free it by destroying that.
		 */
		MemoryContextDelete(obj->mcxt);
	}
	PLLUA_CATCH_RETHROW();
}

static int pllua_funcobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_FUNCTION_OBJECT);
	void *obj = p ? *p : NULL;
	if (p)
		return 0;
	ASSERT_LUA_CONTEXT;
	*p = NULL;
	if (obj)
		pllua_destroy_funcinfo(L, obj);
	return 0;
}

/*
 * metatables for objects and global functions
 */

static struct luaL_Reg funcobj_mt[] = {
	{ "__gc", pllua_funcobject_gc },
	{ NULL, NULL }
};

static struct luaL_Reg actobj_mt[] = {
	{ "__tostring", pllua_dump_activation },
	{ NULL, NULL }
};

static struct luaL_Reg serverdebugfuncs[] = {
	{ "act", pllua_get_cur_act },
	{ NULL, NULL }
};

static struct luaL_Reg serverfuncs[] = {
	{ NULL, NULL }
};

static struct luaL_Reg globfuncs[] = {
	{ "print", pllua_p_print },
	{ NULL, NULL }
};

void pllua_init_objects(lua_State *L, bool trusted)
{
	pllua_newmetatable(L, PLLUA_FUNCTION_OBJECT, funcobj_mt);
	pllua_newmetatable(L, PLLUA_ACTIVATION_OBJECT, actobj_mt);
	lua_pop(L, 2);
	luaL_requiref(L, "pllua.pgtype", pllua_open_pgtype, 0);
	lua_pushvalue(L, -1);
	lua_setglobal(L, "pgtype");
}

static int pllua_open_debugfuncs(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, serverdebugfuncs, 0);
	return 1;
}

static int pllua_open_serverfuncs(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, serverfuncs, 0);
	return 1;
}

void pllua_init_functions(lua_State *L, bool trusted)
{
	lua_pushglobaltable(L);
	luaL_setfuncs(L, globfuncs, 0);
	lua_pop(L, 1);

	luaL_requiref(L, "pllua.dbg", pllua_open_debugfuncs, 0);
	lua_pop(L, 1);
	luaL_requiref(L, "pllua.server", pllua_open_serverfuncs, 0);
	lua_setglobal(L, "server");
	luaL_requiref(L, "pllua.trigger", pllua_open_trigger, 0);
	lua_pop(L, 1);

	pllua_init_error_functions(L);
}
