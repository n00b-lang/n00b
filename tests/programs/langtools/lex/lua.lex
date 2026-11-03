comment "--[[" .. "]]"
comment "--"
string quotes '"' "'" escape "\\" plain -> %STRING
string "[[" .. "]]" raw multiline -> %STRING
number digits "0123456789" point leading trailing exp "eE" int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" point trailing exp "pP" int %INTEGER float %FLOAT
identifier -> %IDENTIFIER
layout brackets "()[]{}"
