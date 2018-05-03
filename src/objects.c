/* objects.c */

/*
 * Misc. lua object handling stuff.
 */

#include "pllua.h"

#include "nodes/makefuncs.h"

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
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, objtype);
}

/*
 * Create a table+metatable with the specified weak mode and (optional) name.
 *
 * Leaves both table and metatable on the stack, with the metatable on top.
 */
void pllua_new_weak_table(lua_State *L, const char *mode, const char *name)
{
	lua_newtable(L);
	lua_newtable(L);
	lua_pushstring(L, mode);
	lua_setfield(L, -2, "__mode");
	if (name)
	{
		lua_pushstring(L, name);
		lua_setfield(L, -2, "__name");
	}
	lua_pushvalue(L, -1);
	lua_setmetatable(L, -3);
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
		int t PG_USED_FOR_ASSERTS_ONLY;
		t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
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
		int t PG_USED_FOR_ASSERTS_ONLY;
		t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
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

/*
 * Create a memory context with the lifetime of a Lua object, useful for
 * temporary contexts that might otherwise get leaked by errors.
 *
 * Normal use would be to reset the context on the normal exit path and
 * leave the rest for GC to clean up.
 *
 * "name" must be a compile-time constant
 */
MemoryContext pllua_newmemcontext(lua_State *L,
								  const char *name,
								  Size minsz,
								  Size initsz,
								  Size maxsz)
{
	void **p = pllua_newrefobject(L, PLLUA_MCONTEXT_OBJECT, NULL, false);
	MemoryContext parent = pllua_get_memory_cxt(L);
	volatile MemoryContext mcxt;
	PLLUA_TRY();
	{
#if PG_VERSION_NUM >= 110000
		mcxt = AllocSetContextCreateExtended(parent, name, minsz, initsz, maxsz);
#else
		mcxt = AllocSetContextCreate(parent, name, minsz, initsz, maxsz);
#endif
		*p = mcxt;
	}
	PLLUA_CATCH_RETHROW();
	return mcxt;
}

static int pllua_mcxtobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_MCONTEXT_OBJECT);
	MemoryContext mcxt = p ? *p : NULL;
	if (!p)
		return 0;
	ASSERT_LUA_CONTEXT;
	*p = NULL;
	if (mcxt)
	{
		PLLUA_TRY();
		{
			MemoryContextDelete(mcxt);
		}
		PLLUA_CATCH_RETHROW();
	}
	return 0;
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
 * make field "field" of the uservalue table of the object at nd
 * contain the value at the top of the stack (which is popped).
 *
 * Install a table in the uservalue if it wasn't already one.
 */
void
pllua_set_user_field(lua_State *L, int nd, const char *field)
{
	nd = lua_absindex(L, nd);
	if (lua_getuservalue(L, nd) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setuservalue(L, nd);
	}
	lua_insert(L, -2);
	lua_setfield(L, -2, field);
	lua_pop(L, 1);
}

int
pllua_get_user_field(lua_State *L, int nd, const char *field)
{
	if (lua_getuservalue(L, nd) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		return LUA_TNIL;
	}
	else
	{
		int typ = lua_getfield(L, -1, field);
		lua_remove(L, -2);
		return typ;
	}
}

int
pllua_get_user_subfield(lua_State *L, int nd, const char *field, const char *subfield)
{
	if (lua_getuservalue(L, nd) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_pushnil(L);
		return LUA_TNIL;
	}
	else if (lua_getfield(L, -1, field) != LUA_TTABLE)
	{
		lua_pop(L, 2);
		lua_pushnil(L);
		return LUA_TNIL;
	}
	else
	{
		int typ	= lua_getfield(L, -1, subfield);
		lua_remove(L, -2);
		lua_remove(L, -2);
		return typ;
	}
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
		/* unsafe to elog here
		 *elog(WARNING, "failed to find an activation: %p", act);
		 */
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
		pllua_warning(L, "failed to find an activation: %p", act);
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
	if (!p)
		return 0;
	ASSERT_LUA_CONTEXT;
	*p = NULL;
	if (obj)
		pllua_destroy_funcinfo(L, obj);
	return 0;
}

/*
 * PGFunc objects are refobjects pointing at the FmgrInfo for some pg function
 * we might call.
 *
 * It would be nice to be able to initialize more stuff here. But the problem
 * is that most fmgr initialization needs to be done from PG context, and so
 * it's better to share a catch block between that and the function call proper
 * than have a new catch block just for this.
 *
 * By storing the memory context in a separate object in the uservalue, we
 * avoid needing a metatable for this; some callers might like to supply their
 * own (e.g. with a __call method). But that does mean that pgfunc objects are
 * not in fact type-checkable as refobjects, and the caller has to do their own
 * type checks.
 */
void pllua_pgfunc_new(lua_State *L)
{
	pllua_newrefobject(L, NULL, NULL, true);
	lua_getuservalue(L, -1);
	pllua_newmemcontext(L, "pllua pgfunc context", ALLOCSET_SMALL_SIZES);
	lua_rawsetp(L, -2, PLLUA_MCONTEXT_MEMBER);
	lua_pop(L, 1);
}

/*
 * __index(tab,key)
 */
static int
pllua_pgfunc_auto_new(lua_State *L)
{
	lua_settop(L,2);
	pllua_pgfunc_new(L);
	lua_pushvalue(L, -2);
	lua_pushvalue(L, -2);
	lua_rawset(L, 1);
	return 1;
}

void pllua_pgfunc_table_new(lua_State *L)
{
	lua_newtable(L);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_PGFUNC_TABLE_OBJECT);
	lua_setmetatable(L, -2);
}

/*
 * Actually allocate (if needed) and fill in a pgfunc. This has to be called
 * from PG context.
 */
FmgrInfo *
pllua_pgfunc_init(lua_State *L, int nd, Oid fnoid, int nargs, Oid *argtypes, Oid rettype)
{
	MemoryContext mcxt;
	MemoryContext oldcontext;
	Node	   *func = NULL;
	FmgrInfo   *fn = NULL;
	void	  **p = lua_touserdata(L, nd);
	int			i;

	ASSERT_PG_CONTEXT;

	if (!p)
		elog(ERROR, "pllua_pgfunc_init: param is not a userdata");

	if (lua_getuservalue(L, nd) != LUA_TTABLE)
		elog(ERROR, "pllua_pgfunc_init: bad uservalue");

	if (lua_rawgetp(L, -1, PLLUA_MCONTEXT_MEMBER) != LUA_TUSERDATA
		|| !(mcxt = *(void **) lua_touserdata(L, -1)))
		elog(ERROR, "pllua_pgfunc_init: missing mcontext");

	lua_pop(L, 2);

	oldcontext = MemoryContextSwitchTo(mcxt);

	if (!*p)
		fn = *p = palloc0(sizeof(FmgrInfo));
	else
		fn = *p;

	if (nargs >= 0)
	{
		List	   *args = NIL;

		for (i = 0; i < nargs; ++i)
		{
			Param	   *argp = makeNode(Param);

			/* make an argument of a dummy Param node of the input type */
			argp->paramkind = PARAM_EXEC;
			argp->paramid = -1;
			argp->paramtype = argtypes[i];
			argp->paramtypmod = -1;
			argp->paramcollid = InvalidOid;
			argp->location = -1;
			args = lappend(args, argp);
		}

		func = (Node *) makeFuncExpr(fnoid, rettype, args,
									 InvalidOid, InvalidOid,
									 COERCE_EXPLICIT_CALL);
	}

	fmgr_info_cxt(fnoid, fn, mcxt);
	fmgr_info_set_expr(func, fn);

	MemoryContextSwitchTo(oldcontext);

	return fn;
}

/*
 * metatables for objects and global functions
 */

static struct luaL_Reg funcobj_mt[] = {
	{ "__gc", pllua_funcobject_gc },
	{ NULL, NULL }
};

static struct luaL_Reg mcxtobj_mt[] = {
	{ "__gc", pllua_mcxtobject_gc },
	{ NULL, NULL }
};

static struct luaL_Reg actobj_mt[] = {
	{ "__tostring", pllua_dump_activation },
	{ NULL, NULL }
};

static struct luaL_Reg pgfunctab_mt[] = {
	{ "__index", pllua_pgfunc_auto_new },
	{ NULL, NULL }
};

int pllua_open_funcmgr(lua_State *L)
{
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);

	pllua_newmetatable(L, PLLUA_FUNCTION_OBJECT, funcobj_mt);
	pllua_newmetatable(L, PLLUA_ACTIVATION_OBJECT, actobj_mt);
	pllua_newmetatable(L, PLLUA_MCONTEXT_OBJECT, mcxtobj_mt);
	pllua_newmetatable(L, PLLUA_PGFUNC_TABLE_OBJECT, pgfunctab_mt);
	lua_pop(L, 4);

	lua_pushboolean(L, 1);
	return 1;
}
