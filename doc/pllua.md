PL/Lua Reference
================


PostgreSQL environment
----------------------

PL/Lua provides two extensions:

	create extension pllua;  -- installs the trusted language
	create extension plluau;  -- installs the untrusted language

Two optional transform modules exist which are useful if the
optional "hstore" extension is loaded:

	create extension hstore_pllua;  -- for hstore type in pllua
	create extension hstore_plluau;  -- for hstore type in plluau

These allow direct conversions between hstore values and Lua tables.

The following optional configuration settings apply to PL/Lua. Most of
them require superuser privileges to set.

  + `shared_preload_libraries='pllua'`

    If set, `pllua.so` will be loaded in the postmaster process and
    the `pllua.on_init` string run there. Be careful with this, since
    errors in the init string will prevent PostgreSQL from starting.
    The benefit of this is that additional modules can be require'd
    into the interpreter and inherited by child processes via
    `fork()`. Most applications will likely not need this.

    By default, `pllua.so` is loaded and the init strings run on the
    first use within each database session.

  + `pllua.check_for_interrupts=boolean` (default: `true`)

    If set, a hook function checks for a query cancel interrupt at
    intervals while running Lua code.

  + `pllua.on_init='lua code chunk'`

    If set, this string is loaded and run early in the interpreter
    setup process. If `shared_preload_libraries` is used (see below),
    this string is run in the postmaster process (which is useful for
    preloading code to be inherited via `fork()`). No database access
    is possible. The `print()` function will output to the server log.

  + `pllua.on_trusted_init='lua code chunk'`

  + `pllua.on_untrusted_init='lua code chunk'`

    This string is run late in initialization of a trusted or
    untrusted interpreter, as applicable. It can do database access.
    The trusted init string is run outside the trusted environment, so
    it has full access to the system; if it wishes to expose loaded
    modules to the trusted environment, this must be done explicitly
    with the `trusted.allow()` or `trusted.require()` functions
    described below.

  + `pllua.on_common_init='lua code chunk'`

    This string is run late (after the previous init strings) in
    initialization of any interpreter. It can do database access. For
    trusted interpreter, the string is run inside the sandbox.

  + `pllua.install_globals=boolean` (default: `true`)

    If true, the `spi` and `pgtype` modules are stored as global
    tables, as if by:

		_G.spi = require 'pllua.spi'
		_G.pgtype = require 'pllua.pgtype'

     If false, this is not done, and functions wanting to access these
     modules will need to require them explicitly.

  + `pllua.prebuilt_interpreters=integer` (default: 1)

    If `pllua.so` was loaded in `shared_preload_libraries`, this
    specifies how many Lua states (interpreters) to prebuild. The
    `on_init` string is run independently in each one.

    The sole benefit of prebuilding more than one interpreter is if
    you expect most database sessions to use both trusted and
    untrusted language functions, or trusted language functions called
    from `SECURITY DEFINER` functions under more than one user. New
    states are always created on demand as needed within each session
    if the prebuilt ones are used up.

    The default is to create 1 prebuilt state if loaded from
    `shared_preload_libraries`.

  + `pllua.interpreter_reload_ident='arbitrary string'` (default: unset)

    If `pllua.so` is loaded in the postmaster, then altering this
    setting will cause any prebuilt interpreters to be destroyed and
    recreated. Also, if this value is set to a nonempty string,
    altering the value of `pllua.on_init` will also cause prebuilt
    interpreters to be rebuilt. The value of
    `pllua.interpreter_reload_ident` is stored in the created
    interpreters (as `_G._PL_IDENT`) for verification purposes.

    If this value is unset or empty then prebuilt interpreters are not
    reloaded except by postmaster restart.

    Additionally, altering the value causes `_G._PL_IDENT_NEW` to be
    set to the new value in existing active interpreters before their
    next use after the value changes.

  + `pllua.extra_gc_multiplier=real` (min 0, default 0, max 1000000)

  + `pllua.extra_gc_threshold=real` (min 0, default 0)

    These options do not require superuser privilege.

    If `multiplier` is 0 (the default), then no additional garbage
    collection is done.

    If `multiplier` is set to a value greater than 0 but less than
    1000000, then the amount of non-Lua memory newly allocated by the
    module is estimated, and before each return to the user, if that
    amount is at least `threshold` kbytes, then a `LUA_GCSTEP` call is
    made with a parameter of `(allocated_kbytes * multiplier)`. If
    `multiplier` is set to 1000000, then a `LUA_GCCOLLECT` call is
    made instead.


Lua environment
---------------

The Lua interpreters are initialized as follows.

The standard Lua libraries are installed and a number of global functions
are replaced:

  + `print()`

    replaced with a version that outputs `INFO:` messages to the
    client (except in the init strings, where it outputs `LOG:`
    to the server log)

  + `pcall()`

  + `xpcall()`

    replaced with versions that provide subtransaction support

  + `lpcall()`

    "light" pcall with no subtransactions, but which doesn't
    catch all errors

  + `coroutine.resume()`

    replaced with a version that propagates PG errors, like lpcall

Then the `pllua.trusted` module is loaded and initialized, but not
stored into any global variable (it can be accessed with `require`).

Then the `on_init` string is run if it is set.

Then the equivalent of the following is done:

	require 'pllua.elog'
	require 'pllua.funcmgr'
	if install_globals then
	  _G.pgtype = require 'pllua.pgtype'
	  _G.spi = require 'pllua.spi'
	else
	  require 'pllua.pgtype'
	  require 'pllua.spi'
	end
	require 'pllua.trigger'
	require 'pllua.numeric'
	require 'pllua.jsonb'

and in trusted interpreters only, the `pllua.trusted` module is assigned
to the global `_G.trusted` (outside the sandbox).

Then the `on_trusted_init` or `on_untrusted_init` string is run if set.

Then the `on_common_init` string is run if set.

Each module is described below.

PL/Lua code is invoked in two ways. Inline code blocks are invoked as:

	DO LANGUAGE pllua $$ string... $$;

This is processed as if by the following Lua code:

	function inline(str)
	  local env = setmetatable({}, { __index = _G })
	  local chunk = assert(load(str,"DO-block","t",env))
	  chunk(env)
	end

SQL-callable function or procedure (PostgreSQL 11+ only) definitions
are created as:

	CREATE FUNCTION name(args...) RETURNS ... LANGUAGE pllua
	  AS $$ body $$;

	CREATE PROCEDURE name(args...) LANGUAGE pllua
	  AS $$ body $$;

These are handled as follows. When the function is first called in a
session, the body is processed as if by the following Lua function:

	function compile(name,argdef,body)
	  local env = setmetatable({}, { __index = _G })
	  local fmt = "local self = (...) local function %s(%s) %s end return %s"
	  local chunk = assert(load(string.format(fmt,name,argdef,body,name),
	                            name,"t",env))
      return chunk(env)
    end

For non-trigger functions, the `argdef` string lists the names of
named arguments (if any) followed by a `...` varargs definition if not
all arguments have names (named arguments must not follow unnamed
ones). For trigger functions, the `argdef` string is always
`"trigger,old,new,..."` (where the additional arguments come from the
`CREATE TRIGGER` definition). For event triggers, it is simply `"trigger"`.

The intended effect is that functions and do-blocks run in their own
`self` environment which inherits the global one. They can still set
global variables, but must do so explicitly. Functions can do their
own first-call initialization by ending the function block early:

	create function name(args)... as $$
	    --[[ code here to execute on normal call]]
	  end
	  do
	    --[[ code here is executed only before first call]]
	$$;


pllua.elog
----------

The pllua.elog module is a table of simple functions:

	elog(severity, message)
	elog(severity, sqlstate, message)
	elog(severity, sqlstate, message, detail)
	elog(severity, sqlstate, message, detail, hint)
	elog(severity, { sqlstate = ?,
	                 message = ?,
	                 detail = ?,
	                 hint = ?,
	                 table = ?,
	                 column = ?,
	                 datatype = ?
	                 constraint = ?
	                 schema = ? })
	debug(...)   = elog('debug',...)
	log(...)     = elog('log',...)
	info(...)    = elog('info',...)
	notice(...)  = elog('notice',...)
	warning(...) = elog('warning',...)
	error(...)   = elog('error',...)

This is just the obvious wrapper around pg's ereport() call.

`sqlstate` parameters may be either 5-character codes or the error
names from the appendix to the PostgreSQL manual.

By default these functions are also available via the `spi` module.


pllua.funcmgr
-------------

This module exposes nothing to Lua.


pllua.pgtype
------------

The pgtype object provides the following functionality:

+ `pgtype(value)`\
  if value is a Datum type, returns its typeinfo, else nil
+ `pgtype(value,0)`\
  if value is a Datum type, returns its typeinfo, otherwise
  returns the typeinfo of the result type of the current
  function (if any)
+ `pgtype(value,argno)`\
  if value is a Datum type, returns its typeinfo, otherwise
  returns the typeinfo of argument `"argno"` (`1..n`) of the current
  function (if any). This is the recommended way to get the type
  of a function parameter which might have been transparently
  converted to a Lua value.
+ `pgtype['typename']`\
  `pgtype.typename`\
  parse `'typename'` as an SQL type string and return the typeinfo
  (or nil if no such type exists)
+ `pgtype.array['typename']`\
  `pgtype.array.typename`\
  parse `'typename'` as an SQL type string and return the typeinfo
  of its array type (or nil if no such type exists)

The typeinfo object returned from any of the above has the following
functionality:

+ `typeinfo(datum)`\
  Construct a new `Datum` object by copying from the specified
  value, which must already be of a compatible type
+ `typeinfo(...)`\
  Construct a new `Datum` object of the specified type from the
  arguments given. The nature of the arguments varies according
  to the category of type being constructed.
+ `typeinfo:fromstring(str)`\
  Construct a new `Datum` object given its standard text
  representation in `str`. For some types the distinction between
  `typeinfo:fromstring(str)` and `typeinfo(str)` is significant.
+ `typeinfo:frombinary(str)`\
  Construct a new `Datum` object given its wire-protocol binary
  representation in `str`. This is less useful than it might seem
  because for many data types, the interpretation of the binary
  representation is dependent on the client_encoding setting.
+ `typeinfo:name([typmod])`\
  Returns the name of the type as SQL syntax (same as the
  `format_type` function in SQL, or `::regtype` output)
+ `typeinfo:element()`\
  For array or range types, returns the typeinfo of the element type
+ `typeinfo:element(str)`\
  For row types, returns the typeinfo of the named column

The type constructor call has the following forms according to the
type category (scalar, row, array, range)

  + `scalartype(nondatum...)`

    In order, stopping on the first success:

    1. If the input value is not a single string, and a transform
       function exists for this type, then the transform function is
       called to try and convert the value.

    0. If there is more than one input value, an error is raised.

    0. The built-in simple transforms from Lua values to SQL types are
       tried, including checking for domains over known types. Note:
       in some cases, especially `bytea`, this gives a different
       result for string input than `:fromstring` would.

    0. If the input is a single string, it is processed as if by
       `scalartype:fromstring(str)`

    0. Otherwise an error is raised.

  + `rowtype(table)`

    If passed a single Lua table or userdata (other than a `Datum`),
    this is assumed to be indexable by column names, and a row is
    constructed by applying the typeinfo operation of each column type
    to the indexed value.

  + `rowtype(...)`

    otherwise, the number of arguments must equal the arity of the row
    (i.e. the number of undropped columns). Each argument is matched
    positionally to its column, converted to the column's type, and
    then has typmod coercion applied if necessary (e.g. length checks
    for `varchar(n)`, padding for `char(n)` etc.)

  + `arraytype()`

    constructs an empty array.

  + `arraytype(val,val,val,...)`

    constructs a one-dimensional array of the specified values.
    _(currently, the ambiguous case where one single Datum is passed
    is resolved as the generic typeinfo(datum) call, NOT this one)_

  + `arraytype(table, dim...)`

    One integer value must be given for each dimension of the array.
    The table is indexed accordingly to populate the new array.

  + `arraytype(table)`

    Constructs a one-dimensional array assuming the largest integer
	index in the table as the array size. (Use the above form for
	multi-dimensional arrays or for precise control over the size when
	trailing nulls are allowed.)

  + `rangetype()`

    constructs an empty range

  + `rangetype(string)`

    as for `rangetype:fromstring(string)`

  + `rangetype(lo,hi[,bounds])`

    Constructs a range from specified bounds, with nil values treated
    as infinities, and the "bounds" string interpreted in the usual
    way (i.e. `"[]"`, `"[)"`, `"(]"`, `"()"`).

Some specific types have additional functions: see the `pllua.jsonb`
and `pllua.numeric` modules.

`Datum` values themselves provide the following:

  + `tostring(datum)`

    returns the datum's standard text representation (inverse
    of `typeinfo:fromstring()`)

(tobinary function/syntax TBD)

`Datum` values of row types allow indexing by name or number:

	rowval.column_name
	rowval['column_name']
	rowval[attno]

Indexed column values can be assigned to.

Note that the `attno` does not correspond to the positional index of
the column if columns have been dropped.

Row types can be iterated with `pairs()` (but do NOT use ipairs):

	for colname,value,attno in pairs(rowval) do ...

This iteration is always in column order.

`Datum` values of array type allow indexing by number, including
multiple dimensions, and assignments to individual elements:

    arrayval[i]
    arrayval[i][j]  etc.

Arrays can be iterated with `pairs()` and, in some Lua versions only,
`ipairs()`:

	for i,val in ipairs(arrayval) do ...

`Datum` values of range type provide the following immutable
pseudo-columns:

	r.lower
	r.upper
	r.lower_inc
	r.upper_inc
	r.lower_inf
	r.upper_inf
	r.isempty

`Datum` values of row, array or `jsonb` type provide a
mapping/deserialization operation:

	rowval{ map = function(colname,value,attno,row) ... return value end,
	        null = (any value, default nil),
	        discard = (boolean, default false)
	      }

	arrayval{ map = function(elem,array,i,j,k...) ... return elem end,
	          null = (any value, default nil),
	          discard = (boolean, default false)
	        }

	jsonbval{ map = function(key,val,...) ... return key,val end,
	          null = (any value, default nil),
	          discard = (boolean, default false),
	          pg_numeric = (boolean, default false)
	        }

The result in all cases is returned as a Lua table, not a datum,
unless the "discard" option was given as true, in which case no
result at all is returned.

The map function for arrays is passed as many indexes as the
original array dimension.

The map function for `jsonb` values is passed the path leading up to
the current key (not including the key) as separate additional
parameters. The key is an integer if the current container is an
array, a string if the container is an object, and nil if this is a
single top-level scalar value (which I believe is not strictly allowed
in the JSON spec, but PostgreSQL allows it). The `key`/`val` returned by
the function are used to store the result, but do not affect the path
values passed to any other function call. If `discard` is not specified,
then the function is also called for completed containers (in which
case `val` will be a table). If `pg_numeric` is not true, then numeric
values are converted to Lua numbers, otherwise they remain as `Datum`
values of `numeric` type (for which see below).

Substitution of null values happens BEFORE the mapping function is
called; if that's not what you want, then do the substitution
yourself before returning the result. (If the mapping function
itself returns a Lua nil, then the entry will be omitted from the
result.)

The built-in simple type transformations from PG to Lua are as
follows:

    text, varchar(n), char(n), xml, json, name, cstring  ->  string

    bytea  ->  string, WITHOUT any escaping or conversions

    enum  ->  string

    boolean  ->  boolean

    float4, float8  ->  number

    oid, smallint, integer  ->  number

    bigint  ->  number  IF the underlying Lua has 64-bit integers

    refcursor  ->  SPI cursor object

    NULL of any type  ->  nil

If a transform function is defined for a given type, then it behaves
as if added to the list of simple transformations. Otherwise, values
received from PG remain as Datum objects.

The built-in simple transforms from Lua to PG are:

    nil  ->  any type

    boolean  ->  boolean

    string  ->  text, varchar, cstring, refcursor

    string  ->  bytea, WITHOUT conversion or escaping

    string  ->  boolean  (accepts only "true","t","1","false","f","0")

    number  ->  smallint, integer, bigint, oid  (error unless exact integer)

    number  ->  float4, float8

    number  ->  boolean  (accepts only 0 or 1)

    number  ->  numeric

    SPI cursor object  ->  refcursor

Conversions not listed as "simple transforms" are done with either a
transform function, if defined, or the type constructor as detailed
above.

Notice that for `bytea`, the simple transform just copies the bytes
(Lua strings are byte strings, not character strings). This makes
the simple conversion quite different to the `fromstring`/`tostring`
conversion, which uses the PG text representation.


pllua.spi
---------

The spi module provides the following functionality (as a table of
functions):

  + `spi.execute("query text", arg, arg, ...)`

  + `spi.execute_count("query text", maxrows, arg, arg, ...)`

    execute the given query text as SQL with the given arguments.
    Returns a table containing a sequence (possibly empty) of rows for
    queries that return rows, otherwise returns an integer count.

    For all query execution methods, if called from a nonvolatile
    function, the query will be run in "readonly" mode using the
    caller's snapshot. Otherwise a new snapshot is taken.

  + `spi.prepare("query text", {argtypes}, [{options}])`

    returns a statement object. `{argtypes}` is a table containing
    type names or typeinfo objects. Allowed options are:

    * `scroll = true or false`
    * `no_scroll = true`
    * `fast_start = true`
    * `custom_plan = true`
    * `generic_plan = true`
    * `fetch_count = integer`

    The `fetch_count` option is used only by `rows()` iterators.

  + `spi.rows("query text", args...)`

    returns an iterator:

		for r in spi.rows("query") do ...

  + `spi.findcursor("name")`

    if "name" is the name of an open portal (i.e. cursor), then
    returns a cursor object to access this portal. Otherwise returns
    nil. The cursor is marked as unowned (it will not be closed by
    garbage collection).

  + `spi.newcursor(["name"])`

    if "name" is the name of an open portal (i.e. cursor), then
    returns a cursor object (unowned) to access this portal. Otherwise
    creates a new cursor object with no portal, recording the name
    given for use with a later open() call.

  + `spi.is_atomic()`

    returns true if the call context is atomic with respect to
    (top-level) transactions; this is always true in pg versions
    before PostgreSQL 11; it is only false when code is being executed
    in PostgreSQL 11+ in a `CALL` or `DO` statement which is outside
    any explicit transaction. (These are the only contexts in which
    `spi.commit` and `spi.rollback` are allowed.)

  + `spi.commit()`

  + `spi.rollback()`

    (Not defined in pg versions before 11). If in a non-atomic
    context, these commit or abort the current transaction, and
    immediately start a new one. An error is raised if they are
    attempted in an atomic context or inside a subtransaction.

  + `spi.elog(...)`

  + `spi.error(...), .warning(...), .notice(...), .info(...), .debug(...), .log(...)`

    These functions from pllua.elog are accessible via spi.* for
    convenience.

SPI statement objects have the following functionality:

  + `stmt:execute(arg, arg, ...)`

  + `stmt:execute_count(maxrows, arg, arg, ...)`

    execute the statement, with the same result as spi.execute

  + `stmt:getcursor(arg, arg, ...)`

    return an open cursor (with an arbitrarily assigned name) for
    the statement. The cursor is marked as owned.

  + `stmt:rows(arg, arg, ...)`

    return an iterator for the statement execution, as `spi.rows()`

  + `stmt:numargs()`

    returns an integer giving the expected number of arguments
    (including any unused numbered params) expected

  + `stmt:argtype(argnum)`

    returns the typeinfo for the expected type of the specified arg

SPI cursor objects have the following functionality:

  + `cur:open(stmt,arg,arg...)`

  + `cur:open(query_string,arg,arg...)`

    The cursor object must not be already open. The specified
    statement or query string is executed in a new portal whose name
    is given by the cursor name (if one has been assigned).

    The original cursor object is returned. Cursors returned by an
    `open()` call are marked as owned.

  + `cur:isopen()`

    returns true if the cursor is open

  + `cur:close()`

    close the portal (whether or not we created it or own it)

  + `cur:isowned()`

    returns true if the cursor is marked as owned. An "owned" cursor
    has its portal closed (if it's still open) if the cursor object is
    garbage-collected; this is intended for cursors opened by Lua
    functions and not returned to callers. An unowned cursor's portal
    is not affected by the collection of the cursor object.

  + `cur:own()`

  + `cur:disown()`

    mark the cursor as owned or not. Returns the cursor.

    Typical intended use is `return c:disown()` when returning a
    cursor opened by a function to its caller.

  + `cur:name()`

    returns the open portal name (if the cursor is open) or the
    assigned name (if not).

  + `cur:fetch([n, [dir]])`

    Fetch according to the specified number and direction parameters.
    `"dir"` can be:

    * `"forward" / "next"`\
       fetch N rows forward
    * `"backward" / "prior"`\
       fetch N rows backward
    * `"absolute"`\
       fetch row at absolute position `n`
    * `"relative"`\
       fetch row at relative position `n`

    By default, fetch one row in the forward direction.

  + `cur:move([n, [dir]])`

    Move the cursor without fetching. Note that the cursor is left at
    the same position it would be _after_ executing the same operation
    as a fetch. So to position the cursor such that the next forward
    fetch will return the first row, use `cur:move(0, 'absolute')`

There can only be one cursor object for a given open portal - doing a
findcursor on an existing cursor will always return the same object.
(But note that this matching is by portal, not name - if a cursor was
closed and reopened with the same name, findcursor will return a
different object for the new cursor.) If a cursor is closed by
external code (or transaction end), then the `:isopen()` state will be
automatically updated (this happens when the portal is actually
dropped). Cursor options are set on the statement object.

`refcursor` parameters and results are transparently converted to and
from SPI cursor objects. But note that when returning a cursor from a
function, it should be explicitly disowned to ensure that garbage
collection won't close it from under the caller's use of it.


pllua.trusted
-------------

The trusted interpreter is implemented using a sandbox system;
trusted-language code is run in an environment into which only safe
functions have been copied (or proxied).

However, in order to allow administrators to provide access to
additional modules inside the sandbox in a controlled manner, the
initialization strings on_init and on_trusted_init are run outside
the sandbox and the functions in pllua.trusted can be used by those
strings to make additional modules accessible.

For example, setting

	pllua.on_trusted_init='trusted.allow{"lpeg","re"}'

would load the `lpeg` and `re` modules and make them accessible inside
the sandbox via `require "lpeg"` etc.

**THE ADMINISTRATOR IS RESPONSIBLE FOR ASSESSING THE SECURITY
AND SAFETY OF MODULES.** It must be stressed that many modules,
whether implemented in Lua or C, perform operations that will either
violate security or risk crashing the server. A non-exhausive list of
things that are dangerous in modules would include:

<div class="no-dl-fudge">

  * any assumption that the caller's `_G` or `_ENV` is the same as the
    module's, or any exposure of the module's `_G` to the caller

  * any i/o or networking functionality exposed by the module to the caller

  * any use of `lua_pcall` or `lua_resume` from C to call code that
    might throw an SQL error

</div>

The available functions are:

  + `trusted.allow(module, newname, mode, global, preload)`

    This makes the module `module` accessible via `require 'newname'`
    (`newname` is defaulted to `module` if nil or omitted) inside the
    sandbox using the adapter specified by `mode` (default `"proxy"`).
    The module is not actually loaded until the first `require` unless
    either `global` or `preload` is a true value.

    Then, if `global` is true or a string, it executes the equivalent
    of:

		_G[ (type(global)=="string" and global) or newname or module ]
		  = require(newname or module)

    inside the sandbox.

    Mode can be `"direct"` (exposes the module to the sandbox
    directly), `"copy"` (makes a recursive copy of it and any
    contained tables, without copying metatables, otherwise as
    `"direct"`), and `"proxy"` which returns a proxy table having the
    module in the metatable index slot (and any table members in the
    module proxied likewise; `"sproxy"` omits this step). All modes
    behave like `"direct"` if the module's value is not a table.

    **PROXY MODE IS NOT INTENDED TO BE A FULLY SECURE WRAPPER FOR
    ARBITRARY MODULES.** It's intended to make it _possible_ for
    simple and well-behaved modules or adapters to be used easily
    while protecting the "outside" copy from direct modification from
    inside. If the module returns any table from a function, that
    table might be modified from inside the sandbox.

    **NEITHER PROXY MODE NOR COPY MODE ARE GUARANTEED TO WORK ON ALL
    MODULES.** The following constructs (for example) will typically
    defeat usage of either mode:

    <div class="no-dl-fudge">

      * use of empty tables as unique identifiers

      * use of table values as keys

      * metatables on the module table or its contents with anything
        other than `__call` methods

    </div>

    If you find yourself wanting to use this on a module more complex
    than (for example) "lpeg" or "re", then consider whether you ought
    to be using the untrusted language instead.

    If the `module` parameter is actually a table, it is treated as a
    sequence, each element of which is either a module name or a table
    `{ 'module', newname, mode, global, preload }` with missing values
    defaulted to the original arguments. This enables the common case
    usage to be just\:

		trusted.allow{"foo", "bar", "baz"}

  + `trusted.require(module, newname, mode)`

    equiv. to `trusted.allow(module, newname, mode, true, true)`

  + `trusted.remove('newname','global')`

    undoes either of the above (probably not very useful, but you
    could do trusted.remove('os') or whatever)

To use these functions from the on_init string, you must
`require 'pllua.trusted'` explicitly, and use the return value of that to
access the functions. Passing a true value for the `preload` argument
of `trusted.allow` allows for preloading of modules before forking
when using prebuilt interpreters.

The trusted environment's version of `load` overrides the text/binary
mode field (loading binary functions is unsafe) and overrides the
environment to be the trusted sandbox if the caller didn't provide one
itself (but the caller can still give an explicit environment of nil
or anything else).


pllua.trigger
-------------

This module provides nothing directly to Lua, but a `trigger`
parameter is passed as the first parameter to trigger functions (and
a different trigger parameter to event-trigger functions).

The `trigger` object for DDL triggers ("event triggers") provides the
following values when indexed:

+ `trigger.event`\
  Event for which the trigger was fired
+ `trigger.tag`\
  Command tag

See the PostgreSQL documentation for details.

The trigger object for DML triggers provides the following values
when indexed:

+ `trigger.new`\
  the "new" row for the operation (or nil)
+ `trigger.old`\
  the "old" row for the operation (or nil)
+ `trigger.row`\
  an alias for whichever of `old` or `new` the operation is
  expected to return; i.e. `new` for insert or update
  operations, `old` for deletes
+ `trigger.name`\
  name used in `CREATE TRIGGER`
+ `trigger.when`\
  `"before"`, `"after"` or `"instead"`
+ `trigger.operation`\
  `trigger.op`\
  `"insert"`, `"update"`, `"delete"`, `"truncate"`
+ `trigger.level`\
  `"row"` or `"statement"`
+ `trigger.relation`\
  a table

The `trigger.relation` table has this form:

	{
	  ["namespace"] = "public",
	  ["attributes"] = {
	    ["test_column"] = 1,
	  },
	  ["name"] = "table_name",
	  ["oid"] = 59059
	}

The fields of the trigger object are immutable with the exception of
`trigger.row`, which can be assigned a new row wholesale in order to
alter the result of the operation in a before trigger. This
immutability does not extend to contained fields: a trigger can
instead assign to individual `new.*` fields and the result will reflect
this.

The result of any trigger function which is not called `BEFORE` or
`INSTEAD`, or is not called `FOR EACH ROW`, is ignored (as are any
changes it makes to the trigger object). Trigger functions which are
called `BEFORE` or `INSTEAD` and `FOR EACH ROW` can do one of three
things:

1. To complete the operation normally, with no changes to the data,
   either return no value at all (not even `nil`), or return `trigger.row`
   without having assigned to `trigger.row` or any field of `old` or `new`.

0. To complete the operation normally with modified data:

   1. A non-nil return value will be converted to the table's row
      type using the type constructor, and this will be the new
      tuple, overriding any previous tuple and superseding any
      changes made to `trigger.row` or `new`/`old`.

   0. Returning no value at all (not even `nil`) having modified the
      content of `trigger.row` (directly or via whichever of `new` or
      `old` is appropriate for the triggered operation) will result in
      the value of `trigger.row` being used as the new tuple.

0. To suppress the operation, return the value `nil`, or assign `nil`
   to `trigger.row`.


pllua.numeric
-------------

PostgreSQL values of `numeric` type (henceforth Numeric values) are
converted to `Datum` objects as normal, but this module provides
substantial additional functionality for such types. The methods and
metamethods for Numeric values are accessible by default; code can
`require 'pllua.numeric'` in order to obtain access to the additional
non-method functions, e.g.:

	num = require 'pllua.numeric'
	if num.equal(x,y) then ...

Equality comparison is restricted by Lua semantics; a Numeric value
will never compare equal (`==`) to a Lua number, however `==` between
two Numerics compares for numerical equality. A plain function
`num.equal(x,y)` is provided for comparing equality. Note that
Numerics used as table keys will likely not work in any useful way
since two equal values are unlikely to compare as raw-equal. Other
operations allow mixed types, and will return Numeric if any input
value is.

Arithmetic operations on Numeric use PG semantics. In particular, the
`//` division operation truncates towards zero, not to `-inf`, and the
`%` modulus operator returns a result with the sign of the dividend,
not the sign of the divisor.

These functions are available directly or as methods on a Numeric
datum. (As direct calls they allow input of any Lua number.)

+ `abs`\
  `ceil`\
  `equal`\
  `exp`\
  `floor`\
  `isnan`\
  `sign`\
  `sqrt`\
  (as expected)
+ `log`\
  (optional base parameter defaults to natural log)
+ `tointeger`\
  returns nil if not exactly representable as a Lua integer
+ `tonumber`\
  returns a Lua number, not exact
+ `trunc`\
  `round`\
  take an optional number of digits parameter

The function `num.new(x)` will construct a new Numeric datum, as will
`pgtype.numeric(x)`.


pllua.jsonb
-----------

`jsonb` supports an inverse mapping operation for construction of JSON
values from Lua data:

	pgtype.jsonb(value,
	             { map = function(val) ... return val end,
	               null = (any value, default nil),
	               empty_object = (boolean, default false)
	               array_thresh = (integer, default 1000)
	               array_frac = (integer, default 1000)
	             })

`value` can be composed of any combination of the following (where
"collection" means a value which is either a table or possesses a
`__pairs` metamethod):

<div class="no-dl-fudge">

* Empty collections, which will convert to empty json arrays unless
  `empty_object=true` in which case they become empty objects

* Collections with only integer keys not less than 1, which will
  convert to json arrays (with lua index 1 becoming json index 0)
  unless either more than `array_thresh` initial null values would
  have to be inserted, or the total size of the array would be more
  than `array_frac` times the number of table keys.

* Collections with keys which can be stringified: strings or numbers, or
  tables or userdata with `__tostring` methods, will convert to json
  objects.

* Values which compare raw-equal to the `null` parameter are converted
  to json nulls

* Values of type `nil`, `boolean`, `number`, `string` are converted to
  corresponding json values

* `Datum` values of type `numeric` convert to json numbers

* `Datum` values of other types convert to json in the same way as they
  do in SQL; in particular, `jsonb` and `json` values are included
  directly, and values with casts to `jsonb` have those casts
  respected

* Values of other types that possess a `__tostring` metamethod are
  converted to strings

</div>

Unlike the other mapping functions, the map function for this
operation is called only for values (including collections), not
keys, and is not passed any path information.

<!--eof-->
