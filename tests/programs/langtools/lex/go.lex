comment "//"
comment "/*" .. "*/"
string '"' .. '"' escape "\\" -> %STRING
string "`" .. "`" raw multiline -> %STRING
string "'" .. "'" escape "\\" scalar -> %RUNE
number digits "0123456789" sep "_" point leading trailing exp "eE" suffix "i" %IMAGINARY int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" after-prefix point trailing exponent-required exp "pP" suffix "i" %IMAGINARY int %INTEGER float %FLOAT
number prefix "0b" digits "01" sep "_" after-prefix suffix "i" %IMAGINARY int %INTEGER
number prefix "0o" digits "01234567" sep "_" after-prefix suffix "i" %IMAGINARY int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]" terminator ";" terminator-after "break" "continue" "fallthrough" "return" "++" "--"
