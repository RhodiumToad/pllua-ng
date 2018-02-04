--

\set VERBOSITY terse

--

create table xatst (a integer);

do language pllua $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  stmt:execute(1);
  pcall(function() stmt:execute(2) end)
  stmt:execute(3);
$$;

-- should now be two different xids in xatst, and 3 rows
select count(*), count(distinct age(xmin)) from xatst;

truncate table xatst;

do language pllua $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  stmt:execute(1);
  print(pcall(function() stmt:execute(2) error("foo") end))
  stmt:execute(3);
$$;

-- should now be one xid in xatst, and 2 rows
select count(*), count(distinct age(xmin)) from xatst;

truncate table xatst;

do language pllua $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  stmt:execute(1);
  print(pcall(function() stmt:execute(2) server.error("foo") end))
  stmt:execute(3);
$$;

-- should now be one xid in xatst, and 2 rows
select count(*), count(distinct age(xmin)) from xatst;

do language pllua $$
  local function f() for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  print(pcall(f))
$$;

do language pllua $$
  local function f() for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  local function f2() error("foo") end
  print(pcall(f2))
  f()
$$;

do language pllua $$
  local function f(e) print("error",e) for r in spi.rows([[ select * from xatst order by a ]]) do print(r) end end
  local function f2() error("foo") end
  print(xpcall(f2,f))
$$;

truncate table xatst;

do language pllua $$
  local stmt = spi.prepare([[ insert into xatst values ($1) ]]);
  local function f(e) print("error",e) stmt:execute(3) end
  local function f2() stmt:execute(2) error("foo") end
  stmt:execute(1)
  print(xpcall(f2,f))
$$;

-- should now be one xid in xatst, and 2 rows
select count(*), count(distinct age(xmin)) from xatst;

do language pllua $$
  local function f(e) error("bar") end
  local function f2() error("foo") end
  print(xpcall(f2,f))
$$;

-- tricky error-in-error cases:
--
-- pg error inside xpcall handler func needs to abort out to the
-- parent of the xpcall, not the xpcall itself.
begin;
-- we get (harmless) warnings with lua53 but not with luajit for this
-- case. suppress them.
set local client_min_messages = error;
do language pllua $$
  local function f(e) server.error("nested") end
  local function f2() error("foo") end
  print("outer pcall",
        pcall(function()
                print("entering xpcall");
                print("inner xpcall", xpcall(f2,f))
		print("should not be reached")
              end))
$$;
commit;

do language pllua $$
  local level = 0
  local function f(e) level = level + 1 if level==1 then print("in error handler",level,e) server.error("nested") end end
  local function f2() error("foo") end
  print("outer pcall",
        pcall(function()
                print("entering xpcall");
                print("inner xpcall", xpcall(f2,f))
		print("should not be reached")
              end))
$$;


do language pllua $$
  print(lpcall(function() error("caught") end))
$$;

do language pllua $$
  print(lpcall(function() server.error("not caught") end))
$$;

-- make sure PG errors in coroutines are propagated (but not lua errors)

do language pllua $$
  local c = coroutine.create(function() coroutine.yield() error("caught") end)
  print(coroutine.resume(c))
  print(coroutine.resume(c))
$$;

do language pllua $$
  local c = coroutine.create(function() coroutine.yield() server.error("not caught") end)
  print(coroutine.resume(c))
  print(coroutine.resume(c))
$$;

-- error object funcs

do language pllua $$
  local err = require 'pllua.error'
  local r,e = pcall(function() server.error("22003", "foo", "bar", "baz") end)
  print(err.type(e), err.category(e), err.errcode(e))
  print(e.severity, e.category, e.errcode, e.sqlstate, e.message, e.detail, e.hint)
  local r,e = pcall(function() error("foo") end)
  print(err.type(e), err.category(e), err.errcode(e), e)
$$;

--end
