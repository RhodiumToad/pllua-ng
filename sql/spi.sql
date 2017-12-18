--

\set VERBOSITY terse

--

-- Test of SPI-related functionality.

create temp table tsttab (
  id integer primary key,
  a integer,
  b text,
  c numeric,
  d date
);
insert into tsttab(id, a,b,c,d)
  values (1, 1,'foo',2.34,'2017-01-01'),
  	 (2, 2,'bar',2.34,'2017-02-01'),
  	 (3, 3,'baz',2.34,'2017-03-01'),
	 (4, 4, 'fred',2.34,'2017-04-01'),
	 (5, 5,'jim',2.34,'2017-05-01'),
	 (6, 6,'sheila',2.34,'2017-06-01');

-- basics

do language pllua $$
  local tbl
  tbl = spi.execute([[ select 1 as a, 'foo'::text as b ]])
  print(#tbl,tbl[1],type(tbl[1]))
  print(tbl[1].a,tbl[1].b)
  tbl = spi.execute([[ select i, 'foo'::text as b from generate_series(1,10000) i ]])
  print(#tbl,tbl[1],tbl[10000])
  tbl = spi.execute([[ select * from tsttab order by id ]])
  for i = 1,#tbl do print(tbl[i]) end
$$;

-- statements

do language pllua $$
  local stmt,tbl
  stmt = spi.prepare([[ select * from tsttab where id=$1 ]], {"integer"})
  tbl = stmt:execute(1)
  print(tbl[1])
  tbl = stmt:execute(6)
  print(tbl[1])
  stmt = spi.prepare([[ select * from tsttab where id = ANY ($1) order by id ]],
                     {pgtype.array.integer})
  tbl = stmt:execute(pgtype.array.integer(1,nil,3))
  print(#tbl,tbl[1],tbl[2])
  -- type deduction:
  stmt = spi.prepare([[ select 1 + $1 as a, pg_typeof($1) ]])
  tbl = stmt:execute(1)
  print(#tbl,tbl[1])
$$;

-- iterators

do language pllua $$
  for r in spi.rows([[ select * from tsttab order by id ]]) do
    print(r)
  end
  stmt = spi.prepare([[ select * from tsttab where id = ANY ($1) ]],
                     {pgtype.array.integer})
  for r in stmt:rows(pgtype.array.integer(1,nil,3)) do
    print(r)
  end
$$;

-- cursors

begin;
declare foo scroll cursor for select * from tsttab order by id;
do language pllua $$
  local c = spi.findcursor("foo")
  local tbl
  tbl = c:fetch(1,'next')
  print(#tbl,tbl[1])
  tbl = c:fetch(2,'forward')  -- same as 'next'
  print(#tbl,tbl[1],tbl[2])
  tbl = c:fetch(1,'absolute')
  print(#tbl,tbl[1])
  tbl = c:fetch(4,'relative')
  print(#tbl,tbl[1])
  tbl = c:fetch(1,'prior')    -- same as 'backward'
  print(#tbl,tbl[1])
  tbl = c:fetch(1,'backward')
  print(#tbl,tbl[1])
  print(c:isopen())
  spi.execute("close foo")
  print(c:isopen())
$$;
commit;

do language pllua $$
  local c = spi.newcursor("bar")
  c:open([[ select * from tsttab where id >= $1 order by id ]], 3)
  local tbl
  tbl = c:fetch(1,'next')
  print(#tbl,tbl[1])
  for k,v in ipairs(spi.execute([[ select name, statement from pg_cursors ]])) do
    print(v.name, v.statement)
  end
  c:close()
  for k,v in ipairs(spi.execute([[ select name, statement from pg_cursors ]])) do
    print(v.name, v.statement)
  end
  c:open([[ select * from tsttab where id < $1 order by id desc ]], 3)
  tbl = c:fetch(3,'next')
  print(#tbl,tbl[1],tbl[2])
  for k,v in ipairs(spi.execute([[ select name, statement from pg_cursors ]])) do
    print(v.name, v.statement)
  end
  c:close()
$$;

-- cursor options on statement
do language pllua $$
  local stmt = spi.prepare([[ select * from tsttab where id >= $1 order by id ]],
                           {"integer"}, { scroll = true, fast_start = true, generic_plan = true })
  local stmt2 = spi.prepare([[ select * from tsttab where id >= $1 order by id ]],
                            {"integer"}, { no_scroll = true })
  local c = stmt:getcursor(4)
  local tbl
  tbl = c:fetch(3,'next')
  print(#tbl,tbl[1],tbl[2])
  c:move(0,'absolute')
  tbl = c:fetch(3,'next')
  print(#tbl,tbl[1],tbl[2])
  for k,v in ipairs(spi.execute([[ select name, statement, is_scrollable from pg_cursors ]])) do
    print(v.name, v.statement, v.is_scrollable)
  end
  c:close()
  c = stmt2:getcursor(4)
  local c2 = spi.findcursor(c:name())
  print(c:name(), rawequal(c,c2))
  for k,v in ipairs(spi.execute([[ select name, statement, is_scrollable from pg_cursors ]])) do
    print(v.name, v.statement, v.is_scrollable)
  end
  c:close()
$$;

-- check missing params are OK
do language pllua $$
  local stmt = spi.prepare([[ select * from generate_series($1::integer, $3) i ]]);
  local argtypes = stmt:argtypes()
  print(argtypes[1]:name())
  print(type(argtypes[2]))
  print(argtypes[3]:name())
$$;

-- check execute_count
do language pllua $$
  local q = [[ select * from generate_series($1::integer,$2) i ]]
  local r1 = spi.execute_count(q, 2, 1, 5)
  print(#r1)
  local s = spi.prepare(q, {"integer","integer"})
  r1 = s:execute_count(3,1,5)
  print(#r1)
$$;

--end
