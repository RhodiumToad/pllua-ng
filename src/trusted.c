/* trusted.c */

#include "pllua.h"

/* from init.c */
extern bool pllua_do_install_globals;

/*
 * Trusted versions or wrappers for functionality that we need to restrict in a
 * trusted interpreter.
 *
 */

/*
 * this defines the trusted subset of the "os" package (installed as
 * "trusted.os" in the outer environment)
 */
static struct luaL_Reg trusted_os_funcs[] = {
	{ "date", NULL },
	{ "clock", NULL },
	{ "time", NULL },
	{ "difftime", NULL },
    { NULL, NULL }
};

static int
pllua_open_trusted_os(lua_State *L)
{
	const luaL_Reg *p;
	lua_getglobal(L, "os");
	luaL_newlibtable(L, trusted_os_funcs);
	for (p = trusted_os_funcs; p->name; ++p)
	{
		lua_getfield(L, -2, p->name);
		lua_setfield(L, -2, p->name);
	}
	return 1;
}

/*
 * load(chunk[,chunkname[,mode[,env]]])
 *
 * Wrapper must force "mode" to be "t" to disallow loading binary chunks.  Also
 * must force "env" to be the sandbox env if not supplied by the caller.
 *
 * Punts to _G.load after munging the args.
 */
static int
pllua_t_load(lua_State *L)
{
	int			nargs = lua_gettop(L);
	if (nargs < 4)
	{
		lua_settop(L, 3);
		nargs = 4;
		lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	}
	lua_pushstring(L, "t");
	lua_replace(L, 3);
	lua_getglobal(L, "load");
	lua_insert(L, 1);
	lua_call(L, nargs, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * user-facing "require" function
 */

static void pllua_t_require_findloader(lua_State *L, int nd, const char *name);

static int
pllua_t_require(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	lua_settop(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_getfield(L, 2, name);  /* LOADED[name] */
	if (lua_toboolean(L, -1))  /* is it there? */
		return 1;  /* package is already loaded */
	lua_pop(L, 1);  /* remove 'getfield' result */

	if (lua_getfield(L, lua_upvalueindex(1), "searchers") != LUA_TTABLE)
		luaL_error(L, "'package.searchers' must be a table");
	pllua_t_require_findloader(L, -1, name);

	lua_pushstring(L, name);  /* pass name as argument to module loader */
	lua_insert(L, -2);  /* name is 1st argument (before search data) */
	lua_call(L, 2, 1);  /* run loader to load module */
	/*
	 * If the module returned nil, see if it stored a non-nil value in the
	 * loaded table itself (older deprecated protocol). If not, use "true".
	 */
	if (lua_isnil(L, -1) &&
		lua_getfield(L, 2, name) == LUA_TNIL)
			lua_pushboolean(L, 1);  /* use true as result */
	lua_pushvalue(L, -1);
	lua_setfield(L, 2, name);  /* LOADED[name] = returned value */
	return 1;
}

/*
 * "require" function helper
 */
static void
pllua_t_require_findloader(lua_State *L, int nd, const char *name)
{
	int			i;
	luaL_Buffer msg;  /* to build error message */

	nd = lua_absindex(L, nd);

	luaL_buffinit(L, &msg);

	/* iterate over available searchers to find a loader */
	for (i = 1; ; ++i)
	{
		if (lua_rawgeti(L, nd, i) == LUA_TNIL)
		{
			lua_pop(L, 1);  /* remove nil */
			luaL_pushresult(&msg);  /* create error message */
			luaL_error(L, "module '%s' not found:%s", name, lua_tostring(L, -1));
		}

		lua_pushstring(L, name);
		lua_call(L, 1, 2);  /* call it */

		if (lua_isfunction(L, -2))  /* did it find a loader? */
			return;  /* module loader found */
		else if (lua_isstring(L, -2))   /* searcher returned error message? */
		{
			lua_pop(L, 1);  /* remove extra return */
			luaL_addvalue(&msg);  /* concatenate error message */
		}
		else
			lua_pop(L, 2);  /* remove both returns */
	}
}

/*
 * searcher functions are called as
 *
 * searcher(name)  returns func,arg
 *
 */
static int
pllua_package_preload_search(lua_State *L)
{
	/* preload searcher works entirely inside the sandbox */
	const char *name = luaL_checkstring(L, 1);
	lua_getfield(L, lua_upvalueindex(1), "preload");
	lua_pushstring(L, name);
	if (lua_gettable(L, -2) == LUA_TNIL)
	{
		lua_pushfstring(L, "\n\tno field package.preload['%s']", name);
		return 1;
	}
	lua_pushnil(L);
	return 2;
}

static int
pllua_package_allowed_search(lua_State *L)
{
	/*
	 * allowed searcher works outside the sandbox; the sandbox can't see its
	 * own "allow" list
	 */
	const char *name = luaL_checkstring(L, 1);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushstring(L, name);
	if (lua_gettable(L, -2) == LUA_TNIL)
	{
		lua_pushfstring(L, "\n\tno module '%s' in list of allowed modules", name);
		return 1;
	}
	lua_pushnil(L);
	return 2;
}

static int
pllua_open_trusted_package(lua_State *L)
{
	lua_newtable(L);

	lua_pushvalue(L, -1);
	lua_pushcclosure(L, pllua_t_require, 1);
	lua_setfield(L, -2, "require");

	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_setfield(L, -2, "loaded");

	lua_newtable(L);
	lua_setfield(L, -2, "preload");

	lua_newtable(L);

	/* first entry in searchers list is the preload searcher */
	lua_pushvalue(L, -2);
	lua_pushcclosure(L, pllua_package_preload_search, 1);
	lua_seti(L, -2, 1);

	/* second entry in searchers list is the permitted-package searcher */
	/* this operates outside the sandbox so we don't close it over sandbox.package */
	lua_pushcfunction(L, pllua_package_allowed_search);
	lua_seti(L, -2, 2);

	lua_setfield(L, -2, "searchers");

	return 1;
}


/*
 * These funcs appear as trusted.func outside the sandbox, for management
 * purposes.
 *
 * trusted.require("module", ["newname"], "mode")
 *    -- as if _G.newname = module   was done inside the sandbox (the
 *       actual 'require "module"' is done outside)
 *
 * trusted.allow("module", ["newname"], "mode", "globname")
 *    -- allow  require "newname"  to work inside the sandbox
 *       note that "module" WILL be loaded immediately (outside)
 *
 * trusted.remove("newname","globname")
 *    -- remove the module from the sandbox; INEFFECTIVE if code has already
 *       been run inside.
 *
 * modules "require"d outside of the sandbox are not exposed as global
 * variables inside it unless specified with require or allow. However,
 * anything a module stores inside itself, including references to other
 * modules, will be accessible if the module is.
 *
 * CAVEAT SUPERUSER: it will be very hard to ensure that any given loaded
 * module doesn't expose the real global table, its functions, or dangerous
 * packages to the untrusted code.
 */
static int
pllua_bind_one_value(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	return 1;
}

static int
pllua_bind_one_call(lua_State *L)
{
	int i;
	lua_settop(L, 0);
	for (i = 1; !lua_isnone(L, lua_upvalueindex(i)); ++i)
	{
		if (i >= 10 && (i % 10) == 0)
			luaL_checkstack(L, 20, NULL);
		lua_pushvalue(L, lua_upvalueindex(i));
	}
	if (i < 2)
		return 0;
	lua_call(L, i-2, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * f(modefunc,requirefunc,modulename)
 *   = return modefunc(requirefunc(modulename))
 *
 * This does the actual out-of-sandbox require, it's split into its own
 * function so that we can wrap it up as a closure for deferred execution
 */
static int
pllua_do_trusted_require(lua_State *L)
{
	lua_settop(L, 3);
	lua_call(L, 1, 1);
	lua_call(L, 1, 1);
	return 1;
}

/*
 * _allow(modname,newname,mode,global,load_now)
 */
static int
pllua_trusted_allow(lua_State *L)
{
	bool load_now = false;

	lua_settop(L, 5);
	luaL_checkstring(L, 1);
	luaL_optstring(L, 2, NULL);
	if (lua_isnil(L, 2))
	{
		lua_pushvalue(L, 1);
		lua_replace(L, 2);
	}
	if (lua_type(L, 4) == LUA_TBOOLEAN)
	{
		if (lua_toboolean(L, 4))
			lua_pushvalue(L, 2);
		else
			lua_pushnil(L);
		lua_replace(L, 4);
	}
	else
		luaL_optstring(L, 4, NULL);

	if (!lua_isnil(L, 4) || lua_toboolean(L, 5))
		load_now = true;

	if (!lua_isfunction(L, 3))
	{
		const char *mode = luaL_optstring(L, 3, "proxy");
		lua_getfield(L, lua_upvalueindex(2), mode);
		if (!lua_isfunction(L, -1))
			luaL_error(L, "trusted.modes value is not a function");
		lua_replace(L, 3);
	}

	lua_pushcfunction(L, pllua_do_trusted_require);
	lua_pushvalue(L, 3);
	lua_pushvalue(L, lua_upvalueindex(3));  /* _G.require */
	lua_pushvalue(L, 1);

	if (load_now)
	{
		lua_call(L, 3, 1);
		lua_pushvalue(L, -1);
		lua_pushcclosure(L, pllua_bind_one_value, 1);
	}
	else
		lua_pushcclosure(L, pllua_bind_one_call, 4);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, -3);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	if (lua_isnil(L, 4))
		return 0;

	lua_pop(L, 1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, -3);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 4);
	lua_pushvalue(L, -3);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	return 0;
}

static int
pllua_trusted_remove(lua_State *L)
{
	lua_settop(L, 2);
	luaL_checkstring(L, 1);
	if (lua_type(L, 2) == LUA_TBOOLEAN)
	{
		if (lua_toboolean(L, 2))
			lua_pushvalue(L, 1);
		else
			lua_pushnil(L);
		lua_replace(L, 2);
	}
	else
		luaL_optstring(L, 2, NULL);
	/* kill sandbox's _G.globname */
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 2);
	lua_pushnil(L);
	lua_rawset(L, -3);
	/* kill ALLOW and LOADED entries for modname */
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_rawset(L, -3);
	lua_rawgetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_LOADED);
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	lua_rawset(L, -3);
	return 0;
}

/*
 * upvalue 1 is our own closure, upvalue 2 is the memo table
 */
static int
pllua_trusted_mode_copy_inner(lua_State *L)
{
	lua_settop(L, 1);

	lua_pushvalue(L, 1);
	if (lua_rawget(L, lua_upvalueindex(2)) != LUA_TNIL)
		return 1;
	lua_pop(L, 1);

	lua_newtable(L);  /* slot 2 */

	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, lua_upvalueindex(2));

	/*
	 * We intentionally raw-iterate rather than pairs()ing
	 */
	lua_pushnil(L);
	while (lua_next(L, 1))
	{
		/* ... key val */
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		/* ... key key val */
		if (lua_type(L, -1) == LUA_TTABLE)
		{
			lua_pushvalue(L, lua_upvalueindex(1));
			lua_insert(L, -2);
			lua_call(L, 1, 1);
		}
		lua_rawset(L, 2);
		/* ... key */
	}

	return 1;
}

static int
pllua_trusted_mode_scopy(lua_State *L)
{
	lua_settop(L, 1);
	lua_newtable(L);  /* slot 2 */

	/*
	 * We intentionally raw-iterate rather than pairs()ing
	 */
	lua_pushnil(L);
	while (lua_next(L, 1))
	{
		/* ... key val */
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		/* ... key key val */
		lua_rawset(L, 2);
		/* ... key */
	}

	return 1;
}

static int
pllua_trusted_mode_direct(lua_State *L)
{
	lua_settop(L, 1);
	return 1;
}

/*
 * Proxy a function call.
 *
 * Upvalue 1 is the real function to call.
 * Upvalue 2 is the value to sub for the first arg (self).
 */
static int
pllua_trusted_mode_proxy_wrap(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(2));
	if (lua_gettop(L) > 1)
		lua_replace(L, 1);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	return lua_gettop(L);
}

/*
 * Common code for metatable handling between "proxy" and "sproxy" modes
 */
static void
pllua_trusted_mode_proxy_metatable(lua_State *L, int ot, int mt)
{
	/*
	 * Logic for metatables:
	 *
	 *   __index:
	 *     always points to the old table, whether or not the old
	 *     metatable has it
	 *   __newindex:
	 *     Points to the old table iff the old metatable has a
	 *     __newindex entry, otherwise is not set
	 *   __call:
	 *     wrapped as a function call if present
	 *   __metatable:
	 *     copied if present, otherwise set to true
	 *   any other key:
	 *     just copied, since we can't hope to guess the semantics
	 *
	 */
	if (lua_getmetatable(L, ot))
	{
		lua_pushnil(L);
		while (lua_next(L, -2))
		{
			const char *keyname = lua_tostring(L, -2);
			/* metatab key val */
			if (strcmp(keyname, "__index") == 0)
				lua_pop(L, 1);
			else if (strcmp(keyname, "__newindex") == 0)
			{
				lua_pushvalue(L, -1);
				lua_setfield(L, mt, keyname);
				lua_pop(L, 1);
			}
			else if (strcmp(keyname, "__call") == 0)
			{
				lua_pushvalue(L, 1);
				lua_pushcclosure(L, pllua_trusted_mode_proxy_wrap, 2);
				lua_setfield(L, mt, keyname);
			}
			else
			{
				lua_pushvalue(L, -2);
				lua_insert(L, -2);
				/* ... key key val */
				lua_rawset(L, mt);
			}
			/* metatab key */
		}
		lua_pop(L, 1);
	}
}

static int
pllua_trusted_mode_sproxy(lua_State *L)
{
	lua_settop(L, 1);
	if (lua_type(L, 1) != LUA_TTABLE)
		return 1;

	lua_newtable(L);  /* slot 2 */
	lua_newtable(L);  /* slot 3 for now */
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");

	pllua_trusted_mode_proxy_metatable(L, 1, 3);

	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, 2);

	return 1;
}

static int
pllua_trusted_mode_proxy_inner(lua_State *L)
{
	lua_settop(L, 1);
	if (lua_type(L, 1) != LUA_TTABLE)
		return 1;

	lua_pushvalue(L, 1);
	if (lua_rawget(L, lua_upvalueindex(2)) != LUA_TNIL)
		return 1;
	lua_pop(L, 1);

	lua_newtable(L);  /* slot 2 */

	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, lua_upvalueindex(2));

	lua_newtable(L);  /* slot 3 for now */
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "__metatable");

	pllua_trusted_mode_proxy_metatable(L, 1, 3);

	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, 2);

	/*
	 * We intentionally raw-iterate rather than pairs()ing
	 */
	lua_pushnil(L);
	while (lua_next(L, 1))
	{
		/* ... key val */
		if (lua_type(L, -1) == LUA_TTABLE)
		{
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			/* ... key key val */
			lua_pushvalue(L, lua_upvalueindex(1));
			lua_insert(L, -2);
			lua_call(L, 1, 1);
			lua_rawset(L, 2);
		}
		else
			lua_pop(L, 1);
		/* ... key */
	}

	return 1;
}

static int
pllua_trusted_mode_outer(lua_State *L)
{
	lua_settop(L, 1);
	if (lua_type(L, 1) != LUA_TTABLE)
		return 1;
	lua_pushnil(L);
	lua_newtable(L);
	if (lua_toboolean(L, lua_upvalueindex(1)))
		lua_pushcclosure(L, pllua_trusted_mode_proxy_inner, 2);
	else
		lua_pushcclosure(L, pllua_trusted_mode_copy_inner, 2);
	lua_pushvalue(L, -1);
	lua_setupvalue(L, -2, 1);
	lua_insert(L, 1);
	lua_call(L, 1, 1);
	return 1;
}

static struct luaL_Reg trusted_modes_funcs[] = {
	{ "direct", pllua_trusted_mode_direct },
	{ "scopy", pllua_trusted_mode_scopy },
	{ "sproxy", pllua_trusted_mode_sproxy },
	{ NULL, NULL }
};

static struct luaL_Reg trusted_funcs[] = {
	{ "_allow", pllua_trusted_allow },
	{ "remove", pllua_trusted_remove },
	{ NULL, NULL }
};

/*
 * This is called with the first arg being the "trusted" module table
 */
static const char *trusted_lua =
"local lib = ...\n"
"local unpack = table.unpack or unpack\n"
"local type, ipairs = type, ipairs\n"
"local allow = lib._allow\n"
#if LUA_VERSION_NUM >= 502
"_ENV = nil\n"
#endif
"function lib.allow(mod,new,mode,glob,immed)\n"
"    if type(mod)==\"string\" then\n"
"        allow(mod,new,mode,glob,immed)\n"
"    elseif type(mod)==\"table\" then\n"
"        for i,v in ipairs(mod) do\n"
"            local e_mod, e_new, e_mode, e_glob, e_immed\n"
"              = unpack(type(v)==\"table\" and v or { v },1,5)\n"
"            if e_glob == nil then e_glob = glob end\n"
"            if e_immed == nil then e_immed = immed end\n"
"            allow(e_mod, e_new, e_mode or mode, e_glob, e_immed)\n"
"        end\n"
"    end\n"
"end\n"
"function lib.require(mod,new,mode)\n"
"    lib.allow(mod,new,mode,true)\n"
"end\n"
;

static struct luaL_Reg sandbox_funcs[] = {
	/* from this file */
	{ "load", pllua_t_load },
	/* "require" is set from package.require */
    {NULL, NULL}
};

/*
 * Whitelist the standard lua globals that we allow into the sandbox.
 */
struct global_info {
	const char *name;
	const char *libname;
};
static struct global_info sandbox_lua_globals[] = {
	/* base lib */
	{ "assert", NULL },
	{ "collectgarbage", NULL },
	{ "error", NULL },
	{ "getmetatable", NULL },
	{ "ipairs", NULL },
	{ "next", NULL },
	{ "pairs", NULL },
	{ "rawequal", NULL },
	{ "rawlen", NULL },
	{ "rawget", NULL },
	{ "rawset", NULL },
	{ "select", NULL },
	{ "setmetatable", NULL },
	{ "tonumber", NULL },
	{ "tostring", NULL },
	{ "type", NULL },
	{ "unpack", NULL },     /* for luajit */
	{ "_VERSION", NULL },
	{ "_PLVERSION", NULL },
	{ "_PLREVISION", NULL },
	{ "_PL_LOAD_TIME", NULL },
	{ "_PL_IDENT", NULL },
	{ "_PG_VERSION", NULL },
	{ "_PG_VERSION_NUM", NULL },
	{ NULL, "pllua.print" },
	{ "print", NULL },
	{ NULL, "pllua.error" },
	{ "pcall", NULL },
	{ "xpcall", NULL },
	{ "lpcall", NULL },
	{ NULL, "pllua.trusted.package" },
	{ "require", NULL },
	{ NULL, NULL }
};

/*
 * List of packages to expose to the sandbox by default
 *
 * "mode" should be either "copy" or "proxy" for anything that might get used
 * by unsandboxed code. "direct" is ok for the trusted OS library because that
 * is not used outside the sandbox.
 */
struct module_info
{
	const char *name;
	const char *newname;
	const char *mode;
	const char *globname;
};

static struct module_info sandbox_packages_early[] = {
	{ "coroutine",				NULL,		"copy",		"coroutine"		},
	{ "string",					NULL,		"copy",		"string"		},
#if LUA_VERSION_NUM == 503
	{ "utf8",					NULL,		"copy",		"utf8"			},
#endif
	{ "table",					NULL,		"copy",		"table"			},
	{ "math",					NULL,		"copy",		"math"			},
	{ "pllua.trusted.os",		"os",		"direct",	"os"			},
	{ "pllua.trusted.package",	"package",	"direct",	"package"		},
	{ "pllua.error",			NULL,		"copy",		NULL			},
	{ NULL, NULL }
};

static struct module_info sandbox_packages_late[] = {
	{ "pllua.spi",			NULL,	"proxy",	"spi"			},
	{ "pllua.pgtype",		NULL,	"proxy",	"pgtype"		},
	{ "pllua.elog",			NULL,	"copy",		NULL			},
	{ "pllua.numeric",		NULL,	"copy",		NULL			},
	{ "pllua.jsonb",		NULL,	"copy",		NULL			},
	{ "pllua.time",			NULL,	"copy",		NULL			},
	{ NULL, NULL }
};

/*
 * This isn't really a module but handles the late initialization phase.
 */
int
pllua_open_trusted_late(lua_State *L)
{
	const struct module_info *np;

	lua_settop(L, 0);
	luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_getfield(L, -1, "pllua.trusted");
	lua_replace(L, 1);

	for (np = sandbox_packages_late; np->name; ++np)
	{
		lua_getfield(L, 1, "_allow");
		lua_pushstring(L, np->name);
		if (np->newname)
			lua_pushstring(L, np->newname);
		else
			lua_pushnil(L);
		lua_pushstring(L, np->mode);
		if (np->globname && pllua_do_install_globals)
			lua_pushstring(L, np->globname);
		else
			lua_pushnil(L);
		lua_pushboolean(L, 1);
		lua_call(L, 5, 0);
	}

	lua_pushvalue(L, 1);
	return 1;
}

int
pllua_open_trusted(lua_State *L)
{
	const struct global_info *p;
	const struct module_info *np;
	lua_settop(L,0);
	/* create the package table itself: index 1 */
	luaL_newlibtable(L, trusted_funcs);

	lua_pushvalue(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, trusted_modes_funcs, 0);
	lua_pushboolean(L, 0);
	lua_pushcclosure(L, pllua_trusted_mode_outer, 1);
	lua_setfield(L, -2, "copy");
	lua_pushboolean(L, 1);
	lua_pushcclosure(L, pllua_trusted_mode_outer, 1);
	lua_setfield(L, -2, "proxy");
	lua_pushvalue(L, -1);
	lua_setfield(L, 1, "modes");

	lua_getglobal(L, "require");

	luaL_setfuncs(L, trusted_funcs, 3);

	if (luaL_loadbuffer(L, trusted_lua, strlen(trusted_lua), "trusted.lua") == LUA_OK)
	{
		lua_pushvalue(L, 1);
		lua_call(L, 1, 0);
	}
	else
		lua_error(L);

	/* create the "permitted package" table */
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX_ALLOW);
	lua_setfield(L, 1, "permit");

	/* create the infrastructure of the sandbox module system */
	luaL_requiref(L, "pllua.trusted.package", pllua_open_trusted_package, 0);
	lua_pop(L, 1);

	/* create the trusted sandbox: index 2 */
	lua_newtable(L);
	lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_pushglobaltable(L);
	for (p = sandbox_lua_globals; p->name || p->libname; ++p)
	{
		if (p->libname)
		{
			lua_getfield(L, -2, p->libname);
			lua_replace(L, -2);
		}
		if (p->name)
		{
			lua_getfield(L, -1, p->name);
			lua_setfield(L, 2, p->name);
		}
	}
	lua_pop(L, 2);
	lua_pushvalue(L, 2);
	lua_setfield(L, 2, "_G");
	luaL_setfuncs(L, sandbox_funcs, 0);
	lua_pushvalue(L, 2);
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_TRUSTED_SANDBOX);
	lua_pushvalue(L, 2);
	lua_setfield(L, 1, "sandbox");

	/* proxy metatable for the sandbox */
	lua_newtable(L);
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, "__index");
	lua_rawsetp(L, LUA_REGISTRYINDEX, PLLUA_SANDBOX_META);

	/* create the minimal trusted "os" library */
	luaL_requiref(L, "pllua.trusted.os", pllua_open_trusted_os, 0);
	lua_pop(L, 1);

	/*
	 * require standard modules into the sandbox
	 */
	lua_getfield(L, 1, "_allow");
	for (np = sandbox_packages_early; np->name; ++np)
	{
		lua_pushvalue(L, -1);
		lua_pushstring(L, np->name);
		if (np->newname)
			lua_pushstring(L, np->newname);
		else
			lua_pushnil(L);
		lua_pushstring(L, np->mode);
		if (np->globname)
			lua_pushstring(L, np->globname);
		else
			lua_pushnil(L);
		lua_pushboolean(L, 1);
		lua_call(L, 5, 0);
	}
	lua_pop(L, 1);

	/*
	 * Ugly hack; we can't tell reliably at compile time whether the lua
	 * library we're linked to enables bit32 or not. So just check whether
	 * it exists and if so, run _allow for it as a special case.
	 */
#ifdef LUA_BITLIBNAME
	lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_getfield(L, -1, LUA_BITLIBNAME);
	if (!lua_isnil(L, -1))
	{
		lua_getfield(L, 1, "_allow");
		lua_pushstring(L, LUA_BITLIBNAME);
		lua_pushnil(L);
		lua_pushstring(L, "copy");
		lua_pushboolean(L, 1);
		lua_call(L, 4, 0);
	}
	lua_pop(L, 2);
#endif
	/*
	 * global "string" is the metatable for all string objects. We don't
	 * want the sandbox to be able to get it via getmetatable("")
	 */
	lua_pushstring(L, "");
	if (lua_getmetatable(L, -1))
	{
		lua_pushboolean(L, 1);
		lua_setfield(L, -2, "__metatable");
		lua_pop(L, 2);
	}
	else
		lua_pop(L, 1);

	/* done */

	lua_pushvalue(L, 1);
	return 1;
}
