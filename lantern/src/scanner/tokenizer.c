#include "scanner/tokenizer.h"
#include "scanner/lex.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_program.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    core_string_t          name;
    parse_scan_cb_t         fn;
    const scan_lex_spec_t *spec;
} tokenizer_t;

core_list_decl(tokenizer_t);

static core_list_t(tokenizer_t) registry;
static pthread_once_t          registry_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t         registry_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool      seeding_registry;

static void
initialize_registry(void)
{
    seeding_registry = true;
    registry = core_list_new(tokenizer_t);

    scan_tokenizers_register_builtin();
    seeding_registry = false;
}

static void
seed(void)
{
    if (!seeding_registry) {
        pthread_once(&registry_once, initialize_registry);
    }
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

    pthread_mutex_lock(&registry_lock);

    tokenizer_t *have = find(name);

    if (have) {
        have->fn   = fn;
        have->spec = nullptr;
        pthread_mutex_unlock(&registry_lock);

        return true;
    }

    tokenizer_t t = {.name = name, .fn = fn};

    core_list_push(registry, t);
    pthread_mutex_unlock(&registry_lock);

    return true;
}

bool
scan_lex_register(const scan_lex_spec_t *spec)
{
    if (!spec || !spec->name.data) {
        return false;
    }

    seed();

    pthread_mutex_lock(&registry_lock);

    tokenizer_t *have = find(spec->name);

    if (have) {
        have->fn   = scan_lex_scan;
        have->spec = spec;
        pthread_mutex_unlock(&registry_lock);

        return true;
    }

    tokenizer_t t = {.name = spec->name, .fn = scan_lex_scan, .spec = spec};

    core_list_push(registry, t);
    pthread_mutex_unlock(&registry_lock);

    return true;
}

const scan_lex_spec_t *
scan_lex_for(core_string_t name)
{
    seed();

    pthread_mutex_lock(&registry_lock);

    tokenizer_t *have = name.data ? find(name) : nullptr;
    const scan_lex_spec_t *spec = have ? have->spec : nullptr;

    pthread_mutex_unlock(&registry_lock);

    return spec;
}

parse_scan_cb_t
scan_tokenizer_for(core_string_t name)
{
    seed();

    if (!name.data) {
        return scan_tokenizer_for(
            CORE_STRING_STATIC(SCAN_TOKENIZER_DEFAULT));
    }

    pthread_mutex_lock(&registry_lock);
    tokenizer_t *have = find(name);
    parse_scan_cb_t fn = have ? have->fn : nullptr;

    pthread_mutex_unlock(&registry_lock);

    return fn;
}

size_t
scan_tokenizer_count(void)
{
    seed();

    pthread_mutex_lock(&registry_lock);
    size_t count = core_list_len(registry);
    pthread_mutex_unlock(&registry_lock);

    return count;
}

core_string_t
scan_tokenizer_name_at(size_t i)
{
    seed();

    pthread_mutex_lock(&registry_lock);
    core_string_t name = i < core_list_len(registry) ? registry.data[i].name
                                                     : (core_string_t){0};
    pthread_mutex_unlock(&registry_lock);

    return name;
}

void
scan_grammar_set_tokenizer(parse_grammar_t *g, core_string_t name)
{
    if (!g) {
        return;
    }

    g->tokenizer_name = name;

    const scan_lex_spec_t *spec = scan_lex_for(name);

    scan_lex_register_output_terminals(spec, g);
    scan_grammar_set_lex(g, spec);

    if (!spec) {
        g->lex_resolved = false;
    }
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
    scan_lex_t    *program = scan_grammar_lex_compiled(g);
    parse_scan_cb_t fn  = program ? scan_lex_scan : scan_grammar_tokenizer_fn(g);

    if (!fn || !src) {
        return nullptr;
    }

    core_buffer_t  *buf = core_buffer_from_bytes(src, (int64_t)len);
    scan_lex_session_t *session = program ? scan_lex_session_new(program)
                                          : nullptr;
    parse_scanner_t *sc  = scan_scanner_new(buf, fn, g,
                                         core_option_none(core_string_t),
                                         session,
                                         program ? scan_lex_reset_state : nullptr);

    if (program) {
        scan_scanner_take_state(sc, scan_lex_session_free);
        scan_scanner_set_state_clone(sc, scan_lex_session_clone);
        scan_scanner_set_state_equal(sc, scan_lex_session_equal);
        scan_scanner_set_state_shift(sc, scan_lex_session_shift);
        scan_scanner_set_compact_callback(sc, scan_lex_compact_state);
        if (program->spec->regions) {
            scan_scanner_set_finalize_callback(sc, scan_lex_finalize_regions);
        }
        scan_lex_reset_state(sc);
    }

    if (buf_out) {
        *buf_out = buf;
    }
    else {
        scan_scanner_take_buffer(sc);
    }

    if (sc_out) {
        *sc_out = sc;
    }

    parse_token_stream_t *ts = scan_token_stream_new(sc);

    if (!sc_out) {
        scan_token_stream_take_scanner(ts);
    }

    return ts;
}

parse_token_stream_t *
scan_tokenize_incremental(parse_grammar_t  *g,
                          core_buffer_t    **buf_out,
                          parse_scanner_t **sc_out)
{
    parse_scanner_t *scanner = nullptr;
    parse_token_stream_t *ts = scan_tokenize(g, "", 0, buf_out, &scanner);

    if (!ts) {
        return nullptr;
    }

    scanner->source_final = false;

    if (sc_out) {
        *sc_out = scanner;
    }
    else {
        scan_token_stream_take_scanner(ts);
    }

    return ts;
}

parse_token_stream_t *
scan_tokenize_owned(parse_grammar_t *g, const char *src, size_t len)
{
    return scan_tokenize(g, src, len, nullptr, nullptr);
}

parse_token_stream_t *
scan_tokenize_incremental_owned(parse_grammar_t *g)
{
    return scan_tokenize_incremental(g, nullptr, nullptr);
}
