#include "scanner/tokenizer.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"

#include <string.h>

typedef struct {
    ncc_string_t  name;
    ncc_scan_cb_t fn;
} tokenizer_t;

ncc_list_decl(tokenizer_t);

static ncc_list_t(tokenizer_t) registry;
static bool                    seeded;

static bool
scan_text(ncc_scanner_t *s)
{
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_emit(s, NCC_TOK_STRING_LIT, ncc_scan_string_double(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        if (ncc_scan_number(s, NCC_TOK_INTEGER, NCC_TOK_FLOAT)) {
            return true;
        }
    }

    if (ncc_unicode_is_id_start(c)) {
        ncc_option_t(ncc_string_t) id = ncc_scan_identifier(s);

        if (ncc_option_is_set(id)) {
            int64_t kw = ncc_scan_terminal_id(s, ncc_option_get(id));

            ncc_scan_emit(s,
                          kw != NCC_TOK_OTHER ? (int32_t)kw
                                              : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    ncc_codepoint_t next = ncc_scan_peek(s, 1);

    if (next > 0 && next < 128) {
        char two[3] = {(char)c, (char)next, 0};

        int64_t op = ncc_scan_terminal_id(s, ncc_string_borrow(two, 2));

        if (op != NCC_TOK_OTHER && op > 0xFF) {
            ncc_scan_advance_n(s, 2);
            ncc_scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);

    return true;
}

static void
seed(void)
{
    if (seeded) {
        return;
    }

    seeded   = true;
    registry = ncc_list_new(tokenizer_t);

    ncc_tokenizer_register(NCC_STRING_STATIC(NCC_TOKENIZER_DEFAULT),
                           scan_text);
    ncc_tokenizers_register_builtin();
}

bool
ncc_tokenizer_register(ncc_string_t name, ncc_scan_cb_t fn)
{
    if (!name.data || !fn) {
        return false;
    }

    seed();

    for (size_t i = 0; i < ncc_list_len(registry); i++) {
        if (ncc_string_eq(registry.data[i].name, name)) {
            registry.data[i].fn = fn;

            return true;
        }
    }

    tokenizer_t t = {.name = name, .fn = fn};

    ncc_list_push(registry, t);

    return true;
}

ncc_scan_cb_t
ncc_tokenizer_for(ncc_string_t name)
{
    seed();

    if (!name.data) {
        return ncc_tokenizer_for(
            NCC_STRING_STATIC(NCC_TOKENIZER_DEFAULT));
    }

    for (size_t i = 0; i < ncc_list_len(registry); i++) {
        if (ncc_string_eq(registry.data[i].name, name)) {
            return registry.data[i].fn;
        }
    }

    return nullptr;
}

size_t
ncc_tokenizer_count(void)
{
    seed();

    return ncc_list_len(registry);
}

ncc_string_t
ncc_tokenizer_name_at(size_t i)
{
    seed();

    return i < ncc_list_len(registry) ? registry.data[i].name
                                      : (ncc_string_t){0};
}

void
ncc_grammar_set_tokenizer(ncc_grammar_t *g, ncc_string_t name)
{
    if (g) {
        g->tokenizer_name = name;
    }
}

ncc_string_t
ncc_grammar_tokenizer(ncc_grammar_t *g)
{
    if (g && g->tokenizer_name.data) {
        return g->tokenizer_name;
    }

    return NCC_STRING_STATIC(NCC_TOKENIZER_DEFAULT);
}

ncc_scan_cb_t
ncc_grammar_tokenizer_fn(ncc_grammar_t *g)
{
    return ncc_tokenizer_for(ncc_grammar_tokenizer(g));
}

ncc_token_stream_t *
ncc_tokenize(ncc_grammar_t  *g,
             const char     *src,
             size_t          len,
             ncc_buffer_t  **buf_out,
             ncc_scanner_t **sc_out)
{
    ncc_scan_cb_t fn = ncc_grammar_tokenizer_fn(g);

    if (!fn || !src) {
        return nullptr;
    }

    ncc_buffer_t *buf = ncc_buffer_from_bytes(src, (int64_t)len);
    ncc_scanner_t *sc  = ncc_scanner_new(buf, fn, g,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);

    if (buf_out) {
        *buf_out = buf;
    }

    if (sc_out) {
        *sc_out = sc;
    }

    return ncc_token_stream_new(sc);
}
