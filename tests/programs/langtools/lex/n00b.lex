region "#[" .. "#]" line "#"
comment "#"
comment "//"
comment "/" .. "/" fill "*"
escapes n00b:
        chars "abfnrtv0"
        chars "\\"
        chars '"'
        chars "'"
        literal "«" "»" "["
        digits "x" 2 "0123456789abcdefABCDEF"
        digits "u" 4 "0123456789abcdefABCDEF"
        digits "U" 8 "0123456789abcdefABCDEF"
string '"' .. '"' escape "\\" escapes n00b -> %STRING_LIT
string "'" .. "'" escape "\\" escapes n00b scalar -> %CHAR_LIT
sigil "'" before-string unless-closed attach-after "]" "}" ")" %STRING_LIT %INT_LIT %HEX_LIT %FLOAT_LIT %EMBED "true" "false" "nil" -> %TYPE_VAR
number digits "0123456789" sep "_" point leading exp "eE" int %INT_LIT float %FLOAT_LIT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" int %HEX_LIT
# soft keywords come from the grammar: i u bits call handler setup layout align step out inline typeof provides enter exit ensure as thread litmod modify consume where count by union from all struct object view fn function not null None elseif
profile n00b-identifiers: normalize nfc mixed-scripts diagnose confusables allow emoji reject
identifier profile n00b-identifiers -> %IDENTIFIER
fence "[" .. "]" fill "-" pad required label bidi warn -> %EMBED
layout brackets "()[]" indent %INDENT %DEDENT newline %NEWLINE each tabs reject mixed-indent reject bracket-newlines keep dedent-newline emit
fallback reject recover
bidi reject
recover %STRING_LIT sync '"' "\n" consume -> %STRING_LIT
recover %CHAR_LIT sync "'" "\n" consume -> %CHAR_LIT
recover %INT_LIT -> %INT_LIT
recover %FLOAT_LIT -> %FLOAT_LIT
recover %HEX_LIT -> %HEX_LIT
recover %IDENTIFIER -> %IDENTIFIER
