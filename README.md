
pllua_ng
========

Embeds Lua into PostgreSQL as a procedural language module.

This code is still under development and probably contains bugs and
missing functionality. However, most of the basics now work.

WARNING: interfaces are not stable and are subject to change.

Currently it should build against pg 9.6 and pg10 (and 11devel). It
is known that this module will never work on pg versions before 9.5
(we rely critically on memory context callbacks, which were introduced
in that version).

Only Lua 5.3 is supported at this time.


CHANGES
-------

Some names and locations have been changed.

The old pllua.init table is gone. Instead we support three init
strings (superuser-only): pllua_ng.on_init, pllua_ng.on_trusted_init,
pllua_ng.on_untrusted_init.

SPI functionality is now in global table spi and has different calling
conventions:

      spi.execute("query text", arg, arg, ...)
      spi.prepare("query text", {argtypes}, [{options}])
        - returns a statement object:
          s:execute(arg, arg, ...)  - returns a result table
          s:rows(arg, arg, ...) - returns iterator
      spi.rows("query text", args...)
        - returns iterator

Execution now returns a table with no number keys (#t == 0) in the
event of no matching rows, whereas the old version returned nil. The
result is also currently a plain table, not an object.

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
If a cursor is closed by external code (or transaction end), then the
:isopen() state will be automatically updated (this happens when the
portal is actually dropped). Cursor options are set on the statement
object.

:save on a statement is now a no-op - all statements seen by lua code
have been passed through SPI_keepplan and are managed by Lua garbage
collection. (It was never safe to do otherwise.)

(SPI interface is particularly subject to change - in particular to
something more compatible with client-side database APIs)

print() is still a global function to print an informational message,
but other error levels such as debug, notice are installed as
server.debug(), server.warning() etc.  server.elog('error', ...)
is equivalent to server.error(...) and so on.

server.error() and friends can take optional args:

      server.error('message')
      server.error('sqlstate', 'message')
      server.error('sqlstate', 'message', 'detail')
      server.error('sqlstate', 'message', 'detail', 'hint')
      server.error({ sqlstate = ?,
                     message = ?,
                     detail = ?,
                     hint = ?,
                     table = ?,
                     column = ?, ...})

(I'd like to deprecate the server.* namespace but I don't have a good
alternative place to put these functions. Suggestions welcome.)

Sqlstates can be given either as 5-character strings or as the string
names used in plpgsql: server.error('invalid_argument', 'message')

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

Function arguments are converted to simple Lua values in the case of:

 + integers, floats  -- passed as Lua numbers

 + text, varchar, char, json (not jsonb), xml, cstring, name -- all passed
   as strings (with the padding preserved in the case of char(n))

 + bytea  -- passed as a string without any escaping or conversion

 + boolean  -- passed as boolean
 
 + nulls of any type  -- passed as nil

Other values are kept as datum objects.

The trusted language is implemented differently - rather than removing
functions and packages, the trusted language evaluates all
user-supplied code (everything but the init strings) in a separate
environment table which contains only whitelisted content. A mini
version of the package library is installed in the sandbox
environment, allowing package.preload and package.searchers to work
(the user can install their own function into package.searchers to
load modules from database queries if they so wish).

pllua_ng.on_trusted_init is run in trusted interpreters in the global
env (not the sandbox env). It can do:

      trusted.allow('module' [,'newname'])
        -- requires 'module', then sets up the sandbox so that lua code
           can do  require 'newname'  and get access to the module
      trusted.require('module' [,'newname'])
        -- as above, but also does sandbox.newname = module
      trusted.remove('newname')
        -- undoes either of the above (probably not very useful, but you
           could do  trusted.remove('os')  or whatever)

The trusted environment's version of "load" overrides the text/binary
mode field (loading binary functions is unsafe) and overrides the
environment to be the trusted sandbox if the caller didn't provide one
itself (but the caller can still give an explicit environment of nil).

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

An interface to pg's "numeric" type (decimal arithmetic) is provided.
Datums of numeric type can be used with Lua arithmetic operators, with
the other argument being converted to numeric if necessary. Supported
functions are available as method calls on a numeric datum or in the
package 'pllua.numeric' (which can be require'd normally):

 *  abs ceil equal exp floor isnan sign sqrt
 *  tointeger (returns nil if not representable as a Lua integer)
 *  tonumber  (returns a Lua number, not exact)
 *  log       (with optional base, defaults to natural log)
 *  trunc round  (with optional number of digits)

Values can be constructed with pgtype.numeric(blah) or, if you
require'd the pllua.numeric package, with the .new function.

NOTE: PG semantics, not Lua semantics, are used for the // and %
operators on numerics (pg uses truncate-to-zero and sign-of-dividend
rules, vs. Lua's truncate-to-minus-infinity and sign-of-divisor rules).
Also beware that == does not work to compare a numeric datum against
another number (this is a limitation of Lua), so use the :equal method
for such cases. (Other comparisons work, though note that PG semantics
are used for NaN.)

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
