--

create extension hstore_pllua cascade;

\set VERBOSITY terse

-- smoke tests

do language pllua $$
  -- these should work even without the transform
  local hs = pgtype.hstore('"foo"=>"bar", "baz"=>"quux"')
  print(hs)
  local res = (spi.execute([[select pg_typeof($1) as type, $1::text as text, $1->'foo' as foo, $1->'baz' as bar]], hs))[1]
  print(res.type, res.text, res.foo, res.bar)
  -- but these use it:
  res = (spi.execute([[select $1 as hs]], hs))[1]
  print(type(res.hs))
  do
    local ks = {}
    for k,v in pairs(res.hs) do ks[1+#ks] = k end
    table.sort(ks)
    for i,k in ipairs(ks) do print(k,res.hs[k]) end
  end
  local hs2 = pgtype.hstore({ foo = "bar", baz = "quux" })
  res = (spi.execute([[select pg_typeof($1) as t2]], hs2))[1]
  print(res.t2)
  res = (spi.execute([[select $1 = $2 as eq]], hs, hs2))[1]
  print(res.eq)
$$;

--end
