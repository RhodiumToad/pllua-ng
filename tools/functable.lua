-- functable.lua

local funcnames = {}
local matchfuncs = {
	pllua_pushcfunction = true,
	pllua_cpcall = true,
	pllua_initial_protected_call = true,
	pllua_register_cfunc = true
}

for i,fn in ipairs{...} do
	for line in io.lines(fn) do
		for fn1,pos in line:gmatch("(pllua_[%w_]+)%(()") do
			if matchfuncs[fn1] then
				local fn2 = line:match("%s*[%w._]+%s*,%s*(pllua_[%w_]+)",pos)
				if fn2 ~= nil then
					funcnames[fn2] = true
				end
			end
		end
	end
end

local out = {}
for k,_ in pairs(funcnames) do
	out[1+#out] = k
end
table.sort(out)
for i,v in ipairs(out) do
	io.write("PLLUA_DECL_CFUNC(",v,")\n")
end
