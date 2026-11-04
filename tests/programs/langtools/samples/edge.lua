local a = 0x1F + 0xA.8p1 + 1.5e3 + .5 + 3. + 0x.1
local s = "dq \"esc\"" .. 'sq \'esc\''
local raw = [[
multi line
raw]]
--[[ block
comment ]] local b = a // 2 ~= 3 and a >= b
-- line comment
local t = {1, 2, 3}; t[#t] = ...
