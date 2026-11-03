extends: typical

comments {
    line:  --
    block: --[[ ]]
}

STRING {
    quotes: '"' "'"
    variant long { quotes: none   pairs: [[ ]]   raw: yes   multiline: yes }
}

FLOAT {
    variant hex { prefix: 0x, any case   digits: hex   allow: 5.   exponent: p P }
}

layout { brackets: () [] {} }
