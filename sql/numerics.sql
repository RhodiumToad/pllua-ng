--

\set VERBOSITY terse

-- test numerics

create function lua_numexec(code text, n1 numeric, n2 numeric)
  returns text
  language pllua
  as $$
    local f,e = load("return function(n1,n2) return "..code.." end")
    assert(f,e)
    f = f()
    assert(f)
    return tostring(f(n1,n2))
$$;
create function pg_numexec(code text, n1 numeric, n2 numeric)
  returns text
  language plpgsql
  as $$
    declare
      r text;
    begin
      execute format('select (%s)::text',
      	      	     regexp_replace(regexp_replace(code, '\mnum\.', '', 'g'),
		                    '\mn([0-9])', '$\1', 'g'))
	 into r using n1,n2;
      return r;
    end;
$$;

do language pllua $$ num = require "pllua.numeric" $$;
with
  t as (select code,
               lua_numexec(code, 5439.123456, -1.9) as lua,
               pg_numexec(code, 5439.123456, -1.9) as pg
          from unnest(array[
		$$ n1 + n2 $$,		$$ n1 - n2 $$,
		$$ n1 * n2 $$,		$$ n1 / n2 $$,
		$$ n1 % n2 $$,		$$ n1 ^ n2 $$,
		$$ (-n1) + n2 $$,	$$ (-n1) - n2 $$,
		$$ (-n1) * n2 $$,	$$ (-n1) / n2 $$,
		$$ (-n1) % n2 $$,	$$ (-n1) ^ 3 $$,
		$$ (-n1) + (-n2) $$,	$$ (-n1) - (-n2) $$,
		$$ (-n1) * (-n2) $$,    $$ (-n1) / (-n2) $$,
		$$ (-n1) % (-n2) $$,	$$ (-n1) ^ (-3) $$,
		$$ (n1) > (n2) $$,	$$ (n1) < (n2) $$,
		$$ (n1) >= (n2) $$,	$$ (n1) <= (n2) $$,
		$$ (n1) > (n2*10000) $$,
		$$ (n1) < (n2*10000) $$,
		$$ (n1) >= (n2 * -10000) $$,
		$$ (n1) <= (n2 * -10000) $$,
		$$ num.round(n1) $$,    $$ num.round(n2) $$,
		$$ num.round(n1,4) $$,	$$ num.round(n1,-1) $$,
		$$ num.trunc(n1) $$,	$$ num.trunc(n2) $$,
		$$ num.trunc(n1,4) $$,	$$ num.trunc(n1,-1) $$,
		$$ num.floor(n1) $$,	$$ num.floor(n2) $$,
		$$ num.ceil(n1) $$,	$$ num.ceil(n2) $$,
		$$ num.abs(n1) $$,	$$ num.abs(n2) $$,
		$$ num.sign(n1) $$,	$$ num.sign(n2) $$,
		$$ num.sqrt(n1) $$,
		$$ num.exp(12.3) $$,
		$$ num.exp(n2) $$
  ]) as u(code))
select (lua = pg) as ok, * from t;

-- calculate pi to 40 places

do language pllua $$
  -- Chudnovsky formula; ~14 digits per round, we use 4 rounds
  local num = require 'pllua.numeric'
  local prec = 100  -- precision of intermediate values
  local function fact(n)
    local r = pgtype.numeric(1):round(prec)
    for i = 2,n do r = r * i end
    return r:round(prec)
  end
  local c640320 = pgtype.numeric(640320):round(prec)
  local c13591409 = pgtype.numeric(13591409):round(prec)
  local c545140134 = pgtype.numeric(545140134):round(prec)
  local function chn(k)
    return (fact(6*k) * (c13591409 + (c545140134 * k)))
           / (fact(3*k) * fact(k)^3 * (-c640320)^(3*k))
  end
  local function pi()
    return (1 / ((chn(0) + chn(1) + chn(2) + chn(3))*12 / num.sqrt(c640320^3))):round(40)
  end
  print(pi())
$$;

--
