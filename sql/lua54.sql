--

\set VERBOSITY terse

--

-- 5.4-specific features.

-- simple close metamethod

do language pllua $$
  local tc <close>
    = setmetatable({}, { __close = function() print("close called") end })
  spi.error("error")
$$;

-- close metamethod on cursor
begin;
do language pllua $$
  for r in spi.rows([[ select * from generate_series(1,5) i ]]) do
    print(r)
    if r.i > 2 then break end
  end
$$;
select * from pg_cursors;  -- should be empty
commit;

-- lua error in close method
do language pllua $$
  local tc <close>
    = setmetatable({}, { __close = function() error("inner error") end })
  error("outer error")
$$;

-- db access in close method with outer lua error
do language pllua $$
  local tc <close>
    = setmetatable({}, { __close = function() print(pgtype.numeric(0)) end })
  error("outer error")
$$;

-- db access in close method with outer db error
do language pllua $$
  local tc <close>
    = setmetatable({}, { __close = function() print(pgtype.numeric(0)) end })
  spi.error("outer error")
$$;

-- close metamethod in SRF
create function pg_temp.sf1(n integer) returns setof integer
  language pllua
  as $$
    local x <close>
        = setmetatable({}, { __close = function() print("close called") end })
    for i = n, n+3 do
        coroutine.yield(i)
    end
$$;
select * from (values (1),(2)) v(n), lateral (select * from pg_temp.sf1(v.n) limit 1) s;

-- error in close metamethod in SRF
create function pg_temp.sf2(n integer) returns setof integer
  language pllua
  as $$
    local x <close>
        = setmetatable({}, { __close = function() error("inner error") end })
    for i = n, n+3 do
        coroutine.yield(i)
    end
$$;
select * from (values (1),(2)) v(n), lateral (select * from pg_temp.sf2(v.n) limit 1) s;

--end
