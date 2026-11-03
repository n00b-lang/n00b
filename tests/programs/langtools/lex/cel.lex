comment "//"
string quotes '"' "'" escape "\\" plain -> %STRING
string '"""' .. '"""' escape "\\" multiline -> %STRING
string 'b"' .. '"' escape "\\" -> %BYTES
string prefix "r" "R" quotes '"' "'" raw -> %STRING
number digits "0123456789" point leading trailing exp "eE" suffix "u" %UINT integer suffix "U" %UINT integer int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" suffix "u" %UINT integer suffix "U" %UINT integer int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]{}"
