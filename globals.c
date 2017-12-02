
#include "pllua.h"

bool pllua_ending = false;

pllua_context_type pllua_context = PLLUA_CONTEXT_PG;

/*
 * Addresses used as lua registry or table keys
 *
 * Note the key is the address, not the string; the string is only for
 * debugging purposes.
 */

char PLLUA_MEMORYCONTEXT[] = "memory context";
char PLLUA_ERRORCONTEXT[] = "error memory context";
char PLLUA_FUNCS[] = "funcs";
char PLLUA_ACTIVATIONS[] = "activations";
char PLLUA_TYPES[] = "types";
char PLLUA_RECORDS[] = "records";
char PLLUA_TRUSTED[] = "trusted";
char PLLUA_USERID[] = "userid";
char PLLUA_FUNCTION_OBJECT[] = "function object metatable";
char PLLUA_ERROR_OBJECT[] = "error object metatable";
char PLLUA_ACTIVATION_OBJECT[] = "activation object metatable";
char PLLUA_TYPEINFO_OBJECT[] = "typeinfo object metatable";
char PLLUA_TYPEINFO_PACKAGE_OBJECT[] = "typeinfo package object metatable";
char PLLUA_TUPCONV_OBJECT[] = "tupconv object metatable";
char PLLUA_LAST_ERROR[] = "last error object";
char PLLUA_RECURSIVE_ERROR[] = "recursive error object";
char PLLUA_FUNCTION_MEMBER[] = "function element";
char PLLUA_THREAD_MEMBER[] = "thread element";

