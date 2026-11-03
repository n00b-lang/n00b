extends: typical

INLINE_HTML {
    like:      embed
    code:      <?php ?>
    open-tag:  OPEN_TAG
    close-tag: CLOSE_TAG
}

comments { add line: "#" }

HEREDOC {
    like:             heredoc
    opener:           <<<
    label:            word single-quoted double-quoted
    end-may-indent:   yes
    end-may-continue: yes          # EOT;
    body-indent:      strip
}

ENCAPSED_STRING { like: STRING   quotes: '"' }
STRING          { quotes: "'" }

VARIABLE { like: sigil   sigil: $ }

INTEGER {
    separator: _
    variant binary { prefix: 0b, any case   digits: binary }
    variant octal  { prefix: 0o, any case   digits: octal }
}

FLOAT { separator: _ }

layout { brackets: () [] {} }
