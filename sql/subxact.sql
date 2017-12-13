--

\set VERBOSITY terse

--

create table xatst (a integer);

do language pllua_ng $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  stmt:execute(1);
  pcall(function() stmt:execute(2) end)
  stmt:execute(3);
$$;

-- should now be two different xids in xatst, and 3 rows
select count(*), count(distinct age(xmin)) from xatst;

truncate table xatst;

do language pllua_ng $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  stmt:execute(1);
  print(pcall(function() stmt:execute(2) error("foo") end))
  stmt:execute(3);
$$;

-- should now be one xid in xatst, and 2 rows
select count(*), count(distinct age(xmin)) from xatst;

do language pllua_ng $$
  local function f() for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  print(pcall(f))
$$;

do language pllua_ng $$
  local function f() for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  local function f2() error("foo") end
  print(pcall(f2))
  f()
$$;

do language pllua_ng $$
  local function f(e) print("error",e) for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  local function f2() error("foo") end
  print(xpcall(f2,f))
$$;

truncate table xatst;

do language pllua_ng $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  local function f(e) print("error",e) stmt:execute(3) end
  local function f2() stmt:execute(2) error("foo") end
  stmt:execute(1)
  print(xpcall(f2,f))
$$;

-- should now be one xid in xatst, and 2 rows
select count(*), count(distinct age(xmin)) from xatst;

do language pllua_ng $$
  local function f(e) error("bar") end
  local function f2() error("foo") end
  print(xpcall(f2,f))
$$;

do language pllua_ng $$
  print(lpcall(function() error("caught") end))
$$;

do language pllua_ng $$
  print(lpcall(function() server.error("not caught") end))
$$;

--end
