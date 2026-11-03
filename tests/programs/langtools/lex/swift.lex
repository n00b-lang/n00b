comment "//"
comment "/*" .. "*/" nested
string '"""' .. '"""' escape "\\" multiline -> %MULTILINE_STRING
fence "#" .. "#" fill "#" body '"' .. '"' min 0 -> %STRING
string '"' .. '"' escape "\\" -> %STRING
regex "/" .. "/" not-after %IDENTIFIER %IMPLICIT_PARAM %INTEGER %FLOAT %STRING %MULTILINE_STRING %REGEX_LIT ")" "]" "}" "true" "false" "nil" "self" "super" -> %REGEX_LIT
sigil "$" digits -> %IMPLICIT_PARAM
number digits "0123456789" sep "_" point leading trailing exp "eE" int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" after-prefix point trailing exponent-required exp "pP" int %INTEGER float %FLOAT
number prefix "0b" digits "01" sep "_" after-prefix int %INTEGER
number prefix "0o" digits "01234567" sep "_" after-prefix int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]" terminator %NEWLINE terminator-after "break" "continue" "fallthrough" "return"
