escapes json: chars "\"\\/bfnrt" digits "u" 4 "0123456789abcdefABCDEF"
string '"' .. '"' escape "\\" escapes json -> %STRING
number sign "-" digits "0123456789" point exp "eE" int %NUMBER float %NUMBER
fallback reject
