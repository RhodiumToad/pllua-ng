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
 * Leaves the object on the stack, and returns the pointer to the pointer slot.
 */
void **pllua_newrefobject(lua_State *L, char *objtype, void *value)
{
	void **p = lua_newuserdata(L, sizeof(void*));
	*p = value;
	if (objtype)
	{
		int t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
		Assert(t == LUA_TTABLE);
		lua_setmetatable(L, -2);
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
	if (objtype)
	{
		int t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
		Assert(t == LUA_TTABLE);
		lua_setmetatable(L, -2);
	}
	if (uservalue)
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
	if (!p)
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

static int pllua_freeactivation(lua_State *L)
{
	pllua_func_activation *act = lua_touserdata(L, 1);

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

static int pllua_resetactivation(lua_State *L)
{
	pllua_func_activation *act = lua_touserdata(L, 1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
	{
		elog(WARNING, "failed to find an activation: %p", act);
		return 0;
	}

	pllua_checkobject(L, -1, PLLUA_ACTIVATION_OBJECT);

	act->thread = NULL;
	lua_getuservalue(L, 2);
	lua_pushnil(L);
	lua_rawsetp(L, -2, PLLUA_THREAD_MEMBER);
	
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
 * Create a new activation object with the specified function and memory
 * context.
 */
int pllua_newactivation(lua_State *L)
{
	void **p = pllua_checkrefobject(L, 1, PLLUA_FUNCTION_OBJECT);
	MemoryContext mcxt = lua_touserdata(L, 2);
	pllua_func_activation *act = pllua_newobject(L, PLLUA_ACTIVATION_OBJECT,
												 sizeof(pllua_func_activation), true);

	act->func_info = (*p);
	act->thread = NULL;
	
	lua_getuservalue(L, -1);
	lua_pushvalue(L, 1);
	lua_rawsetp(L, -2, PLLUA_FUNCTION_MEMBER);
	lua_pop(L, 1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	lua_pushvalue(L, -2);
	lua_rawsetp(L, -2, act);
	lua_pop(L, 1);

	/* this can't throw a pg error, thankfully */
	act->L = L;
	act->cb.func = pllua_freeactivation_cb;
	act->cb.arg = act;
	MemoryContextRegisterResetCallback(mcxt, &act->cb);
	
	return 1;
}

/*
 * Update the activation to point to a new function (e.g. after a recompile)
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
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
		elog(ERROR, "failed to find an activation: %p", act);
	lua_getuservalue(L, -1);
	lua_rawgetp(L, -1, PLLUA_FUNCTION_MEMBER);
	lua_insert(L, -4);
	lua_pop(L, 3);
}

/*
 * Function objects are refobjects containing cached function info.
 *
 * The uservalue slot of the object contains the actual Lua function, and we
 * proxy function calls to that in a __call method.
 */
static void pllua_destroy_funcinfo(lua_State *L, pllua_function_info *obj)
{
	MemoryContext oldmcxt = CurrentMemoryContext;
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_PG);
	PG_TRY();
	{
		/*
		 * funcinfo is allocated in its own memory context (since we expect it
		 * to have stuff dangling off), so free it by destroying that.
		 */
		MemoryContextDelete(obj->mcxt);
	}
	PG_CATCH();
	{
		pllua_setcontext(oldctx);
		pllua_rethrow_from_pg(L, oldmcxt);
	}
	PG_END_TRY();
	pllua_setcontext(oldctx);
}

static int pllua_funcobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_FUNCTION_OBJECT);
	void *obj = *p;
	*p = NULL;
	if (obj)
		pllua_destroy_funcinfo(L, obj);
	return 0;
}

static int pllua_funcobject_call(lua_State *L)
{
	int nargs = lua_gettop(L) - 1;
	lua_getuservalue(L, 1);
	lua_replace(L, 1);
	lua_call(L, nargs, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * metatables for objects and global functions
 */

static struct luaL_Reg funcobj_mt[] = {
	{ "__gc", pllua_funcobject_gc },
	{ "__call", pllua_funcobject_call },
	{ NULL, NULL }
};

static struct luaL_Reg actobj_mt[] = {
	{ NULL, NULL }
};

static struct luaL_Reg globfuncs[] = {
	{ "print", pllua_p_print },
	{ "info", pllua_p_print },
	{ NULL, NULL }
};

void pllua_init_objects(lua_State *L, bool trusted)
{
	pllua_newmetatable(L, PLLUA_FUNCTION_OBJECT, funcobj_mt);
	pllua_newmetatable(L, PLLUA_ACTIVATION_OBJECT, actobj_mt);
}

void pllua_init_functions(lua_State *L, bool trusted)
{
	luaL_openlibs(L);

	lua_pushglobaltable(L);
	luaL_setfuncs(L, globfuncs, 0);
	lua_pop(L, 1);
}
