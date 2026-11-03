comment "//"
comment "/*" .. "*/"
string '"""' .. '"""' multiline raw -> %RAW_STRING
string '$"' .. '"' escape "\\" -> %INTERPOLATED_STRING
string '@"' .. '"' raw multiline -> %VERBATIM_STRING
string '"' .. '"' escape "\\" -> %STRING
string "'" .. "'" escape "\\" scalar -> %CHAR
suffixes csharp-integer: "UL" "ul" "LU" "lu" "L" "l" "U" "u" -> %INTEGER integer
suffixes csharp-float: "f" "F" "m" "M" "d" "D" -> %FLOAT
number digits "0123456789" sep "_" point leading trailing exp "eE" suffixes csharp-integer csharp-float int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" after-prefix suffixes csharp-integer int %INTEGER
number prefix "0b" digits "01" sep "_" after-prefix suffixes csharp-integer int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]{}"
