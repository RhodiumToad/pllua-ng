/* init.c */

/*
 * Overall module initialization, and also per-interpreter initialization and
 * maintenance of the interpreter hashtable.
 */

#include "pllua.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "nodes/pg_list.h"
#include "storage/ipc.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include <time.h>

PGDLLEXPORT void _PG_init(void);

#define PLLUA_ERROR_CONTEXT_SIZES 8*1024, 8*1024, 8*1024

static bool simulate_memory_failure = false;

static HTAB *pllua_interp_hash = NULL;

static List *held_states = NIL;

static char *pllua_on_init = NULL;
static char *pllua_on_trusted_init = NULL;
static char *pllua_on_untrusted_init = NULL;
static char *pllua_on_common_init = NULL;
static bool pllua_do_check_for_interrupts = true;
/* trusted.c also needs this */
bool pllua_do_install_globals = true;
static int pllua_num_held_interpreters = 1;
static char *pllua_reload_ident = NULL;
static double pllua_gc_threshold = 0;
static double pllua_gc_multiplier = 0;

static const char *pllua_pg_version_str = NULL;
static const char *pllua_pg_version_num = NULL;

bool pllua_track_gc_debt = false;

static lua_State *pllua_newstate_phase1(const char *ident);
static void pllua_newstate_phase2(lua_State *L,
								  bool trusted,
								  Oid user_id,
								  pllua_interpreter *interp_desc,
								  pllua_activation_record *act);
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
	{
		if (interp_desc->new_ident)
		{
			lua_State *L = interp_desc->L;
			int rc = pllua_cpcall(L, pllua_set_new_ident, interp_desc);
			if (rc)
				pllua_rethrow_from_lua(L, rc);  /* unlikely, but be safe */
		}
		return interp_desc;
	}

	if (!found)
	{
		interp_desc->L = NULL;
		interp_desc->trusted = trusted;
		interp_desc->new_ident = false;

		interp_desc->gc_debt = 0;

		interp_desc->cur_activation.fcinfo = NULL;
		interp_desc->cur_activation.retval = (Datum) 0;
		interp_desc->cur_activation.trusted = trusted;
		interp_desc->cur_activation.cblock = NULL;
		interp_desc->cur_activation.validate_func = InvalidOid;
		interp_desc->cur_activation.interp = NULL;
		interp_desc->cur_activation.err_text = NULL;
	}

	/*
	 * this can throw a pg error, but is required to ensure the interpreter is
	 * removed from interp_desc first if it does.
	 */
	if (held_states != NIL)
	{
		lua_State *L = linitial(held_states);
		held_states = list_delete_first(held_states);
		pllua_newstate_phase2(L, trusted, user_id, interp_desc, act);
	}
	else
	{
		lua_State *L = pllua_newstate_phase1(pllua_reload_ident);
		if (!L)
			elog(ERROR, "PL/Lua: interpreter creation failed");
		pllua_newstate_phase2(L, trusted, user_id, interp_desc, act);
	}

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

static void
pllua_create_held_states(const char *ident)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	int i;
	for (i = 0; i < pllua_num_held_interpreters; ++i)
	{
		lua_State *L = pllua_newstate_phase1(ident);
		if (!L)
		{
			elog(WARNING, "PL/Lua: interpreter creation failed");
			break;
		}
		held_states = lcons(L, held_states);
	}
	MemoryContextSwitchTo(oldcontext);
}

static void
pllua_destroy_held_states(void)
{
	/* zap a "held" interp if any */
	while (held_states != NIL)
	{
		lua_State *L = linitial(held_states);
		held_states = list_delete_first(held_states);
		/*
		 * We intentionally do not worry about trying to rethrow any errors
		 * happening here; we're trying to shut down, and ignoring an error
		 * is probably less likely to crash us than rethrowing it.
		 */
		pllua_setcontext(PLLUA_CONTEXT_LUA);
		lua_close(L); /* can't throw, but has internal lua catch blocks */
		pllua_setcontext(PLLUA_CONTEXT_PG);
	}
}

static void
pllua_assign_on_init(const char *newval, void *extra)
{
	/* if we're not this far into initialization, do nothing */
	if (!pllua_interp_hash)
		return;
	/* changed? */
	if (newval == pllua_on_init ||
		(newval && pllua_on_init && strcmp(newval,pllua_on_init) == 0))
		return;
	if ((pllua_reload_ident && *pllua_reload_ident) || IsUnderPostmaster)
	{
		pllua_destroy_held_states();
		if (!IsUnderPostmaster)
		{
			pllua_on_init = (char *) newval;
			pllua_create_held_states(pllua_reload_ident);
		}
	}
}

static void
pllua_assign_reload_ident(const char *newval, void *extra)
{
	/* if we're not this far into initialization, do nothing */
	if (!pllua_interp_hash)
		return;
	/* changed? */
	if (newval == pllua_reload_ident ||
		(newval && pllua_reload_ident && strcmp(newval,pllua_reload_ident) == 0))
		return;
	/* make sure we don't reload if turning off reloading. */
	if (newval && *newval)
	{
		pllua_destroy_held_states();
		if (!IsUnderPostmaster)
			pllua_create_held_states(newval);
		else if (pllua_interp_hash)
		{
			HASH_SEQ_STATUS hash_seq;
			pllua_interpreter *interp;
			hash_seq_init(&hash_seq, pllua_interp_hash);
			while ((interp = hash_seq_search(&hash_seq)) != NULL)
				interp->new_ident = true;
		}
	}
}

int
pllua_set_new_ident(lua_State *L)
{
	pllua_interpreter *interp_desc = lua_touserdata(L, 1);
	lua_pushglobaltable(L);
	lua_pushliteral(L, "_PL_IDENT_NEW");
	lua_pushstring(L, pllua_reload_ident);
	lua_rawset(L, -3);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushliteral(L, "_PL_IDENT_NEW");
	lua_pushstring(L, pllua_reload_ident);
	lua_rawset(L, -3);
	interp_desc->new_ident = false;
	return 0;
}


static void
pllua_assign_gc_multiplier(double newval, void *extra)
{
	if (newval > 0.0)
		pllua_track_gc_debt = true;
	else
		pllua_track_gc_debt = false;
}

void
pllua_run_extra_gc(lua_State *L, unsigned long gc_debt)
{
	double val;

	if (pllua_gc_multiplier == 0.0)
		return;

	val = (gc_debt / 1024.0);
	if (val < pllua_gc_threshold)
		return;
	if (pllua_gc_multiplier > 999999.0)
	{
		pllua_debug(L, "pllua_run_extra_gc: full collect");
		lua_gc(L, LUA_GCCOLLECT, 0);
	}
	else
	{
		int ival;

		val *= pllua_gc_multiplier;
		if (val >= (double) INT_MAX)
			ival = INT_MAX;
		else
			ival = (int) val;
		pllua_debug(L, "pllua_run_extra_gc: step %d", ival);
		lua_gc(L, LUA_GCSTEP, ival);
	}
}

static const char *
pllua_get_config_value(const char *name)
{
#if PG_VERSION_NUM >= 90600
	return MemoryContextStrdup(TopMemoryContext,
							   GetConfigOptionByName(name, NULL, false));
#else
	return MemoryContextStrdup(TopMemoryContext,
							   GetConfigOptionByName(name, NULL));
#endif
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

	pllua_pg_version_str = pllua_get_config_value("server_version");
	pllua_pg_version_num = pllua_get_config_value("server_version_num");

	/*
	 * Initialize GUCs. These are mostly SUSET or SIGHUP for security reasons!
	 */
	DefineCustomStringVariable("pllua.on_init",
							   gettext_noop("Code to execute early when a Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, pllua_assign_on_init, NULL);
	DefineCustomStringVariable("pllua.on_trusted_init",
							   gettext_noop("Code to execute when a trusted Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_trusted_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("pllua.on_untrusted_init",
							   gettext_noop("Code to execute when an untrusted Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_untrusted_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("pllua.on_common_init",
							   gettext_noop("Code to execute when any Lua interpreter is initialized."),
							   NULL,
							   &pllua_on_common_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
	DefineCustomBoolVariable("pllua.install_globals",
							 gettext_noop("Install key modules as global tables."),
							 NULL,
							 &pllua_do_install_globals,
							 true,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pllua.check_for_interrupts",
							 gettext_noop("Check for query cancels while running the Lua interpreter."),
							 NULL,
							 &pllua_do_check_for_interrupts,
							 true,
							 PGC_SUSET, 0,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("pllua.prebuilt_interpreters",
							gettext_noop("Number of interpreters to prebuild if preloaded"),
							NULL,
							&pllua_num_held_interpreters,
							1,
							0,
							10,
							PGC_SIGHUP, 0,
							NULL, NULL, NULL);
	DefineCustomStringVariable("pllua.interpreter_reload_ident",
							   gettext_noop("Altering this id reloads any held interpreters"),
							   NULL,
							   &pllua_reload_ident,
							   NULL,
							   PGC_SIGHUP, 0,
							   NULL, pllua_assign_reload_ident, NULL);

	/*
	 * These don't need to be SUSET because we're not concerned about resource
	 * attacks, and we expose the collectgarbage() function to the user anyway, so nothing
	 * done with these has any real security consequence.
	 */
	DefineCustomRealVariable("pllua.extra_gc_multiplier",
							 gettext_noop("Multiplier for additional GC calls"),
							 NULL,
							 &pllua_gc_multiplier,
							 0,
							 0,
							 1000000,
							 PGC_USERSET, 0,
							 NULL, pllua_assign_gc_multiplier, NULL);
	DefineCustomRealVariable("pllua.extra_gc_threshold",
							 gettext_noop("Threshold for additional GC calls in kbytes"),
							 NULL,
							 &pllua_gc_threshold,
							 0,
							 0,
							 LONG_MAX / 1024.0,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("pllua");

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

	if (!IsUnderPostmaster)
		pllua_create_held_states(pllua_reload_ident);

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

	pllua_destroy_held_states();

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
pllua_callback_broadcast(Datum arg, lua_CFunction cfunc, pllua_cache_inval *inval)
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
			interp_desc->inval = inval;
			rc = pllua_cpcall(L, /* keep line split to avoid functable hack */
							  cfunc,
							  interp_desc);
			if (rc)
				pllua_poperror(L);
		}
	}
}

static void
pllua_relcache_callback(Datum arg, Oid relid)
{
	pllua_cache_inval inval;

	memset(&inval, 0, sizeof(inval));
	inval.inval_rel = true;
	inval.inval_reloid = InvalidOid;
	pllua_callback_broadcast(arg, pllua_register_cfunc(L, pllua_typeinfo_invalidate), &inval);
}

static void
pllua_syscache_typeoid_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	pllua_cache_inval inval;

	memset(&inval, 0, sizeof(inval));
	inval.inval_type = true;
	inval.inval_typeoid = InvalidOid;
	pllua_callback_broadcast(arg, pllua_register_cfunc(L, pllua_typeinfo_invalidate), &inval);
}

static void
pllua_syscache_cast_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	pllua_cache_inval inval;

	memset(&inval, 0, sizeof(inval));
	inval.inval_cast = true;
	pllua_callback_broadcast(arg, pllua_register_cfunc(L, pllua_typeconv_invalidate), &inval);
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

	if (nsize == 0)
	{
		free(ptr);							/* free(NULL) is explicitly safe */
		simulate_memory_failure = false;
		return NULL;
	}

	if (simulate_memory_failure)
		nptr = NULL;
	else
		nptr = realloc(ptr, nsize);

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
 * Hook function to check for interrupts. We have lua call this for every
 * function return or set number of opcodes executed.
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
 * Simple bare-bones execution of a single string.
 */
static void
pllua_runstring(lua_State *L, const char *chunkname, const char *str, bool use_sandbox)
{
	if (str)
	{
		int rc = luaL_loadbuffer(L, str, strlen(str), chunkname);
		if (rc)
			lua_error(L);
		if (use_sandbox)
		{
			lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
			pllua_set_environment(L, -2);
		}
		lua_call(L, 0, 0);
	}
}

/*
 * Lua-environment part of interpreter setup.
 *
 * Phase 1 might be executed pre-fork; it can't know whether the interpreter
 * will be trusted, what the language oid is, what the user id is or anything
 * related to the database state. Nor does it have a pointer to the
 * pllua_interpreter structure.
 */
static int
pllua_init_state_phase1(lua_State *L)
{
	MemoryContext *mcxt = lua_touserdata(L, 1);
	MemoryContext *emcxt = lua_touserdata(L, 2);
	const char *ident = lua_touserdata(L, 3);

	lua_pushliteral(L, PLLUA_VERSION_STR);
	lua_setglobal(L, "_PLVERSION");
	lua_pushstring(L, pllua_pg_version_str);
	lua_setglobal(L, "_PG_VERSION");
	lua_pushstring(L, pllua_pg_version_num);
	lua_pushinteger(L, lua_tointeger(L, -1));
	lua_setglobal(L, "_PG_VERSION_NUM");
	lua_pop(L, 1);
	lua_pushstring(L, ident ? ident : "");
	lua_setglobal(L, "_PL_IDENT");
	lua_pushinteger(L, (lua_Integer) time(NULL));
	lua_setglobal(L, "_PL_LOAD_TIME");
	lua_pushlightuserdata(L, mcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
	lua_pushlightuserdata(L, emcxt);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_ERRORCONTEXT);
	lua_pushlightuserdata(L, 0);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_INTERP);

	/* install our hack to push C functions without throwing error */
#define PLLUA_DECL_CFUNC(f_) lua_pushcfunction(L, f_); lua_rawsetp(L, LUA_REGISTRYINDEX, f_);
#include "pllua_functable.h"
#undef PLLUA_DECL_CFUNC

	luaL_openlibs(L);

	/*
	 * Initialize our error handling, which replaces many base functions
	 * (pcall, xpcall, error, assert). Must be done after openlibs but before
	 * anything might throw a pg error.
	 */
	luaL_requiref(L, "pllua.error", pllua_open_error, 0);

	/*
	 * Default print() is not useful (can't rely on stdout/stderr going
	 * anywhere safe). Replace with our version, and direct output initially to
	 * the log.
	 */
	luaL_requiref(L, "pllua.print", pllua_open_print, 0);

	/*
	 * Early init of the trusted sandbox, so that on_init can do trusted setup
	 * (even though we don't know in on_init whether we're trusted or not).
	 *
	 * Note that this relies on the fact that the pllua.trusted module doesn't
	 * offer any interaction with postgres, so it's postmaster-safe.
	 */
	luaL_requiref(L, "pllua.trusted", pllua_open_trusted, 0);

	/*
	 * Actually run the phase1 init string.
	 */
	pllua_runstring(L, "on_init", pllua_on_init, false);

	/*
	 * Do this _after_ the init string so that the init string can't get at
	 * server.error, which would kill the postmaster messily if done when
	 * reloading preloaded states. (But don't defer it to phase2 to allow it
	 * to pre-generate the error tables.)
	 */
	luaL_requiref(L, "pllua.elog", pllua_open_elog, 0);
	if (pllua_do_install_globals)
		lua_setglobal(L, "server");  /* XXX fixme: needs a better name/location */

	lua_settop(L, 0);

	if (!IsUnderPostmaster)
		lua_gc(L, LUA_GCCOLLECT, 0);

	return 0;
}

static int
pllua_init_state_phase2(lua_State *L)
{
	bool		trusted = lua_toboolean(L, 1);
	lua_Integer	user_id = lua_tointeger(L, 2);
	lua_Integer	lang_oid = lua_tointeger(L, 3);
	pllua_interpreter *interp = lua_touserdata(L, 4);

	lua_pushlightuserdata(L, interp);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_INTERP);
	lua_pushinteger(L, user_id);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_USERID);
	lua_pushinteger(L, lang_oid);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_LANG_OID);
	lua_pushboolean(L, trusted);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED);

	/* Treat these as actual modules */

	luaL_requiref(L, "pllua.funcmgr", pllua_open_funcmgr, 0);

	luaL_requiref(L, "pllua.pgtype", pllua_open_pgtype, 0);
	if (pllua_do_install_globals)
		lua_setglobal(L, "pgtype");

	luaL_requiref(L, "pllua.spi", pllua_open_spi, 0);
	if (pllua_do_install_globals)
		lua_setglobal(L, "spi");

	luaL_requiref(L, "pllua.trigger", pllua_open_trigger, 0);

	luaL_requiref(L, "pllua.numeric", pllua_open_numeric, 0);

	luaL_requiref(L, "pllua.jsonb", pllua_open_jsonb, 0);

	/*
	 * complete the initialization of the trusted-mode sandbox.
	 * We do this in untrusted interps too, but for those, we don't
	 * store the module table in a global.
	 *
	 * This must be last, after all module loads, so that it can copy
	 * appropriate modules into the sandbox.
	 */
	luaL_requiref(L, "pllua.trusted.late", pllua_open_trusted_late, 0);
	if (trusted && pllua_do_install_globals)
		lua_setglobal(L, "trusted");

	/* enable interrupt checks */
	if (pllua_do_check_for_interrupts)
		lua_sethook(L, pllua_hook, LUA_MASKRET | LUA_MASKCOUNT, 100000);

	/* don't run user code yet */
	return 0;
}

/*
 * Execute the post-fork init strings. Note that they are executed outside any
 * trusted sandbox, except on_common_init which executes inside.
 */
static int
pllua_run_init_strings(lua_State *L)
{
	bool	trusted;

	/* sheer paranoia */
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED) != LUA_TBOOLEAN)
		luaL_error(L, "inconsistency in interpreter setup");

	trusted = lua_toboolean(L, -1);

	if (trusted)
		pllua_runstring(L, "on_trusted_init", pllua_on_trusted_init, false);
	else
		pllua_runstring(L, "on_untrusted_init", pllua_on_untrusted_init, false);

	pllua_runstring(L, "on_common_init", pllua_on_common_init, trusted);

	/*
	 * Redirect print() output to the client now.
	 */
	lua_pushinteger(L, INFO);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_PRINT_SEVERITY);

	return 0;
}

/*
 * PG-environment part of interpreter setup.
 *
 * Phase 1 can run in postmaster, before we know anything about what the
 * interp will be used for or by whom.
 */
static lua_State *
pllua_newstate_phase1(const char *ident)
{
	MemoryContext	mcxt = NULL;
	MemoryContext	emcxt = NULL;
	lua_State	   *L = NULL;
	int				rc;

	ASSERT_PG_CONTEXT;

	mcxt = AllocSetContextCreate(TopMemoryContext,
								 "PL/Lua context",
								 ALLOCSET_DEFAULT_SIZES);

	emcxt = AllocSetContextCreate(mcxt,
								  "PL/Lua error context",
								  PLLUA_ERROR_CONTEXT_SIZES );

#if LUA_VERSION_NUM == 501
	L = luaL_newstate();
#else
	L = lua_newstate(pllua_alloc, NULL);
#endif

	if (!L)
		elog(ERROR, "Out of memory creating Lua interpreter");

	lua_atpanic(L, pllua_panic);  /* can't throw */

	/*
	 * Since we just created this interpreter, we know we're not in any
	 * protected environment yet, so Lua errors outside of pcall will
	 * become pg errors via pllua_panic. The only possible errors here
	 * should be out-of-memory errors.
	 */

	/* note that pllua_pushcfunction is not available yet. */
	lua_pushcfunction(L, pllua_init_state_phase1);
	lua_pushlightuserdata(L, mcxt);
	lua_pushlightuserdata(L, emcxt);
	lua_pushlightuserdata(L, (void *) ident);
	rc = pllua_pcall_nothrow(L, 3, 0, 0);

	/*
	 * We don't allow phase1 init to do anything that interacts with pg in any
	 * way other than memory allocation. So if we got an error, we can be
	 * pretty sure it's not actually a pg error (the one exception being
	 * out-of-memory errors).
	 *
	 * So, we avoid trying to rethrow any error here, because we might be in
	 * the postmaster and that would be fatal. Leave it to the caller to decide
	 * what to do.
	 */
	if (rc)
	{
		elog(WARNING, "PL/Lua initialization error: %s",
			 (lua_type(L,-1) == LUA_TSTRING) ? lua_tostring(L,-1) : "(not a string)");

		pllua_setcontext(PLLUA_CONTEXT_LUA);
		lua_close(L); /* can't throw, but has internal lua catch blocks */
		pllua_setcontext(PLLUA_CONTEXT_PG);

		MemoryContextDelete(mcxt);

		L = NULL;
	}

	return L;
}


/*
 * PG-environment part of interpreter setup.
 */
static void
pllua_newstate_phase2(lua_State *L,
					  bool trusted,
					  Oid user_id,
					  pllua_interpreter *interp_desc,
					  pllua_activation_record *act)
{
	static bool first_time = true;
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile MemoryContext mcxt = NULL;  /* L's mcxt if known */

	ASSERT_PG_CONTEXT;

	PG_TRY();
	{
		Oid		langoid;

		/* can't throw */
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_MEMORYCONTEXT);
		mcxt = lua_touserdata(L, -1);
		lua_pop(L, 1);

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

		/*
		 * Since we just created this interpreter, we know we're not in any
		 * protected environment yet, so Lua errors outside of pcall will
		 * become pg errors via pllua_panic. In other contexts we must be more
		 * cautious about Lua errors, because of this scenario: if a Lua
		 * function calls into SPI which invokes another Lua function, then any
		 * Lua error thrown in the nested invocation might longjmp back to the
		 * outer interpreter...
		 */
		lua_pushcfunction(L, pllua_init_state_phase2);
		lua_pushboolean(L, trusted);
		lua_pushinteger(L, (lua_Integer) user_id);
		lua_pushinteger(L, (lua_Integer) langoid);
		lua_pushlightuserdata(L, interp_desc);
		pllua_pcall(L, 4, 0, 0);

		if (first_time)
		{
			on_proc_exit(pllua_fini, (Datum)0);
			CacheRegisterRelcacheCallback(pllua_relcache_callback, (Datum)0);
			CacheRegisterSyscacheCallback(TYPEOID, pllua_syscache_typeoid_callback, (Datum)0);
			CacheRegisterSyscacheCallback(TRFTYPELANG, pllua_syscache_typeoid_callback, (Datum)0);
			CacheRegisterSyscacheCallback(CASTSOURCETARGET, pllua_syscache_cast_callback, (Datum)0);
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
		pllua_syscache_cast_callback(PointerGetDatum(interp_desc), CASTSOURCETARGET, 0);

		/*
		 * Now that we have everything set up, it should finally be safe to run
		 * some arbitrary code that might access the db.
		 */
		lua_pushcfunction(L, pllua_run_init_strings);
		pllua_pcall(L, 0, 0, 0);
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
