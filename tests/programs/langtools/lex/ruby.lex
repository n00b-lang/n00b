extends: typical

comments {
    line:  "#"
    block: none
}

HEREDOC {
    like:           heredoc
    opener:         <<~
    label:          word single-quoted double-quoted
    rest-of-line:   code
    end-may-indent: yes
    body-indent:    strip
    variant keep { opener: <<-   body-indent: keep }
}

STRING {
    quotes: '"' "'"
    variant percent {
        quotes:    none
        prefix:    %q %w
        pairs:     ( ) [ ] { } < >
        nested:    yes
        multiline: yes
    }
}

REGEX {
    like:       regex
    delimiters: / /
    flags:      i m x o u n s e
    not-after:  IDENTIFIER CONSTANT INTEGER FLOAT STRING REGEX IVAR CVAR GVAR
                HEREDOC ) ] } true false nil self
}

CVAR { like: sigil   sigil: @@ }
IVAR { like: sigil   sigil: @ }
GVAR { like: sigil   sigil: $ }

INTEGER {
    separator: _
    variant binary { prefix: 0b, any case   digits: binary }
    variant octal  { prefix: 0o, any case   digits: octal }
}

FLOAT { separator: _ }

IDENTIFIER {
    add_continue: ? !
    remove_start: Lu
}

CONSTANT { like: IDENTIFIER   start: Lu }     # Foo

layout {
    brackets:      () []
    continue:      \
    statement-end: NEWLINE after end break next redo retry return
}
