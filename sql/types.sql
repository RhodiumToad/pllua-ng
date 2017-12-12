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

do language pllua_ng $$ a = pgtype.array.integer({{{1,2}},{{3,4}},{{5,6}}},3,1,2) print(a) $$;
do language pllua_ng $$ print(#a,#(a[1]),#(a[1][1])) $$;
do language pllua_ng $$ print(a[3][1][2],a[1][1][1]) $$;

do language pllua_ng $$ print(pgtype.int4range(123,456)) $$;
do language pllua_ng $$ print(pgtype.int4range()) $$;
do language pllua_ng $$ print(pgtype.int4range(123,456,'(]')) $$;
do language pllua_ng $$ print(pgtype.int4range(nil,456,'(]')) $$;
do language pllua_ng $$ print(pgtype.int4range(nil,nil)) $$;
do language pllua_ng $$ print(pgtype.int4range(123,nil)) $$;

create type myenum as enum ('TRUE', 'FALSE', 'FILE_NOT_FOUND');

create function pg_temp.f1(a myenum) returns text language pllua_ng as $$ print(a,type(a)) return a $$;
select pg_temp.f1(x) from unnest(enum_range(null::myenum)) x;

create function pg_temp.f2() returns myenum language pllua_ng as $$ return 'FILE_NOT_FOUND' $$;
select pg_temp.f2();

--
