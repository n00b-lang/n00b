comment "//"
comment "/*" .. "*/" nested
fence "br#" .. "#" fill "#" body '"' .. '"' min 0 -> %RAW_BYTE_STRING
fence "r#" .. "#" fill "#" body '"' .. '"' min 0 -> %RAW_STRING
string 'br"' .. '"' raw -> %RAW_BYTE_STRING
string 'r"' .. '"' raw multiline -> %RAW_STRING
string "b'" .. "'" escape "\\" scalar -> %BYTE_LIT
string 'b"' .. '"' escape "\\" -> %BYTE_STRING
string '"' .. '"' escape "\\" multiline -> %STRING
string "'" .. "'" escape "\\" scalar -> %CHAR
suffixes rust-integer: "usize" "isize" "i8" "i16" "i32" "i64" "i128" "u8" "u16" "u32" "u64" "u128" -> %INTEGER integer
suffixes rust-float: "f32" "f64" -> %FLOAT
number digits "0123456789" sep "_" repeat trailing point trailing exp "eE" suffixes rust-integer rust-float int %INTEGER float %FLOAT
number prefix "0x" digits "0123456789abcdefABCDEF" sep "_" after-prefix repeat trailing suffixes rust-integer int %INTEGER
number prefix "0b" digits "01" sep "_" after-prefix repeat trailing suffixes rust-integer int %INTEGER
number prefix "0o" digits "01234567" sep "_" after-prefix repeat trailing suffixes rust-integer int %INTEGER
sigil "r#" -> %IDENTIFIER
identifier -> %IDENTIFIER
layout brackets "()[]{}"
