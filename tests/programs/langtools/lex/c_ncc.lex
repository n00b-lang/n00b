extends: typical

CHAR { length: any }           # C allows 'ab'

INTEGER {
    separator: "'"
    suffix:    u U l L ul UL ll LL ull ULL llu LLU
    variant decimal { add suffix: f F -> FLOAT }
    variant binary  { prefix: 0b, any case   digits: binary }
}

FLOAT {
    separator: "'"
    suffix:    f F l L
    variant hex {
        prefix:   0x, any case
        digits:   hex
        allow:    5.
        exponent: p P, required
    }
}

layout {
    brackets: () []
    continue: \
}

directives {                   # preprocessor lines are skipped
    line-starts-with: "#"
}
