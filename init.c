/* init.c */

/*
 * Overall module initialization, and also per-interpreter initialization and
 * maintenance of the interpreter hashtable.
 */

#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "storage/ipc.h"
#include "utils/inval.h"
#include "utils/syscache.h"

PGDLLEXPORT void _PG_init(void);

static bool simulate_memory_failure = false;

static HTAB *pllua_interp_hash = NULL;

static char *pllua_on_init = NULL;
static char *pllua_on_trusted_init = NULL;
static char *pllua_on_untrusted_init = NULL;
static bool pllua_do_check_for_interrupts = true;


static void pllua_newstate(bool trusted, Oid user_id, pllua_interpreter *interp_desc, pllua_activation_record *act);
static int pllua_init_state(lua_State *L);
static void pllua_fini(int code, Datum arg);
static void *pllua_alloc (void *ud, void *ptr, size_t osize, size_t nsize);


/*
 * pllua_getstate
 *
 * Returns the interpreter structure to be used for the current call.
 */
pllua_interpreter *
pllua_getstate(bool trusted, pllua_activation_record *act)
{
	Oid	user_id = trusted ? GetUserId() : InvalidOid;
	pllua_interpreter *interp_desc;
	bool found;

	Assert(pllua_context == PLLUA_CONTEXT_PG);

	interp_desc = hash_search(pllua_interp_hash, &user_id,
							  HASH_ENTER,
							  &found);

	if (found && interp_desc->L)
		return interp_desc;

	if (!found)
	{
		interp_desc->L = NULL;
		interp_desc->trusted = trusted;
	}

	/*
	 * this can throw a pg error, but is required to ensure the interpreter is
	 * removed from interp_desc first if it does.
	 */
	pllua_newstate(trusted, user_id, interp_desc, act);

	return interp_desc;
}

/*
 * careful, mustn't throw
 */
pllua_interpreter *
pllua_getinterpreter(lua_State *L)
{
	void *p;
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_INTERP);
	p = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return p;
}


/*
 * _PG_init
 *
 * Called by the function manager on module load.
 *
 * This should be kept postmaster-safe in case someone wants to preload the
 * module in shared_preload_libraries.
 */
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
	DefineCustomBoolVariable("pllua_ng.check_for_interrupts",
							 gettext_noop("Check for query cancels while running the Lua interpreter."),
							 NULL,
							 &pllua_do_check_for_interrupts,
							 true,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("pllua_ng");

	/*
	 * Create hash table for interpreters.
	 */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(pllua_interpreter);
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
	pllua_interpreter *interp_desc;

	elog(DEBUG2, "pllua_fini");

	if (pllua_ending)
		return;

	pllua_ending = true;

	/* Only perform cleanup if we're exiting cleanly */
	if (code)
	{
		elog(DEBUG2, "pllua_fini: skipped");
		return;
	}

	/* Zap any fully-initialized interpreters */
	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		if (interp_desc->L)
		{
			lua_State *L = interp_desc->L;
			interp_desc->L = NULL;
			/*
			 * We intentionally do not worry about trying to rethrow any errors
			 * happening here; we're trying to shut down, and ignoring an error
			 * is probably less likely to crash us than rethrowing it.
			 */
			pllua_setcontext(PLLUA_CONTEXT_LUA);
			lua_close(L); /* can't throw, but has internal lua catch blocks */
			pllua_setcontext(PLLUA_CONTEXT_PG);
		}
		/*
		 * we intentionally do not worry about deleting the memory contexts
		 * here; we're about to die anyway.
		 */
	}

	elog(DEBUG2, "pllua_fini: done");
}

/*
 * Broadcast an invalidation to all interpreters (if arg==0) or the specified
 * interpreter.
 */
static void
pllua_relcache_callback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS hash_seq;
	pllua_interpreter *interp_desc;

	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		lua_State *L = interp_desc->L;
		if (L
			&& (arg == (Datum)0
				|| arg == PointerGetDatum(interp_desc)))
		{
			int rc;
			pllua_pushcfunction(L, pllua_typeinfo_invalidate);
			lua_pushnil(L);
			lua_pushinteger(L, (lua_Integer) relid);
			rc = pllua_pcall_nothrow(L, 2, 0, 0);
			if (rc)
			{
				elog(WARNING, "lua error in relcache invalidation");
			}
		}
	}
}

/*
 * Broadcast an invalidation to all interpreters (if arg==0) or the specified
 * interpreter.
 */
static void
pllua_syscache_typeoid_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS hash_seq;
	pllua_interpreter *interp_desc;

	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		lua_State *L = interp_desc->L;
		if (L
			&& (arg == (Datum)0
				|| arg == PointerGetDatum(interp_desc)))
		{
			int rc;
			pllua_pushcfunction(L, pllua_typeinfo_invalidate);
			lua_pushinteger(L, (lua_Integer) InvalidOid);
			lua_pushnil(L);
			rc = pllua_pcall_nothrow(L, 2, 0, 0);
			if (rc)
			{
				elog(WARNING, "lua error in syscache invalidation");
			}
		}
	}
}

/*
 * Would be nice to be able to use repalloc, but at present there is no flag to
 * have that return null rather than throwing. So for now, we keep the actual
 * lua data in the malloc heap (lua handles its own garbage collection), while
 * associated objects (referenced by userdata values) go in the context
 * associated with the interpreter. Lua's memory usage can be queried within
 * lua if one needs to monitor usage.
 */
static void *
pllua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	void	   *nptr;

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

/*
 * Hook function to check for interrupts. We have lua call this for every few
 * million opcodes executed.
 */
static void
pllua_hook(lua_State *L, lua_Debug *ar)
{
	PLLUA_TRY();
	{
		CHECK_FOR_INTERRUPTS();
	}
	PLLUA_CATCH_RETHROW();
}

/*
 * Lua-environment part of interpreter setup.
 */
static int
pllua_init_state(lua_State *L)
{
	bool		trusted = lua_toboolean(L, 1);
	lua_Integer	user_id = lua_tointeger(L, 2);
	lua_Integer	lang_oid = lua_tointeger(L, 3);
	MemoryContext *mcxt = lua_touserdata(L, 4);
	MemoryContext *emcxt = lua_touserdata(L, 5);
	pllua_interpreter *interp = lua_touserdata(L, 6);

	lua_pushliteral(L, "0.01");
	lua_setglobal(L, "_PLVERSION");
	lua_pushlightuserdata(L, mcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	lua_pushlightuserdata(L, emcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);
	lua_pushlightuserdata(L, interp);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_INTERP);
	lua_pushinteger(L, user_id);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_USERID);
	lua_pushinteger(L, lang_oid);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LANG_OID);
	lua_pushboolean(L, trusted);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED);

	/* install our hack to push C functions without throwing error */
#define PLLUA_DECL_CFUNC(f_) lua_pushcfunction(L, f_); lua_rawsetp(L, LUA_REGISTRYINDEX, f_);
#include "pllua_functable.h"
#undef PLLUA_DECL_CFUNC

	/* require the base lib early so that we can overwrite bits */
	luaL_requiref(L, "_G", luaopen_base, 1);

	pllua_init_objects(L, trusted);
	pllua_init_error(L);

	luaL_openlibs(L);

	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECORDS);

	pllua_init_functions(L, trusted);

	/* Treat SPI as an actual module */
	luaL_requiref(L, "pllua.spi", pllua_open_spi, 0);
	lua_setglobal(L, "spi");

	/* Treat the numeric handler as an actual module */
	luaL_requiref(L, "pllua.numeric", pllua_open_numeric, 0);
	lua_pop(L, 1);

	/*
	 * If in trusted mode, load the "trusted" module which allows the superuser
	 * to control (in the init strings) what modules can be exposed to the user.
	 */
	if (trusted)
	{
		luaL_requiref(L, "pllua.trusted", pllua_open_trusted, 0);
		lua_setglobal(L, "trusted");
	}

	/* enable interrupt checks */
	if (pllua_do_check_for_interrupts)
		lua_sethook(L, pllua_hook, LUA_MASKCOUNT, 10000000);

	/* don't run user code yet. */
	return 0;
}

/*
 * Simple bare-bones execution of a single string. Always uses the global
 * environment, not the sandbox (if any).
 */
static void
pllua_runstring(lua_State *L, const char *chunkname, const char *str)
{
	if (str)
	{
		int rc = luaL_loadbuffer(L, str, strlen(str), chunkname);
		if (rc)
			lua_error(L);
		lua_call(L, 0, 0);
	}
}

/*
 * Execute the init strings. Note that they are executed outside any trusted
 * sandbox.
 */
int
pllua_run_init_strings(lua_State *L)
{
	bool	trusted;

	/* sheer paranoia */
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED) != LUA_TBOOLEAN)
		luaL_error(L, "inconsistency in interpreter setup");

	trusted = lua_toboolean(L, -1);

	pllua_runstring(L, "on_init", pllua_on_init);
	if (trusted)
		pllua_runstring(L, "on_trusted_init", pllua_on_trusted_init);
	else
		pllua_runstring(L, "on_untrusted_init", pllua_on_untrusted_init);

	return 0;
}

/*
 * PG-environment part of interpreter setup.
 */
static void
pllua_newstate(bool trusted,
			   Oid user_id,
			   pllua_interpreter *interp_desc,
			   pllua_activation_record *act)
{
	static bool first_time = true;
	MemoryContext mcxt = NULL;
	MemoryContext emcxt = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	lua_State  *L;
	Oid			langoid;

	ASSERT_PG_CONTEXT;

	/*
	 * Get our own language oid; this is somewhat unnecessarily hard.
	 */
	if (act->cblock)
		langoid = act->cblock->langOid;
	else
	{
		Oid procoid = (act->fcinfo) ? act->fcinfo->flinfo->fn_oid : act->validate_func;
		HeapTuple procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procoid));
		if (!HeapTupleIsValid(procTup))
			elog(ERROR, "cache lookup failed for function %u", procoid);
		langoid = ((Form_pg_proc) GETSTRUCT(procTup))->prolang;
		ReleaseSysCache(procTup);
	}

	mcxt = AllocSetContextCreate(TopMemoryContext,
								 "PL/Lua context",
								 ALLOCSET_DEFAULT_SIZES);

	emcxt = AllocSetContextCreate(mcxt,
								  "PL/Lua error context",
								  8*1024,
								  8*1024,
								  8*1024);

#if LUA_VERSION_NUM == 501
	L = luaL_newstate();
#else
	L = lua_newstate(pllua_alloc, NULL);
#endif

	if (!L)
		elog(ERROR, "Out of memory creating Lua interpreter");

	lua_atpanic(L, pllua_panic);  /* can't throw */

	PG_TRY();
	{
		int		rc;

		/*
		 * Since we just created this interpreter, we know we're not in any
		 * protected environment yet, so Lua errors outside of pcall will
		 * become pg errors via pllua_panic. In other contexts we must be more
		 * cautious about Lua errors, because of this scenario: if a Lua
		 * function calls into SPI which invokes another Lua function, then any
		 * Lua error thrown in the nested invocation might longjmp back to the
		 * outer interpreter...
		 */

		/* note that pllua_pushcfunction is not available yet. */
		lua_pushcfunction(L, pllua_init_state);
		lua_pushboolean(L, trusted);
		lua_pushinteger(L, (lua_Integer) user_id);
		lua_pushinteger(L, (lua_Integer) langoid);
		lua_pushlightuserdata(L, mcxt);
		lua_pushlightuserdata(L, emcxt);
		lua_pushlightuserdata(L, interp_desc);
		pllua_pcall(L, 6, 0, 0);

		if (first_time)
		{
			on_proc_exit(pllua_fini, (Datum)0);
			CacheRegisterRelcacheCallback(pllua_relcache_callback, (Datum)0);
			CacheRegisterSyscacheCallback(TYPEOID, pllua_syscache_typeoid_callback, (Datum)0);
			CacheRegisterSyscacheCallback(TRFTYPELANG, pllua_syscache_typeoid_callback, (Datum)0);
			first_time = false;
		}

		interp_desc->L = L;

		/*
		 * force invalidation of the caches now anyway, since we might have
		 * missed something (prior to the assignment above the invalidation
		 * callbacks will ignore us); but for this interpreter only, no need to
		 * involve any others.
		 */
		pllua_relcache_callback(PointerGetDatum(interp_desc), InvalidOid);
		pllua_syscache_typeoid_callback(PointerGetDatum(interp_desc), TYPEOID, 0);

		/*
		 * Now that we have everything set up, it should finally be safe to run
		 * some arbitrary code.
		 */
		rc = pllua_cpcall(L, pllua_run_init_strings, NULL);
		if (rc)
			pllua_rethrow_from_lua(L, rc);
	}
	PG_CATCH();
	{
		ErrorData *e;
		Assert(pllua_context == PLLUA_CONTEXT_PG);

		interp_desc->L = NULL;

		/*
		 * If we got a lua error (which could be a caught pg error) during the
		 * protected part of interpreter initialization, then we need to kill
		 * the interpreter; but that could provoke further errors, so we exit
		 * pg's error handling first. Since we need to kill off the lua memory
		 * contexts too, we temporarily use the caller's memory (which should
		 * be a transient context) to store the error data.
		 */
		MemoryContextSwitchTo(oldcontext);
		e = CopyErrorData();
		FlushErrorState();

		pllua_setcontext(PLLUA_CONTEXT_LUA);
		pllua_ending = true;  /* we're ending _this_ interpreter at least */
		lua_close(L); /* can't throw, but has internal lua catch blocks */
		pllua_ending = false;
		pllua_setcontext(PLLUA_CONTEXT_PG);

		MemoryContextDelete(mcxt);

		ReThrowError(e);
	}
	PG_END_TRY();
}
