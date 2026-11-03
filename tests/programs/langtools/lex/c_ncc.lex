comment "//"
comment "/*" .. "*/"
string '"' .. '"' escape "\\" -> %STRING
string "'" .. "'" escape "\\" -> %CHAR
suffixes c-integer: "ull" "ULL" "llu" "LLU" "ul" "UL" "ll" "LL" "u" "U" -> %INTEGER integer
suffixes c-long: "l" "L"
suffixes c-float: "f" "F" -> %FLOAT
number digits "0123456789" sep "'" point leading trailing exp "eE" suffixes c-integer c-long c-float int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "'" point trailing exponent-required exp "pP" suffixes c-integer c-long c-float int %INTEGER float %FLOAT
number prefix "0X" digits "0123456789abcdefABCDEF" sep "'" point trailing exponent-required exp "pP" suffixes c-integer c-long c-float int %INTEGER float %FLOAT
number prefix "0b" digits "01" sep "'" suffixes c-integer c-long int %INTEGER
number prefix "0B" digits "01" sep "'" suffixes c-integer c-long int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]" continue "\\"
directive "#"
