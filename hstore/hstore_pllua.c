/* hstore_pllua.c */

/* note, we do not support out-of-pllua-tree building */
#include "pllua.h"

#include "hstore/hstore.h"

#include "mb/pg_wchar.h"

PG_MODULE_MAGIC;

#ifndef PG_GETARG_HSTORE_P
#define PG_GETARG_HSTORE_P(h_) PG_GETARG_HS(h_)
#endif

extern void _PG_init(void);

/* Linkage to functions in hstore module */
typedef HStore *(*hstoreUpgrade_t) (Datum orig);
static hstoreUpgrade_t hstoreUpgrade_p;
typedef int (*hstoreUniquePairs_t) (Pairs *a, int32 l, int32 *buflen);
static hstoreUniquePairs_t hstoreUniquePairs_p;
typedef HStore *(*hstorePairs_t) (Pairs *pairs, int32 pcount, int32 buflen);
static hstorePairs_t hstorePairs_p;
typedef size_t (*hstoreCheckKeyLen_t) (size_t len);
static hstoreCheckKeyLen_t hstoreCheckKeyLen_p;
typedef size_t (*hstoreCheckValLen_t) (size_t len);
static hstoreCheckValLen_t hstoreCheckValLen_p;

/* Linkage to functions in pllua module */
typedef void (*pllua_pcall_t)(lua_State *L, int nargs, int nresults, int msgh);
static pllua_pcall_t pllua_pcall_p;
typedef lua_CFunction pllua_trampoline_t;
static lua_CFunction pllua_trampoline_p;
typedef bool (*pllua_pairs_start_t) (lua_State *L, int nd, bool noerror);
static pllua_pairs_start_t pllua_pairs_start_p;
typedef int (*pllua_pairs_next_t) (lua_State *L);
static pllua_pairs_next_t pllua_pairs_next_p;
typedef const char * (*pllua_tolstring_t) (lua_State *L, int idx, size_t *len);
static pllua_tolstring_t pllua_tolstring_p;

/*
 * Module initialize function: fetch function pointers for cross-module calls.
 */
void
_PG_init(void)
{
#define EXTFUNCS(x_) #x_
#define EXTFUNCT(xp_) xp_##_t
#define EXTFUNCP(xp_) xp_##_p
#define EXTFUNC(lib_, n_)									\
	AssertVariableIsOfType(&n_, EXTFUNCT(n_));				\
	EXTFUNCP(n_) = (EXTFUNCT(n_))							\
		load_external_function(lib_, EXTFUNCS(n_), true, NULL);

	EXTFUNC("$libdir/hstore", hstoreUpgrade);
	EXTFUNC("$libdir/hstore", hstoreUniquePairs);
	EXTFUNC("$libdir/hstore", hstorePairs);
	EXTFUNC("$libdir/hstore", hstoreCheckKeyLen);
	EXTFUNC("$libdir/hstore", hstoreCheckValLen);

	EXTFUNC("$libdir/pllua", pllua_pcall);
	EXTFUNC("$libdir/pllua", pllua_trampoline);
	EXTFUNC("$libdir/pllua", pllua_pairs_start);
	EXTFUNC("$libdir/pllua", pllua_pairs_next);
	EXTFUNC("$libdir/pllua", pllua_tolstring);
}


/* These defines must be after the module init function */
#define hstoreUpgrade hstoreUpgrade_p
#define hstoreUniquePairs hstoreUniquePairs_p
#define hstorePairs hstorePairs_p
#define hstoreCheckKeyLen hstoreCheckKeyLen_p
#define hstoreCheckValLen hstoreCheckValLen_p

#define pllua_pcall pllua_pcall_p
#define pllua_trampoline pllua_trampoline_p
#define pllua_pairs_start pllua_pairs_start_p
#define pllua_pairs_next pllua_pairs_next_p
#define pllua_tolstring pllua_tolstring_p


static int
hstore_to_pllua_real(lua_State *L)
{
	HStore	   *in = lua_touserdata(L, 1);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);

	lua_createtable(L, 0, count);

	for (i = 0; i < count; i++)
	{
		lua_pushlstring(L,
						HSTORE_KEY(entries, base, i),
						HSTORE_KEYLEN(entries, i));
		if (HSTORE_VALISNULL(entries, i))
			lua_pushboolean(L, 0);
		else
			lua_pushlstring(L,
						   HSTORE_VAL(entries, base, i),
						   HSTORE_VALLEN(entries, i));
		lua_rawset(L, -3);
	}

	return 1;
}

/*
 * equivalent to:
 *
 *  local keys,vals = {},{}
 *  for k,v in pairs(hs) do keys[#keys+1] = k vals[#vals+1] = v end
 *  then makes a full userdata with a Pairs array and refs to keys,vals
 *
 */
static int
pllua_to_hstore_real(lua_State *L)
{
	Pairs	   *pairs = NULL;
	int			idx = 0;
	int			pcount = 0;
	bool		metaloop;

	/*
	 * Decline if there isn't exactly 1 arg.
	 */
	if (lua_gettop(L) != 1)
	{
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}

	lua_newtable(L); /* index 2: keys */
	lua_newtable(L); /* index 3: vals */

	metaloop = pllua_pairs_start(L, 1, true);

	/*
	 * If it doesn't have a pairs metamethod and it's not a plain table,
	 * then we have to decline it.
	 */
	if (!metaloop && !lua_istable(L, 1))
	{
		/* pairs_start already pushed one nil */
		lua_pushnil(L);
		return 2;
	}

	while (metaloop ? pllua_pairs_next(L) : lua_next(L, 1))
	{
		++idx;
		if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && !lua_toboolean(L, -1)))
		{
			lua_pop(L, 1);
		}
		else
		{
			pllua_tolstring(L, -1, NULL);
			lua_rawseti(L, 3, idx);
			lua_pop(L, 1);
		}
		pllua_tolstring(L, -1, NULL);
		lua_rawseti(L, 2, idx);
	}

	lua_settop(L, 3);
	pcount = idx;
	lua_pushinteger(L, pcount);  /* first result */
	pairs = lua_newuserdata(L, (idx ? idx : 1) * sizeof(Pairs));
	lua_newtable(L);
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, "keys");
	lua_pushvalue(L, 3);
	lua_setfield(L, -2, "values");
	lua_setuservalue(L, -2);
	for (idx = 0; idx < pcount; ++idx)
	{
		lua_rawgeti(L, 2, idx+1);
		pairs[idx].key = (char *) lua_tolstring(L, -1, &(pairs[idx].keylen));
		pairs[idx].needfree = false;
		lua_pop(L, 1);

		if (lua_rawgeti(L, 3, idx+1) == LUA_TNIL)
		{
			pairs[idx].val = NULL;
			pairs[idx].vallen = 0;
			pairs[idx].isnull = true;
		}
		else
		{
			pairs[idx].val = (char *) lua_tolstring(L, -1, &(pairs[idx].vallen));
			pairs[idx].isnull = false;
		}
		lua_pop(L, 1);
	}
	return 2;
}


PG_FUNCTION_INFO_V1(hstore_to_pllua);

Datum
hstore_to_pllua(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	pllua_node *node = (pllua_node *) fcinfo->context;
	lua_State  *L;

	if (!node || node->type != T_Invalid || node->magic != PLLUA_MAGIC)
		elog(ERROR, "hstore_to_pllua must only be called from pllua");

	L = node->L;
	pllua_pushcfunction(L, pllua_trampoline);
	lua_pushlightuserdata(L, hstore_to_pllua_real);
	lua_pushlightuserdata(L, in);
	pllua_pcall(L, 2, 1, 0);

	return (Datum)0;
}


PG_FUNCTION_INFO_V1(pllua_to_hstore);

Datum
pllua_to_hstore(PG_FUNCTION_ARGS)
{
	pllua_node *node = (pllua_node *) fcinfo->context;
	lua_State  *L;
	int32		pcount = 0;
	HStore	   *out = NULL;
	Pairs	   *pairs;

	if (!node || node->type != T_Invalid || node->magic != PLLUA_MAGIC)
		elog(ERROR, "pllua_to_hstore must only be called from pllua");

	L = node->L;
	pllua_pushcfunction(L, pllua_trampoline);
	lua_insert(L, 1);
	lua_pushlightuserdata(L, pllua_to_hstore_real);
	lua_insert(L, 2);
	pllua_pcall(L, lua_gettop(L) - 1, 2, 0);

	/*
	 * this ptr is the Pairs struct as a Lua full userdata, which carries
	 * refs to the tables holding the keys and values strings to prevent
	 * them being GC'd. hstorePairs will copy everything into a new palloc'd
	 * value, and the storage will be GC'd sometime later after we pop it.
	 */
	pcount = lua_tointeger(L, -2);
	pairs = lua_touserdata(L, -1);

	if (pairs)
	{
		int i;
		int32 buflen;
		for (i = 0; i < pcount; ++i)
		{
			pairs[i].keylen = hstoreCheckKeyLen(pairs[i].keylen);
			pairs[i].vallen = hstoreCheckKeyLen(pairs[i].vallen);
			pg_verifymbstr(pairs[i].key, pairs[i].keylen, false);
			pg_verifymbstr(pairs[i].val, pairs[i].vallen, false);
		}
		pcount = hstoreUniquePairs(pairs, pcount, &buflen);
		out = hstorePairs(pairs, pcount, buflen);
	}

	lua_pop(L, 2);

	if (out)
		PG_RETURN_POINTER(out);
	else
		PG_RETURN_NULL();
}
