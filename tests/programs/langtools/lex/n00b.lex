extends: typical

comments {
    add line: "#"
    block:    /* */, stretch *     # /** */ **/ nests one level; /**/ is empty
    region:   "#[" "#]", ends-with "#"
}

escapes n00b: \a \b \f \n \r \t \v \0 \\ \" \' \« \» \[ \xHH \uHHHH \UHHHHHHHH

STRING_LIT {
    like:     STRING
    escapes:  n00b
    on-error: skip to '"' \n
}

CHAR_LIT {
    like:     CHAR
    escapes:  n00b
    on-error: skip to "'" \n
}

INT_LIT {
    like:      integer
    digits:    decimal
    separator: _
}

HEX_LIT {
    like:      integer
    prefix:    0x
    digits:    hex
    separator: _
}

FLOAT_LIT {
    like:      FLOAT
    separator: _
    allow:     .5
}

IDENTIFIER {
    normalize:     NFC
    mixed-scripts: reject
    confusables:   allow
}

# 'T is a type variable; 'a' stays a character; "x"'re attaches `'re`.
TYPE_VAR {
    like:           sigil
    sigil:          "'"
    not-if-closed:  yes
    modifier-after: STRING_LIT INT_LIT HEX_LIT FLOAT_LIT EMBED ] } ) true false nil
}

# [-- ... --]
EMBED {
    like:    fence
    open:    [
    close:   ]
    stretch: -, at least one
    padding: required
    label:   allowed
    bidi:    warn
}

# Soft keywords come from the grammar: i u bits call handler setup layout
# align step out inline typeof provides enter exit ensure as thread litmod
# modify consume where count by union from all struct object view fn
# function not null None elseif

layout {
    brackets:             () []
    indent:               INDENT DEDENT
    newline:              NEWLINE, every line
    newlines-in-brackets: keep
    newline-after-dedent: yes
}

source {
    indent-with:  spaces
    mixed-indent: reject
    bidi:         reject
}
