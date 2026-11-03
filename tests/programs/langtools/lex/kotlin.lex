extends: typical

comments { nested: yes }

MULTILINE_STRING { like: STRING   quotes: '"""'   multiline: yes   raw: yes }

INTEGER {
    separator: _
    suffix:    L u
    variant decimal { add suffix: f -> FLOAT }
    variant binary  { prefix: 0b, any case   digits: binary }
}

FLOAT {
    separator: _
    allow:     5.
    suffix:    f
}

layout {
    brackets:    () []
    newline:     NL
    end-of-file: EOF
}
