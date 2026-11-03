extends: typical
# JSON has no comments, and no identifiers: true, false and null are literals.
remove: comments IDENTIFIER

escapes json: \" \\ \/ \b \f \n \r \t \uHHHH

STRING {
    escapes: json
}

# JSON has one class for integers and floats, so the fraction is optional.
NUMBER {
    like:     FLOAT
    sign:     -
    fraction: ., optional
    allow:    none
}
