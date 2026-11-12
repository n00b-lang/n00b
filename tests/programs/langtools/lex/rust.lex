extends: typical

comments { nested: yes }

RAW_STRING {                   # r"…", r#"…"#, r##"…"##
    like:      STRING
    prefix:    r
    raw:       yes
    multiline: yes
    pad:       "#"
}

RAW_BYTE_STRING { like: RAW_STRING   prefix: br   multiline: no }

STRING      { multiline: yes }
BYTE_STRING { like: STRING   prefix: b   multiline: no }
BYTE_LIT    { like: CHAR     prefix: b }

INTEGER {
    separator: _
    allow:     0x_1 1__0 1_
    suffix:    i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize
    variant decimal { add suffix: f32 f64 -> FLOAT }
    variant hex     { prefix: 0x }     # Rust allows only lowercase
    variant binary  { prefix: 0b   digits: binary }
    variant octal   { prefix: 0o   digits: octal }
}

FLOAT {
    separator: _
    allow:     5. 1__0 1_
    suffix:    f32 f64
}

IDENTIFIER {
    variant raw { prefix: r# }     # r#match
}

layout { brackets: () [] {} }
