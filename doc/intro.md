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

### Returning multiple rows (SRFs)

Functions that return multiple rows (i.e. `RETURNS SETOF ...`) work as
coroutines; each row should be returned by passing it to
`coroutine.yield`, and when done, the function should return no
values. As a special case, if the function does a `return` with values
before doing any yield, it is considered to return 1 row.

	create function val3() returns setof integer language pllua
	as $$
	  for i = 1,3 do
	    coroutine.yield(i)
	  end
    $$;
	select val3();
	 val3
	------
	    1
	    2
	    3
	(3 rows)

SRFs written in PL/Lua run in value-per-call mode, so the execution of
the function may be (but often will not be) interleaved with other
parts of the query, depending on which part of the query the function
was called from.

In Lua 5.4, if execution of an SRF is aborted early due to a LIMIT
clause or other form of rescan in the calling query, or if the calling
portal is closed, then any `<close>` variables (including implicit
ones in `for` iterators) in the function are immediately closed. In
earlier Lua versions, the coroutine and any referenced objects are
subject to garbage collection at some indefinite future time.

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

The above simplistic approach will tend to drop json null values
(since Lua does not store nulls in tables), and loses precision on
numeric values not representable as floats; this can be avoided as
follows:

	create function add_stuff(val jsonb) returns jsonb language pllua
	as $$
	  local nullval = {}    -- use some unique object to mark nulls
	  local t = val{ null = nullval, pg_numeric = true }
	  t.newkey = { { foo = 1 }, { bar = 2 } }
	  return t, { null = nullval }
	$$;
	select add_stuff('{"oldkey":[147573952589676412928,null]}');
	                                   add_stuff
	-------------------------------------------------------------------------------
	 {"newkey": [{"foo": 1}, {"bar": 2}], "oldkey": [147573952589676412928, null]}
	(1 row)

Tables that originated from JSON are tagged as to whether they were
originally objects or arrays, so as long as you provide a unique null
value, this form of round-trip conversion should not change anything.
(See the `pllua.jsonb` module documentation for more detail.)

<!--eof-->
