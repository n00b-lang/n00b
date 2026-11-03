extends: typical

TYPE_COMMENT {
    like: comment
    line: "# type:"
}

comments {
    line:  "#"
    block: none
}

STRING {
    quotes:    '"' "'" '"""' "'''"
    multiline: '"""' "'''"
    prefix:    u b, any case, optional
    variant raw { prefix: r rb br, any case   raw: yes }
}

FSTRING {
    like:              STRING
    prefix:            f, any case
    interpolate:       { }
    braces-are-tokens: yes
    start:             FSTRING_START
    middle:            FSTRING_MIDDLE
    end:               FSTRING_END
    variant raw { prefix: fr rf, any case   raw: yes }
}

INTEGER {
    separator: _
    allow:     0x_1
    variant decimal { suffix: j J -> FLOAT }
    variant binary  { prefix: 0b, any case   digits: binary }
    variant octal   { prefix: 0o, any case   digits: octal }
}

FLOAT {
    separator: _
    suffix:    j J
}

# Soft keywords come from the grammar: match case type

layout {
    brackets:    () [] {}
    continue:    \
    indent:      INDENT DEDENT
    newline:     NEWLINE
    end-of-file: ENDMARKER
}
