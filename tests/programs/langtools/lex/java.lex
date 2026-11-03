comment "//"
comment "/*" .. "*/"
string '"""' .. '"""' escape "\\" multiline -> %TEXT_BLOCK
string '"' .. '"' escape "\\" -> %STRING
string "'" .. "'" escape "\\" scalar -> %CHAR
number digits "0123456789" sep "_" point leading trailing exp "eE" suffix "L" %INTEGER integer suffix "l" %INTEGER integer suffix "f" %FLOAT suffix "F" %FLOAT suffix "d" %FLOAT suffix "D" %FLOAT int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" point trailing exponent-required exp "pP" suffix "L" %INTEGER integer suffix "l" %INTEGER integer suffix "f" %FLOAT suffix "F" %FLOAT suffix "d" %FLOAT suffix "D" %FLOAT int %INTEGER float %FLOAT
number prefix "0b" digits "01" sep "_" suffix "L" %INTEGER integer suffix "l" %INTEGER integer int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]{}"
