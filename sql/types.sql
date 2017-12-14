--

\set VERBOSITY terse

--

create type ctype3 as (fred integer, jim numeric);
do $$
  begin
    if current_setting('server_version_num')::integer >= 110000 then
      execute 'create domain dtype as ctype3 check((VALUE).jim is not null)';
    else
      execute 'create type dtype as (fred integer, jim numeric)';
    end if;
  end;
$$;
create type ctype2 as (thingy text, wotsit integer);
create type ctype as (foo text, bar ctype2, baz dtype);
create table tdata (
    intcol integer,
    textcol text,
    charcol char(32),
    varcharcol varchar(32),
    compcol ctype,
    dcompcol dtype
);

insert into tdata
values (1, 'row 1', 'padded with blanks', 'not padded', ('x',('y',1111),(111,11.1)), (11,1.1)),
       (2, 'row 2', 'padded with blanks', 'not padded', ('x',('y',2222),(222,22.2)), (22,2.2)),
       (3, 'row 3', 'padded with blanks', 'not padded', ('x',('y',3333),(333,33.3)), (33,3.3));

create function tf1() returns setof tdata language pllua_ng as $f$
  for i = 1,4 do
    coroutine.yield({ intcol = i,
                      textcol = "row "..i,
		      charcol = "padded with blanks",
		      varcharcol = "not padded",
		      compcol = { foo = "x",
		      	      	  bar = { thingy = "y", wotsit = i*1111 },
				  baz = { fred = i*111, jim = i*11.1 }
				},
		      dcompcol = { fred = i*11, jim = i*1.1 }
		    })
  end
$f$;

select * from tf1();

--
-- various checks of type handling
--

do language pllua_ng $$ print(pgtype(nil,'ctype3')(1,2)) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')({1,2})) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')(true,true)) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')("1","2")) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')("fred","jim")) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')({fred=1,jim=2})) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')({fred=1,jim={}})) $$;
do language pllua_ng $$ print(pgtype(nil,'ctype3')({fred=1,jim=nil})) $$;
--do language pllua_ng $$ print(pgtype(nil,'dtype')({fred=1,jim=nil})) $$;

create function tf2() returns setof tdata language pllua_ng as $f$
  local t = spi.execute("select * from tdata")
  for i,v in ipairs(t) do coroutine.yield(v) end
$f$;

select * from tf2();

-- ensure detoasting of nested composites works right

do language pllua_ng $f$
  for r in spi.rows("select * from tdata") do
    print(r.intcol, r.compcol.foo, r.compcol.bar.wotsit, r.dcompcol.jim)
  end
$f$;

do language pllua_ng $$ a = pgtype.array.integer({{{1,2}},{{3,4}},{{5,6}}},3,1,2) print(a) $$;
do language pllua_ng $$ print(#a,#(a[1]),#(a[1][1])) $$;
do language pllua_ng $$ print(a[3][1][2],a[1][1][1]) $$;

do language pllua_ng $$ print(pgtype.int4range(123,456)) $$;
do language pllua_ng $$ print(pgtype.int4range()) $$;
do language pllua_ng $$ print(pgtype.int4range(123,456,'(]')) $$;
do language pllua_ng $$ print(pgtype.int4range(nil,456,'(]')) $$;
do language pllua_ng $$ print(pgtype.int4range(nil,nil)) $$;
do language pllua_ng $$ print(pgtype.int4range(123,nil)) $$;
do language pllua_ng $$ print(pgtype.int4range('[12,56]')) $$;

do language pllua_ng $$
  local r1,r2,r3 = pgtype.numrange('[12,56]'),
                   pgtype.numrange('empty'),
                   pgtype.numrange('(12,)')
  print(r1.lower,r1.upper,r1.lower_inc,r1.upper_inc,r1.lower_inf,r1.upper_inf,r1.isempty)
  print(r2.lower,r2.upper,r2.lower_inc,r2.upper_inc,r2.lower_inf,r2.upper_inf,r2.isempty)
  print(r3.lower,r3.upper,r3.lower_inc,r3.upper_inc,r3.lower_inf,r3.upper_inf,r3.isempty)
$$;

create type myenum as enum ('TRUE', 'FALSE', 'FILE_NOT_FOUND');

create function pg_temp.f1(a myenum) returns text language pllua_ng as $$ print(a,type(a)) return a $$;
select pg_temp.f1(x) from unnest(enum_range(null::myenum)) x;

create function pg_temp.f2() returns myenum language pllua_ng as $$ return 'FILE_NOT_FOUND' $$;
select pg_temp.f2();

-- domains

create domain mydom1 as varchar(3);
create domain mydom2 as varchar(3) check (value in ('foo','bar','baz'));
create domain mydom3 as varchar(3) not null;
create domain mydom4 as varchar(3) not null check (value in ('foo','bar','baz'));

create function pg_temp.f3(a mydom1) returns void language pllua_ng as $$
  print(pgtype(nil,1):name(), type(a), #a)
$$;
select pg_temp.f3('foo') union all select pg_temp.f3('bar   ');

create function pg_temp.f4d1(a text) returns mydom1 language pllua_ng as $$
  return a
$$;
select pg_temp.f4d1('foo');
select pg_temp.f4d1('bar   ');
select pg_temp.f4d1('toolong');
select pg_temp.f4d1(null);

create function pg_temp.f4d2(a text) returns mydom2 language pllua_ng as $$
  return a
$$;
select pg_temp.f4d2('bar   ');
select pg_temp.f4d2('bad');
select pg_temp.f4d1('toolong');
select pg_temp.f4d2(null);

create function pg_temp.f4d3(a text) returns mydom3 language pllua_ng as $$
  return a
$$;
select pg_temp.f4d3('bar   ');
select pg_temp.f4d1('toolong');
select pg_temp.f4d3(null);

create function pg_temp.f4d4(a text) returns mydom4 language pllua_ng as $$
  return a
$$;
select pg_temp.f4d3('bar   ');
select pg_temp.f4d2('bad');
select pg_temp.f4d2('toolong');
select pg_temp.f4d3(null);

--
