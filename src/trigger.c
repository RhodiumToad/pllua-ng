/* trigger.c */

#include "pllua.h"

#include "access/htup_details.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "utils/reltrigger.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"

typedef struct pllua_trigger
{
	TriggerData *td;		/* NULLed out when trigger ends */
	bool		modified;
} pllua_trigger;

typedef struct pllua_event_trigger
{
	EventTriggerData *etd;		/* NULLed out when trigger ends */
} pllua_event_trigger;

/*
 * Push a new trigger object on the stack
 */
void
pllua_trigger_begin(lua_State *L, TriggerData *td)
{
	pllua_trigger *obj = pllua_newobject(L, PLLUA_TRIGGER_OBJECT, sizeof(pllua_trigger), 1);
	obj->td = td;
}

void
pllua_trigger_end(lua_State *L, int nd)
{
	pllua_trigger *obj = pllua_checkobject(L, nd, PLLUA_TRIGGER_OBJECT);
	obj->td = NULL;
}

static pllua_trigger *
pllua_checktrigger(lua_State *L, int nd)
{
	pllua_trigger *obj = pllua_checkobject(L, nd, PLLUA_TRIGGER_OBJECT);
	if (!obj->td)
		luaL_error(L, "cannot access dead trigger object");
	return obj;
}

/*
 * Push a new event trigger object on the stack
 */
void
pllua_evtrigger_begin(lua_State *L, EventTriggerData *etd)
{
	pllua_event_trigger *obj = pllua_newobject(L, PLLUA_EVENT_TRIGGER_OBJECT, sizeof(pllua_event_trigger), 1);
	obj->etd = etd;
}

void
pllua_evtrigger_end(lua_State *L, int nd)
{
	pllua_event_trigger *obj = pllua_checkobject(L, nd, PLLUA_EVENT_TRIGGER_OBJECT);
	obj->etd = NULL;
}

static pllua_event_trigger *
pllua_checkevtrigger(lua_State *L, int nd)
{
	pllua_event_trigger *obj = pllua_checkobject(L, nd, PLLUA_EVENT_TRIGGER_OBJECT);
	if (!obj->etd)
		luaL_error(L, "cannot access dead event trigger object");
	return obj;
}

/*
 * We support the following:
 *
 *  trigger.new   - always the new row (or nil)
 *  trigger.old   - always the old row (or nil)
 *  trigger.row   - new for insert/update, old for delete
 *  trigger.name
 *  trigger.when
 *  trigger.operation
 *  trigger.level
 *  trigger.relation
 *
 * Assigning nil or a new row to trigger.row modifies the result of the
 * trigger, though this is for compatibility and returning a new row or nil
 * from the function overrides this.
 *
 */

static void
pllua_trigger_get_typeinfo(lua_State *L, pllua_trigger *obj, int cache)
{
	cache = lua_absindex(L, cache);
	if (lua_getfield(L, cache, ".typeinfo") != LUA_TUSERDATA)
	{
		lua_pushcfunction(L, pllua_typeinfo_lookup);
		lua_pushinteger(L, (lua_Integer) obj->td->tg_relation->rd_att->tdtypeid);
		lua_pushinteger(L, (lua_Integer) obj->td->tg_relation->rd_att->tdtypmod);
		lua_call(L, 2, 1);
		if (lua_isnil(L, -1))
			luaL_error(L, "trigger failed to find relation typeinfo");
		lua_pushvalue(L, -1);
		lua_setfield(L, cache, ".typeinfo");
	}
}

static int
pllua_trigger_getrow(lua_State *L, pllua_trigger *obj, HeapTuple tuple)
{
	pllua_datum *d = pllua_newdatum(L, -1, (Datum)0);

	/*
	 * Bit of a dance to avoid an extra copy step
	 */
	PLLUA_TRY();
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(pllua_get_memory_cxt(L));
		Datum htup = heap_copy_tuple_as_datum(tuple, obj->td->tg_relation->rd_att);
		d->value = htup;
		d->need_gc = 1;
		MemoryContextSwitchTo(oldcontext);
	}
	PLLUA_CATCH_RETHROW();

	return 1;
}

static int
pllua_trigger_get_new(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	HeapTuple	tuple = NULL;
	lua_settop(L, 1);
	lua_getuservalue(L, 1); /* index 2 */
	if (!TRIGGER_FIRED_FOR_ROW(obj->td->tg_event))
		return 0;
	if (TRIGGER_FIRED_BY_INSERT(obj->td->tg_event))
		tuple = obj->td->tg_trigtuple;
	else if (TRIGGER_FIRED_BY_UPDATE(obj->td->tg_event))
		tuple = obj->td->tg_newtuple;
	if (!tuple)
		return 0;
	pllua_trigger_get_typeinfo(L, obj, 2);
	return pllua_trigger_getrow(L, obj, tuple);
}

static int
pllua_trigger_get_old(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	lua_settop(L, 1);
	lua_getuservalue(L, 1); /* index 2 */
	if (!TRIGGER_FIRED_FOR_ROW(obj->td->tg_event)
		|| TRIGGER_FIRED_BY_INSERT(obj->td->tg_event))
		return 0;
	pllua_trigger_get_typeinfo(L, obj, 2);
	return pllua_trigger_getrow(L, obj, obj->td->tg_trigtuple);
}

static int
pllua_trigger_get_name(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	lua_pushstring(L, obj->td->tg_trigger->tgname);
	return 1;
}

static int
pllua_trigger_get_when(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	TriggerEvent ev = obj->td->tg_event;
	if (TRIGGER_FIRED_BEFORE(ev))
		lua_pushstring(L, "before");
	else if (TRIGGER_FIRED_AFTER(ev))
		lua_pushstring(L, "after");
	else if (TRIGGER_FIRED_INSTEAD(ev))
		lua_pushstring(L, "instead");
	else
		lua_pushnil(L);
	return 1;
}

static int
pllua_trigger_get_operation(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	TriggerEvent ev = obj->td->tg_event;
	if (TRIGGER_FIRED_BY_INSERT(ev))
		lua_pushstring(L, "insert");
	else if (TRIGGER_FIRED_BY_UPDATE(ev))
		lua_pushstring(L, "update");
	else if (TRIGGER_FIRED_BY_DELETE(ev))
		lua_pushstring(L, "delete");
	else if (TRIGGER_FIRED_BY_TRUNCATE(ev))
		lua_pushstring(L, "truncate");
	else
		lua_pushnil(L);
	return 1;
}

static int
pllua_trigger_get_level(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	TriggerEvent ev = obj->td->tg_event;
	if (TRIGGER_FIRED_FOR_ROW(ev))
		lua_pushstring(L, "row");
	else if (TRIGGER_FIRED_FOR_STATEMENT(ev))
		lua_pushstring(L, "statement");
	else
		lua_pushnil(L);
	return 1;
}

/*
 * Structure copied from old pllua:
 *
 *   ["relation"] = {
 *      ["namespace"] = "public",
 *      ["attributes"] = {
 *         ["test_column"] = 0,
 *      },
 *      ["name"] = "table_name",
 *      ["oid"] = 59059
 *   }
 *
 */
static int
pllua_trigger_get_relation(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	Relation	rel = obj->td->tg_relation;
	const char *schema = NULL;
	int			i;
	TupleDesc	tupdesc = rel->rd_att;
	int			natts = tupdesc->natts;

	PLLUA_TRY();
	{
		schema = get_namespace_name(rel->rd_rel->relnamespace);
	}
	PLLUA_CATCH_RETHROW();

	lua_createtable(L, 0, 4);
	lua_pushstring(L, schema ? schema : "");
	lua_setfield(L, -2, "namespace");
	lua_pushstring(L, NameStr(rel->rd_rel->relname));
	lua_setfield(L, -2, "name");
	lua_pushinteger(L, (lua_Integer) rel->rd_id);
	lua_setfield(L, -2, "oid");
	lua_createtable(L, 0, natts);
	for (i = 0; i < natts; ++i)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
			continue;
		lua_pushinteger(L, i);
		lua_setfield(L, -2, NameStr(TupleDescAttr(tupdesc, i)->attname));
	}
	lua_setfield(L, -2, "attributes");
	return 1;
}

static int
pllua_trigger_index(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	const char *str = luaL_checkstring(L, 2);
	lua_settop(L, 2);
	lua_getuservalue(L, 1); /* index 3 */
	if (!*str || *str == '_' || *str == '.')
	{
		lua_pushnil(L);
		return 1;
	}
	/* treat "row" as an alias for old or new, depending */
	if (strcmp(str, "row") == 0)
	{
		if (TRIGGER_FIRED_BY_DELETE(obj->td->tg_event))
			str = "old";
		else
			str = "new";
		lua_pushstring(L, str);
		lua_replace(L, 2);
	}
	else if (strcmp(str, "op") == 0)
	{
		/* some people like shorter names */
		str = "operation";
		lua_pushstring(L, str);
		lua_replace(L, 2);
	}
	/* previously cached result? */
	lua_pushvalue(L, 2);
	switch (lua_rawget(L, -2))
	{
		case LUA_TBOOLEAN:
			if (!lua_toboolean(L, -1))
				lua_pushnil(L);
			return 1;
		case LUA_TNIL:
			break;
		default:
			return 1;
	}
	lua_pop(L, 1);

	if (luaL_getmetafield(L, 1, "_keys") != LUA_TTABLE)
		luaL_error(L, "missing trigger keys");
	if (lua_getfield(L, -1, str) == LUA_TFUNCTION)
	{
		lua_pushvalue(L, 1);
		lua_call(L, 1, 1);
		if (!lua_isnil(L, -1))
		{
			lua_pushvalue(L, -1);
			lua_setfield(L, 3, str);
		}
	}
	else
		lua_pushnil(L);
	return 1;
}

static int
pllua_trigger_newindex(lua_State *L)
{
	pllua_trigger *obj = pllua_checktrigger(L, 1);
	const char *str = luaL_checkstring(L, 2);
	luaL_checkany(L, 3);
	lua_settop(L, 3);
	lua_getuservalue(L, 1); /* index 4 */

	if (strcmp(str, "row") != 0)
		luaL_error(L, "cannot modify field trigger.%s", str);

	if (!TRIGGER_FIRED_FOR_ROW(obj->td->tg_event))
		luaL_error(L, "trigger row can only be modified in a per-row trigger");

	/*
	 * what we're assigning must be nil, or convertible to the correct type, or
	 * a table. Simplest approach is just feed it to the type constructor if
	 * it's not nil. If it _is_ nil, we have to store some random other value
	 * (we choose "false") because nil values aren't allowed.
	 */
	if (!lua_isnil(L, 3))
	{
		pllua_trigger_get_typeinfo(L, obj, 4);
		lua_pushvalue(L, 3);
		lua_call(L, 1, 1);
	}
	else
		lua_pushboolean(L, false);

	/*
	 * at this point, stack top should be a value of a suitable type
	 *
	 * "row" corresponds to "new" for insert/update triggers or "old" for delete,
	 * we don't put "row" in the cache so update only the real name
	 */
	if (TRIGGER_FIRED_BY_DELETE(obj->td->tg_event))
		lua_setfield(L, 4, "old");
	else
		lua_setfield(L, 4, "new");
	obj->modified = true;
	return 0;
}

static Datum
pllua_trigger_copytuple(lua_State *L, Datum val, Oid tableoid)
{
	volatile Datum res;
	PLLUA_TRY();
	{
		HeapTupleHeader htup = (HeapTupleHeader) DatumGetPointer(val);
		HeapTupleData tuple;
		tuple.t_len = HeapTupleHeaderGetDatumLength(htup);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = tableoid;
		tuple.t_data = htup;
		res = PointerGetDatum(heap_copytuple(&tuple));
	}
	PLLUA_CATCH_RETHROW();
	return res;
}

/*
 * "nret" return values are on the stack ending at the current stack top.
 *
 * "nd" indexes a trigger object.
 *
 * We must return as a pointer datum a HeapTuple (NOT a HeapTupleHeader)
 * which is the result of heap_copytuple in the caller's memory context.
 * Or we can return PointerGetDatum(NULL) to suppress the operation.
 */
Datum
pllua_return_trigger_result(lua_State *L, int nret, int nd)
{
	pllua_trigger *obj = pllua_checktrigger(L, nd);
	Datum retval = PointerGetDatum(NULL);
	TriggerEvent ev = obj->td->tg_event;
	int retindex = lua_gettop(L);
	const char *fieldname = TRIGGER_FIRED_BY_DELETE(ev) ? "old" : "new";
	pllua_datum *d;

	/* no point doing anything fancy for these cases */
	if (!TRIGGER_FIRED_FOR_ROW(ev)
		|| TRIGGER_FIRED_AFTER(ev))
		return retval;

	if (nret < 0 || nret > 1)
		luaL_error(L, "invalid number of results from trigger");

	/* trigger returned an explicit nil */
	if (nret == 1 && lua_isnil(L, retindex))
		return PointerGetDatum(NULL);

	/* pick the default tuple to return */
	if (TRIGGER_FIRED_BY_UPDATE(ev))
		retval = PointerGetDatum(obj->td->tg_newtuple);
	else
		retval = PointerGetDatum(obj->td->tg_trigtuple);

	/*
	 * if no return result and the trigger object was not modified, just return
	 * the default tuple. Note that we have to check whether the existing tuple
	 * was exploded in-place (which does not call __newindex) so obj->modified
	 * can't be trusted completely, it only tells us whether the row has been
	 * replaced wholesale.
	 */
	if (nret == 0)
	{
		lua_getuservalue(L, nd);
		pllua_trigger_get_typeinfo(L, obj, -1);
		switch (lua_getfield(L, -2, fieldname))
		{
			/* if it's not even in the cache, it can't have been modified */
			case LUA_TNIL:
				return retval;
			/* check for dummied-out "nil" */
			case LUA_TBOOLEAN:
				if (!lua_toboolean(L, -1))
					return PointerGetDatum(NULL);
				break;
			default:
				break;
		}

		d = pllua_todatum(L, -1, -2);
		if (!d)
			luaL_error(L, "incorrect type in trigger.row on return from trigger");
		/*
		 * newindex func has already built us a tuple of the correct form, but
		 * it's possible that the user subsequently exploded it by assigning to
		 * it element-wise. If so, we just leave it on the stack as if it was a
		 * function return value and drop out to the general case.
		 *
		 * But if it's unmodified, we can just copy it out.
		 */
		if (!d->modified)
		{
			if (!obj->modified)
			{
				/*
				 * If the user didn't replace or modify the tuple, it must be
				 * the original one, so no need to copy
				 */
				return retval;
			}
			return pllua_trigger_copytuple(L, d->value, obj->td->tg_relation->rd_id);
		}

		retindex = lua_gettop(L);
		nret = 1;
	}
	else if (!obj->modified)
	{
		/*
		 * Check whether the return value is raw-equal to the original unmodified
		 * and unexploded val.
		 */
		lua_getuservalue(L, nd);
		pllua_trigger_get_typeinfo(L, obj, -1);
		lua_getfield(L, -2, fieldname);
		if (lua_rawequal(L, -1, retindex))
		{
			d = pllua_todatum(L, -1, -2);
			if (!d)
				luaL_error(L, "incorrect type in trigger.row on return from trigger");
			if (!d->modified)
				return retval;   /* user returned the row unchanged */
		}
		lua_pop(L, 3);
	}

	/*
	 * no short cuts: take the value at retindex, push it through the value
	 * constructor, and return it as a new tuple
	 */
	lua_getuservalue(L, nd);
	pllua_trigger_get_typeinfo(L, obj, -1);
	lua_pushvalue(L, -1);
	lua_pushvalue(L, retindex);
	lua_call(L, 1, 1);

	d = pllua_todatum(L, -1, -2);
	if (!d)
		luaL_error(L, "incorrect type on return from trigger");
	return pllua_trigger_copytuple(L, d->value, obj->td->tg_relation->rd_id);
}

int
pllua_push_trigger_args(lua_State *L, TriggerData *td)
{
	char **tgargs = td->tg_trigger->tgargs;
	int nargs = td->tg_trigger->tgnargs;
	int i;
	for (i = 0; i < nargs; ++i)
		lua_pushstring(L, tgargs[i]);
	return nargs;
}

static struct luaL_Reg triggerobj_keys[] = {
	{ "new", pllua_trigger_get_new },
	{ "old", pllua_trigger_get_old },
	{ "name", pllua_trigger_get_name },
	{ "when", pllua_trigger_get_when },
	{ "operation", pllua_trigger_get_operation },
	{ "level", pllua_trigger_get_level },
	{ "relation", pllua_trigger_get_relation },
	{ NULL, NULL }
};

static struct luaL_Reg triggerobj_mt[] = {
	{ "__index", pllua_trigger_index },
	{ "__newindex", pllua_trigger_newindex },
	{ NULL, NULL }
};

/*
 * For event triggers we don't bother doing anything fancy
 */
static int
pllua_evtrigger_index(lua_State *L)
{
	pllua_event_trigger *obj = pllua_checkevtrigger(L, 1);
	const char *str = luaL_checkstring(L, 2);
	lua_settop(L, 2);

	if (strcmp(str, "event") == 0)
		lua_pushstring(L, obj->etd->event);
	else if (strcmp(str, "tag") == 0)
		lua_pushstring(L, obj->etd->tag);
	else
		lua_pushnil(L);
	return 1;
}

static struct luaL_Reg evtriggerobj_mt[] = {
	{ "__index", pllua_evtrigger_index },
	{ NULL, NULL }
};

int pllua_open_trigger(lua_State *L)
{
	pllua_newmetatable(L, PLLUA_TRIGGER_OBJECT, triggerobj_mt);
	lua_newtable(L);
	luaL_setfuncs(L, triggerobj_keys, 0);
	lua_setfield(L, -2, "_keys");
	lua_pop(L,1);

	pllua_newmetatable(L, PLLUA_EVENT_TRIGGER_OBJECT, evtriggerobj_mt);
	lua_pop(L,1);

	lua_pushboolean(L, 1);
	return 1;
}
