comment "#!" -> %HASHBANG
comment "//"
comment "/*" .. "*/"
string "`" .. "`" escape "\\" interp "${" .. "}" multiline -> %TEMPLATE_HEAD %TEMPLATE_MIDDLE %TEMPLATE_TAIL
string quotes '"' "'" escape "\\" plain -> %STRING
regex "/" .. "/" flags "dgimsuvy" not-after %IDENTIFIER %INTEGER %FLOAT %STRING %BIGINT %TEMPLATE_TAIL %REGEX_LIT ")" "]" "}" "++" "--" "this" "super" "true" "false" "null" -> %REGEX_LIT
sigil "#" -> %PRIVATE_ID
number digits "0123456789" sep "_" point leading trailing exp "eE" suffix "n" %BIGINT integer int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" suffix "n" %BIGINT integer int %INTEGER
number prefix "0b" digits "01" sep "_" suffix "n" %BIGINT integer int %INTEGER
number prefix "0o" digits "01234567" sep "_" suffix "n" %BIGINT integer int %INTEGER
identifier start "$" extra "$" -> %IDENTIFIER
layout brackets "()[]{}"
