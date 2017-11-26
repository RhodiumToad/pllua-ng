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

static inline pllua_context_type
pllua_setcontext(pllua_context_type newctx)
{
	pllua_context_type oldctx = pllua_context;
	pllua_context = newctx;
	return oldctx;
}

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

	bool retset;
	
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
	
} pllua_function_compile_info;
   

/* this one ends up in flinfo->fn_extra */

typedef struct pllua_func_activation
{
	pllua_function_info *func_info;
	lua_State *thread;

	/*
	 * this data is allocated in lua, so we need to arrange to drop it for GC
	 * when the context containing the pointer to it is reset
	 */
	lua_State *L;
	MemoryContextCallback cb;
	
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

/*
 * are we shutting down?
 */

extern bool pllua_ending;

/*
 * Addresses used as lua registry keys
 *
 * Note the key is the address, not the string; the string is only for
 * debugging purposes.
 */

extern char PLLUA_MEMORYCONTEXT[];
extern char PLLUA_ERRORCONTEXT[];
extern char PLLUA_FUNCS[];
extern char PLLUA_ACTIVATIONS[];
extern char PLLUA_FUNCTION_OBJECT[];
extern char PLLUA_ERROR_OBJECT[];
extern char PLLUA_ACTIVATION_OBJECT[];
extern char PLLUA_LAST_ERROR[];
extern char PLLUA_RECURSIVE_ERROR[];
extern char PLLUA_FUNCTION_MEMBER[];
extern char PLLUA_THREAD_MEMBER[];

/* functions */

/* init.c */

lua_State *pllua_getstate(bool trusted);

/* compile.c */

void pllua_validate_and_push(lua_State *L, FmgrInfo *flinfo, bool trusted);

/* elog.c */

int pllua_p_print (lua_State *L);

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

int pllua_call_function(lua_State *L);
int pllua_call_trigger(lua_State *L);
int pllua_call_event_trigger(lua_State *L);
int pllua_call_inline(lua_State *L);
int pllua_validate(lua_State *L);

/* objects.c */

bool pllua_isobject(lua_State *L, int nd, char *objtype);	
void pllua_newmetatable(lua_State *L, char *objtype, luaL_Reg *mt);
MemoryContext pllua_get_memory_cxt(lua_State *L);
void **pllua_newrefobject(lua_State *L, char *objtype, void *value);
void **pllua_torefobject(lua_State *L, int nd, char *objtype);
void *pllua_newobject(lua_State *L, char *objtype, size_t sz, bool uservalue);
void *pllua_toobject(lua_State *L, int nd, char *objtype);
void pllua_type_error(lua_State *L, char *expected);
void **pllua_checkrefobject(lua_State *L, int nd, char *objtype);
void *pllua_checkobject(lua_State *L, int nd, char *objtype);

int pllua_newactivation(lua_State *L);
int pllua_setactivation(lua_State *L);
void pllua_getactivation(lua_State *L, pllua_func_activation *act);

void pllua_init_objects(lua_State *L, bool trusted);
void pllua_init_functions(lua_State *L, bool trusted);

#endif
