extends: typical

STRING {
    variant raw { quotes: "`"   raw: yes   multiline: yes }
}

RUNE { like: CHAR }

INTEGER {
    separator: _
    allow:     0x_1
    suffix:    i -> IMAGINARY
    variant binary { prefix: 0b, any case   digits: binary }
    variant octal  { prefix: 0o, any case   digits: octal }
}

FLOAT {
    separator: _
    suffix:    i -> IMAGINARY
    variant hex {
        prefix:   0x, any case
        digits:   hex
        allow:    5. 0x_1
        exponent: p P, required
    }
}

layout {
    brackets:      () []
    statement-end: ";" after break continue fallthrough return ++ --
}
