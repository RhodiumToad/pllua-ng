
pllua [![Build Status](https://travis-ci.org/pllua/pllua.svg)](https://travis-ci.org/pllua/pllua)
=====

Embeds Lua into PostgreSQL as a procedural language module.

Please report any remaining bugs or missing functionality on github.

Currently it should build against (recent point releases of) pg
versions 9.5, 9.6, 10, 11, 12 and 13. It is known that this module
will never work on pg versions before 9.5 (we rely critically on
memory context callbacks, which were introduced in that version).

Lua 5.4 (only 5.4.2 onwards), Lua 5.3, and LuaJIT 2.1beta (with
COMPAT52) are fully supported at this time. (Lua 5.4.0 and 5.4.1 are
checked for at runtime and rejected, because they would otherwise
require runtime stack limit calculations.)

Documentation is being migrated from this README to a more comprehensive
document:

[Documentation](https://pllua.github.io/pllua/)


COMPATIBILITY WITH 0.x (previous pllua-ng releases)
---------------------------------------------------

The server.* namespace is removed, to facilitate the compatibility
option described below.

server.error, server.debug, etc. are now available via spi.error,
spi.debug, or can be obtained from the 'pllua.elog' package via
require().  If you want, you can do:

        pllua.on_common_init='server = require "pllua.elog"'

to restore the previous behavior.


COMPATIBILITY WITH 1.x (old pllua)
----------------------------------

This version permits a degree of compatibility with pllua 1.x via
the following setting:

        pllua.on_common_init='require "pllua.compat"'

This creates the server.* namespace with functions that match the 1.x
calling conventions (e.g. passing args as tables). It also creates
global functions fromstring(), subtransaction(), debug(), log(),
info(), notice(), warning(), and setshared().

The following incompatibilities remain:

 + coroutine.yield() with no result values in 1.x ended execution of
   the SRF; in this version it returns a NULL row and continues.

 + The global error() is not modified, so using it to throw a table
   has the same effect it would have in plain Lua. In 1.x, a metatable
   was added to the thrown value in this case.

 + The pgfunc library is not supported at all.

 + The "readonly" parameter to server.execute and friends is ignored.
   All queries in a stable function are readonly, and all queries in
   a volatile function are read-write.

 + Returning multi-dimensional arrays by doing a simple return of a
   Lua table is no longer supported.

The pllua.compat module is implemented in pure Lua (inside the sandbox
in trusted mode), see src/compat.lua for the implementation.

Please report any incompatibilities discovered.


CHANGES
-------

*NOTE:* the name of the module is now just "pllua", and its extension
packaging is split into two according to usual practice for pl modules
(in this case "pllua" for the trusted language and "plluau" for the
untrusted language).

(Compared to the old pllua project:)

Some names and locations have been changed.

The old pllua.init table is gone. Instead we support three init
strings (superuser-only): pllua.on_init, pllua.on_trusted_init,
pllua.on_untrusted_init.

Note that the on_init string can be run in the postmaster process, by
including pllua in shared_preload_libraries. Accordingly, on_init
cannot do any database access, and the only function directly
available from this module is print(), which in this environment will
output to the server log as LOG: messages.

The on_init string can now access the trusted.allow() functionality,
but only by doing an explicit require 'pllua.trusted'. e.g.

      local t = require 'pllua.trusted'
      t.allow{ "lpeg", "re" }

SPI functionality is now in global table spi and has different calling
conventions:

      spi.execute("query text", arg, arg, ...)
      spi.execute_count("query text", maxrows, arg, arg, ...)
      spi.prepare("query text", {argtypes}, [{options}])
        - returns a statement object:
          s:execute(arg, arg, ...)  - returns a result table
          s:execute_count(maxrows, arg, arg, ...)  - returns a result table
          s:rows(arg, arg, ...) - returns iterator
          s:numargs() - returns integer
          s:argtype(argnum) - returns typeinfo
      spi.rows("query text", args...)
        - returns iterator

Execution now returns a table with no number keys (#t == 0) in the
event of no matching rows, whereas the old version returned nil. The
result is also currently a plain table, not an object.

spi.prepare takes an options table with these possible values:

      scroll = true or false
      no_scroll = true
      fast_start = true
      custom_plan = true
      generic_plan = true
      fetch_count = integer

Note that "scroll" and "no_scroll" are independent options to the
planner, but we treat { scroll = false } as if it were { no_scroll = true }
because not doing so would be too confusing. The fetch_count value is
used only by the :rows iterator, to determine how much prefetch to use;
the default is 50. (Smaller values might be desirable for fetching very
large rows, or a value of 1 disables prefetch entirely.)

Cursors work:

      spi.findcursor("name")   - find already-open portal by name
      spi.newcursor(["name"])  - find existing cursor or create new one
      s:getcursor(args)   - get cursor from statement (can't specify name)
      c:open(stmt,args)   - open a cursor
      c:open(query,args)  - open a cursor
      c:isopen()          - is it open
      c:name()
      c:fetch([n, [dir]])  - fetch n rows in dir (default: forward 1)
      c:move([n, [dir]])

There can only be one cursor object for a given open portal - doing a
findcursor on an existing cursor will always return the same object.
(But note that this matching is by portal, not name - if a cursor was
closed and reopened with the same name, findcursor will return a
different object for the new cursor.) If a cursor is closed by
external code (or transaction end), then the :isopen() state will be
automatically updated (this happens when the portal is actually
dropped). Cursor options are set on the statement object.

Refcursor parameters and results are transparently converted to and
from SPI cursor objects.

:save on a statement is now a no-op - all statements seen by lua code
have been passed through SPI_keepplan and are managed by Lua garbage
collection. (It was never safe to do otherwise.)

(SPI interface is particularly subject to change - in particular to
something more compatible with client-side database APIs)

print() is still a global function to print an informational message,
but other error levels such as debug, notice are installed as
spi.debug(), spi.warning() etc. spi.elog('error', ...) is equivalent
to spi.error(...) and so on.

spi.error() and friends can take optional args:

      spi.error('message')
      spi.error('sqlstate', 'message')
      spi.error('sqlstate', 'message', 'detail')
      spi.error('sqlstate', 'message', 'detail', 'hint')
      spi.error({ sqlstate = ?,
                  message = ?,
                  detail = ?,
                  hint = ?,
                  table = ?,
                  column = ?, ...})

Sqlstates can be given either as 5-character strings or as the string
names used in plpgsql: spi.error('invalid_argument', 'message')

Subtransactions are implemented via pcall() and xpcall(), which now
run the called function in a subtransaction. In the case of xpcall,
the subtransaction is ended *before* running the error function, which
therefore runs in the outer subtransaction. This does mean that while
Lua errors in the error function cause recursion and are eventually
caught by the xpcall, if an error function causes a PG error then the
xpcall will eventually rethrow that to its own caller. (This is
subject to change if I decide it was a bad idea.)

e.g.

      local ok,err = pcall(function() --[[ do stuff in subxact ]] end)
      if not ok then print("subxact failed with error",err) end

Currently there's also an lpcall function which does NOT create
subtransactions, but which will catch only Lua errors and not PG
errors (which are immediately rethrown). It's not clear yet how useful
this is; it saves the (possibly significant) subxact overhead, but it
can be quite unpredictable whether any given error will manifest as a
Lua error or a PG error.

The readonly-global-table and setshared() hacks are omitted. As the
trusted language now creates an entirely separate lua_State for each
calling userid, anything the user does in the global environment can
only affect themselves.

Type handling is all different. The global fromstring() is replaced by
the pgtype package/function:

      pgtype(d)
        -- if d is a pg datum value, returns an object representing its
           type

      pgtype(d,n)
        -- if d is a datum, as above; if not, returns the object
           describing the type of argument N of the function, or the
           return type of the function if N==0

      pgtype['typename']
      pgtype.typename
      pgtype(nil, 'typename')
        -- parse 'typename' as an SQL type and return the object for it

      pgtype.array.typename
      pgtype.array['typename']
        -- return the type for "typename[]" if it exists

The object representing a type can then be called as a constructor for
datum objects of that type:

      pgtype['mytablename'](col1,col2,col3)
      pgtype['mytablename']({ col1 = val1, col2 = val2, col3 = val3})
      pgtype.numeric(1234)
      pgtype.date('2017-12-01')
      pgtype.array.integer(1,2,3,4)
      pgtype.array.integer({1,2,3,4}, 4)        -- dimension mandatory
      pgtype.array.integer({{1,2},{3,4}},2,2)   -- dimensions mandatory
      pgtype.numrange(1,2)        -- range type constructor
      pgtype.numrange(1,2,'[]')   -- range type constructor

or the :fromstring method can be used:

      pgtype.date:fromstring('string')

In turn, datum objects of composite type can be indexed by column
number or name:

      row.foo  -- value of column "foo"
      row[3]   -- note this is attnum=3, which might not be the third
                  column if columns have been dropped

Arrays can be indexed normally as a[1] or a[3][6] etc. By default
array indexes in PG start at 1, but values starting at other indexes
can be constructed. One-dimensional arrays (but not higher dimensions)
can be extended by adding elements with indexes outside the current
bounds; ranges of unassigned elements between assigned ones contain
NULL.

tostring() works on any datum and returns its string representation.

pairs() works on a composite datum (and actually returns the attnum as a
third result):

      for colname,value,attnum in pairs(row) do ...

The result is always in column order.

ipairs() should NOT be used on a composite datum since it will stop at
a null value or dropped column.

Arrays, composite types, and jsonb values support a mapping operation
controlled by a configuration table:

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

The map function for arrays is passed as many indexes as the original
array dimension.

The map function for jsonb values is passed the path leading up to the
current key (not including the key) as separate additional parameters.
The key is an integer if the current container is an array, a string
if the container is an object, and nil if this is a single top-level
scalar value (which I believe is not strictly allowed in the json
spec, but pg allows it). The key/val returned by the function are used
to store the result, but do not affect the path values passed to any
other function call. If discard is not specified, then the function is
also called for completed containers (in which case val will be a
table). If pg_numeric is not true, then numeric values are converted
to Lua numbers, otherwise they remain as Datum values of numeric type
(for which see below).

Substitution of null values happens BEFORE the mapping function is
called; if that's not what you want, then do the substitution yourself
before returning the result. (If the mapping function itself returns a
Lua nil, then the entry will be omitted from the result.)

As a convenience shorthand, these work:

      d(nvl)   -> d{null = nvl}
      d(func)  -> d{map = func}
      d()      -> d{}

Jsonb supports an inverse mapping operation for construction of json
values from lua data:

      pgtype.jsonb(value,
                   { map = function(val) ... return val end,
                     null = (any value, default nil),
                     empty_object = (boolean, default false)
                     array_thresh = (integer, default 1000)
                     array_frac = (integer, default 1000)
                   }

"value" can be composed of any combination of (where "collection"
means a value which is either a table or possesses a __pairs
metamethod):

 + Empty collections, which will convert to empty json arrays unless
   empty_object=true in which case they become empty objects

 + Collections with only integer keys >= 1, which will convert to json
   arrays (with lua index 1 becoming json index 0) unless either more
   than array_thresh initial null values would have to be inserted, or
   the total size of the array would be more than array_frac times the
   number of table keys.

 + Collections with keys which can be stringified: strings or numbers, or
   tables or userdata with __tostring methods, will convert to json
   objects.

 + Values which compare raw-equal to the "null" parameter are converted
   to json nulls

 + Values of type nil, boolean, number, string are converted to
   corresponding json values

 + Datum values of type pgtype.numeric convert to json numbers

 + Datum values of other types convert to json in the same way as they
   do in SQL; in particular, jsonb and json values are included
   directly, and values with casts to jsonb have those casts
   respected

 + Values of other types that possess a __tostring metamethod are
   converted to strings

Unlike the other mapping functions, the map function for this
operation is called only for values (including collections), not keys,
and is not passed any path information.

Range types support the following pseudo-columns (immutable):

      r.lower
      r.upper
      r.lower_inc
      r.upper_inc
      r.lower_inf
      r.upper_inf
      r.isempty

Function arguments are converted to simple Lua values in the case of:

 + integers, floats  -- passed as Lua numbers

 + text, varchar, char, json (not jsonb), xml, cstring, name -- all passed
   as strings (with the padding preserved in the case of char(n))

 + enums  -- passed as the text label

 + bytea  -- passed as a string without any escaping or conversion

 + boolean  -- passed as boolean
 
 + nulls of any type  -- passed as nil

 + refcursor values are converted to or from SPI cursor objects
   (whether or not they correspond to open portals)

 + domains over any of the above are treated as the base types

Other values are kept as datum objects.

The trusted language is implemented differently - rather than removing
functions and packages, the trusted language evaluates all
user-supplied code (everything but the init strings) in a separate
environment table which contains only whitelisted content. A mini
version of the package library is installed in the sandbox
environment, allowing package.preload and package.searchers to work
(the user can install their own function into package.searchers to
load modules from database queries if they so wish).

See the main documentation for details on making additional modules
available to the trusted language.

A set-returning function isn't considered to end until it either
returns or throws an error; yielding with no results is considered the
same as yielding with explicit nils. (Old version killed the thread in
that scenario.) A set-returning function that returns on the first
call with no result is treated as returning 0 rows, but if the first
call returns values, those are treated as the (only) result row.

Trigger functions no longer have a global "trigger" object, but rather
are compiled with the following definition:

      function(trigger,old,new,...) --[[ body here ]] end

"trigger" is now a userdata, not a table, but can be indexed as
before.  Trigger functions may assign a row to trigger.row, or modify
fields of trigger.row or trigger.new, or may return a row or table; if
they do none of these and return nothing, they're treated as returning
trigger.row unchanged. Note that returning nil or assigning row=nil to
suppress the triggered operation is in general a bad idea; if you need
to prevent an action, then throw an error instead.

An interface to pg's "numeric" type (decimal arithmetic) is provided;
see the main documentation for details.

Polymorphic and variadic functions are fully supported, including
VARIADIC "any". VARIADIC of non-"any" type is passed as an array as
usual.

Interpreters are shut down on backend exit, meaning that finalizers
will be run for all objects at this time (including user-defined ones).
Currently, SPI functionality is disabled during exit.

AUTHOR
------

Andrew Gierth, aka RhodiumToad

The author acknowledges the work of Luis Carvalho and other contributors
to the original pllua project (of which this is a ground-up redesign).

License: MIT license
