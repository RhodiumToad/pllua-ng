/* paths.c */

#include "pllua.h"

#include "miscadmin.h"

typedef void (pathfunc_type)(const char *, char *);

static int
pllua_get_path(lua_State *L)
{
	pathfunc_type *func = (pathfunc_type *) lua_touserdata(L, lua_upvalueindex(1));
	char path[MAXPGPATH];

	path[0] = '\0';
	(*func)(my_exec_path, path);

	if (path[0])
		lua_pushstring(L, path);
	else
		lua_pushnil(L);

	return 1;
}

static void
get_bin_path(const char *exec_path, char *retpath)
{
	char *lastsep;
	strlcpy(retpath, exec_path, MAXPGPATH);
	lastsep = strrchr(retpath, '/');
	if (lastsep)
		*lastsep = '\0';
	else
		*retpath = '\0';
}

static struct {
	const char *name;
	pathfunc_type *func;
} path_funcs[] = {
	{ "bin",		get_bin_path },
	{ "doc",		get_doc_path },
	{ "etc",		get_etc_path },
	{ "html",		get_html_path },
	{ "include",	get_include_path },
	{ "includeserver", get_includeserver_path },
	{ "lib",		get_lib_path },
	{ "libdir",		get_pkglib_path },
	{ "locale",		get_locale_path },
	{ "man",		get_man_path },
	{ "pkginclude", get_pkginclude_path },
	{ "pkglib",		get_pkglib_path },
	{ "share",		get_share_path },
	{ NULL, NULL }
};

int pllua_open_paths(lua_State *L)
{
	int i;

	lua_settop(L, 0);

	lua_newtable(L);

	for (i = 0; path_funcs[i].name; ++i)
	{
		lua_pushlightuserdata(L, path_funcs[i].func);
		lua_pushcclosure(L, pllua_get_path, 1);
		lua_setfield(L, 1, path_funcs[i].name);
	}

	return 1;
}
