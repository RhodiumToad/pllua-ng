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

#define PLLUA_LOCALVAR "_U"


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

/*
 * We don't put this in the body of a lua userdata for error handling reasons;
 * we want to build it from pg data without involving lua too much until we're
 * ready to actually compile the function. Instead, the lua object is a pointer
 * to this with a __gc method, and the object itself is palloc'd. The
 * activation records (corresponding to flinfo) are lua objects that reference
 * the funcinfo, preventing it from being GC'd while in use.
 *
 * The actual lua function object is stored in the uservalue slot, and the
 * funcinfo has a __call method that proxies the call to it, so this object can
 * be treated as the function.
 */

typedef struct pllua_function_info
{
	Oid	fn_oid;
	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	Oid rettype;
	bool retset;
	bool readonly;

	int nargs;
	bool variadic;
	bool variadic_any;
	bool polymorphic;

	Oid *argtypes;

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

	int nargs;
	int nallargs;

	Oid variadic;

	Oid *allargtypes;
	char *argmodes;
	char **argnames;

} pllua_function_compile_info;


/* this one ends up in flinfo->fn_extra */

typedef struct pllua_func_activation
{
	FmgrInfo *flinfo;
	pllua_function_info *func_info;
	lua_State *thread;

	bool resolved;

	bool polymorphic;
	bool variadic_call;
	bool retset;
	bool readonly;

	Oid rettype;
	TupleDesc tupdesc;
	TypeFuncClass typefuncclass;

	int nargs;
	Oid *argtypes;

	/*
	 * this data is allocated in lua, so we need to arrange to drop it for GC
	 * when the context containing the pointer to it is reset
	 */
	lua_State *L;
	MemoryContextCallback cb;
	bool dead;

} pllua_func_activation;


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

typedef struct pllua_datum {
	Datum value;
	int32 typmod;
	bool need_gc;
} pllua_datum;

typedef struct pllua_typeinfo {

	Oid typeoid;
	int32 typmod;  /* only for RECORD */

	/* 1 for scalars, otherwise natts - num_dropped_cols */
	int arity;

	/* -1 for scalars, since composites might have no columns */
	int natts;
	bool hasoid;
	bool revalidate;
	TupleDesc tupdesc;
	Oid reloid;  /* for named composite types */
	Oid basetype;  /* for domains */

	int16 typlen;
	bool typbyval;
	char typalign;
	char typdelim;
	Oid typioparam;
	Oid outfuncid;

	/* we don't look these up until we need them */
	Oid infuncid;
	Oid sendfuncid;
	Oid recvfuncid;

	FmgrInfo outfunc;
	FmgrInfo infunc;
	FmgrInfo sendfunc;
	FmgrInfo recvfunc;

	/* typmod coercions */
	bool coerce_typmod;
	bool coerce_typmod_element;
	Oid typmod_funcid;
	FmgrInfo typmod_func;

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
 * reg[PLLUA_LAST_ERROR] = last pg error object to enter error handling
 * reg[PLLUA_RECURSIVE_ERROR] = preallocated error object
 *
 * registries for cached data:
 * reg[PLLUA_FUNCS] = { [integer oid] = funcinfo object }
 * reg[PLLUA_ACTIVATIONS] = { [light(act)] = activation object }
 * reg[PLLUA_TYPES] = { [integer oid] = typeinfo object }
 * reg[PLLUA_RECORDS] = { [integer typmod] = typeinfo object }
 *
 * metatables:
 * reg[PLLUA_FUNCTION_OBJECT]
 * reg[PLLUA_ACTIVATION_OBJECT]
 * reg[PLLUA_ERROR_OBJECT]
 * reg[PLLUA_TYPEINFO_OBJECT]
 * reg[PLLUA_TYPEINFO_PACKAGE_OBJECT]  (the pgtype() object itself)
 *  - datum objects and tupconv objects have dynamic metatables
 *
 * reg[PLLUA_]
 *
 *
 */

extern char PLLUA_MEMORYCONTEXT[];
extern char PLLUA_ERRORCONTEXT[];
extern char PLLUA_USERID[];
extern char PLLUA_TRUSTED[];
extern char PLLUA_FUNCS[];
extern char PLLUA_TYPES[];
extern char PLLUA_RECORDS[];
extern char PLLUA_ACTIVATIONS[];
extern char PLLUA_FUNCTION_OBJECT[];
extern char PLLUA_ERROR_OBJECT[];
extern char PLLUA_ACTIVATION_OBJECT[];
extern char PLLUA_TYPEINFO_OBJECT[];
extern char PLLUA_TYPEINFO_PACKAGE_OBJECT[];
extern char PLLUA_TUPCONV_OBJECT[];
extern char PLLUA_LAST_ERROR[];
extern char PLLUA_RECURSIVE_ERROR[];
extern char PLLUA_FUNCTION_MEMBER[];
extern char PLLUA_THREAD_MEMBER[];
extern char PLLUA_TYPEINFO_MEMBER[];

/* functions */

/* init.c */

lua_State *pllua_getstate(bool trusted);

/* compile.c */

pllua_func_activation *pllua_validate_and_push(lua_State *L, FunctionCallInfo fcinfo, bool trusted);

/* datum.c */

pllua_datum *pllua_checkanydatum(lua_State *L, int nd, pllua_typeinfo **ti);
void pllua_init_datum_objects(lua_State *L);
int pllua_typeinfo_invalidate(lua_State *L);
struct pllua_datum;
struct pllua_typeinfo;
void pllua_savedatum(lua_State *L,
					 struct pllua_datum *d,
					 struct pllua_typeinfo *t);
int pllua_value_from_datum(lua_State *L,
						   Datum value,
						   Oid typeid);
bool pllua_datum_from_value(lua_State *L, int nd,
							Oid typeid,
							Datum *result,
							bool *isnull);
pllua_datum *pllua_newdatum(lua_State *L);
void pllua_savedatum(lua_State *L,
					 pllua_datum *d,
					 pllua_typeinfo *t);
int pllua_typeinfo_lookup(lua_State *L);

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

void pllua_initial_protected_call(lua_State *L,
								  lua_CFunction func,
								  void *arg);

void pllua_init_error(lua_State *L);

/* exec.c */

int pllua_resume_function(lua_State *L);
int pllua_call_function(lua_State *L);
int pllua_call_trigger(lua_State *L);
int pllua_call_event_trigger(lua_State *L);
int pllua_call_inline(lua_State *L);
int pllua_validate(lua_State *L);

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

lua_State *pllua_activate_thread(lua_State *L, int nd, ExprContext *econtext);
void pllua_deactivate_thread(lua_State *L, pllua_func_activation *act, ExprContext *econtext);

void pllua_init_objects(lua_State *L, bool trusted);
void pllua_init_functions(lua_State *L, bool trusted);

#endif
