/*
 * pllua.c: PL/Lua NG call handler
 * By Andrew "RhodiumToad" Gierth, rhodiumtoad at postgresql.org
 * Based in some part on pllua by Luis Carvalho and others
 * License: MIT license or PostgreSQL licence
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "storage/ipc.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "commands/trigger.h"
#include "commands/event_trigger.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define PLLUA_LOCALVAR "_U"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT Datum pllua_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pllua_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum plluau_inline_handler(PG_FUNCTION_ARGS);

static Datum pllua_common_call(FunctionCallInfo fcinfo, bool trusted);
static Datum pllua_common_inline(FunctionCallInfo fcinfo, bool trusted);
static Datum pllua_common_validator(FunctionCallInfo fcinfo, bool trusted);

static HTAB *pllua_interp_hash = NULL;

static char *pllua_on_init = NULL;
static char *pllua_on_trusted_init = NULL;
static char *pllua_on_untrusted_init = NULL;

static bool pllua_ending = false;

#define pllua_pushcfunction(L_,f_) lua_pushcfunction((L_),(f_))

/*
 * Track what error handling context we're in, to try and detect any violations
 * of error-handling protocol (lua errors thrown through pg catch blocks and
 * vice-versa).
 */
typedef enum
{
	PLLUA_CONTEXT_PG,
	PLLUA_CONTEXT_LUA
} pllua_context_type;

static pllua_context_type pllua_context = PLLUA_CONTEXT_PG;

static pllua_context_type pllua_setcontext(pllua_context_type newctx)
{
	pllua_context_type oldctx = pllua_context;
	pllua_context = newctx;
	return oldctx;
}

typedef struct pllua_interp_desc
{
	Oid			user_id;		/* Hash key (must be first!) */
	lua_State  *interp;			/* The interpreter */
} pllua_interp_desc;

typedef struct pllua_activation_record
{
	FunctionCallInfo fcinfo;
	Datum		retval;
	bool		trusted;

	/* if fcinfo is null, we're validating or doing inline */
	InlineCodeBlock *cblock;
	Oid			validate_func;
	
} pllua_activation_record;

/*
 * We don't put this in the body of a lua userdata because we want to reference
 * it from flinfo, and lua could garbage-collect it at the wrong time. Instead,
 * the lua object is a pointer to this with a __gc method, and the object
 * itself is palloc'd. (If lua decides to garbage-collect while the object is
 * actually in use, we just leak it; this can only happen when a modification
 * of the pg_proc row is detected in a recursive call. plpgsql leaks in the
 * same way, so we are in good company.)
 *
 * The actual lua function value is stashed in a table in the registry, keyed
 * by the address of this struct as a lightuserdata.
 */

typedef struct pllua_function_info
{
	int use_count;			/* >0 if live flinfos point to this */
	
	Oid	fn_oid;
	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	bool retset;
	
	Oid language_oid;
	bool trusted;

	MemoryContext mcxt;

	const char *name;
	
} pllua_function_info;

typedef struct pllua_function_compile_info
{
	pllua_function_info *func_info;
	
	MemoryContext mcxt;

	text *prosrc;
	
} pllua_function_compile_info;
   

/* this one ends up in flinfo->fn_extra */

typedef struct pllua_func_activation
{
	pllua_function_info *func_info;
	lua_State *thread;

	/*
	 * this data is allocated in lua, so we need to arrange to drop it for GC
	 * when the context containing the pointer to it is reset
	 */
	lua_State *L;
	MemoryContextCallback cb;
	
} pllua_func_activation;


/*
 * Addresses used as lua registry keys
 *
 * Note the key is the address, not the string; the string is only for
 * debugging purposes.
 */

static char PLLUA_MEMORYCONTEXT[] = "memory context";
static char PLLUA_ERRORCONTEXT[] = "error memory context";
static char PLLUA_FUNCS[] = "funcs";
static char PLLUA_ACTIVATIONS[] = "activations";
static char PLLUA_FUNCTION_OBJECT[] = "function object metatable";
static char PLLUA_ERROR_OBJECT[] = "error object metatable";
static char PLLUA_ACTIVATION_OBJECT[] = "activation object metatable";
static char PLLUA_LAST_ERROR[] = "last error object";
static char PLLUA_RECURSIVE_ERROR[] = "recursive error object";
static char PLLUA_FUNCTION_MEMBER[] = "function element";
static char PLLUA_THREAD_MEMBER[] = "thread element";

void _PG_init(void)
{
	static bool init_done = false;
	HASHCTL		hash_ctl;

	if (init_done)
		return;

	/*
	 * Initialize GUCs. These are SUSET for security reasons!
	 */
	DefineCustomStringVariable("pllua_ng.on_init",
							   gettext_noop("Code to execute when a Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("pllua_ng.on_trusted_init",
							   gettext_noop("Code to execute when a Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_trusted_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("pllua_ng.on_untrusted_init",
							   gettext_noop("Code to execute when a Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_untrusted_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("pllua_ng");

	/*
	 * Create hash table for interpreters.
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(pllua_interp_desc);
	pllua_interp_hash = hash_create("PLLua interpreters",
									8,
									&hash_ctl,
									HASH_ELEM | HASH_BLOBS);

	init_done = true;
}

/*
 * Cleanup interpreters.
 * Does not fully undo the actions of _PG_init() nor make it callable again.
 */
static void
pllua_fini(int code, Datum arg)
{
	HASH_SEQ_STATUS hash_seq;
	pllua_interp_desc *interp_desc;

	elog(DEBUG3, "pllua_fini");

	if (pllua_ending)
		return;

	pllua_ending = true;

	/* Only perform cleanup if we're exiting cleanly */
	if (code)
	{
		elog(DEBUG3, "pllua_fini: skipped");
		return;
	}

	/* Zap any fully-initialized interpreters */
	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		if (interp_desc->interp)
			lua_close(interp_desc->interp);
		/*
		 * we intentionally do not worry about deleting the memory contexts
		 * here; we're about to die anyway.
		 */
	}

	elog(DEBUG3, "pllua_fini: done");
}

/*
 * Would be nice to be able to use repalloc, but at present there is no flag to
 * have that return null rather than throwing. So for now, we keep the actual
 * lua data in the malloc heap (lua handles its own garbage collection), while
 * associated objects (referenced by userdata values) go in the context
 * associated with the interpreter. Lua's memory usage can be queried within
 * lua if one needs to monitor usage.
 */

static bool simulate_memory_failure = false;

static void *pllua_alloc (void *ud, void *ptr, size_t osize, size_t nsize)
{
	void *nptr;
	
	(void)ud;  /* not used */

	if (simulate_memory_failure)
	{
		nptr = NULL;
		if (nsize == 0)
			simulate_memory_failure = false;
	}
	else
		nptr = realloc(ptr, nsize);
	
	if (nsize == 0)
		return NULL;
	
	if (ptr && nsize < osize)
	{
		if (!nptr)
		{
			elog(WARNING, "pllua: failed to shrink a block of size %lu to %lu",
				 (unsigned long) osize, (unsigned long) nsize);
			return ptr;
		}
	}

	return nptr;
}

static int pllua_panic(lua_State *L)
{
	elog(pllua_context == PLLUA_CONTEXT_PG ? ERROR : PANIC,
		 "Uncaught Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	return 0;
}

static void pllua_poperror(lua_State *L)
{
	elog(WARNING,
		 "Ignored Lua error: %s",
		 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)"));
	lua_pop(L, 1);
}

static bool pllua_isobject(lua_State *L, int nd, char *objtype)
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

static void pllua_newrefobject(lua_State *L, char *objtype, void *value)
{
	void *p = lua_newuserdata(L, sizeof(void*));
	*(void **)p = value;
	if (objtype)
	{
		int t = lua_rawgetp(L, LUA_REGISTRYINDEX, objtype);
		Assert(t == LUA_TTABLE);
		lua_setmetatable(L, -2);
	}
}

static int pllua_newerror(lua_State *L)
{
	void *p = lua_touserdata(L, 1);
	pllua_newrefobject(L, PLLUA_ERROR_OBJECT, p);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LAST_ERROR);
	return 1;
}

static int pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	
	return rc;
}

/*
 * Having caught an error in lua, which is now on top of the stack (as returned
 * by pcall), rethrow it either back into lua or into pg according to what
 * context we're now in.
 */
static void pllua_rethrow_from_lua(lua_State *L, int rc)
{
	if (pllua_context == PLLUA_CONTEXT_LUA)
		lua_error(L);

	/*
	 * If out of memory, avoid doing anything even slightly fancy.
	 */
	
	if (rc == LUA_ERRMEM)
	{
		lua_pop(L, -1);
		elog(ERROR, "pllua: out of memory");
	}

	/*
	 * The thing on top of the stack is either a lua object with a pg error, a
	 * string, or something else.
	 */
	if (pllua_isobject(L, -1, PLLUA_ERROR_OBJECT))
	{
		ErrorData **p = lua_touserdata(L, -1);
		ErrorData *edata = *p;

		/*
		 * safe to pop the object since it should still be referenced from the
		 * registry
		 */
		lua_pop(L, -1);

		if (edata)
			ReThrowError(edata);
		else
			elog(ERROR, "recursive error in Lua error handling");
	}
	
	ereport(ERROR,
			(errmsg_internal("pllua: %s",
							 (lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(error is not a string)")),
			 (lua_pop(L, 1), 1)));
}

/*
 * Having caught an error in PG_CATCH, rethrow it either back into lua or into
 * pg according to what context we're now in. Note, we must have restored the
 * previous context; the idiom is:
 *
 * oldmcxt = CurrentMemoryContext;
 * oldctx = pllua_setcontext(PLLUA_CONTEXT_PG);
 * PG_TRY();
 * { ... must not modify oldmcxt or oldctx ... }
 * PG_CATCH();
 * {
 *   pllua_setcontext(oldctx);
 *   pllua_rethrow_from_pg(L, oldmcxt);
 * }
 * PG_END_TRY();
 * pllua_setcontext(oldctx);
 *
 * There had better be space on the Lua stack for a couple of objects - it's
 * the caller's responsibility to deal with that.
 */
static void pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt)
{
	MemoryContext emcxt;
	volatile ErrorData *edata = NULL;
	
	if (pllua_context == PLLUA_CONTEXT_PG)
		PG_RE_THROW();

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);
	emcxt = lua_touserdata(L, -1);
	lua_pop(L, 1);
	MemoryContextSwitchTo(emcxt);
	
	/*
	 * absorb the error and exit pg's error handling.
	 *
	 * Unfortunately, CopyErrorData can fail, and we can't afford to rethrow
	 * via pg since we're in lua context, so we have to catch that.
	 * FlushErrorState can fail too, but only if things are really badly
	 * screwed up (memory corruption in ErrorContext) so we can legitimately
	 * die if things get that far. (Honestly, the chances that elog won't
	 * already have recursed itself to death in that case are slim.)
	 */
	PG_TRY();
	{
		edata = CopyErrorData();
	}
	PG_CATCH();
	{
		/*
		 * recursive error, don't bother trying to figure out what.
		 */
		edata = NULL;
	}
	PG_END_TRY();
	
	PG_TRY();
	{
		FlushErrorState();
	}
	PG_CATCH();
	{
		elog(PANIC, "error recursion trouble: FlushErrorState failed");
	}
	PG_END_TRY();

	MemoryContextSwitchTo(mcxt);
		
	/*
	 * make a lua object to hold the error. This can throw an out of memory
	 * error from lua, but we don't want to let that supersede a pg error,
	 * because we sometimes want to let the user-supplied lua code catch lua
	 * errors but not pg errors. So if that happens, replace it with our
	 * special prebuilt "recursive error" object.
	 */

	if (edata)
	{
		pllua_pushcfunction(L, pllua_newerror);
		lua_pushlightuserdata(L, (void *) edata);
		if (pllua_pcall_nothrow(L, 1, 1, 0) != 0)
		{
			pllua_poperror(L);
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);
		}
	}
	else
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);

	lua_error(L);
}

#if LUA_VERSION_NUM > 501
static int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	lua_pushcfunction(L, func); /* can't throw */
	lua_pushlightuserdata(L, arg); /* can't throw */
	rc = lua_pcall(L, 1, 0, 0);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	return rc;
}
#else
static int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_cpcall(L, func, arg);
	
	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	return rc;
}
#endif

static void pllua_pcall(lua_State *L, int nargs, int nresults, int msgh)
{
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_LUA);
	int rc;

	rc = lua_pcall(L, nargs, nresults, msgh);

	/* check for violation of protocol */
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	
	pllua_setcontext(oldctx);
	
	if (rc)
		pllua_rethrow_from_lua(L, rc);
}

static MemoryContext pllua_get_memory_cxt(lua_State *L)
{
	void *p;
	
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	p = lua_touserdata(L, -1);
	lua_pop(L, 1);

	return (MemoryContext) p;
}

static void pllua_newmetatable(lua_State *L, char *objtype, luaL_Reg *mt)
{
	lua_newtable(L);
	luaL_setfuncs(L, mt, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__metatable");
	lua_pushstring(L, objtype);
	lua_setfield(L, -2, "__name");
	lua_rawsetp(L, LUA_REGISTRYINDEX, objtype);
}


static void *pllua_newobject(lua_State *L, char *objtype, size_t sz, bool uservalue)
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

static void **pllua_torefobject(lua_State *L, int nd, char *objtype)
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

static void *pllua_toobject(lua_State *L, int nd, char *objtype)
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

static void pllua_type_error(lua_State *L, char *expected)
{
	luaL_error(L, "wrong parameter type (expected %s)", expected);
}

static void **pllua_checkrefobject(lua_State *L, int nd, char *objtype)
{
	void **p = pllua_torefobject(L, nd, objtype);
	if (!p)
		luaL_argerror(L, nd, objtype);
	return p;
}

static void *pllua_checkobject(lua_State *L, int nd, char *objtype)
{
	void *p = pllua_toobject(L, nd, objtype);
	if (!p)
		pllua_type_error(L, objtype);
	return p;
}


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

static int pllua_newactivation(lua_State *L)
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

static int pllua_setactivation(lua_State *L)
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

static void pllua_getactivation(lua_State *L, pllua_func_activation *act)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	if (lua_rawgetp(L, -1, act) == LUA_TNIL)
		elog(ERROR, "failed to find an activation: %p", act);
	lua_getuservalue(L, -1);
	lua_rawgetp(L, -1, PLLUA_FUNCTION_MEMBER);
	lua_insert(L, -4);
	lua_pop(L, 3);
}

static void pllua_runstring(lua_State *L, const char *chunkname, const char *str)
{
	if (str)
	{
		int rc = luaL_loadbuffer(L, str, strlen(str), chunkname);
		if (rc)
			lua_error(L);
		lua_call(L, 0, 0);
	}
}

static int pllua_funcobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_FUNCTION_OBJECT);
	void *obj = *p;
	*p = NULL;
	if (obj)
		pfree(obj);
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

static int pllua_errobject_gc(lua_State *L)
{
	void **p = pllua_torefobject(L, 1, PLLUA_ERROR_OBJECT);
	void *obj = *p;
	*p = NULL;
	if (obj)
		FreeErrorData(obj);
	return 0;
}


static void pllua_info(lua_State *L, const char *str)
{
	MemoryContext oldmcxt = CurrentMemoryContext;
	pllua_context_type oldctx = pllua_setcontext(PLLUA_CONTEXT_PG);
	
	PG_TRY();
	{
		ereport(INFO,
				(errmsg_internal("%s", str)));
	}
	PG_CATCH();
	{
		pllua_setcontext(oldctx);
		pllua_rethrow_from_pg(L, oldmcxt);
	}
	PG_END_TRY();
	pllua_setcontext(oldctx);
}

static int pllua_p_print (lua_State *L)
{
	int i, n = lua_gettop(L); /* nargs */
	int fidx;
	const char *s;
	luaL_Buffer b;

	lua_getglobal(L, "tostring");
	fidx = lua_absindex(L, -1);
	
	luaL_buffinit(L, &b);

	for (i = 1; i <= n; i++)
	{
		lua_pushvalue(L, fidx); /* tostring */
		lua_pushvalue(L, i); /* arg */
		lua_call(L, 1, 1);
		s = lua_tostring(L, -1);
		if (s == NULL)
			return luaL_error(L, "cannot convert to string");
		if (i > 1) luaL_addchar(&b, '\t');
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	s = lua_tostring(L, -1);
	pllua_info(L, s);
	return 0;
}


struct luaL_Reg funcobj_mt[] = {
	{ "__gc", pllua_funcobject_gc },
	{ "__call", pllua_funcobject_call },
	{ NULL, NULL }
};

struct luaL_Reg errobj_mt[] = {
	{ "__gc", pllua_errobject_gc },
	{ NULL, NULL }
};

struct luaL_Reg actobj_mt[] = {
	{ NULL, NULL }
};

struct luaL_Reg globfuncs[] = {
	{ "print", pllua_p_print },
	{ "info", pllua_p_print },
	{ NULL, NULL }
};

static int pllua_init_state(lua_State *L)
{
	bool trusted = lua_toboolean(L, 1);
	MemoryContext *mcxt = lua_touserdata(L, 2);
	MemoryContext *emcxt = lua_touserdata(L, 3);

	lua_pushliteral(L, "0.01");
	lua_setglobal(L, "_PLVERSION");
	lua_pushlightuserdata(L, mcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	lua_pushlightuserdata(L, emcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);

	pllua_newmetatable(L, PLLUA_FUNCTION_OBJECT, funcobj_mt);
	pllua_newmetatable(L, PLLUA_ERROR_OBJECT, errobj_mt);
	pllua_newmetatable(L, PLLUA_ACTIVATION_OBJECT, actobj_mt);

	lua_pushlightuserdata(L, NULL);
	pllua_newerror(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECURSIVE_ERROR);

	luaL_openlibs(L);

	lua_pushglobaltable(L);
	luaL_setfuncs(L, globfuncs, 0);
	lua_pop(L, 1);

	pllua_runstring(L, "on_init", pllua_on_init);
	if (trusted)
		pllua_runstring(L, "on_trusted_init", pllua_on_trusted_init);
	else
		pllua_runstring(L, "on_untrusted_init", pllua_on_untrusted_init);

	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);

	return 0;
}

static lua_State *pllua_newstate(bool trusted)
{
	static bool first_time = true;
	MemoryContext mcxt = NULL;
	MemoryContext emcxt = NULL;
	lua_State *L;

	Assert(pllua_context == PLLUA_CONTEXT_PG);
		
	mcxt = AllocSetContextCreate(TopMemoryContext,
								 "PL/Lua context",
								 ALLOCSET_DEFAULT_MINSIZE,
								 ALLOCSET_DEFAULT_INITSIZE,
								 ALLOCSET_DEFAULT_MAXSIZE);

	emcxt = AllocSetContextCreate(mcxt,
								  "PL/Lua error context",
								  8*1024,
								  8*1024,
								  8*1024);

	L = lua_newstate(pllua_alloc, NULL);

	if (!L)
		elog(ERROR, "Out of memory creating Lua interpreter");

	lua_atpanic(L, pllua_panic);  /* can't throw */

	PG_TRY();
	{
		/*
		 * Since we just created this interpreter, we know we're not in any
		 * protected environment yet, so Lua errors outside of pcall will
		 * become pg errors via pllua_panic. In other contexts we must be more
		 * cautious about Lua errors, because of this scenario: if a Lua
		 * function calls into SPI which invokes another Lua function, then any
		 * Lua error thrown in the nested invocation might longjmp back to the
		 * outer interpreter...
		 */

		lua_pushcfunction(L, pllua_init_state);
		lua_pushboolean(L, trusted);
		lua_pushlightuserdata(L, mcxt);
		lua_pushlightuserdata(L, emcxt);
		pllua_pcall(L, 3, 0, 0);
	}
	PG_CATCH();
	{
		Assert(pllua_context == PLLUA_CONTEXT_PG);
		lua_close(L); /* can't throw */
		MemoryContextDelete(mcxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (first_time)
	{
		first_time = false;
		on_proc_exit(pllua_fini, (Datum)0);
	}

	return L;
}

static lua_State *pllua_getstate(bool trusted)
{
	Oid			user_id = trusted ? GetUserId() : InvalidOid;
	pllua_interp_desc *interp_desc;
	bool found;

	Assert(pllua_context == PLLUA_CONTEXT_PG);

	interp_desc = hash_search(pllua_interp_hash, &user_id,
							  HASH_ENTER,
							  &found);
	
	if (found && interp_desc->interp)
		return interp_desc->interp;

	if (!found)
		interp_desc->interp = NULL;

	interp_desc->interp = pllua_newstate(trusted);

	return interp_desc->interp;
}

/* Trusted entry points */

PG_FUNCTION_INFO_V1(pllua_validator);
Datum pllua_validator(PG_FUNCTION_ARGS)
{
	return pllua_common_validator(fcinfo, true);
}

PG_FUNCTION_INFO_V1(pllua_call_handler);
Datum pllua_call_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_call(fcinfo, true);
}

PG_FUNCTION_INFO_V1(pllua_inline_handler);
Datum pllua_inline_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_inline(fcinfo, true);
}

/* Untrusted entry points */

PG_FUNCTION_INFO_V1(plluau_validator);
Datum plluau_validator(PG_FUNCTION_ARGS)
{
	return pllua_common_validator(fcinfo, false);
}

PG_FUNCTION_INFO_V1(plluau_call_handler);
Datum plluau_call_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_call(fcinfo, false);
}

PG_FUNCTION_INFO_V1(plluau_inline_handler);
Datum plluau_inline_handler(PG_FUNCTION_ARGS)
{
	return pllua_common_inline(fcinfo, false);
}


static int pllua_compile(lua_State *L)
{
	pllua_function_compile_info *comp_info = lua_touserdata(L, 1);
	pllua_function_info *func_info = comp_info->func_info;
	const char *fname = func_info->name;
	const char *src;
	luaL_Buffer b;

	/*
	 * caller fills in pointer value, but it's our job to put in the lua
	 * function object
	 */
	pllua_newrefobject(L, PLLUA_FUNCTION_OBJECT, NULL);

	luaL_buffinit(L, &b);

	/*
	 * The string we construct is:
	 *   local upvalue,f f=function(args) body end return f
	 * which we then execute and expect it to return the
	 * function object.
	 */
	luaL_addstring(&b, "local " PLLUA_LOCALVAR ",");
	luaL_addstring(&b, fname);
	luaL_addchar(&b, ' ');
	luaL_addstring(&b, fname);
	luaL_addstring(&b, "=function(...) ");
	luaL_addlstring(&b, VARDATA_ANY(comp_info->prosrc),
					VARSIZE_ANY_EXHDR(comp_info->prosrc));
	luaL_addstring(&b, " end return ");
	luaL_addstring(&b, fname);
	luaL_pushresult(&b);
	src = lua_tostring(L, -1);

	if (luaL_loadbuffer(L, src, strlen(src), fname))
		pllua_rethrow_from_lua(L, LUA_ERRRUN);
	lua_remove(L, -2); /* source */
	lua_call(L, 0, 1);
	
	lua_setuservalue(L, -2);
	return 1;
}


static int pllua_intern_function(lua_State *L)
{
	lua_Integer oid = luaL_checkinteger(L, 2);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);

	if (!lua_isnil(L, 1))
	{
		pllua_checkrefobject(L, 1, PLLUA_FUNCTION_OBJECT);
		lua_rawgeti(L, -1, oid);
		if (!lua_isnil(L, -1))
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_pop(L, 1);
	}

	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, oid);
	lua_pushboolean(L, 1);
	
	return 1;
}

/*
 * Return true if func_info is an up to date compile of procTup.
 */
static bool pllua_function_valid(pllua_function_info *func_info,
								 HeapTuple procTup)
{
	return (func_info &&
			func_info->fn_xmin == HeapTupleHeaderGetRawXmin(procTup->t_data) &&
			ItemPointerEquals(&func_info->fn_tid, &procTup->t_self));
}

/*
 * Returns with a function object on top of the lua stack.
 */
static void pllua_validate_and_push(lua_State *L, FmgrInfo *flinfo, bool trusted)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	Assert(pllua_context == PLLUA_CONTEXT_LUA);

	/*
	 * We need the pg_proc row etc. every time, but we have to avoid throwing
	 * pg errors through lua.
	 */

	pllua_setcontext(PLLUA_CONTEXT_PG);
	PG_TRY();
	{
		pllua_func_activation *act = flinfo->fn_extra;
		Oid			fn_oid = flinfo->fn_oid;
		HeapTuple	procTup;
		Form_pg_proc procStruct;
		pllua_function_info *func_info;
		pllua_function_compile_info *comp_info;
		bool isnull;
		Datum psrc;
		MemoryContext fcxt;
		MemoryContext ccxt;
		int rc;
		
		/*
		 * This part may have to be repeated in some rare recursion scenarios.
		 */
		for (;;)
		{
			/* We'll need the pg_proc tuple in any case... */
			procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
			if (!HeapTupleIsValid(procTup))
				elog(ERROR, "cache lookup failed for function %u", fn_oid);
			procStruct = (Form_pg_proc) GETSTRUCT(procTup);

			if (act && pllua_function_valid(act->func_info, procTup))
			{
				/* fastpath out when data is already valid. */
				pllua_getactivation(L, act);
				ReleaseSysCache(procTup);
				break;
			}
			
			/*
			 * Lookup function by oid in our lua table (this can't throw)
			 */

			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
			if (lua_rawgeti(L, -1, (lua_Integer) fn_oid) == LUA_TUSERDATA)
			{
				void **p = pllua_torefobject(L, -1, PLLUA_FUNCTION_OBJECT);
				func_info = p ? *p : NULL;
				/* might be out of date. */
				if (pllua_function_valid(func_info, procTup))
				{
					if (act)
					{
						/*
						 * The activation is out of date, but the existing
						 * compiled function is not. Just update the activation.
						 */
						pllua_pushcfunction(L, pllua_setactivation);
						lua_pushlightuserdata(L, act);
						lua_pushvalue(L, -3);
						pllua_pcall(L, 2, 0, 0);
					}
					else
					{
						/*
						 * The activation doesn't exist yet, but the existing
						 * compiled function is up to date.
						 */
						pllua_pushcfunction(L, pllua_newactivation);
						lua_pushvalue(L, -2);
						lua_pushlightuserdata(L, flinfo->fn_mcxt);
						pllua_pcall(L, 2, 1, 0);
						act = lua_touserdata(L, -1);
						lua_pop(L, 1); /* activation is referenced, so won't be GCed */
						if (flinfo && flinfo->fn_extra == NULL)
							flinfo->fn_extra = act;
					}
					lua_remove(L, -2);
					ReleaseSysCache(procTup);
					break;
				}

				/*
				 * Compiled function in cache is out of date. Unintern it
				 * before proceeding (recursion worries). This might lead to it
				 * being GC'd, so forget about the pointer too.
				 */
				func_info = NULL;
				pllua_pushcfunction(L, pllua_intern_function);
				lua_pushnil(L);
				lua_pushinteger(L, (lua_Integer) fn_oid);
				pllua_pcall(L, 2, 0, 0);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);

			/*
			 * If we get this far, we need to compile up the function from
			 * scratch.  Create the func_info, compile_info and
			 * contexts. Note that the compile context is always transient,
			 * but the function context is reparented to the long-lived lua
			 * context on success.
			 */

			psrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
			if (isnull)
				elog(ERROR, "null prosrc");

			fcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua function object",
										 ALLOCSET_SMALL_SIZES);
			ccxt = AllocSetContextCreate(CurrentMemoryContext,
										 "pllua compile context",
										 ALLOCSET_SMALL_SIZES);
			
			MemoryContextSwitchTo(fcxt);
				
			func_info = palloc(sizeof(pllua_function_info));
			func_info->mcxt = fcxt;
			func_info->name = pstrdup(NameStr(procStruct->proname));
			func_info->fn_xmin = HeapTupleHeaderGetRawXmin(procTup->t_data);
			func_info->fn_tid = procTup->t_self;

			MemoryContextSwitchTo(ccxt);
				
			comp_info = palloc(sizeof(pllua_function_compile_info));
			comp_info->mcxt = ccxt;
			comp_info->func_info = func_info;
			comp_info->prosrc = DatumGetTextPP(psrc);
			
			/*
			 * XXX dig out all the needed info about arg and result types
			 * and stash it in the compile info.
			 *
			 * Beware, compiling can invoke user-supplied code, which might
			 * in turn recurse here. We trust that stack depth checks will
			 * break any such loop.
			 */

			pllua_pushcfunction(L, pllua_compile);
			lua_pushlightuserdata(L, comp_info);
			rc = pllua_pcall_nothrow(L, 1, 1, 0);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(ccxt);

			if (rc)
			{
				MemoryContextDelete(fcxt);
				pllua_rethrow_from_lua(L, rc);
			}

			MemoryContextSetParent(fcxt, pllua_get_memory_cxt(L));
				
			void *p = lua_touserdata(L, -1);
			*(void **)p = func_info;

			/*
			 * Try and intern the function. Since we uninterned it earlier,
			 * we expect this to succeed, but a recursive call could have
			 * interned a new version already. Worse, if so, that new
			 * version could already be out of date, meaning that we have
			 * to loop back to check the pg_proc row again.
			 */

			pllua_pushcfunction(L, pllua_intern_function);
			lua_insert(L, -2);
			lua_pushinteger(L, (lua_Integer) fn_oid);
			pllua_pcall(L, 2, 0, 0);
			func_info = NULL;
			ReleaseSysCache(procTup);
		}
	}
	PG_CATCH();
	{
		pllua_setcontext(PLLUA_CONTEXT_LUA);
		pllua_rethrow_from_pg(L, oldcontext);
	}
	PG_END_TRY();
	pllua_setcontext(PLLUA_CONTEXT_LUA);
	MemoryContextSwitchTo(oldcontext);
}

static void pllua_common_lua_init(lua_State *L)
{
	Assert(pllua_context == PLLUA_CONTEXT_LUA);
	luaL_checkstack(L, 20, NULL);
}

static int pllua_call_function(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	pllua_validate_and_push(L, fcinfo->flinfo, act->trusted);
	
	/* XXX push the args at some point here */

	lua_call(L, 0, 0);

	fcinfo->isnull = true;
	act->retval = (Datum)0;
	return 0;
}

static int pllua_call_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	fcinfo->isnull = true;
	act->retval = (Datum)0;
	return 0;
}

static int pllua_call_event_trigger(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);
		
	return 0;
}

static int pllua_call_inline(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);

	return 0;
}

static int pllua_validate(lua_State *L)
{
	pllua_activation_record *act = lua_touserdata(L, 1);
	FunctionCallInfo fcinfo = act->fcinfo;

	pllua_common_lua_init(L);
		
	return 0;
}

/*
 * We store a lot of our state inside lua for convenience, but that means
 * we have to consider possible lua errors (e.g. out of memory) happening
 * even outside the actual function call. So, arrange to do all the work in
 * a protected environment to catch any such errors.
 *
 * We can only safely pass 1 arg, which will be a light userdata.
 *
 * Annoyingly, the code needed for this changes with Lua version, since which
 * functions can throw errors changes.  Specifically, on 5.1, neither
 * lua_checkstack nor lua_pushcfunction are safe, but lua_cpcall is; on 5.2 or
 * later, lua_cpcall may be missing, but lua_checkstack and lua_pushcfunction
 * are safe.
 */

static void pllua_initial_protected_call(lua_State *L,
										 lua_CFunction func,
										 void *arg)
{
	sigjmp_buf *cur_catch_block PG_USED_FOR_ASSERTS_ONLY = PG_exception_stack;
	int rc;

	Assert(pllua_context == PLLUA_CONTEXT_PG);

#if LUA_VERSION_NUM > 501
	if (!lua_checkstack(L, 5))
		elog(ERROR, "pllua: out of memory error on stack setup");
#endif

	rc = pllua_cpcall(L, func, arg);

	/*
	 * We better not have longjmp'd past any pg catch blocks.
	 */
	Assert(cur_catch_block == PG_exception_stack);
	
	if (rc)
		pllua_rethrow_from_lua(L, rc);
}	

Datum pllua_common_call(FunctionCallInfo fcinfo, bool trusted)
{
	lua_State *L;
	pllua_activation_record act;

	/* XXX luajit may need this to be palloc'd. check later */
	
	act.fcinfo = fcinfo;
	act.retval = (Datum) 0;
	act.trusted = trusted;

	pllua_setcontext(PLLUA_CONTEXT_PG);

	L = pllua_getstate(trusted);

	if (CALLED_AS_TRIGGER(fcinfo))
		pllua_initial_protected_call(L, pllua_call_trigger, &act);
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		pllua_initial_protected_call(L, pllua_call_event_trigger, &act);
	else
		pllua_initial_protected_call(L, pllua_call_function, &act);

	return act.retval;
}

Datum pllua_common_validator(FunctionCallInfo fcinfo, bool trusted)
{
	lua_State *L;
	pllua_activation_record act;
	Oid funcoid = PG_GETARG_OID(0);

	/* security checks */
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	/* XXX luajit may need this to be palloc'd. check later */
	
	act.fcinfo = NULL;
	act.retval = (Datum) 0;
	act.trusted = trusted;
	act.validate_func = funcoid;

	pllua_setcontext(PLLUA_CONTEXT_PG);

	L = pllua_getstate(trusted);

	pllua_initial_protected_call(L, pllua_validate, &act);

	PG_RETURN_VOID();
}

Datum pllua_common_inline(FunctionCallInfo fcinfo, bool trusted)
{
	lua_State *L;
	pllua_activation_record act;

	/* XXX luajit may need this to be palloc'd. check later */
	
	act.fcinfo = NULL;
	act.retval = (Datum) 0;
	act.trusted = trusted;
	act.cblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);

	pllua_setcontext(PLLUA_CONTEXT_PG);

	L = pllua_getstate(trusted);

	pllua_initial_protected_call(L, pllua_call_inline, &act);

	PG_RETURN_VOID();
}
