--

\set VERBOSITY terse

--

-- test procedures, non-atomic DO-blocks, and spi.commit/rollback
-- (all pg11 new features)

create table xatst2 (a integer);

create procedure pg_temp.tp1(a text)
  language pllua
  as $$
  print("hello world", a)
  print(spi.is_atomic() and "atomic context" or "non-atomic context")
$$;

call pg_temp.tp1('foo');
begin; call pg_temp.tp1('foo'); commit;

do language pllua $$
  print(spi.is_atomic() and "atomic context" or "non-atomic context")
$$;

begin;
do language pllua $$
  print(spi.is_atomic() and "atomic context" or "non-atomic context")
$$;
commit;

create procedure pg_temp.tp2()
  language pllua
  as $$
  local stmt = spi.prepare([[ insert into xatst2 values ($1) ]]);
  stmt:execute(1);
  spi.commit();
  stmt:execute(2);
  spi.rollback();
  stmt:execute(3);
  spi.commit();
  stmt:execute(4);
$$;

call pg_temp.tp2();

-- should now be three different xids in xatst2, and 3 rows
select count(*), count(distinct age(xmin)) from xatst2;

-- proper handling of open cursors

create procedure pg_temp.tp3()
  language pllua
  as $$
  local stmt = spi.prepare([[ select i from generate_series(1,10) i ]]);
  for r in stmt:rows() do
    print(r.i)
    spi.commit();
  end
$$;

call pg_temp.tp3();

create procedure pg_temp.tp4()
  language pllua
  as $$
  local stmt = spi.prepare([[ select i from generate_series(1,10) i ]], {}, { hold = true });
  for r in stmt:rows() do
    print(r.i)
    spi.commit();
  end
$$;

call pg_temp.tp4();

-- no commit inside subxact

truncate table xatst2;

do language pllua $$
  local stmt = spi.prepare([[ insert into xatst2 values ($1) ]]);
  stmt:execute(1);
  spi.commit();
  stmt:execute(2);
  print(pcall(function() stmt:execute(3) spi.commit() end))
  -- the commit threw a lua error and the subxact was rolled back,
  -- so we should be in the same xact as row 2
  stmt:execute(4);
  spi.commit();
$$;

-- should now be two different xids in xatst2, and 3 rows
select count(*), count(distinct age(xmin)) from xatst2;

--end
