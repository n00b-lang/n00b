@lex-string("\"", "\"", escape("\\"), as("STRING"))
@lex-number(digits("0123456789"), point("leading", "trailing"), exp("eE"), int("INTEGER"), float("FLOAT"))
@lex-ident(as("IDENTIFIER"))
