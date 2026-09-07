local M = {}
function M.greet(name, punct)
  if name then
    return "hi " .. name .. punct
  elseif punct then
    return punct
  else
    return "?"
  end
end
for i = 1, 10 do
  print(i, i * 2)
end
while true do
  break
end
return M
