extends: c_ncc

STRING {
    prefix: b, optional
    variant raw { prefix: r   raw: yes }
}

remove: directives

PP_DIRECTIVE {
    like:             directive
    line-starts-with: "#" %
    end-token:        PP_EOL
}
