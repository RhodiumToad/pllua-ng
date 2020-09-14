--

\set VERBOSITY terse

--

-- tests of operations on nested row values

create type ntype1 as (fred integer, jim numeric);
create type ntype2 as (thingy text[], wotsit ntype1);
create type ntype3 as (foo text, bar ntype1, baz ntype2);
create type ntype4 as (col0 text, col1 ntype1, col2 ntype2, col3 ntype3);

--
do language pllua $$
  local r = pgtype.ntype1(1,2)
  print(r.fred, r.jim)  -- deform
  r.fred = 3            -- explode after deform
  print(r.fred, r.jim)
  r.jim = 4             -- modify pre-exploded value
  print(pgtype.ntype1(r), r.fred, r.jim)
  r = pgtype.ntype1(1,2)
  r.fred = 3            -- explode before deform
  print(r.fred, r.jim)
  r.jim = 4             -- modify pre-exploded value
  print(pgtype.ntype1(r), r.fred, r.jim)
$$;

do language pllua $$
  local r0 = pgtype.ntype4("zzz",
       	                   { fred = 1, jim = 2 },
  	    		   { thingy = {"a","b","c"},
			     wotsit = { fred = 3, jim = 4}
			   },
			   { foo = "abcde",
			     bar = { fred = 5, jim = 6 },
			     baz = { thingy = {"x","y","z"},
			             wotsit = { fred = 7, jim = 8 } }
			   })
  local r = pgtype.ntype4(r0)
  -- deform from inner to outer
  print(r.col3.baz.wotsit.fred)
  print(r.col3.foo)
  print(r.col1.fred)
  -- and from outer to inner
  print(r.col2.wotsit.jim)
  print(r.col3.bar.jim)
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  --deform from outer to inner
  print(r.col1, r.col2, r.col3)
  print(r.col1.jim, r.col2.thingy[3], r.col3.foo)
  print(r.col2.wotsit.jim, r.col3.bar.jim, r.col3.baz.thingy[3])
  print(r.col3.baz.wotsit.jim)
  print(r) print(pgtype.ntype4(r))
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  -- explode from inner
  r.col3.baz.wotsit.jim = 10
  r.col2.thingy[2] = "k"
  print(r) print(pgtype.ntype4(r))
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  -- explode from middle
  r.col3.foo = "edcba"
  r.col1.fred = -1
  r.col3.baz.wotsit.jim = 80
  r.col3.baz.wotsit.fred = 0
  print(r) print(pgtype.ntype4(r))
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  -- explode from top
  r.col0 = "yyy"
  r.col3.baz.thingy[1] = "@"
  r.col1.jim = 20
  print(r) print(pgtype.ntype4(r))
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  -- partially deform then explode from a deformed element
  print(r.col0, r.col2.wotsit.fred)
  r.col2.wotsit.jim = 40
  r.col1.jim = 0
  r.col3.bar.jim = 0
  print(r) print(pgtype.ntype4(r))
  -- start over with un-deformed datum
  r = pgtype.ntype4(r0)
  -- partially deform then explode from an undeformed element
  print(r.col0, r.col2.wotsit.fred)
  r.col3.bar.jim = 0
  r.col3.bar.fred = -1
  r.col2.wotsit.fred = 0
  r.col0 = "yyy"
  r.col1 = { fred = 100, jim = 200 }
  print(r) print(pgtype.ntype4(r))
$$;

-- various dropped-column scenarios

create table ntab1 (fred integer, sheila text, jim numeric);
create table ntab2 (thingy text[], wotsit ntab1);
create table ntab3 (foo text, bar ntab1, baz ntab2);
create table ntab4 (col0 text, col1 ntab1, col2 ntab2, col3 ntab3);

insert into ntab1 values (1, 'foo', 1.23);
insert into ntab2 select array['x','y'], t from ntab1 t;
insert into ntab3 select 'wot', t1, t2 from ntab1 t1, ntab2 t2;
insert into ntab4 select 'rabbit', t1, t2, t3 from ntab1 t1, ntab2 t2, ntab3 t3;

alter table ntab1 drop column sheila;

do language pllua $$
  local r = pgtype.ntab1(1,2)
  print(r.fred, r.jim)  -- deform
  r.fred = 3            -- explode after deform
  print(r.fred, r.jim)
  r.jim = 4             -- modify pre-exploded value
  print(pgtype.ntab1(r), r.fred, r.jim)
  r = pgtype.ntab1(1,2)
  r.fred = 3            -- explode before deform
  print(r.fred, r.jim)
  r.jim = 4             -- modify pre-exploded value
  print(pgtype.ntab1(r), r.fred, r.jim)
$$;

do language pllua $$
  local tbl = spi.execute([[ select t from ntab4 t, generate_series(1,8) ]])
  local r = tbl[1].t
  -- deform from inner to outer
  print(r.col3.baz.wotsit.fred)
  print(r.col3.foo)
  print(r.col1.fred)
  -- and from outer to inner
  print(r.col2.wotsit.jim)
  print(r.col3.bar.jim)
  -- start over with un-deformed datum
  r = tbl[2].t
  --deform from outer to inner
  print(r.col1, r.col2, r.col3)
  print(r.col1.jim, r.col2.thingy[3], r.col3.foo)
  print(r.col2.wotsit.jim, r.col3.bar.jim, r.col3.baz.thingy[3])
  print(r.col3.baz.wotsit.jim)
  print(r) print(pgtype.ntab4(r))
  -- start over with un-deformed datum
  r = tbl[3].t
  -- explode from inner
  r.col3.baz.wotsit.jim = 10
  r.col2.thingy[2] = "k"
  print(r) print(pgtype.ntab4(r))
  -- start over with un-deformed datum
  r = tbl[4].t
  -- explode from middle
  r.col3.foo = "edcba"
  r.col1.fred = -1
  r.col3.baz.wotsit.jim = 80
  r.col3.baz.wotsit.fred = 0
  print(r) print(pgtype.ntab4(r))
  -- start over with un-deformed datum
  r = tbl[5].t
  -- explode from top
  r.col0 = "yyy"
  r.col3.baz.thingy[1] = "@"
  r.col1.jim = 20
  print(r) print(pgtype.ntab4(r))
  -- start over with un-deformed datum
  r = tbl[6].t
  -- partially deform then explode from a deformed element
  print(r.col0, r.col2.wotsit.fred)
  r.col2.wotsit.jim = 40
  r.col1.jim = 0
  r.col3.bar.jim = 0
  print(r) print(pgtype.ntab4(r))
  -- start over with un-deformed datum
  r = tbl[7].t
  -- partially deform then explode from an undeformed element
  print(r.col0, r.col2.wotsit.fred)
  r.col3.bar.jim = 0
  r.col3.bar.fred = -1
  r.col2.wotsit.fred = 0
  r.col0 = "yyy"
  r.col1 = { fred = 100, jim = 200 }
  print(r) print(pgtype.ntab4(r))
$$;

--end
