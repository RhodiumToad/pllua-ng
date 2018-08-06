--

\set VERBOSITY terse

--

--
-- By having a real table with both short and long array values,
-- we get to involve both short varlenas and external toast.
--
create temp table adata (id serial,
       	    	  	 a integer[],
			 b text[],
			 c numeric[],
			 d date[]);
insert into adata(a) values
  (array[1,2]),
  (array[10,20,30,40,50]),
  (array(select i from generate_series(1,100) i)),
  (array(select i from generate_series(1,100000) i)),
  ('{}');
insert into adata(b) values
  ('{}'),
  (array['foo','bar','baz']),
  (array(select 'val'||i from generate_series(1,100) i)),
  (array(select 'val'||i from generate_series(1,10000) i));
insert into adata(c) values
  ('{}'),
  (array[1.234,exp(1.0::numeric(32,30)),27!]),
  (array(select i from generate_series(1,100) i)),
  (array(select i from generate_series(1,10000) i));
insert into adata(d) values
  ('{}'),
  (array[date '2017-12-11', '1968-05-10', '1983-09-26', '1962-10-27']),
  (array(select date '2017-01-01' + i from generate_series(0,364,18) i));

do language pllua $$
package.preload['myutil'] = function()
  local expect_next =
    { string = function(s)
                 return string.gsub(s, "%d+$",
                                    function(n)
			              return string.format("%d", n + 1)
				    end)
	       end,
      number = function(n) return n + 1 end,
      [pgtype.numeric] = function(n) return n + 1 end,
    }
  local function map(a,f)
    local r = {}
    for i = 1,#a do r[#r+1] = f(a[i]) end
    return r
  end
  local function summarize(a)
    if a == nil then return nil end
    local expect,first_match,result = nil,nil,{}
    for i = 1,#a do
      if first_match == nil then
        expect,first_match = a[i],i
      elseif a[i] ~= expect then
        if first_match < i-1 then
          result[#result+1] = { a[first_match], a[i-1] }
        else
          result[#result+1] = a[i-1]
        end
        expect,first_match = a[i],i
      end
      --[[ update the "expected" next element ]]
      expect = (expect_next[pgtype(expect) or type(expect)]
                or function(x) return x end)(expect)
    end
    if first_match == #a then
      result[#result+1] = a[#a]
    elseif first_match ~= nil then
      result[#result+1] = { a[first_match], a[#a] }
    end
    return table.concat(map(result, function(e)
                                      if type(e)=='table' then
				        return string.format("[%s..%s]",
				                             tostring(e[1]),
							     tostring(e[2]))
				      else
				        return tostring(e)
				      end
			            end),
			',')
  end
  return {
    map = map,
    summarize = summarize
  }
end
$$;

do language pllua $$
local u = require 'myutil'
for r in spi.rows([[ select a, b,
                            array_append(a, -1) as xa,
		            array_append(b, 'wombat') as xb
		       from adata
		      where a is not null or b is not null
	  	      order by id ]])
do
  print(u.summarize(r.a),u.summarize(r.b))
  print(u.summarize(r.xa),u.summarize(r.xb))
end
for r in spi.rows([[ select c,
                            array_append(c, -1.0) as xc
		       from adata
		      where c is not null
	  	      order by id ]])
do
  print(u.summarize(r.c))
  print(u.summarize(r.xc))
end
for r in spi.rows([[ select d
		       from adata
		      where d is not null
	  	      order by id ]])
do
  print(r.d)
end
$$;

create function af1(a anyarray)
  returns text
  language pllua
  stable
  as $$
    return tostring(u.summarize(a))
  end
    u = require 'myutil'
  do
$$;

-- array_append returns its result as an expanded datum

select af1(a) from adata where a is not null order by id;
with t as (select a from adata where a is not null order by id)
  select af1(array_append(a, -1)) from t;

select af1(b) from adata where b is not null order by id;
with t as (select b from adata where b is not null order by id)
  select af1(array_append(b, 'wombat')) from t;

select af1(c) from adata where c is not null order by id;
with t as (select c from adata where c is not null order by id)
  select af1(array_append(c, -1.0)) from t;

select af1(d) from adata where d is not null order by id;
with t as (select d from adata where d is not null order by id)
  select af1(array_append(d, date '1970-01-01')) from t;

-- conversion edge cases

create function pg_temp.af2() returns integer[] language pllua
  as $$
    return nil
$$;
select pg_temp.af2();

create function pg_temp.af3() returns integer[] language pllua
  as $$
    return
$$;
select pg_temp.af3();

create function pg_temp.af4() returns integer[] language pllua
  as $$
    return 1,2
$$;
select pg_temp.af4();

create function pg_temp.af5() returns integer[] language pllua
  as $$
    return pgtype(nil,0)()
$$;
select pg_temp.af5();

create function pg_temp.af5b() returns integer[] language pllua
  as $$
    return {}
$$;
select pg_temp.af5b();

create function pg_temp.af6() returns integer[] language pllua
  as $$
    return { 1, nil, 3 }
$$;
select pg_temp.af6();

create function pg_temp.af7() returns integer[] language pllua
  as $$
    return pgtype.integer(1)
$$;
select pg_temp.af7();

create function pg_temp.af8() returns integer[] language pllua
  as $$
    return { pgtype.integer(1) }
$$;
select pg_temp.af8();

create type acomp1 as (foo integer, bar text);

create function pg_temp.af9() returns acomp1[] language pllua
  as $$
    return { { foo = 1, bar = "zot" } }
$$;
select pg_temp.af9();

create function pg_temp.af10() returns acomp1[] language pllua
  as $$
    return { pgtype(nil,0):element()(1, "zot") }
$$;
select pg_temp.af10();

--
