/*
 * pllua.h
 */

#ifndef PLLUA_H
#define PLLUA_H

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"

#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "miscadmin.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* PG version cruft */

#if PG_VERSION_NUM < 100000
#define TupleDescAttr(tupdesc, i) ((tupdesc)->attrs[(i)])
#endif

/* Lua cruft */

#if LUA_VERSION_NUM == 501
/* assume Lua 5.1 is actually luajit, and get the luajit version. */
#include <luajit.h>
#endif

#ifndef LUAJIT_VERSION_NUM
#define LUAJIT_VERSION_NUM 0
#endif

#define PLLUA_LOCALVAR "_U"

#define pllua_pushcfunction(L_,f_) \
	do { int rc = lua_rawgetp((L_),LUA_REGISTRYINDEX,(f_)); Assert(rc==LUA_TFUNCTION); } while(0)

#if LUA_VERSION_NUM == 501
/*
 * Parts of the lua 5.1 compatibility cruft here is derived from the
 * lua-compat-5.3 project, which is licensed under the same terms as this
 * project and carries the following copyright:
 *
 * Copyright (c) 2015 Kepler Project.
 */
static inline int lua_absindex(lua_State *L, int nd)
{
	return (nd < 0 && nd > LUA_REGISTRYINDEX) ? nd + lua_gettop(L) + 1 : nd;
}
static inline int lua_rawgetp(lua_State *L, int nd, void *p)
{
	int tnd = lua_absindex(L, nd);
	lua_pushlightuserdata(L, p);
	lua_rawget(L, tnd);
	return lua_type(L, -1);
}
static inline void lua_rawsetp(lua_State *L, int nd, void *p)
{
	int tnd = lua_absindex(L, nd);
	lua_pushlightuserdata(L, p);
	lua_insert(L, -2);
	lua_rawset(L, tnd);
}
static inline int lua_geti(lua_State *L, int nd, lua_Integer i)
{
	int tnd = lua_absindex(L, nd);
	lua_pushinteger(L, i);
	lua_gettable(L, tnd);
	return lua_type(L, -1);
}
static inline void lua_seti(lua_State *L, int nd, lua_Integer i)
{
	int tnd = lua_absindex(L, nd);
	lua_pushinteger(L, i);
	lua_insert(L, -2);
	lua_settable(L, tnd);
}
#define lua_rawgeti(L_,nd_,i_) ((lua_rawgeti)(L_,nd_,i_),lua_type(L_, -1))
#define lua_rawget(L_,nd_) ((lua_rawget)(L_,nd_),lua_type(L_, -1))
#define lua_getfield(L_,nd_,i_) ((lua_getfield)(L_,nd_,i_),lua_type(L_, -1))
#define lua_gettable(L_,nd_) ((lua_gettable)(L_,nd_),lua_type(L_, -1))
#define luaL_getmetafield(L_,nd_,f_) ((luaL_getmetafield)(L_,nd_,f_) ? lua_type(L_, -1) : LUA_TNIL)
#define lua_resume(L_,from_,nargs_) ((lua_resume)(L_,nargs_))
/* luajit 2.1's version of this one is ok. */
#if LUAJIT_VERSION_NUM < 20100
static inline lua_Number lua_tonumberx(lua_State *L, int i, int *isnum)
{
	lua_Number n = lua_tonumber(L, i);
	if (isnum != NULL) {
		*isnum = (n != 0 || lua_isnumber(L, i));
	}
	return n;
}
#endif
/* but for these we need to kill luajit's version and use ours: */
#if LUAJIT_VERSION_NUM >= 20100
#define lua_isinteger pllua_isinteger
#define lua_tointegerx pllua_tointegerx
#endif
static inline bool lua_isinteger(lua_State *L, int nd)
{
	if (lua_type(L, nd) == LUA_TNUMBER)
	{
		lua_Number n = lua_tonumber(L, nd);
		lua_Integer i = lua_tointeger(L, nd);
		if (i == n)
			return 1;
	}
	return 0;
}
static inline lua_Integer lua_tointegerx(lua_State *L, int i, int *isnum)
{
	lua_Integer n = lua_tointeger(L, i);
	if (isnum != NULL) {
		*isnum = (n == lua_tonumber(L, i));
	}
	return n;
}
#define lua_getuservalue(L_,nd_) lua_getfenv(L_,nd_)
#define lua_setuservalue(L_,nd_) lua_setfenv(L_,nd_)
#define lua_pushglobaltable(L_) lua_pushvalue((L_), LUA_GLOBALSINDEX)
#if LUAJIT_VERSION_NUM < 20100
#define luaL_setfuncs(L_,f_,u_) pllua_setfuncs(L_,f_,u_)
void pllua_setfuncs(lua_State *L, const luaL_Reg *reg, int nup);
#endif
#define luaL_getsubtable(L_,i_,n_) pllua_getsubtable(L_,i_,n_)
int pllua_getsubtable(lua_State *L, int i, const char *name);
#define luaL_requiref(L_,m_,f_,g_) pllua_requiref(L_,m_,f_,g_)
void pllua_requiref(lua_State *L, const char *modname, lua_CFunction openf, int glb);
#ifndef luaL_newlibtable
#define luaL_newlibtable(L, l)							\
  (lua_createtable((L), 0, sizeof((l))/sizeof(*(l))-1))
#define luaL_newlib(L, l) \
  (luaL_newlibtable((L), (l)), luaL_register((L), NULL, (l)))
#endif
#define LUA_OK 0
/* these are probably wrong but will do for now */
#define LUA_MAXINTEGER (INT64CONST(900719925474099))
#define LUA_MININTEGER (-INT64CONST(900719925474099))
#define pllua_set_environment(L_,i_) lua_setfenv(L, i_)
#else
#define pllua_set_environment(L_,i_) lua_setupvalue(L_, i_, 1)
#endif

#if LUA_VERSION_NUM == 503
#if LUA_MAXINTEGER == PG_INT64_MAX
#define PLLUA_INT8_OK
#endif
#endif

#if LUA_VERSION_NUM == 501
#define MANDATORY_USERVALUE 1
#else
#define MANDATORY_USERVALUE 0
#endif

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

extern pllua_context_type pllua_context;

#define ASSERT_PG_CONTEXT Assert(pllua_context == PLLUA_CONTEXT_PG)
#define ASSERT_LUA_CONTEXT Assert(pllua_context == PLLUA_CONTEXT_LUA)

static inline pllua_context_type
pllua_setcontext(pllua_context_type newctx)
{
	pllua_context_type oldctx = pllua_context;
	pllua_context = newctx;
	return oldctx;
}

/*
 * Abbreviate the most common form of catch block.
 */
#define PLLUA_TRY() do {												\
	pllua_context_type _pllua_oldctx = pllua_setcontext(PLLUA_CONTEXT_PG); \
	MemoryContext _pllua_oldmcxt = CurrentMemoryContext;				\
	PG_TRY()

#define PLLUA_CATCH_RETHROW()											\
	PG_CATCH();															\
	{ pllua_setcontext(_pllua_oldctx); pllua_rethrow_from_pg(L, _pllua_oldmcxt); } \
	PG_END_TRY(); pllua_setcontext(_pllua_oldctx); } while(0)

#define pllua_debug(L_, ...)											\
	do { if (pllua_context==PLLUA_CONTEXT_PG) elog(DEBUG1, __VA_ARGS__); \
		else pllua_debug_lua(L_, __VA_ARGS__); } while(0)

#define PLLUA_CHECK_PG_STACK_DEPTH()							\
	do { if (stack_is_too_deep()) luaL_error(L, "stack depth exceeded"); } while (0)


/*
 * Describes one call to the top-level handler.
 */
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
 * Top-level data for one interpreter. We keep a hashtable of these per user_id
 * (for trusted mode isolation). We keep a pointer to this in the Lua registry
 * and use it to access the current activation fields (which are saved/restored
 * on recursive entries).
 */
typedef struct pllua_interpreter
{
	Oid			user_id;		/* Hash key (must be first!) */

	bool		trusted;
	lua_State  *L;				/* The interpreter proper */

	/* state below must be saved/restored for recursive calls */
	pllua_activation_record cur_activation;
} pllua_interpreter;

/* We abuse the node system to pass this in fcinfo->context */

#define PLLUA_MAGIC 0x4c554101

typedef struct pllua_node
{
	NodeTag		type;   /* we put T_Invalid here */
	uint32		magic;
	lua_State  *L;
} pllua_node;

/*
 * We don't put this in the body of a lua userdata for error handling reasons;
 * we want to build it from pg data without involving lua too much until we're
 * ready to actually compile the function. Instead, the lua object is a pointer
 * to this with a __gc method, and the object itself is palloc'd (in its own
 * memory context). The activation records (corresponding to flinfo) are lua
 * objects that reference the funcinfo, preventing it from being GC'd while in
 * use.
 *
 * The actual lua function object is stored in the uservalue slot under key
 * light(PLLUA_FUNCTION_MEMBER).
 */
typedef struct pllua_function_info
{
	Oid			fn_oid;
	/* for revalidation checks */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	Oid			rettype;
	bool		returns_row;
	bool		retset;
	bool		readonly;
	bool		is_trigger;

	int			nargs;
	bool		variadic;
	bool		variadic_any;
	bool		polymorphic;
	bool		polymorphic_ret;

	Oid		   *argtypes;

	Oid			language_oid;
	bool		trusted;

	MemoryContext mcxt;

	const char *name;
} pllua_function_info;

/*
 * This is info we need to compile the function but not needed to run it.
 */
typedef struct pllua_function_compile_info
{
	pllua_function_info *func_info;

	MemoryContext mcxt;

	text	   *prosrc;

	int			nargs;
	int			nallargs;

	Oid			variadic;

	Oid		   *allargtypes;
	char	   *argmodes;
	char	  **argnames;

	bool		validate_only;		/* don't run any code when compiling */
} pllua_function_compile_info;


/* this one ends up in flinfo->fn_extra */

typedef struct pllua_func_activation
{
	lua_State  *thread;		/* non-null for a running SRF */
	pllua_interpreter *interp;		/* direct access for SRF resume */

	pllua_function_info *func_info;

	bool		resolved;

	bool		polymorphic;
	bool		variadic_call;		/* only if variadic_any */
	bool		retset;
	bool		readonly;

	Oid			rettype;
	TupleDesc	tupdesc;
	TypeFuncClass typefuncclass;

	int			nargs;
	Oid		   *argtypes;	/* with polymorphism resolved */

	/*
	 * this data is allocated and referenced in lua, so we need to arrange to
	 * drop it for GC when the context containing the pointer to it is reset
	 */
	lua_State  *L;
	bool		dead;
	MemoryContextCallback cb;
} pllua_func_activation;

/*
 * Body of a Datum object. typmod is usually -1 except when we got the value
 * from a source with a declared typmod (such as a column).
 */
typedef struct pllua_datum
{
	Datum		value;
	int32		typmod;
	bool		need_gc;
	bool		modified;		/* composite value has been exploded */
} pllua_datum;

/*
 * Stuff we store about types. Datum values reference this from their
 * metatables (in fact the metatable of the Datum is the uservalue of
 * this object, which also contains a reference to the object itself).
 */
typedef struct pllua_typeinfo
{
	Oid			typeoid;
	int32 		typmod;			/* only for RECORD */

	int			arity;	/* 1 for scalars, otherwise no. undropped cols */
	int			natts;	/* -1 for scalars */

	TupleDesc	tupdesc;
	Oid			reloid;		/* for named composite types */
	Oid			basetype;	/* for domains */
	Oid			elemtype;	/* for arrays */
	Oid			rangetype;	/* for ranges */
	bool		hasoid;
	bool		nested;		/* may contain nested explodable values */
	bool		is_array;
	bool		is_range;
	bool		is_enum;

	bool		revalidate;

	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			outfuncid;

	Oid			infuncid;	/* we don't look these up until we need them */
	Oid			sendfuncid;
	Oid			recvfuncid;

	FmgrInfo	outfunc;
	FmgrInfo	infunc;
	FmgrInfo	sendfunc;
	FmgrInfo	recvfunc;

	bool		coerce_typmod;		/* typmod coercions needed */
	bool		coerce_typmod_element;
	Oid			typmod_funcid;
	FmgrInfo	typmod_func;

	ArrayMetaState array_meta;		/* array workspace */

	int16		elemtyplen;			/* for arrays only */
	bool		elemtypbyval;
	char		elemtypalign;

	Oid			fromsql;		/* fromsql(internal) returns internal */
	Oid			tosql;			/* tosql(internal) returns datum */
	FmgrInfo	fromsql_func;
	FmgrInfo	tosql_func;

	/*
	 * we give this its own context, because we can't control what fmgr will
	 * dangle off the FmgrInfo structs
     */
	MemoryContext mcxt;
} pllua_typeinfo;

/*
 * are we shutting down?
 */

extern bool pllua_ending;

/*
 * Addresses used as lua registry or object keys
 *
 * Note the key is the address, not the string; the string is only for
 * debugging purposes.
 *
 * The registry looks like this (all keys are light userdata):
 *
 * global state:
 * reg[PLLUA_MEMORYCONTEXT] = light(MemoryContext) - for refobj data
 * reg[PLLUA_ERRORCONTEXT] = light(MemoryContext) - for error handling
 * reg[PLLUA_USERID] = int user_id for trusted, InvalidOid for untrusted
 * reg[PLLUA_TRUSTED] = boolean
 * reg[PLLUA_LANG_OID] = oid of language
 * reg[PLLUA_LAST_ERROR] = last pg error object to enter error handling
 * reg[PLLUA_RECURSIVE_ERROR] = preallocated error object
 * reg[PLLUA_INTERP] = pllua_interpreter struct
 *
 * registries for cached data:
 * reg[PLLUA_FUNCS] = { [integer oid] = funcinfo object }
 * reg[PLLUA_ACTIVATIONS] = { [light(act)] = activation object }
 * reg[PLLUA_TYPES] = { [integer oid] = typeinfo object }
 * reg[PLLUA_RECORDS] = { [integer typmod] = typeinfo object }
 * reg[PLLUA_PORTALS] = { [light(Portal)] = cursor object }
 *
 * metatables:
 * reg[PLLUA_FUNCTION_OBJECT]
 * reg[PLLUA_ACTIVATION_OBJECT]
 * reg[PLLUA_ERROR_OBJECT]
 * reg[PLLUA_TYPEINFO_OBJECT]
 * reg[PLLUA_TYPEINFO_PACKAGE_OBJECT]  (the pgtype() object itself)
 *  - datum objects and tupconv objects have dynamic metatables
 *
 * sandbox:
 * reg[PLLUA_TRUSTED_SANDBOX] = value of _ENV for trusted funcs
 * reg[PLLUA_TRUSTED_SANDBOX_LOADED] = modules loaded in sandbox
 * reg[PLLUA_TRUSTED_SANDBOX_ALLOW] = modules allowed in sandbox
 *
 * reg[PLLUA_]
 *
 *
 */

extern char PLLUA_MEMORYCONTEXT[];
extern char PLLUA_ERRORCONTEXT[];
extern char PLLUA_INTERP[];
extern char PLLUA_USERID[];
extern char PLLUA_LANG_OID[];
extern char PLLUA_TRUSTED[];
extern char PLLUA_FUNCS[];
extern char PLLUA_TYPES[];
extern char PLLUA_RECORDS[];
extern char PLLUA_ACTIVATIONS[];
extern char PLLUA_PORTALS[];
extern char PLLUA_FUNCTION_OBJECT[];
extern char PLLUA_ERROR_OBJECT[];
extern char PLLUA_IDXLIST_OBJECT[];
extern char PLLUA_ACTIVATION_OBJECT[];
extern char PLLUA_TYPEINFO_OBJECT[];
extern char PLLUA_TYPEINFO_PACKAGE_OBJECT[];
extern char PLLUA_TYPEINFO_PACKAGE_ARRAY_OBJECT[];
extern char PLLUA_TUPCONV_OBJECT[];
extern char PLLUA_TRIGGER_OBJECT[];
extern char PLLUA_SPI_STMT_OBJECT[];
extern char PLLUA_SPI_CURSOR_OBJECT[];
extern char PLLUA_LAST_ERROR[];
extern char PLLUA_RECURSIVE_ERROR[];
extern char PLLUA_FUNCTION_MEMBER[];
extern char PLLUA_THREAD_MEMBER[];
extern char PLLUA_TYPEINFO_MEMBER[];
extern char PLLUA_TRUSTED_SANDBOX[];
extern char PLLUA_TRUSTED_SANDBOX_LOADED[];
extern char PLLUA_TRUSTED_SANDBOX_ALLOW[];

/* functions */

/* init.c */

pllua_interpreter *pllua_getstate(bool trusted, pllua_activation_record *act);
pllua_interpreter *pllua_getinterpreter(lua_State *L);
int pllua_run_init_strings(lua_State *L);

/* compile.c */

pllua_func_activation *pllua_validate_and_push(lua_State *L, FunctionCallInfo fcinfo, bool trusted);
int pllua_compile(lua_State *L);
int pllua_intern_function(lua_State *L);
void pllua_validate_function(lua_State *L, Oid fn_oid, bool trusted);

/* datum.c */

void pllua_verify_encoding(lua_State *L, const char *str);
bool pllua_verify_encoding_noerror(lua_State *L, const char *str);
void *pllua_palloc(lua_State *L, size_t sz);
pllua_datum *pllua_checkanydatum(lua_State *L, int nd, pllua_typeinfo **ti);
pllua_datum *pllua_checkdatum(lua_State *L, int nd, int td);
int pllua_open_pgtype(lua_State *L);
int pllua_typeinfo_invalidate(lua_State *L);
struct pllua_datum;
struct pllua_typeinfo;
void pllua_savedatum(lua_State *L,
					 struct pllua_datum *d,
					 struct pllua_typeinfo *t);
int pllua_value_from_datum(lua_State *L,
						   Datum value,
						   Oid typeid);
int pllua_datum_transform_fromsql(lua_State *L,
								  Datum value,
								  int nt,
								  pllua_typeinfo *t);
bool pllua_datum_from_value(lua_State *L, int nd,
							Oid typeid,
							Datum *result,
							bool *isnull,
							const char **errstr);
pllua_datum *pllua_newdatum(lua_State *L);
void pllua_savedatum(lua_State *L,
					 pllua_datum *d,
					 pllua_typeinfo *t);
int pllua_typeinfo_lookup(lua_State *L);
pllua_typeinfo *pllua_newtypeinfo_raw(lua_State *L, Oid oid, int32 typmod, TupleDesc tupdesc);
pllua_datum *pllua_toanydatum(lua_State *L, int nd, pllua_typeinfo **ti);
pllua_datum *pllua_todatum(lua_State *L, int nd, int td);
int pllua_typeinfo_parsetype(lua_State *L);

/* elog.c */

int pllua_p_print (lua_State *L);
void pllua_init_error_functions(lua_State *L);
void pllua_debug_lua(lua_State *L, const char *msg, ...);

/* error.c */
int pllua_panic(lua_State *L);
void pllua_poperror(lua_State *L);
int pllua_newerror(lua_State *L);
int pllua_pcall_nothrow(lua_State *L, int nargs, int nresults, int msgh);
void pllua_rethrow_from_lua(lua_State *L, int rc);
void pllua_rethrow_from_pg(lua_State *L, MemoryContext mcxt);
int pllua_cpcall(lua_State *L, lua_CFunction func, void* arg);
void pllua_pcall(lua_State *L, int nargs, int nresults, int msgh);

void pllua_initial_protected_call(pllua_interpreter *interp,
								  lua_CFunction func,
								  pllua_activation_record *arg);

void pllua_init_error(lua_State *L);

int pllua_t_pcall(lua_State *L);
int pllua_t_xpcall(lua_State *L);

/* exec.c */

int pllua_resume_function(lua_State *L);
int pllua_call_function(lua_State *L);
int pllua_call_trigger(lua_State *L);
int pllua_call_event_trigger(lua_State *L);
int pllua_call_inline(lua_State *L);
int pllua_validate(lua_State *L);

/* numeric.c */
int pllua_open_numeric(lua_State *L);

/* objects.c */

bool pllua_isobject(lua_State *L, int nd, char *objtype);
void pllua_newmetatable(lua_State *L, char *objtype, luaL_Reg *mt);
MemoryContext pllua_get_memory_cxt(lua_State *L);
void **pllua_newrefobject(lua_State *L, char *objtype, void *value, bool uservalue);
void **pllua_torefobject(lua_State *L, int nd, char *objtype);
void *pllua_newobject(lua_State *L, char *objtype, size_t sz, bool uservalue);
void *pllua_toobject(lua_State *L, int nd, char *objtype);
void pllua_type_error(lua_State *L, char *expected);
void **pllua_checkrefobject(lua_State *L, int nd, char *objtype);
void *pllua_checkobject(lua_State *L, int nd, char *objtype);

int pllua_newactivation(lua_State *L);
int pllua_setactivation(lua_State *L);
void pllua_getactivation(lua_State *L, pllua_func_activation *act);
int pllua_activation_getfunc(lua_State *L);
int pllua_get_cur_act(lua_State *L);
FmgrInfo *pllua_get_cur_flinfo(lua_State *L);
bool pllua_get_cur_act_readonly(lua_State *L);
int pllua_freeactivation(lua_State *L);
int pllua_resetactivation(lua_State *L);

lua_State *pllua_activate_thread(lua_State *L, int nd, ExprContext *econtext);
void pllua_deactivate_thread(lua_State *L, pllua_func_activation *act, ExprContext *econtext);

void pllua_init_objects(lua_State *L, bool trusted);
void pllua_init_functions(lua_State *L, bool trusted);

/* spi.c */
int pllua_open_spi(lua_State *L);
int pllua_spi_convert_args(lua_State *L);
int pllua_spi_prepare_result(lua_State *L);
int pllua_cursor_cleanup_portal(lua_State *L);

/* trigger.c */
struct TriggerData;
void pllua_trigger_begin(lua_State *L, struct TriggerData *td);
void pllua_trigger_end(lua_State *L, int nd);
int pllua_push_trigger_args(lua_State *L, struct TriggerData *td);
Datum pllua_return_trigger_result(lua_State *L, int nret, int nd);
int pllua_open_trigger(lua_State *L);

/* trusted.c */
int pllua_open_trusted(lua_State *L);

#endif
