extends: typical

TEXT_BLOCK { like: STRING   quotes: '"""'   multiline: yes }

INTEGER {
    separator: _
    suffix:    L l
    variant decimal { add suffix: f F d D -> FLOAT }
    variant binary  { prefix: 0b, any case   digits: binary }
}

FLOAT {
    separator: _
    suffix:    f F d D
    variant hex {
        prefix:   0x, any case
        digits:   hex
        allow:    5.
        exponent: p P, required
    }
}

layout { brackets: () [] {} }
