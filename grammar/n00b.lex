@lex-region("#[", "#]", "#")
@lex-comment("#")
@lex-comment("//")
@lex-comment("/", "/", fill("*"))
@lex-string-escapes("n00b",
		    escapes("abfnrtv0"),
		    escapes('\\'),
		    escapes('"'),
		    escapes("'"),
		    escape_literal("«"),
		    escape_literal("»"),
		    escape_literal("["),
		    escape_digits("x", "0123456789abcdefABCDEF", "2"),
		    escape_digits("u", "0123456789abcdefABCDEF", "4"),
		    escape_digits("U", "0123456789abcdefABCDEF", "8"))
@lex-string('"', '"', escape('\\'), use_escapes("n00b"), as("STRING_LIT"))
@lex-string("'", "'", escape('\\'), use_escapes("n00b"), scalar, as("CHAR_LIT"))
@lex-sigil("'", before_string, unless_closed, attach_previous)
@lex-number(digits("0123456789"),
	    sep("_"),
	    point("leading"),
	    exp("eE"),
	    int("INT_LIT"),
	    float("FLOAT_LIT"))
@lex-number(prefix("0x"), digits("0123456789abcdefABCDEF"), sep("_"),
	    int("HEX_LIT"))
@lex-soft-keywords("i", "u", "bits", "call", "handler", "setup",
                   "layout", "align", "step", "out", "inline", "typeof",
                   "provides", "enter", "exit", "ensure", "as", "thread",
                   "litmod", "modify", "consume", "where", "count", "by",
                   "union")
@lex-ident-profile("n00b-identifiers",
		   normalize("nfc"),
		   mixed_scripts("diagnose"),
		   confusables("allow"),
		   emoji("reject"))
@lex-ident(as("IDENTIFIER"), profile("n00b-identifiers"))
@lex-fence("[", "]",
	   fill("-"),
	   pad("required"),
	   label,
	   as("EMBED"))
@lex-layout(indent("INDENT", "DEDENT"), newline("NEWLINE", "each"),
	    tabs("reject"), mixed_indent("reject"), brackets("()[]"),
	    bracket_newlines("keep"), dedent_newline("emit"))
@lex-fallback("reject")
@lex-operators(add(".."))
