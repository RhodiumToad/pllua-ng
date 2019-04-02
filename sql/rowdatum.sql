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

--end
