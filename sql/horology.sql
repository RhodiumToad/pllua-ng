--

\set VERBOSITY terse

--

-- 1. Input of datetime values from table form.

set timezone = 'GMT';
set datestyle = 'ISO,YMD';

do language pllua $$
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=2419, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=1919, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=1819, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=1019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=-2019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=-4713, month=12, day=1, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34, usec=123456 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, msec=1, usec=1 });
$$;
do language pllua $$
  print(pgtype.timestamptz{ epoch=0 });
  print(pgtype.timestamptz{ epoch=1555891200 });
  print(pgtype.timestamptz{ epoch=1555891200.000001 });
  print(pgtype.timestamptz{ epoch_msec=1555891200001 });
  print(pgtype.timestamptz{ epoch_usec=1555891200000001 });
  print(pgtype.timestamptz{ epoch=1555891200, msec=1, usec=1 });
  print(pgtype.timestamptz{ epoch=1555891200, msec=-1, usec=1 });
  print(pgtype.timestamptz{ epoch=-1555891200 });
  print(pgtype.timestamptz{ epoch=-1555891200, msec=1, usec=-1 });
  print(pgtype.timestamptz{ epoch=-1555891200, msec=-1, usec=1 });
  print(pgtype.timestamptz{ epoch_msec=-1555891200001 });
  print(pgtype.timestamptz{ epoch_usec=-1555891200000001 });
$$;
do language pllua $$
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone=10800 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone=-10800 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="+0300" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="-0300" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="+1400" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="-1400" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="America/Los_Angeles" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="Pacific/Auckland" });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, timezone="Asia/Kathmandu" });
  print(pgtype.timestamptz{ year=2018, month=10, day=28, hour=1, min=30, sec=0, timezone="Europe/London" });
  print(pgtype.timestamptz{ year=2018, month=10, day=28, hour=1, min=30, sec=0, isdst=true, timezone="Europe/London" });
  print(pgtype.timestamptz{ year=2018, month=10, day=28, hour=1, min=30, sec=0, isdst=false, timezone="Europe/London" });
$$;
do language pllua $$
  print(pgtype.timestamptz{ epoch=1/0 });
  print(pgtype.timestamptz{ epoch=-1/0 });
$$;

do language pllua $$
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=1, usec=59000000 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=-1 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=-60 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=-61 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=60 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=61 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=23, sec=120 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=0, sec=3661 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=0, sec=43201 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=0, sec=-43201 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=0, sec=864000 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=12, min=720, sec=3661 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=24, min=0, sec=0 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=24, min=0, sec=1 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=240, min=0, sec=1 });
  print(pgtype.timestamptz{ year=2019, month=4, day=22, hour=-1, min=0, sec=1 });
  print(pgtype.timestamptz{ year=2019, month=-4, day=22, hour=12, min=23, sec=34 });
  print(pgtype.timestamptz{ year=2019, month=16, day=22, hour=12, min=23, sec=34 });
$$;

do language pllua $$
  print(pgtype.timestamp{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=2419, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=1919, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=1819, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=1019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=-2019, month=4, day=22, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=-4713, month=12, day=1, hour=12, min=23, sec=34.1 });
  print(pgtype.timestamp{ year=2019, month=4, day=22, hour=12, min=23, sec=34, usec=123456 });
  print(pgtype.timestamp{ year=2019, month=4, day=22, hour=12, min=23, sec=34.1, msec=1, usec=1 });
$$;
do language pllua $$
  print(pgtype.timestamp{ epoch=0 });
  print(pgtype.timestamp{ epoch=1555891200 });
  print(pgtype.timestamp{ epoch=1555891200.000001 });
  print(pgtype.timestamp{ epoch_msec=1555891200001 });
  print(pgtype.timestamp{ epoch_usec=1555891200000001 });
  print(pgtype.timestamp{ epoch=1555891200, msec=1, usec=1 });
  print(pgtype.timestamp{ epoch=1555891200, msec=-1, usec=1 });
  print(pgtype.timestamp{ epoch=-1555891200 });
  print(pgtype.timestamp{ epoch=-1555891200, msec=1, usec=-1 });
  print(pgtype.timestamp{ epoch=-1555891200, msec=-1, usec=1 });
  print(pgtype.timestamp{ epoch_msec=-1555891200001 });
  print(pgtype.timestamp{ epoch_usec=-1555891200000001 });
  print(pgtype.timestamp{ epoch=1555891200, timezone="America/Los_Angeles" });
  print(pgtype.timestamp{ epoch=1555891200, timezone="Pacific/Auckland" });
  print(pgtype.timestamp{ epoch=1555891200, timezone="Asia/Kathmandu" });
$$;

do language pllua $$
  print(pgtype.date{ year=2019, month=4, day=22 });
  print(pgtype.date{ year=2419, month=2, day=22 });
  print(pgtype.date{ year=1919, month=1, day=22 });
  print(pgtype.date{ year=1819, month=3, day=22 });
  print(pgtype.date{ year=1019, month=5, day=22 });
  print(pgtype.date{ year=-2019, month=4, day=22 });
  print(pgtype.date{ year=-4713, month=12, day=1 });
  print(pgtype.date{ year=2019, month=4, day=22, hour=24 });
  print(pgtype.date{ year=2019, month=4, day=22, hour=24, min=1 });
$$;
do language pllua $$
  print(pgtype.date{ epoch=0 });
  print(pgtype.date{ epoch=1555891200 });
  print(pgtype.date{ epoch=1555891200.000001 });
  print(pgtype.date{ epoch_msec=1555891200001 });
  print(pgtype.date{ epoch_usec=1555891200000001 });
  print(pgtype.date{ epoch=1555891200 });
  print(pgtype.date{ epoch=-1555891200 });
  print(pgtype.date{ epoch_msec=-1555891200001 });
  print(pgtype.date{ epoch_usec=-1555891200000001 });
  print(pgtype.date{ epoch=1555891200, timezone="America/Los_Angeles" });
  print(pgtype.date{ epoch=1555891200, timezone="Pacific/Auckland" });
  print(pgtype.date{ epoch=1555891200, timezone="Asia/Kathmandu" });
$$;

do language pllua $$
  print(pgtype.time{ hour=12, min=23, sec=34.1 });
  print(pgtype.time{ hour=12, min=120, sec=1 });
  print(pgtype.time{ hour=24, min=23, sec=34.1 });
  print(pgtype.time{ hour=25, min=23, sec=34.1 });
  print(pgtype.time{ hour=12, min=23, sec=34, usec=123456 });
  print(pgtype.time{ hour=12, min=23, sec=34.1, msec=1, usec=1 });
$$;
do language pllua $$
  print(pgtype.time{ epoch=0 });
  print(pgtype.time{ epoch=3601 });
  print(pgtype.time{ epoch=86400 });
  print(pgtype.time{ epoch=1555891200 });
$$;

do language pllua $$
  print(pgtype.timetz{ hour=12, min=23, sec=34.1, timezone=7200 });
$$;
do language pllua $$
  print(pgtype.timetz{ epoch=0, timezone=3600 });
  print(pgtype.timetz{ epoch=3601, timezone=-3600 });
  print(pgtype.timetz{ epoch=86400, timezone=-43200 });
  print(pgtype.timetz{ epoch=1555891200, timezone=0 });
$$;

do language pllua $$
  print(pgtype.interval{ year=0, month=0, day=0, hour=0, min=0, sec=0, usec=0 });
  print(pgtype.interval{ year=100 });
  print(pgtype.interval{ month=100 });
  print(pgtype.interval{ day=100 });
  print(pgtype.interval{ hour=100 });
  print(pgtype.interval{ min=100 });
  print(pgtype.interval{ sec=100 });
  print(pgtype.interval{ usec=1 });
  print(pgtype.interval{ year=1, month=2, day=3, hour=4, min=5, sec=6, usec=7 });
$$;
do language pllua $$
  print(pgtype.interval{ epoch=0 });
  print(pgtype.interval{ epoch=120 });
  print(pgtype.interval{ epoch=43200 });
  print(pgtype.interval{ epoch=86400 });
$$;

-- input error cases.

do language pllua $$ print(pgtype.interval{ hour="foo" }); $$;
do language pllua $$ print(pgtype.interval{ hour=1.2 }); $$;
do language pllua $$ print(pgtype.interval{ hour=0/0 }); $$;
do language pllua $$ print(pgtype.interval{ hour=1/0 }); $$;
do language pllua $$ print(pgtype.interval{ sec=1/0 }); $$;
do language pllua $$ print(pgtype.interval{ usec=1/0 }); $$;

do language pllua $$ print(pgtype.timestamptz{ year=1/0, month=1, day=-1/0 }); $$;

do language pllua $$ print(pgtype.timestamp{ epoch=0, timezone="1234" }); $$;
do language pllua $$ print(pgtype.timestamp{ epoch=0, timezone=1234.5 }); $$;
do language pllua $$ print(pgtype.timestamp{ epoch=0, timezone=function() end }); $$;

do language pllua $$ print(pgtype.timestamptz{ epoch=0, epoch_msec=0 }); $$;
do language pllua $$ print(pgtype.timestamptz{ epoch=0, year=2019 }); $$;
do language pllua $$ print(pgtype.timestamptz{ epoch=0, hour=20 }); $$;

do language pllua $$ print(pgtype.timestamptz{ year=2019 }); $$;
do language pllua $$ print(pgtype.timestamptz{ year=2019, month=4 }); $$;
do language pllua $$ print(pgtype.time{ min=10 }); $$;
do language pllua $$ print(pgtype.time{ hour=20, sec=10 }); $$;

do language pllua $$ print(pgtype.timetz{ hour=20, timezone="America/Los_Angeles" }); $$;
do language pllua $$ print(pgtype.time{ hour=20, timezone="America/Los_Angeles" }); $$;
do language pllua $$ print(pgtype.date{ year=2019, month=4, day=22, timezone="America/Los_Angeles" }); $$;
do language pllua $$ print(pgtype.timestamp{ year=2019, month=4, day=22, hour=20, timezone="America/Los_Angeles" }); $$;

do language pllua $$ print(pgtype.timestamptz{ epoch=-300000000000 }); $$;
do language pllua $$ print(pgtype.timestamptz{ year=-5000, month=1, day=1 }); $$;
do language pllua $$ print(pgtype.date{ year=-5000, month=1, day=1 }); $$;

-- 2. output of values in table form.

do language pllua $$
  local function prt(t,z)
    print(t)
    local rt = t:as_table(z)
    local o = {}
    for k,_ in pairs(rt) do o[1+#o] = k end
    table.sort(o)
    for _,k in ipairs(o) do print(k,rt[k]) end
  end
  prt(pgtype.timestamptz('2019-04-22 10:20:30+00'))
  prt(pgtype.timestamptz('2019-04-22 10:20:30+00'),'America/Los_Angeles')
  prt(pgtype.timestamptz('2019-04-22 10:20:30+00'),'Asia/Kathmandu')
  prt(pgtype.timestamp('2019-04-22 10:20:30+00'))
  prt(pgtype.date('2019-04-22'))
  prt(pgtype.time('10:20:30'))
  prt(pgtype.timetz('10:20:30+04'))
  prt(pgtype.interval('P1Y2M3DT4H5M6S'))
$$;

-- error
do language pllua $$ print(pgtype.timestamp('2019-04-22 10:20:30+00'):as_table('Europe/London')) $$;

-- 3. Field access

-- note, fields come out in session timezone, so set that:
set timezone = 'Europe/London';

do language pllua $$
  local t = pgtype.timestamptz('1968-05-10 03:45:01.234567+01')

  for _,k in ipairs{ 'century', 'day', 'decade', 'dow', 'doy',
                     'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'isodow', 'isoweek', 'isoyear',
		     'julian', 'microseconds', 'millennium',
		     'milliseconds', 'minute', 'month',
		     'quarter', 'second', 'timezone',
		     'timezone_hour', 'timezone_minute',
		     'week', 'year' } do
    print(k, t[k])
  end
$$;

set timezone = 'UTC';

do language pllua $$
  local t = pgtype.timestamp('1968-05-10 03:45:01.234567')

  for _,k in ipairs{ 'century', 'day', 'decade', 'dow', 'doy',
                     'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'isodow', 'isoweek', 'isoyear',
		     'julian', 'microseconds', 'millennium',
		     'milliseconds', 'minute', 'month',
		     'quarter', 'second',
		     'week', 'year' } do
    print(k, t[k])
  end
$$;

do language pllua $$
  local t = pgtype.date('1968-05-10')

  for _,k in ipairs{ 'century', 'day', 'decade', 'dow', 'doy',
                     'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'isodow', 'isoweek', 'isoyear',
		     'julian', 'microseconds', 'millennium',
		     'milliseconds', 'minute', 'month',
		     'quarter', 'second',
		     'week', 'year' } do
    print(k, t[k])
  end
$$;

do language pllua $$
  local t = pgtype.time('03:45:01.234567')

  for _,k in ipairs{ 'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'microseconds',
		     'milliseconds', 'minute', 'second' } do
    print(k, t[k])
  end
$$;

do language pllua $$
  local t = pgtype.timetz('03:45:01.234567+01')

  for _,k in ipairs{ 'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'microseconds',
		     'milliseconds', 'minute', 'second',
		     'timezone', 'timezone_hour',
		     'timezone_minute', } do
    print(k, t[k])
  end
$$;

do language pllua $$
  local t = pgtype.interval('P1Y2M3DT4H5M6S')

  for _,k in ipairs{ 'century', 'day', 'decade',
                     'epoch', 'epoch_msec', 'epoch_usec',
		     'hour', 'microseconds', 'millennium',
		     'milliseconds', 'minute', 'month',
		     'quarter', 'second',
		     'year' } do
    print(k, t[k])
  end
$$;

-- errors (not worth testing many combinations, they all share a code path)
do language pllua $$ print(pgtype.time('03:45:01.234567').dow) $$;

--end
