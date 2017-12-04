/* init.c */

/*
 * Overall module initialization, and also per-interpreter initialization and
 * maintenance of the interpreter hashtable.
 */

#include "pllua.h"

#include "storage/ipc.h"
#include "utils/inval.h"
#include "utils/syscache.h"

PGDLLEXPORT void _PG_init(void);

static bool simulate_memory_failure = false;

static HTAB *pllua_interp_hash = NULL;

static char *pllua_on_init = NULL;
static char *pllua_on_trusted_init = NULL;
static char *pllua_on_untrusted_init = NULL;


typedef struct pllua_interp_desc
{
	Oid			user_id;		/* Hash key (must be first!) */
	bool		trusted;
	lua_State  *interp;			/* The interpreter */
} pllua_interp_desc;


static void pllua_newstate(bool trusted, Oid user_id, pllua_interp_desc *interp_desc);
static int pllua_init_state(lua_State *L);
static void pllua_fini(int code, Datum arg);
static void *pllua_alloc (void *ud, void *ptr, size_t osize, size_t nsize);

/*
 * pllua_getstate
 *
 * Returns the Lua interpreter (main thread) to be used for the current call.
 */

lua_State *pllua_getstate(bool trusted)
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
	{
		interp_desc->interp = NULL;
		interp_desc->trusted = trusted;
	}

	/*
	 * this can throw a pg error, but is required to ensure the interpreter is
	 * removed from interp_desc first if it does.
	 */
	pllua_newstate(trusted, user_id, interp_desc);

	return interp_desc->interp;
}


/*
 * _PG_init
 *
 * Called by the function manager on module load.
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
		{
			lua_State *L = interp_desc->interp;
			interp_desc->interp = NULL;
			/*
			 * We intentionally do not worry about trying to rethrow any errors
			 * happening here; we're trying to shut down, and ignoring an error
			 * is probably less likely to crash us than
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

	elog(DEBUG3, "pllua_fini: done");
}


static void pllua_relcache_callback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS hash_seq;
	pllua_interp_desc *interp_desc;

	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		lua_State *L = interp_desc->interp;
		if (L)
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

static void pllua_syscache_typeoid_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS hash_seq;
	pllua_interp_desc *interp_desc;

	hash_seq_init(&hash_seq, pllua_interp_hash);
	while ((interp_desc = hash_seq_search(&hash_seq)) != NULL)
	{
		lua_State *L = interp_desc->interp;
		if (L)
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



/*
 * Lua-environment part of interpreter setup.
 */

static int pllua_init_state(lua_State *L)
{
	bool trusted = lua_toboolean(L, 1);
	lua_Integer user_id = lua_tointeger(L, 2);
	MemoryContext *mcxt = lua_touserdata(L, 3);
	MemoryContext *emcxt = lua_touserdata(L, 4);

	lua_pushliteral(L, "0.01");
	lua_setglobal(L, "_PLVERSION");
	lua_pushlightuserdata(L, mcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	lua_pushlightuserdata(L, emcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);
	lua_pushinteger(L, user_id);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_USERID);
	lua_pushboolean(L, trusted);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED);

	/* require the base lib early so that we can overwrite bits */
	luaL_requiref(L, "_G", luaopen_base, 1);

	pllua_init_objects(L, trusted);
	pllua_init_error(L);
	pllua_init_functions(L, trusted);
	pllua_init_spi(L);

	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_FUNCS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ACTIVATIONS);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TYPES);
	lua_newtable(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_RECORDS);

	/* don't run user code yet. */
	return 0;
}

/*
 * Remove anything installed by pllua_init_state that isn't safe for the user
 * to play with. This is a separate step because we want the
 * (superuser-controlled) init strings to be able to load modules and so on.
 */
static int pllua_trusted_lockdown(lua_State *L)
{
	/* XXX TODO */
	return 0;
}

/*
 * Simple bare-bones execution of a single string.
 */
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

static int pllua_run_init_strings(lua_State *L)
{
	bool trusted;

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
static void pllua_newstate(bool trusted, Oid user_id, pllua_interp_desc *interp_desc)
{
	static bool first_time = true;
	MemoryContext mcxt = NULL;
	MemoryContext emcxt = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
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
		int rc;
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
		lua_pushinteger(L, (lua_Integer) user_id);
		lua_pushlightuserdata(L, mcxt);
		lua_pushlightuserdata(L, emcxt);
		pllua_pcall(L, 4, 0, 0);

		if (first_time)
		{
			on_proc_exit(pllua_fini, (Datum)0);
			CacheRegisterRelcacheCallback(pllua_relcache_callback, (Datum)0);
			CacheRegisterSyscacheCallback(TYPEOID, pllua_syscache_typeoid_callback, (Datum)0);
			first_time = false;
		}

		interp_desc->interp = L;

		/*
		 * Now that we have everything set up, it should finally be safe to run
		 * some arbitrary code.
		 */
		rc = pllua_cpcall(L, pllua_run_init_strings, NULL);
		if (rc)
			pllua_rethrow_from_lua(L, rc);

		if (trusted)
		{
			/*
			 * Anything not nailed down belongs to the user. Anything they can
			 * pry loose is not nailed down.
			 */
			rc = pllua_cpcall(L, pllua_trusted_lockdown, NULL);
			if (rc)
				pllua_rethrow_from_lua(L, rc);
		}
	}
	PG_CATCH();
	{
		ErrorData *e;
		Assert(pllua_context == PLLUA_CONTEXT_PG);

		interp_desc->interp = NULL;

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

