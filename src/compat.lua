-- compat.lua

--[[
This is an attempt to emulate the old pllua as closely as possible to
try and make porting easy.

Configure  pllua.on_common_init='require "pllua.compat"'  to enable it.
]]

do
	local pgtype = require 'pllua.pgtype'

	function _G.fromstring(t,s)
		return pgtype[t]:fromstring(s)
	end
end

do
	local meta = ...
	local shared = setmetatable({}, { __index = _G })
	_G.shared = shared
	local rawget, rawset = rawget, rawset
	local function shared_assign(t,k,v)
		rawset(rawget(shared,k) and shared or t, k, v)
	end
	function _G.setshared(k,v)
		if meta.__index ~= shared then
			meta.__index = shared
			meta.__newindex = shared_assign
		end
		shared[k] = v
	end
end

do
	local pcall = pcall
	local error = error

	function _G.subtransaction(...)
		return pcall(...)
	end

	function _G.pgfunc(...)
		error('pgfunc is not implemented')
	end
end

do
	local e = require 'pllua.elog'
	_G.info, _G.log, _G.notice, _G.warning = e.info, e.log, e.notice, e.warning
end

do
	local spi = require 'pllua.spi'
	local unpack = table.unpack or unpack
	local type = type
	local setmetatable = setmetatable

	-- map new result convention to old one
	local function fixresult(r)
		return type(r)=='table' and #r > 0 and r or nil
	end

	-- wrap the SPI cursor object
	local curs = {
		fetch = function(self,n)
			return fixresult(self.curs:fetch(n))
		end;
		move = function(self,n)
			return fixresult(self.curs:move(n))
		end;
		posfetch = function(self,n,rel)
			return fixresult(self.curs:fetch(n, rel and 'relative' or 'absolute'))
		end;
		posmove = function(self,n,rel)
			return fixresult(self.curs:move(n, rel and 'relative' or 'absolute'))
		end;
		close = function(self)
			return self.curs:close()
		end;
	}
	local curs_meta = { __index = curs }
	local function newcurs(c)
		return setmetatable({ curs = c }, curs_meta)
	end

	-- wrap the SPI statement object
	local plan = {
		execute = function(self,args,ronly,count)
			local s = self.stmt
			local nargs = s:numargs()
			return fixresult(s:execute_count(count, unpack(args, 1, nargs)))
		end;
		getcursor = function(self,args,ronly,name)
			local s = self.stmt
			local nargs = s:numargs()
			local c = spi.newcursor(type(name)=='string' and name or nil)
			return newcurs(c:open(s, unpack(args, 1, nargs)))
		end;
		rows = function(self,args)
			local s = self.stmt
			local nargs = s:numargs()
			return s:rows(unpack(args, 1, nargs))
		end;
		issaved = function() return true end;
		save = function(self) return self end;
	}
	local plan_meta = { __index = plan }
	local function newplan(s)
		return setmetatable({ stmt = s }, plan_meta)
	end

	local server = {
		execute = function(cmd,ronly,count)
			return fixresult(spi.execute_count(cmd,count))
		end;

		rows = spi.rows;

		prepare = function(cmd,argtypes)
			return newplan(spi.prepare(cmd,argtypes))
		end;

		find = function(name)
			return newcurs(spi.findcursor(name))
		end;
	}

	_G.server = server
end

return true
