#include "scanner/tokenizer.h"
#include "scanner/lex.h"

#define S(x) CORE_STRING_STATIC(x)

#define DECIMAL_DIGITS "0123456789"
#define HEX_DIGITS     "0123456789abcdefABCDEF"
#define OCTAL_DIGITS   "01234567"
#define BINARY_DIGITS  "01"

static const scan_lex_string_t TEXT_STRINGS[] = {
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_spec_t TEXT = {
    .name      = S("text"),
    .strings   = TEXT_STRINGS,
    .number    = {.exponent = S("eE"),
                  .tok_int  = S("INTEGER"),
                  .tok_float = S("FLOAT")},
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_spec_t JSON = {
    .name      = S("json"),
    .strings   = TEXT_STRINGS,
    .number    = {.exponent  = S("eE"),
                  .sign      = S("-"),
                  .tok_int   = S("INT_LIT"),
                  .tok_float = S("FLOAT_LIT")},
    .tok_ident = S("IDENTIFIER"),
};

static const scan_lex_comment_t LISP_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S(";")},
    {},
};

static const scan_lex_spec_t LISP = {
    .name              = S("lisp"),
    .comments          = LISP_COMMENTS,
    .strings           = TEXT_STRINGS,
    .number            = {.exponent  = S("eE"),
                          .tok_int   = S("INT_LIT"),
                          .tok_float = S("FLOAT_LIT")},
    .tok_ident         = S("SYMBOL"),
    .ident_start_extra = S("+-*/<>=!?"),
    .ident_extra       = S("+-*/<>=!?."),
};

static const scan_lex_comment_t SHELL_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("#")},
    {},
};

static const scan_lex_string_t SHELL_STRINGS[] = {
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING_LIT")},
    {.opener = S("'"), .closer = S("'"), .flags = SCAN_LEX_STR_RAW,
     .tok = S("STRING_LIT")},
    {},
};

static const scan_lex_spec_t SHELL = {
    .name              = S("shell"),
    .comments          = SHELL_COMMENTS,
    .strings           = SHELL_STRINGS,
    .number            = {.tok_int = S("INT_LIT")},
    .layout            = SCAN_LEX_LAYOUT_NEWLINE,
    .tok_newline       = S("NEWLINE"),
    .tok_ident         = S("WORD"),
    .line_continue     = S("\\"),
    .brackets          = S("()"),
    .ident_start_extra = S("./-"),
    .ident_extra       = S("./-="),
};

static const scan_lex_comment_t C_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("//")},
    {.kind = SCAN_LEX_BLOCK, .opener = S("/*"), .closer = S("*/")},
    {},
};

static const scan_lex_string_t C_STRINGS[] = {
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING_LIT")},
    {.opener = S("'"), .closer = S("'"), .escape = S("\\"),
     .tok = S("CHAR_LIT")},
    {},
};

static const scan_lex_radix_t C_RADIXES[] = {
    {.prefix = S("0x"), .digits = S(HEX_DIGITS)},
    {.prefix = S("0X"), .digits = S(HEX_DIGITS)},
    {.prefix = S("0b"), .digits = S(BINARY_DIGITS)},
    {},
};

static const scan_lex_spec_t C = {
    .name      = S("c"),
    .comments  = C_COMMENTS,
    .strings   = C_STRINGS,
    .number    = {.radixes   = C_RADIXES,
                  .exponent  = S("eE"),
                  .tok_int   = S("INT_LIT"),
                  .tok_float = S("FLOAT_LIT")},
    .tok_ident = S("IDENTIFIER"),
    .brackets  = S("()[]{}"),
};

static const scan_lex_spec_t *const BUILTIN[] = {
    &TEXT, &JSON, &LISP, &SHELL, &C,
};

void
scan_tokenizers_register_builtin(void)
{
    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        scan_lex_register(BUILTIN[i]);
    }
}
