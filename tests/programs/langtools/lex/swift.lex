extends: typical

comments { nested: yes }

MULTILINE_STRING { like: STRING   quotes: '"""'   multiline: yes }

STRING {
    variant extended { raw: yes   pad: "#", at least one }   # #"…"#
}

REGEX_LIT {
    like:       regex
    delimiters: / /
    not-after:  IDENTIFIER IMPLICIT_PARAM INTEGER FLOAT STRING MULTILINE_STRING
                REGEX_LIT ) ] } true false nil self super
}

IMPLICIT_PARAM { like: sigil   sigil: $   then: digits }   # $0

INTEGER {
    separator: _
    allow:     0x_1
    variant hex    { prefix: 0x }      # Swift allows only lowercase
    variant binary { prefix: 0b   digits: binary }
    variant octal  { prefix: 0o   digits: octal }
}

FLOAT {
    separator: _
    variant hex { prefix: 0x   digits: hex   allow: 5. 0x_1   exponent: p P, required }
}

layout {
    brackets:      () []
    statement-end: NEWLINE after break continue fallthrough return
}
