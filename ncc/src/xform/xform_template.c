// xform_template.c — Parameterized code template engine for ncc transforms.
//
// Pre-lexes fixed portions of C templates at registration time.
// At instantiation, only the dynamic $N argument strings are lexed,
// spliced into the pre-lexed token runs, and parsed as a unit.

#include "xform/xform_template.h"
#include "slay/pwz.h"
#include "internal/slay/grammar_internal.h"
#include "parsers/token_stream.h"
#include "core/alloc.h"
#include "core/buffer.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal: lex a string into a token pointer array
// ============================================================================

typedef struct {
    n00b_token_info_t **tokens;
    int32_t             count;
    int32_t             cap;
} token_array_t;

static void
token_array_push(token_array_t *ta, n00b_token_info_t *tok)
{
    if (ta->count >= ta->cap) {
        int32_t new_cap = ta->cap ? ta->cap * 2 : 32;
        n00b_token_info_t **new_arr
            = realloc(ta->tokens, (size_t)new_cap * sizeof(n00b_token_info_t *));
        ta->tokens = new_arr;
        ta->cap    = new_cap;
    }
    ta->tokens[ta->count++] = tok;
}

// Deep-copy a token so it survives scanner/stream cleanup.
// Follows the pattern from n00b_xform_clone (transform.c).
static n00b_token_info_t *
deep_copy_token(n00b_token_info_t *orig)
{
    n00b_token_info_t *copy = n00b_alloc(n00b_token_info_t);
    *copy = *orig;

    if (n00b_option_is_set(orig->value)) {
        n00b_string_t val = n00b_option_get(orig->value);
        if (val.data && val.u8_bytes > 0) {
            n00b_string_t val_copy
                = n00b_string_from_raw(val.data, (int64_t)val.u8_bytes);
            copy->value = n00b_option_set(n00b_string_t, val_copy);
        }
    }

    // Trivia not needed for template tokens — clear to avoid dangling refs.
    copy->leading_trivia  = NULL;
    copy->trailing_trivia = NULL;

    return copy;
}

// Lex source into a freshly allocated token array.
// Returns true on success, false if lexing produces zero tokens.
static bool
lex_to_token_array(const char      *source,
                   n00b_scan_cb_t   cb,
                   n00b_grammar_t  *grammar,
                   token_array_t   *out)
{
    out->tokens = NULL;
    out->count  = 0;
    out->cap    = 0;

    if (!source || source[0] == '\0') {
        return true; // empty source → empty array, that's fine
    }

    size_t src_len          = strlen(source);
    n00b_buffer_t *buf      = n00b_buffer_from_bytes(source, (int64_t)src_len);
    n00b_scanner_t *scanner = n00b_scanner_new(buf, cb, grammar,
                                                n00b_option_none(n00b_string_t),
                                                NULL, NULL);
    n00b_token_stream_t *ts = n00b_token_stream_new(scanner);

    n00b_token_info_t *tok;
    while ((tok = n00b_stream_next(ts)) != NULL) {
        token_array_push(out, deep_copy_token(tok));
    }

    n00b_token_stream_free(ts);
    n00b_scanner_free(scanner);

    return true;
}

static void
token_array_free(token_array_t *ta)
{
    // Tokens are n00b_alloc'd — not individually freed.
    free(ta->tokens);
    ta->tokens = NULL;
    ta->count  = 0;
    ta->cap    = 0;
}

// ============================================================================
// Registry init / free
// ============================================================================

void
n00b_template_registry_init(n00b_template_registry_t *reg,
                            n00b_grammar_t           *grammar,
                            n00b_scan_cb_t            tokenize)
{
    assert(reg && grammar && tokenize);
    n00b_dict_init(&reg->templates, n00b_hash_cstring, n00b_dict_cstr_eq);
    reg->grammar  = grammar;
    reg->tokenize = tokenize;
}

void
n00b_template_registry_free(n00b_template_registry_t *reg)
{
    if (!reg) {
        return;
    }

    // Walk the dict buckets and free compiled templates.
    n00b_dict_t *d = &reg->templates;

    for (size_t i = 0; i < d->capacity; i++) {
        if (d->buckets[i].state != _N00B_BUCKET_OCCUPIED) {
            continue;
        }

        n00b_compiled_template_t *ct = d->buckets[i].value;

        for (int32_t s = 0; s < ct->num_segments; s++) {
            free(ct->segments[s].tokens);
        }
        free(ct->segments);
        free(ct->name);
        free(ct->start_symbol);
        free(ct);
    }

    n00b_dict_free(&reg->templates);
}

// ============================================================================
// Template compilation (register)
// ============================================================================

bool
n00b_template_register(n00b_template_registry_t *reg,
                       const char               *name,
                       const char               *start_symbol,
                       const char               *template_text)
{
    assert(reg && name && start_symbol && template_text);

    // Scan template_text for $N slots.
    // Collect segments: each is (fixed text before slot, slot_index).

    // Temporary growable segment list.
    int32_t seg_cap  = 8;
    int32_t seg_cnt  = 0;
    n00b_template_segment_t *segs
        = malloc((size_t)seg_cap * sizeof(n00b_template_segment_t));

    int32_t max_slot   = -1;
    const char *p      = template_text;
    const char *run_st = p;

    while (*p) {
        if (*p == '$' && isdigit((unsigned char)p[1])) {
            // Flush the fixed text before this slot.
            size_t run_len = (size_t)(p - run_st);
            char *fixed    = malloc(run_len + 1);
            memcpy(fixed, run_st, run_len);
            fixed[run_len] = '\0';

            // Parse slot number.
            p++; // skip '$'
            int32_t slot = 0;
            while (isdigit((unsigned char)*p)) {
                slot = slot * 10 + (*p - '0');
                p++;
            }
            if (slot > max_slot) {
                max_slot = slot;
            }

            // Lex the fixed text.
            token_array_t ta;
            if (!lex_to_token_array(fixed, reg->tokenize, reg->grammar, &ta)) {
                free(fixed);
                free(segs);
                return false;
            }
            free(fixed);

            // Grow segment array if needed.
            if (seg_cnt >= seg_cap) {
                seg_cap *= 2;
                segs = realloc(segs, (size_t)seg_cap * sizeof(n00b_template_segment_t));
            }

            segs[seg_cnt++] = (n00b_template_segment_t){
                .tokens     = ta.tokens,
                .count      = ta.count,
                .slot_index = slot,
            };

            run_st = p;
        }
        else {
            p++;
        }
    }

    // Trailing text (no slot after it).
    if (p > run_st) {
        size_t run_len = (size_t)(p - run_st);
        char *fixed    = malloc(run_len + 1);
        memcpy(fixed, run_st, run_len);
        fixed[run_len] = '\0';

        token_array_t ta;
        if (!lex_to_token_array(fixed, reg->tokenize, reg->grammar, &ta)) {
            free(fixed);
            // Free already-collected segments.
            for (int32_t i = 0; i < seg_cnt; i++) {
                free(segs[i].tokens);
            }
            free(segs);
            return false;
        }
        free(fixed);

        if (seg_cnt >= seg_cap) {
            seg_cap *= 2;
            segs = realloc(segs, (size_t)seg_cap * sizeof(n00b_template_segment_t));
        }

        segs[seg_cnt++] = (n00b_template_segment_t){
            .tokens     = ta.tokens,
            .count      = ta.count,
            .slot_index = -1,
        };
    }

    // Build compiled template.
    n00b_compiled_template_t *ct = malloc(sizeof(n00b_compiled_template_t));
    ct->name         = strdup(name);
    ct->start_symbol = strdup(start_symbol);
    ct->segments     = segs;
    ct->num_segments = seg_cnt;
    ct->num_slots    = max_slot + 1;

    n00b_dict_put(&reg->templates, ct->name, ct);

    return true;
}

// ============================================================================
// Template instantiation
// ============================================================================

n00b_result_t(n00b_parse_tree_ptr_t)
n00b_template_instantiate(n00b_template_registry_t *reg,
                          const char               *name,
                          const char              **args,
                          int                       nargs)
{
    assert(reg && name);

    // 1. Look up compiled template.
    bool found = false;
    n00b_compiled_template_t *ct
        = n00b_dict_get(&reg->templates, (void *)name, &found);

    if (!found) {
        return n00b_result_err(n00b_parse_tree_ptr_t, N00B_TMPL_ERR_NOT_FOUND);
    }

    // 2. Check argument count.
    if (nargs != ct->num_slots) {
        return n00b_result_err(n00b_parse_tree_ptr_t, N00B_TMPL_ERR_ARG_COUNT);
    }

    // 3. Lex each argument string.
    token_array_t *arg_tokens = NULL;

    if (nargs > 0) {
        arg_tokens = calloc((size_t)nargs, sizeof(token_array_t));

        for (int i = 0; i < nargs; i++) {
            if (!lex_to_token_array(args[i], reg->tokenize,
                                    reg->grammar, &arg_tokens[i])) {
                // Cleanup already-lexed args.
                for (int j = 0; j < i; j++) {
                    token_array_free(&arg_tokens[j]);
                }
                free(arg_tokens);
                return n00b_result_err(n00b_parse_tree_ptr_t,
                                       N00B_TMPL_ERR_ARG_LEX);
            }
        }
    }

    // 4. Concatenate all tokens: fixed segments interleaved with arg tokens.
    token_array_t all = {0};

    for (int32_t s = 0; s < ct->num_segments; s++) {
        n00b_template_segment_t *seg = &ct->segments[s];

        // Append fixed tokens (deep-copy so each instantiation is independent).
        for (int32_t t = 0; t < seg->count; t++) {
            token_array_push(&all, deep_copy_token(seg->tokens[t]));
        }

        // Append arg tokens for this slot.
        if (seg->slot_index >= 0 && seg->slot_index < nargs) {
            token_array_t *at = &arg_tokens[seg->slot_index];
            for (int32_t t = 0; t < at->count; t++) {
                token_array_push(&all, deep_copy_token(at->tokens[t]));
            }
        }
    }

    // 5. Re-index all tokens sequentially (critical for PWZ memoization).
    for (int32_t i = 0; i < all.count; i++) {
        all.tokens[i]->index = i;
    }

    // 6. Build token stream from assembled array.
    n00b_token_stream_t *ts = n00b_token_stream_from_array(all.tokens, all.count);

    // 7. Temporarily override grammar default_start.
    n00b_string_t nt_str    = n00b_string_from_cstr(ct->start_symbol);
    n00b_nonterm_t *nt      = n00b_nonterm(reg->grammar, nt_str);
    int32_t saved_start     = reg->grammar->default_start;

    if (nt) {
        reg->grammar->default_start = (int32_t)n00b_nonterm_id(nt);
    }

    // 8. Parse.
    n00b_pwz_parser_t *parser = n00b_pwz_new(reg->grammar);
    reg->grammar->default_start = saved_start;

    bool ok = n00b_pwz_parse(parser, ts);

    n00b_result_t(n00b_parse_tree_ptr_t) ret;

    if (ok) {
        n00b_parse_tree_t *tree   = n00b_pwz_get_tree(parser);
        n00b_parse_tree_t *cloned = n00b_xform_clone(tree);
        n00b_xform_set_parent_pointers(cloned);
        ret = n00b_result_ok(n00b_parse_tree_ptr_t, cloned);
    }
    else {
        ret = n00b_result_err(n00b_parse_tree_ptr_t, N00B_TMPL_ERR_PARSE_FAILED);
    }

    // 9. Cleanup.
    n00b_pwz_free(parser);
    n00b_token_stream_free(ts);
    free(all.tokens);

    if (arg_tokens) {
        for (int i = 0; i < nargs; i++) {
            token_array_free(&arg_tokens[i]);
        }
        free(arg_tokens);
    }

    return ret;
}

int
n00b_template_slot_count(n00b_template_registry_t *reg,
                         const char               *name)
{
    assert(reg && name);

    bool found = false;
    n00b_compiled_template_t *ct
        = n00b_dict_get(&reg->templates, (void *)name, &found);

    if (!found) {
        return -1;
    }

    return ct->num_slots;
}
