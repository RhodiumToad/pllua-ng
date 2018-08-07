PL/Lua Introduction
===================

PL/Lua is a procedural language module for the PostgreSQL database
that allows server-side functions to be written in Lua.


Quick start
-----------

	create extension pllua;

	create function hello(person text) returns text language pllua as $$
	  return "Hello, " .. person .. ", from Lua!"
	$$;

	select hello('Fred');
	         hello
	------------------------
	 Hello, Fred, from Lua!
	(1 row)


Basic Examples
--------------

### Using `print()` for interactive diagnostics

Anything passed to the `print()` function will be raised as a
notification at `INFO` level, causing `psql` to display it
interactively; program clients will usually just ignore non-error
notices.

	create function print_lua_ver() returns void language pllua as $$
	  print(_VERSION)
	$$;
	select print_lua_ver();
	INFO:  Lua 5.3
	 print_lua_ver 
	---------------
	 
	(1 row)

### Simple arguments and results

Simple scalar types (integers, floats, text, bytea, boolean) are
converted to the matching Lua type, and conversely for results.

	create function add2(a integer, b integer) returns integer language pllua
	  as $$
	    return a + b
	$$;

Other data types are passed as userdata objects that can be converted
to strings with `tostring()` or accessed via provided methods and
metamethods. In particular, arrays and records are accessible in most
ways as though they were Lua tables, though they're actually not.

	create type myrow as (a integer, b text[]);
	create function foo(rec myrow) returns myrow language pllua as $$
	  print("a is", rec.a)
	  print("b[1] is", rec.b[1])
	  print("b[2] is", rec.b[2])
	  return { a = 123, b = {"fred","jim"} }
	$$;
	select * from foo(row(1,array['foo','bar'])::myrow);
	INFO:  a is     1
	INFO:  b[1] is  foo
	INFO:  b[2] is  bar
	  a  |     b
	-----+------------
	 123 | {fred,jim}
	(1 row)

### Sum of an array

	create function array_sum(a integer[]) returns integer language pllua
	as $$
	  local total = 0
	  for k,v in pairs(a) do
	    total = total + v
	  end
	  return total
	$$;

The above assume single-dimension arrays with no NULLs. A more generic
method uses the array mapping function provided by the array userdata:

	create function array_sum(a integer[]) returns integer language pllua
	as $$
	  local total = 0
	  a{ null = 0,
	     map = function(v,...) total = total + v end,
		 discard = true }
	  return total
	$$;

### Simple database queries

The local environment created for each function is a good place to
cache prepared queries:

	create table objects (id integer primary key, value text);
	create function get_value(id integer) returns text language pllua stable
	as $$
	  local r = q:execute(id)
	  return r and r[1] and r[1].value or 'value not found'
	end
	do -- the part below will be executed once before the first call
	  q = spi.prepare("select value from objects where id=$1")
	$$;

The result of executing a query is a table containing rows (if any)
for select queries, or an integer rowcount for queries that do not
return rows.

### Triggers

	create function mytrigger() returns trigger language pllua
	as $$
	  -- trigger functions are implicitly declared f(trigger,old,new,...)
	  new.total_cost = new.price * new.qty;
	  return new
	$$;

### JSON handling

Values of type `json` are passed to Lua simply as strings. But the
`jsonb` data type is supported in a more direct fashion.

`jsonb` values can be mapped to Lua tables in a configurable way, and
Lua tables converted back to `jsonb` values:

	create function add_stuff(val jsonb) returns jsonb language pllua
	as $$
	  local t = val{}	-- convert jsonb to table with default settings
	  t.newkey = { { foo = 1 }, { bar = 2 } }
	  return t
	$$;
	select add_stuff('{"oldkey":123}');
		                       add_stuff
	-----------------------------------------------------
	 {"newkey": [{"foo": 1}, {"bar": 2}], "oldkey": 123}
	(1 row)

