embed "<?php" .. "?>" open %OPEN_TAG close %CLOSE_TAG -> %INLINE_HTML
comment "//"
comment "#"
comment "/*" .. "*/"
header php-label: quotes single double
heredoc "<<<" header php-label terminator-indent allow terminator-tail body-indent strip -> %HEREDOC
string '"' .. '"' escape "\\" -> %ENCAPSED_STRING
string "'" .. "'" escape "\\" -> %STRING
sigil "$" -> %VARIABLE
number digits "0123456789" sep "_" point leading trailing exp "eE" int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" int %INTEGER
number prefix "0b" digits "01" sep "_" int %INTEGER
number prefix "0o" digits "01234567" sep "_" int %INTEGER
identifier -> %IDENTIFIER
layout brackets "()[]{}"
