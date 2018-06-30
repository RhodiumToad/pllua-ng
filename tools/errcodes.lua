-- errcodes.lua

local fn = ...
for line in io.lines(fn) do
	if line:match("^[^#%s]") and not line:match("^Section:") then
		local f1,f2,f3,f4 = line:match("^(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s*$")
		if f4 then
			io.write("{\n    \"",f4,"\", ",f3,"\n},\n")
		end
	end
end
