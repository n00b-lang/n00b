#include "scanner/tokenizer.h"
#include "scanner/lex.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"

#include <string.h>

typedef struct {
    core_string_t          name;
    parse_scan_cb_t         fn;
    const scan_lex_spec_t *spec;
} tokenizer_t;

core_list_decl(tokenizer_t);

static core_list_t(tokenizer_t) registry;
static bool                    seeded;

static void
seed(void)
{
    if (seeded) {
        return;
    }

    seeded   = true;
    registry = core_list_new(tokenizer_t);

    scan_tokenizers_register_builtin();
}

static tokenizer_t *
find(core_string_t name)
{
    for (size_t i = 0; i < core_list_len(registry); i++) {
        if (core_string_eq(registry.data[i].name, name)) {
            return &registry.data[i];
        }
    }

    return nullptr;
}

bool
scan_tokenizer_register(core_string_t name, parse_scan_cb_t fn)
{
    if (!name.data || !fn) {
        return false;
    }

    seed();

    tokenizer_t *have = find(name);

    if (have) {
        have->fn   = fn;
        have->spec = nullptr;

        return true;
    }

    tokenizer_t t = {.name = name, .fn = fn};

    core_list_push(registry, t);

    return true;
}

bool
scan_lex_register(const scan_lex_spec_t *spec)
{
    if (!spec || !spec->name.data) {
        return false;
    }

    seed();

    tokenizer_t *have = find(spec->name);

    if (have) {
        have->fn   = scan_lex_scan;
        have->spec = spec;

        return true;
    }

    tokenizer_t t = {.name = spec->name, .fn = scan_lex_scan, .spec = spec};

    core_list_push(registry, t);

    return true;
}

const scan_lex_spec_t *
scan_lex_for(core_string_t name)
{
    seed();

    tokenizer_t *have = name.data ? find(name) : nullptr;

    return have ? have->spec : nullptr;
}

parse_scan_cb_t
scan_tokenizer_for(core_string_t name)
{
    seed();

    if (!name.data) {
        return scan_tokenizer_for(
            CORE_STRING_STATIC(SCAN_TOKENIZER_DEFAULT));
    }

    tokenizer_t *have = find(name);

    return have ? have->fn : nullptr;
}

size_t
scan_tokenizer_count(void)
{
    seed();

    return core_list_len(registry);
}

core_string_t
scan_tokenizer_name_at(size_t i)
{
    seed();

    return i < core_list_len(registry) ? registry.data[i].name
                                      : (core_string_t){0};
}

void
scan_grammar_set_tokenizer(parse_grammar_t *g, core_string_t name)
{
    if (!g) {
        return;
    }

    g->tokenizer_name = name;

    scan_grammar_set_lex(g, scan_lex_for(name));
}

core_string_t
scan_grammar_tokenizer(parse_grammar_t *g)
{
    if (g && g->tokenizer_name.data) {
        return g->tokenizer_name;
    }

    return CORE_STRING_STATIC(SCAN_TOKENIZER_DEFAULT);
}

parse_scan_cb_t
scan_grammar_tokenizer_fn(parse_grammar_t *g)
{
    return scan_tokenizer_for(scan_grammar_tokenizer(g));
}

parse_token_stream_t *
scan_tokenize(parse_grammar_t  *g,
             const char     *src,
             size_t          len,
             core_buffer_t  **buf_out,
             parse_scanner_t **sc_out)
{
    scan_lex_t    *lex = scan_grammar_lex_compiled(g);
    parse_scan_cb_t fn  = lex ? scan_lex_scan : scan_grammar_tokenizer_fn(g);

    if (!fn || !src) {
        return nullptr;
    }

    core_buffer_t  *buf = core_buffer_from_bytes(src, (int64_t)len);
    parse_scanner_t *sc  = scan_scanner_new(buf, fn, g,
                                         core_option_none(core_string_t),
                                         lex,
                                         lex ? scan_lex_reset_state : nullptr);

    if (lex) {
        scan_lex_reset_state(sc);
    }

    if (buf_out) {
        *buf_out = buf;
    }

    if (sc_out) {
        *sc_out = sc;
    }

    return scan_token_stream_new(sc);
}
