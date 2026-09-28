#include "parse/symtab.h"
#include "harness.h"

static void
test_namespaced_symbol_bindings(void)
{
    parse_symtab_t *symbols = parse_symtab_new();
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC(""),
                            CORE_STRING_STATIC("module"));
    parse_sym_entry_t *symbol = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("value"),
        PARSE_SYM_VARIABLE, nullptr);
    int first = 1, replacement = 2, other = 3;
    bool stored = symbol
        && parse_sym_set_binding(symbol, CORE_STRING_STATIC("ir.shape"),
                                 &first)
        && parse_sym_set_binding(symbol, CORE_STRING_STATIC("frontend.fact"),
                                 &other)
        && parse_sym_set_binding(symbol, CORE_STRING_STATIC("ir.shape"),
                                 &replacement);
    check("symbol bindings are namespaced and replaceable",
          stored
              && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("ir.shape")) == &replacement
              && parse_sym_binding(symbol,
                    CORE_STRING_STATIC("frontend.fact")) == &other
              && !parse_sym_binding(symbol,
                    CORE_STRING_STATIC("missing")));
    parse_symtab_free(symbols);
}

static void
test_symbol_binding_origins(void)
{
    parse_symtab_t *symbols = parse_symtab_new();
    parse_sym_entry_t *source = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("item"),
        PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_push_scope(symbols, CORE_STRING_STATIC(""),
                            CORE_STRING_STATIC("branch"));
    parse_sym_entry_t *shadow = parse_symtab_add(symbols,
        CORE_STRING_STATIC(""), CORE_STRING_STATIC("item"),
        PARSE_SYM_VARIABLE, nullptr);
    bool related = source && shadow && shadow->shadowed == source
        && parse_sym_set_binding_origin(shadow, source);
    check("semantic shadow views retain their storage origin",
          related && parse_sym_binding_origin(shadow) == source
              && !parse_sym_binding_origin(source)
              && !parse_sym_set_binding_origin(source, shadow));
    parse_symtab_free(symbols);
}

int
main(void)
{
    test_namespaced_symbol_bindings();
    test_symbol_binding_origins();
    return fails != 0;
}
