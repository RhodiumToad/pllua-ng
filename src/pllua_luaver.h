/* pllua_luaver.h */

#ifndef PLLUA_LUAVER_H
#define PLLUA_LUAVER_H

#ifndef LUAJIT_VERSION_NUM
#define LUAJIT_VERSION_NUM 0
#endif

/*
 * We can only use 5.1 environments in a compatible way to 5.2+ uservalues if
 * we forcibly set an environment on every userdata we create.
 */
#if LUA_VERSION_NUM == 501
#define MANDATORY_USERVALUE 1
#else
#define MANDATORY_USERVALUE 0
#endif

/*
 * pllua_pushcfunction must absolutely not throw error.
 *
 * In Lua 5.4+ (and also 5.3.5+, but we don't check that) it's safe to just use
 * lua_pushcfunction, but in 5.1 (by design) and 5.3.[34] (due to bugs) we must
 * instead arrange to store all the function values in the registry, and use a
 * rawget to fetch them.
 */
#if LUA_VERSION_NUM > 503

#define pllua_pushcfunction(L_,f_) lua_pushcfunction(L_,f_)

#else

#define pllua_pushcfunction(L_,f_) do {					\
		int rc_ PG_USED_FOR_ASSERTS_ONLY;				\
		rc_ = lua_rawgetp((L_),LUA_REGISTRYINDEX,(f_));	\
		Assert(rc_==LUA_TFUNCTION); } while(0)

#endif

/*
 * used to label functions that need registration despite not being
 * directly passed to pcall or cpcall; the first arg is unused
 */
#define pllua_register_cfunc(L_, f_) (f_)

/*
 * Function to use to set an environment on a code chunk.
 */
#if LUA_VERSION_NUM == 501
#define pllua_set_environment(L_,i_) lua_setfenv(L, i_)
#else
#define pllua_set_environment(L_,i_) lua_setupvalue(L_, i_, 1)
#endif

/*
 * Handle API differences for lua_resume by emulating the 5.4 API on earlier
 * versions. Also fake out the warning system on earlier versions, and provide
 * a minimal emulation of <close> that works for normal exits (leaving error
 * exits to be cleaned up by the GC, but that can't be helped).
 */
#if LUA_VERSION_NUM < 504

static inline int
pllua_resume(lua_State *L, lua_State *from, int nargs, int *nret)
{
#if LUA_VERSION_NUM == 501
	int rc = (lua_resume)(L, nargs);
#else
	int rc = (lua_resume)(L, from, nargs);
#endif
	*nret = lua_gettop(L);
	return rc;
}
#define lua_resume(L_,f_,a_,r_) (pllua_resume(L_,f_,a_,r_))

#define lua_setwarnf(L_, f_, p_) ((void)(f_))

#define lua_setcstacklimit(L_, n_) (200)

#define lua_resetthread(L_) (LUA_OK)

#define PLLUA_WARNBUF_SIZE 4

#define lua_toclose(L_, i_) ((void)0)

static inline void
pllua_closevar(lua_State *L, int idx)
{
	if (lua_toboolean(L, idx)
		&& luaL_callmeta(L, idx, "__close"))
		lua_pop(L, 1);
}

#else

#define PLLUA_WARNBUF_SIZE 1000

#define pllua_closevar(L_, i_) ((void)0)

#endif /* LUA_VERSION_NUM < 504 */

#endif
