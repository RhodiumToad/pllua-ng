/* preload.c */

/*
 * Preloading of lua modules built into the binary.
 */

#include "pllua.h"

/*
 * OSX has to be different, of course.
 */
#ifdef __darwin__

#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

static void
osx_get_chunk(lua_State *L,
			  const char *name,
			  const void **ptr,
			  size_t *sz)
{
	void *tptr;
	Dl_info info;
	if (!dladdr(&osx_get_chunk, &info))
		luaL_error(L, "dladdr failed: %s", dlerror());
	tptr = getsectiondata(info.dli_fbase, "binary", name, sz);
	if (!tptr)
		luaL_error(L, "getsectiondata failed");
	*ptr = tptr;
	return;
}

#define GETCHUNK(L_, chunk_,start_,sz_) \
	osx_get_chunk(L_, #chunk_, &(start_), &(sz_))

#else

extern const char _binary_src_compat_luac_start[];
extern const char _binary_src_compat_luac_end[];

#define MKSYM(a_,b_,c_) a_##b_##c_

#define GETCHUNK(L_, chunk_,start_,sz_)						\
	do { (start_) = MKSYM(_binary_src_,chunk_,_start);		\
		 (sz_) = MKSYM(_binary_src_,chunk_,_end)			\
				 - MKSYM(_binary_src_,chunk_,_start);		\
	} while (0)

#endif

static void
pllua_load_binary_chunk(lua_State *L, const char *chunkname, const char *start, size_t len)
{
	int rc = luaL_loadbuffer(L, start, len, chunkname);
	if (rc)
		lua_error(L);
}

/*
 * Upvalue 1 is the metatable to use with the environment
 */
int
pllua_preload_compat(lua_State *L)
{
	const void *ptr;
	size_t sz;
	GETCHUNK(L, compat_luac, ptr, sz);
	pllua_load_binary_chunk(L, "compat.lua", ptr, sz);
	lua_newtable(L);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setmetatable(L, -2);
	pllua_set_environment(L, -2);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_call(L, 1, 1);
	return 1;
}
