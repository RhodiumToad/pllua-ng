--

\set VERBOSITY terse

set pllua.on_trusted_init=$$
  local e = require 'pllua.elog'
  package.preload['testmod1'] = function() e.info("testmod1 loaded") return { testfunc = function() print("testfunc1") end } end;
  package.preload['testmod2'] = function() e.info("testmod2 loaded") return { testfunc = function() print("testfunc2") end } end;
  trusted.allow('testmod1', nil, nil, nil, false);
  trusted.allow('testmod2', nil, nil, nil, true);
$$;

--

do language pllua $$ print("interpreter loaded") $$;

do language pllua $$
  local m = require 'testmod1'
  m.testfunc()
$$;

do language pllua $$
  local m = require 'testmod2'
  m.testfunc()
$$;

--end
