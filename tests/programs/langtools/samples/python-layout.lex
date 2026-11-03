comment "# type:" -> %TYPE_COMMENT
comment "#"
string prefix "u" "U" "b" "B" quotes '"' "'" escape "\\" plain -> %STRING
string prefix "r" "R" "br" "Br" "bR" "BR" "rb" "rB" "Rb" "RB" quotes '"' "'" raw -> %STRING
string prefix "u" "U" "b" "B" quotes '"""' "'''" escape "\\" plain multiline -> %STRING
string prefix "r" "R" "br" "Br" "bR" "BR" "rb" "rB" "Rb" "RB" quotes '"""' "'''" raw multiline -> %STRING
number digits "0123456789" sep "_" point leading trailing exp "eE" suffix "j" %FLOAT suffix "J" %FLOAT int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" after-prefix int %INTEGER
number prefix "0b" digits "01" sep "_" after-prefix int %INTEGER
number prefix "0o" digits "01234567" sep "_" after-prefix int %INTEGER
identifier -> %IDENTIFIER
# soft keywords come from the grammar: match case type
layout brackets "()[]{}" continue "\\" indent %INDENT %DEDENT newline %NEWLINE eof %ENDMARKER
