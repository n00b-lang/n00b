extends: typical

HASHBANG { like: comment   line: "#!" }

STRING { quotes: '"' "'" }

TEMPLATE {
    like:        STRING
    quotes:      "`"
    multiline:   yes
    interpolate: ${ }
    start:       TEMPLATE_HEAD
    middle:      TEMPLATE_MIDDLE
    end:         TEMPLATE_TAIL
}

REGEX_LIT {
    like:       regex
    delimiters: / /
    flags:      d g i m s u v y
    not-after:  IDENTIFIER PRIVATE_ID INTEGER FLOAT STRING BIGINT TEMPLATE_TAIL
                REGEX_LIT ) ] } ++ -- this super true false null
}

PRIVATE_ID { like: sigil   sigil: "#" }

INTEGER {
    separator: _
    suffix:    n -> BIGINT
    variant binary { prefix: 0b, any case   digits: binary }
    variant octal  { prefix: 0o, any case   digits: octal }
}

FLOAT { separator: _ }

IDENTIFIER { add: $ }

layout { brackets: () [] {} }
