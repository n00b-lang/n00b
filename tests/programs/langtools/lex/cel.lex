extends: typical

comments { block: none }

STRING {
    quotes:    '"' "'" '"""'
    multiline: '"""'
    variant raw { prefix: r R   quotes: '"' "'"   raw: yes }
}

BYTES {
    like:   STRING
    quotes: '"'
    prefix: b
    remove variant: raw
}

INTEGER {
    suffix: u U -> UINT
    variant hex { prefix: 0x }     # CEL allows only lowercase
}

layout { brackets: () [] {} }
