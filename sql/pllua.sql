--

CREATE EXTENSION pllua;
CREATE EXTENSION plluau;

\set VERBOSITY terse

-- smoke test

do language pllua $$ print "hello world!" $$;
do language plluau $$ print "hello world!" $$;

create function pg_temp.f1() returns text language pllua as $$ return "hello world" $$;
select pg_temp.f1();

create function pg_temp.f2() returns text language plluau as $$ return "hello world" $$;
select pg_temp.f2();

-- Rest of this file concentrates on simple tests of code paths in
-- compile, exec, and interpreter setup. Tests of other parts of the
-- module are separate.

-- validator
create function pg_temp."bad name"() returns text language pllua as $$ $$;
create function pg_temp.f3("bad arg" integer) returns text language pllua as $$ $$;

-- simple params and results (see types.sql for detailed checks)

create function pg_temp.f4(a integer) returns integer language pllua as $$ return a + 1 $$;
select pg_temp.f4(1);

create function pg_temp.f5(a text) returns text language pllua as $$ return a.."bar" $$;
select pg_temp.f5('foo');

create function pg_temp.f6(a text, b integer) returns text language pllua as $$ return a..b $$;
select pg_temp.f6('foo',1);

-- try some polymorphism too
create function pg_temp.f7(a anyelement) returns anyelement language pllua as $$ return a $$;
select pg_temp.f7(text 'foo');
select pg_temp.f7(json '{"foo":1}');
--select pg_temp.f7(xml '<foo>bar</foo>'); -- don't bother with this, might be compiled out
select pg_temp.f7(varchar 'foo');
select 'x',pg_temp.f7('foo'::char(20)),'x';
select pg_temp.f7(cstring 'foo');
select pg_temp.f7(name 'foo');
select pg_temp.f7(bytea 'foo\000bar');
select pg_temp.f7(smallint '2');
select pg_temp.f7(integer '2');
select pg_temp.f7(bigint '123456789012345');
select pg_temp.f7(oid '10');
select pg_temp.f7(oid '4294967295');
select pg_temp.f7(true);
select pg_temp.f7(false);
select pg_temp.f7(1.5::float8);
select pg_temp.f7(1.5::float4);

-- variadics

create function pg_temp.f8(a text, variadic b integer[]) returns void language pllua as $$ print(a,type(b),b) $$;
select pg_temp.f8('foo', 1, 2, 3);

create function pg_temp.f9(a integer, variadic b text[]) returns void language pllua as $$ print(a,type(b),b) $$;
select pg_temp.f9(1, 'foo', 'bar', 'baz');

create function pg_temp.f10(a integer, variadic "any") returns void language pllua as $$ print(a,...) $$;
select pg_temp.f10(1, 'foo', 2, 'baz');

-- SRF code paths

create function pg_temp.f11(a integer) returns setof text
  language pllua as $$ return $$;  -- 0 rows
select * from pg_temp.f11(1);

create function pg_temp.f11b(a integer) returns setof text
  language pllua as $$ return 'foo' $$;  -- 1 row
select * from pg_temp.f11b(1);

create function pg_temp.f12(a integer) returns setof text
  language pllua as $$ coroutine.yield() $$;  -- 1 row, null
select * from pg_temp.f12(1);

create function pg_temp.f13(a integer) returns setof text
  language pllua as $$ for i = 1,a do coroutine.yield("row "..i) end $$;
select * from pg_temp.f13(4);

create function pg_temp.f14(a integer, out x text, out y integer) returns setof record
  language pllua as $$ for i = 1,a do coroutine.yield("row "..i, i) end $$;
select * from pg_temp.f14(4);

create function pg_temp.f15(a integer) returns table(x text, y integer)
  language pllua as $$ for i = 1,a do coroutine.yield("row "..i, i) end $$;
select * from pg_temp.f15(4);

create function pg_temp.f16(a inout integer, x out text) returns setof record
  language pllua as $$ for i = 1,a do coroutine.yield(i, "row "..i) end $$;
select * from pg_temp.f16(4);

-- SRF vs null returns

create function pg_temp.f16b(a integer) returns table(x text, y integer)
  language pllua as $$ coroutine.yield() $$;  -- 1 row, null
select * from pg_temp.f16b(1);

create function pg_temp.f16c(a integer) returns table(x text, y integer)
  language pllua as $$ coroutine.yield() for i = 1,a do coroutine.yield('foo',i) end $$;
select * from pg_temp.f16c(3);

-- compiler and validator code paths

do language pllua $$ _G.rdepth = 40 $$;  -- global var hack

-- This function will try and call itself at a point where it is visible
-- but has no definition interned yet; the recursive call will likewise
-- not see an interned definition and recurses again. without any limits
-- this would hit a stack depth check somewhere; we eat about 3 levels of
-- C function recursion inside lua each time, and that gets capped at 200.
-- We don't expect this to be actually useful, the test is just that we
-- don't crash.
create function pg_temp.f17(a integer) returns integer language pllua
  as $$
    return a
  end
  do
    if _G.rdepth > 0 then
      _G.rdepth = _G.rdepth - 1
      u = spi.execute("select pg_temp.f17(1)")
    end
$$;
select pg_temp.f17(1);

create type pg_temp.t1 as (a integer, b text);
create function pg_temp.f18(a integer, b text) returns pg_temp.t1
  language pllua as $$ return a,b $$;
select * from pg_temp.f18(123,'foo');

create function pg_temp.f19(a integer) returns text language pllua as $$ return 'foo '..a $$;
select pg_temp.f19(2);
create or replace function pg_temp.f19(a integer) returns text language pllua as $$ return 'bar '..a $$;
select pg_temp.f19(3);

-- trusted interpreter setup

-- check we really do have different interpreters

-- this is hard because we intentionally isolate trusted-language code
-- from the normal global env of its interpreter, so we would only be
-- able to verify isolation if we were able to break out of the
-- sandbox, which would rather defeat the point. We have to take the
-- outside view, by generating an interpreter-dependent value and
-- checking that it differs. The stringification of a closure, such as
-- server.error, suffices since this contains an interpreter-dependent
-- address (whereas base C functions do not differ between
-- interpreters in recent lua versions).

create function pg_temp.f20() returns text language pllua as $$ return tostring(spi.error) $$;
create function pg_temp.f21() returns text language plluau as $$ return tostring(spi.error) $$;
select pg_temp.f20() as a intersect select pg_temp.f21();  -- should be empty

-- check the global table
do language pllua $$
  local gk = { "io", "dofile", "debug" }  -- must not exist
  for i = 1,#gk do print(gk[i],type(_G[gk[i]])) end
$$;
do language plluau $$
  local gk = { "io", "dofile" }  -- probably exist
  for i = 1,#gk do print(gk[i],type(_G[gk[i]])) end
$$;

-- check that trusted gets only the restricted os module, even from
-- require
do language pllua $$
  local os = require 'os'
  local gk = { "time", "difftime", "execute", "getenv", "exit" }
  for i = 1,#gk do print(gk[i],type(os[gk[i]])) end
$$;

-- check that trusted can't require dangerous core modules
do language pllua $$
  print((lpcall(require,"debug")))
  print((lpcall(require,"io")))
$$;

--end
