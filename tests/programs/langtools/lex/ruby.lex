comment "#"
header ruby-label: quotes single double
heredoc "<<~" header ruby-label queue terminator-indent allow body-indent strip -> %HEREDOC
heredoc "<<-" header ruby-label queue terminator-indent allow body-indent keep -> %HEREDOC
string quotes '"' "'" escape "\\" plain -> %STRING
string prefix "%q" "%w" pairs "(" .. ")" "[" .. "]" "{" .. "}" "<" .. ">" escape "\\" nested multiline -> %STRING
regex "/" .. "/" flags "imxounse" not-after %IDENTIFIER %CONSTANT %INTEGER %FLOAT %STRING %REGEX %IVAR %CVAR %GVAR %HEREDOC ")" "]" "}" "true" "false" "nil" "self" -> %REGEX
sigil "@@" -> %CVAR
sigil "@" -> %IVAR
sigil "$" -> %GVAR
number digits "0123456789" sep "_" point leading trailing exp "eE" int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" int %INTEGER
number prefix "0b" digits "01" sep "_" int %INTEGER
identifier extra "?!" upper %CONSTANT -> %IDENTIFIER
layout brackets "()[]" continue "\\" terminator %NEWLINE terminator-after "end" "break" "next" "redo" "retry" "return"
