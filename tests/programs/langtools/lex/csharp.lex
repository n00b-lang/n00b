extends: typical

RAW_STRING          { like: STRING   quotes: '"""'   multiline: yes   raw: yes }
INTERPOLATED_STRING { like: STRING   prefix: $ }
VERBATIM_STRING     { like: STRING   prefix: @   multiline: yes   raw: yes }

INTEGER {
    separator: _
    allow:     0x_1
    suffix:    UL ul LU lu L l U u
    variant decimal { add suffix: f F m M d D -> FLOAT }
    variant binary  { prefix: 0b, any case   digits: binary }
}

FLOAT {
    separator: _
    suffix:    f F m M d D
}

layout { brackets: () [] {} }
