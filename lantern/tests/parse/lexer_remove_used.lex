@lex-ident(as("WORD"))
@lex-operator-set("comparison", "==", "!=")
@lex-operators(use("comparison"), remove("!="))
@lex-fallback("reject")
