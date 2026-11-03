comment "//"
comment "/*" .. "*/" nested
string '"""' .. '"""' multiline -> %MULTILINE_STRING
string '"' .. '"' escape "\\" -> %STRING
string "'" .. "'" escape "\\" scalar -> %CHAR
number digits "0123456789" sep "_" point trailing exp "eE" suffix "L" %INTEGER integer suffix "u" %INTEGER integer suffix "f" %FLOAT int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" suffix "L" %INTEGER integer suffix "u" %INTEGER integer int %INTEGER
number prefix "0b" digits "01" sep "_" suffix "L" %INTEGER integer suffix "u" %INTEGER integer int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]" newline %NL eof %EOF
