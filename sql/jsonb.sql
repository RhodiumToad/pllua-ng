--

\set VERBOSITY terse

--

do language pllua_ng $$

  a = { json_test = "This is only a test.",
        foo = "If this were real data, it would make more sense.",
	piem = [[
Now I, even I, would celebrate
In rhymes unapt the great
Immortal Syracusan rivaled nevermore,
Who in his wondrous lore,
Passed on before,
Left men his guidance
How to circles mensurate.
]],
        names = { "Dougal", "Florence", "Ermintrude", "Zebedee",
	          "Brian", "Dylan" },
	mixed = { nil, nil, 123, "foo", nil, true, false, [23] = "fred" },
	empty = {},
	empty2 = {{},{}},
	nested = { "arrayelem", { ["object key"] = "object val",
	                          subobject = { subarray = { { { 123 } } } } } }
      }
  b = pgtype.jsonb(a)
  print(b)
  c = pgtype.jsonb(a, { null = false })
  print(c)
  d = pgtype.jsonb(a, { map = function(v) if type(v) == "boolean" then v = tostring(v) end return v end })
  print(d)
  e = pgtype.jsonb(a, { array_thresh = 1 })
  print(e)
  f = pgtype.jsonb(a, { empty_object = true })
  print(f)

  spi.execute([[ create temp table jt1 as select $1 as a ]], b)

$$;

select a, pg_typeof(a) from jt1;

create temp table jt2(id serial, a jsonb);
insert into jt2(a) values ('1');
insert into jt2(a) values ('"foo"');
insert into jt2(a) values ('true');
insert into jt2(a) values ('null');
insert into jt2(a) values ('{"foo":123}');
insert into jt2(a) values ('{"foo":null}');
insert into jt2(a) values ('[10,20,30]');
insert into jt2(a) values ('{"foo":[2,4,6]}');
insert into jt2(a) values ('[{"foo":"bar"},{"baz":"foo"},123,null]');

do language pllua_ng $$
  s = spi.prepare([[ select a from jt2 order by id ]])
  for r in s:rows() do
    print(r.a)
    b = r.a(function(k,v,...)
              if type(v)~="table" then
	        print("mapfunc",k,v,...)
	      else
	        print("mapfunc",k,type(v),...)
	      end
	      return k,v
	    end)
    print(type(b))
    if type(b)=="table" then
      for k,v in pairs(b) do
        if type(v)=="table" then
	  for k2,v2 in pairs(v) do print("result",k,k2,v2) end
	else
	  print("result",k,v)
	end
      end
    end
  end
$$;

create temp table jt3(id integer, a jsonb);
-- first row should be plain, then a couple with compressed values,
-- then a couple with external toast
insert into jt3 select i, ('[' || repeat('"foo",',10*(10^i)::integer) || i || ']')::jsonb from generate_series(1,5) i;
do language pllua_ng $$
  s = spi.prepare([[ select a from jt3 where id = $1 ]])
  for i = 1,5 do
    local r = (s:execute(i))[1]
    local a = r.a()
    print(#a,a[#a])
  end
$$;

--end
