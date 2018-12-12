-- logo.lua

-- make_encoder([separator [,blocksize [,alphabet]]])
--
-- Given an alphabet (or default), return an encoding function,
-- which is callable as enc(str)

local function make_encoder(sep,blksz,b64a)
	local char, byte = string.char, string.byte
	local concat = table.concat
	local unpack = table.unpack or unpack
	local fdiv = function(p,q) return math.floor(p/q) end	-- cope with missing //

	-- default separator
	sep = sep or "\n"

	-- default blocksize
	blksz = fdiv((blksz or 76), 4)
	assert(blksz > 0, "invalid blocksize")
	-- input and output block sizes
	local iblksz, oblksz = blksz * 3, blksz * 4

	-- default b64 alphabet
	b64a = b64a or "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
	assert(#b64a == 65, "invalid alphabet")
	local pad = byte(b64a, 65)
	local function b64chr(c)
		return byte(b64a, 1 + c)
	end

	-- precomputed lookup tables:
	-- given input bytes b1,b2,b3:
	--   b64a1[b1]     = first output byte (from bits b1:111111xx)
	--   b64a2[b1][b2] = second output byte (from bits b1:xxxxxx11 b2:1111xxxx)
	--   b64a3[b3][b2] = third output byte (from bits b2:xxxx1111 b3:11xxxxxx)
	--   b64a4[b3]     = fourth output byte (from bits b3:xx111111)
	--
	local b64a1, b64a2, b64a3, b64a4 = {}, {}, {}, {}
	-- b64a1 is easy
	for b1 = 0,255 do
		b64a1[b1] = b64chr(fdiv(b1,4))
	end
	-- b64a2[b1][b2]
	-- we take only the bottom 2 bits from b1, so there are only 4 distinct b2 tables
	-- we take the high 4 bits from b2
	for b1 = 0,3 do
		local t = {}
		for b2 = 0,255 do
			t[b2] = b64chr(fdiv(b2,16) + (b1 * 16))
		end
		b64a2[b1] = t
	end
	for b1 = 4,255 do
		b64a2[b1] = b64a2[b1 % 4]
	end
	-- b64a3[b3][b2] (note reversed order to keep number of tables small)
	-- we take only the top 2 bits from b3, so there are only 4 distinct b2 tables
	-- we take the low 4 bits from b2
	for b3 = 0,255,64 do
		local t = {}
		for b2 = 0,255 do
			t[b2] = b64chr((b2 % 16)*4 + fdiv(b3,64))
		end
		for i = 0,63 do
			b64a3[b3+i] = t
		end
	end
	-- b64a4 is easy
	for i = 0,255 do
		b64a4[i] = b64chr(i % 64)
	end

	return function(str)
		local chunks, chunk = {}, {}
		local c = 0
		local len = #str
		for i = 1, len - (len % iblksz), iblksz do
			-- fastpath loop
			for j = 1,blksz do
				local k = j*4 - 3
				local b1,b2,b3 = byte(str, i+k-j, i+k-j+2)
				chunk[k], chunk[k+1], chunk[k+2], chunk[k+3]
					= b64a1[b1], b64a2[b1][b2], b64a3[b3][b2], b64a4[b3]
			end
			c = c + 1
			chunks[c] = char(unpack(chunk, 1, oblksz))
		end
		chunk = {}
		for i = len - (len % iblksz) + 1, len, 3 do
			local b1,b2,b3 = byte(str, i, i+2)
			chunk[1+#chunk] = char( b64a1[b1],
									b64a2[b1][b2 or 0],
									b2 and b64a3[b3 or 0][b2] or pad,
									b3 and b64a4[b3] or pad )
		end
		if #chunk > 0 then
			c = c + 1
			chunks[c] = concat(chunk, "")
		end
		return concat(chunks, sep, 1, c)
	end
end

local mode,fmt,fn = ...
local str = ""
if mode == "-text" then
	for line in io.lines(fn) do
		str = str .. line .. "\x0D\x0A"  -- canonical line endings
	end
elseif mode == "-binary" then
	local file = io.open(fn, "rb")
	str = file:read("*a")
	file:close()
else
	error("unknown mode")
end

if fmt:match("^-icon") then
	local sz = fmt:match("^-icon=(.*)")
	local basename = fn:match(".*/([^/]+)$") or fn
	local ext = fn:match("%.([^.]+)$")
	local mediatype = { ico = "image/x-icon",
						png = "image/png",
						gif = "image/gif",
						jpg = "image/jpeg" }
	local typ = mediatype[ext]
	local b64enc = make_encoder("", 120)
	io.write([[<link rel="icon" type="]], typ, [[" ]],
		(sz and [[sizes="]]..sz..[[" ]] or ""),
		[[id="]]..basename..[[" ]],
		[[href="data:]], typ, [[;base64,]], b64enc(str), [[" />
]])
elseif fmt == "-logo" then
	local b64enc = make_encoder("", 120)
	io.write([[
#logo {
  background-image: url("data:image/svg+xml;base64,]], b64enc(str), [[");
}
]])
else
	error("unknown format")
end
