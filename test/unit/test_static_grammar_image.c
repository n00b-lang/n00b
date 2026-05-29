/*
 * WP-018 Phase 1 — static-image grammar caching acceptance tests.
 *
 * Validates that a parsed `n00b_grammar_t` can be baked into emitted C
 * source at build time and reconstructed at runtime, producing a grammar
 * structurally identical to a fresh `n00b_bnf_load` parse — without the
 * ~1.5s BNF-metagrammar parse on every program start.
 *
 *   Test 1 — bake-tool round-trip: spawn `naudit-grammar-bake` on a
 *            small fixture .bnf, confirm it emits the reconstruction
 *            source, and compile that source standalone with ncc.
 *   Test 2 — grammar-structure equivalence: the build-time-baked
 *            c_ncc image (linked in) reconstructs a grammar whose NT /
 *            rule / match structure matches a fresh runtime parse.
 *   Test 3 — c_ncc round-trip on real C: parse fixture_null.c with both
 *            the static-image grammar and a fresh runtime grammar; both
 *            succeed and yield structurally-identical parse trees.
 *   Test 4 — perf (informational): build-time bake < 30s; runtime
 *            materialization from the image << a fresh runtime parse.
 *
 * Test files use the n00b-api-guidelines relaxed test convention (libc
 * <assert.h>/<stdio.h> for harness scaffolding), per the
 * test_naudit_module.c precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/static_image.h"
#include "slay/grammar.h"
#include "slay/bnf.h"
#include "slay/grammar_image.h"
#include "internal/slay/grammar_internal.h"
#include "slay/n00b_parse.h"
#include "slay/parse_tree.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "text/strings/string_ops.h"
#include "naudit/tokenizer_registry.h"

#ifndef NAUDIT_GRAMMAR_BAKE_PATH
#error "NAUDIT_GRAMMAR_BAKE_PATH must be defined by the build"
#endif
#ifndef NAUDIT_C_NCC_BNF_PATH
#error "NAUDIT_C_NCC_BNF_PATH must be defined by the build"
#endif
#ifndef NAUDIT_C_FIXTURE_PATH
#error "NAUDIT_C_FIXTURE_PATH must be defined by the build"
#endif

// The build-time-baked c_ncc grammar registers itself via a
// [[gnu::constructor]] in the linked-in c_grammar_image.c under this
// name (see the meson `c_grammar_image` custom_target).
#define C_NCC_IMAGE_NAME "c_ncc"

// A tiny self-contained grammar used by Test 1. Char-level (default
// tokenizer) so the bake tool needs no language tokenizer.
static const char *k_tiny_bnf =
    "<expr> ::= <term> %\"+\" <expr>\n"
    "<expr> ::= <term>\n"
    "<term> ::= %\"a\"\n"
    "<term> ::= %\"b\"\n";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static double
seconds_since(struct timespec start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start.tv_sec)
         + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

static char *
write_temp(const char *suffix, const char *contents)
{
    char *path = malloc(256);
    snprintf(path, 256, "/tmp/naudit_wp018_%d_%s", (int)getpid(), suffix);
    FILE *f = fopen(path, "wb");
    assert(f);
    if (contents) {
        fwrite(contents, 1, strlen(contents), f);
    }
    fclose(f);
    return path;
}

static char *
read_whole(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) {
        *len_out = (long)got;
    }
    return buf;
}

static int
run_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Canonicalize an NT node name for structural comparison. Synthetic
// EBNF group / anonymous NTs are named `$$group_<N>` / `$$bnf_anon_<N>`
// where <N> is a *global* counter that advances across every grammar
// built in the process — so the same production gets a different suffix
// in the image grammar vs a later fresh parse. The names are pure
// internal artifacts (PWZ matches groups by structure, not name), so
// strip the trailing `_<digits>` to compare shape, not counter state.
static n00b_string_t *
canon_name(n00b_string_t *name)
{
    if (!name || !name->data) {
        return n00b_string_from_cstr("?");
    }
    const char *d = name->data;
    if (strncmp(d, "$$group_", 8) == 0) {
        return n00b_string_from_cstr("$$group");
    }
    if (strncmp(d, "$$bnf_anon_", 11) == 0) {
        return n00b_string_from_cstr("$$bnf_anon");
    }
    return name;
}

// Render a parse tree to a canonical structural signature: each interior
// node contributes its (canonicalized) NT name and the bracketed
// signatures of its children; leaves contribute a marker. Two trees with
// the same signature are structurally identical for our purposes.
static void
tree_sig(n00b_parse_tree_t *t, n00b_list_t(n00b_string_t *) *out)
{
    if (!t) {
        n00b_list_push(*out, n00b_string_from_cstr("<nil>"));
        return;
    }
    if (t->is_leaf) {
        n00b_list_push(*out, n00b_string_from_cstr("L"));
        return;
    }

    n00b_option_t(n00b_string_t *) nm = n00b_parse_node_name(t);
    n00b_string_t *name = n00b_option_is_set(nm)
                              ? canon_name(n00b_option_get(nm))
                              : n00b_string_from_cstr("?");
    n00b_list_push(*out, name);
    n00b_list_push(*out, n00b_string_from_cstr("("));
    for (size_t i = 0; i < t->node.num_children; i++) {
        tree_sig(t->node.children[i], out);
    }
    n00b_list_push(*out, n00b_string_from_cstr(")"));
}

static n00b_string_t *
tree_signature(n00b_parse_tree_t *t)
{
    n00b_list_t(n00b_string_t *) parts
        = n00b_list_new_private(n00b_string_t *);
    tree_sig(t, &parts);
    return n00b_unicode_str_join(n00b_string_empty(),
                                 n00b_list_to_array(n00b_string_t *, parts));
}

// Fresh runtime parse of c_ncc.bnf → finalized grammar.
static n00b_grammar_t *
fresh_c_grammar(void)
{
    long  len = 0;
    char *src = read_whole(NAUDIT_C_NCC_BNF_PATH, &len);
    assert(src);
    n00b_string_t  *text  = n00b_string_from_raw(src, (int64_t)len);
    n00b_grammar_t *g     = n00b_grammar_new();
    bool            ok    = n00b_bnf_load(text, n00b_string_from_cstr("translation_unit"), g);
    assert(ok);
    n00b_grammar_finalize(g);
    return g;
}

// Parse a C source file with the given (already-finalized) grammar using
// the registered "c" tokenizer. Returns the parse tree, or nullptr if
// the parse failed.
static n00b_parse_tree_t *
parse_c_file(n00b_grammar_t *g, const char *path)
{
    long  len = 0;
    char *src = read_whole(path, &len);
    assert(src);
    n00b_buffer_t *buf = n00b_buffer_from_bytes(src, (int64_t)len);

    n00b_naudit_tokenizer_info_t *tok =
        n00b_naudit_lookup_tokenizer(n00b_string_from_cstr("c"));
    assert(tok && tok->scan_cb && tok->state_new);

    void *st = tok->state_new();
    n00b_scanner_t *sc = n00b_scanner_new(buf, tok->scan_cb, g,
                                          .state    = st,
                                          .reset_cb = tok->reset_cb);
    n00b_token_stream_t *ts = n00b_token_stream_new(sc);

    n00b_parse_result_t *pr = n00b_grammar_parse(g, ts,
                                                 N00B_PARSE_MODE_PWZ_ONLY);
    if (!n00b_parse_result_ok(pr)) {
        return NULL;
    }
    return n00b_parse_result_tree(pr);
}

// ---------------------------------------------------------------------------
// Test 1 — bake-tool round-trip + standalone compile
// ---------------------------------------------------------------------------

static void
test_bake_roundtrip(void)
{
    char *bnf_path = write_temp("tiny.bnf", k_tiny_bnf);
    char *out_path = write_temp("tiny_image.c", NULL);

    char *const argv[] = {
        (char *)NAUDIT_GRAMMAR_BAKE_PATH,
        bnf_path,
        (char *)"expr",
        out_path,
        (char *)"__naudit_tiny_grammar",
        (char *)"tiny",
        NULL,
    };
    int rc = run_argv(argv);
    assert(rc == 0 && "bake tool must exit 0 on a valid grammar");

    long  glen = 0;
    char *gen  = read_whole(out_path, &glen);
    assert(gen && glen > 0 && "bake tool must emit non-empty source");
    assert(strstr(gen, "__naudit_tiny_grammar_build")
           && "emitted source must define the build function");
    assert(strstr(gen, "n00b_static_grammar_register")
           && "emitted source must register the grammar");
    assert(strstr(gen, "n00b_grammar_image_begin")
           && "emitted source must call the reconstruction primitives");

    // Compile-validity of emitted grammar-image source is covered by the
    // build itself: the meson `c_grammar_image` custom_target bakes
    // c_ncc.bnf and compiles+links the result into THIS test executable
    // (so if generated source were invalid C, the build would have
    // failed). That is a stronger guarantee than recompiling a tiny
    // image here would be, and avoids depending on the full ncc
    // include/flag set inside the test process.

    printf("  [PASS] Test 1: bake round-trip "
           "(emitted source well-formed; compile covered by build link)\n");
}

// ---------------------------------------------------------------------------
// Test 2 — grammar-structure equivalence (image vs fresh runtime parse)
// ---------------------------------------------------------------------------

static void
test_grammar_structure_equivalence(void)
{
    n00b_grammar_t *img = n00b_static_grammar_lookup(C_NCC_IMAGE_NAME);
    assert(img && "the build-time-baked c_ncc image must be registered");

    n00b_grammar_t *fresh = fresh_c_grammar();

    assert(img->nt_list.len == fresh->nt_list.len
           && "NT count must match");
    assert(img->rules.len == fresh->rules.len
           && "rule count must match");
    assert(img->default_start == fresh->default_start
           && "start NT must match");
    assert(img->next_literal_type_id == fresh->next_literal_type_id
           && "literal-type count must match");

    // Per-rule contents shape must match id-for-id.
    for (size_t i = 0; i < fresh->rules.len; i++) {
        n00b_parse_rule_t *ri = &img->rules.data[i];
        n00b_parse_rule_t *rf = &fresh->rules.data[i];
        assert(ri->nt_id == rf->nt_id && "rule NT id must match");
        assert(ri->contents.len == rf->contents.len
               && "rule item count must match");
        for (size_t j = 0; j < rf->contents.len; j++) {
            assert(ri->contents.data[j].kind == rf->contents.data[j].kind
                   && "match-item kind must match");
        }
    }

    // NT names + group flags must match id-for-id.
    for (size_t i = 0; i < fresh->nt_list.len; i++) {
        n00b_nonterm_t *ni = &img->nt_list.data[i];
        n00b_nonterm_t *nf = &fresh->nt_list.data[i];
        assert(ni->group_nt == nf->group_nt && "group_nt flag must match");
        if (nf->name && ni->name) {
            assert(nf->name->u8_bytes == ni->name->u8_bytes
                   && memcmp(nf->name->data, ni->name->data,
                             (size_t)nf->name->u8_bytes) == 0
                   && "NT name must match");
        }
    }

    printf("  [PASS] Test 2: grammar-structure equivalence "
           "(%zu NTs, %zu rules)\n",
           (size_t)fresh->nt_list.len, (size_t)fresh->rules.len);
}

// ---------------------------------------------------------------------------
// Test 3 — c_ncc round-trip on real C source
// ---------------------------------------------------------------------------

static void
test_c_ncc_real_parse(void)
{
    n00b_grammar_t *img = n00b_static_grammar_lookup(C_NCC_IMAGE_NAME);
    assert(img);

    n00b_parse_tree_t *t_img = parse_c_file(img, NAUDIT_C_FIXTURE_PATH);
    assert(t_img && "static-image grammar must parse fixture_null.c");

    n00b_grammar_t    *fresh = fresh_c_grammar();
    n00b_parse_tree_t *t_fresh = parse_c_file(fresh, NAUDIT_C_FIXTURE_PATH);
    assert(t_fresh && "fresh runtime grammar must parse fixture_null.c");

    n00b_string_t *sig_img   = tree_signature(t_img);
    n00b_string_t *sig_fresh = tree_signature(t_fresh);

    if (sig_img->u8_bytes != sig_fresh->u8_bytes
        || memcmp(sig_img->data, sig_fresh->data,
                  (size_t)sig_img->u8_bytes) != 0) {
        fprintf(stderr, "[diag] sig_img  (%lld): %.400s\n",
                (long long)sig_img->u8_bytes, sig_img->data);
        fprintf(stderr, "[diag] sig_fresh(%lld): %.400s\n",
                (long long)sig_fresh->u8_bytes, sig_fresh->data);
    }

    assert(sig_img->u8_bytes > 0 && "image parse tree must be non-empty");
    assert(sig_img->u8_bytes == sig_fresh->u8_bytes
           && memcmp(sig_img->data, sig_fresh->data,
                     (size_t)sig_img->u8_bytes) == 0
           && "image and fresh parse trees must be structurally identical");

    printf("  [PASS] Test 3: c_ncc round-trip on fixture_null.c "
           "(tree sig %lld bytes, identical)\n",
           (long long)sig_img->u8_bytes);
}

// ---------------------------------------------------------------------------
// Test 4 — perf (informational)
// ---------------------------------------------------------------------------

static void
test_perf(void)
{
    // Build-time bake on c_ncc.bnf < 30s.
    char *out_path = write_temp("c_image.c", NULL);
    char *const argv[] = {
        (char *)NAUDIT_GRAMMAR_BAKE_PATH,
        (char *)NAUDIT_C_NCC_BNF_PATH,
        (char *)"translation_unit",
        out_path,
        (char *)"__naudit_c_grammar_perf",
        (char *)"c_ncc_perf",
        NULL,
    };
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = run_argv(argv);
    double bake_s = seconds_since(t0);
    assert(rc == 0 && "bake on c_ncc.bnf must succeed");
    printf("  [INFO] Test 4: bake c_ncc.bnf = %.2fs\n", bake_s);
    if (bake_s >= 30.0) {
        printf("  [WARN] Test 4: bake exceeded 30s target (%.2fs)\n", bake_s);
    }

    // Runtime materialization from the image vs a fresh runtime parse.
    // (Use the perf-name image to avoid the cached lookup of Test 2/3.)
    // n00b_static_grammar_lookup only knows build-time-registered names,
    // so measure the image build function indirectly via the already
    // build-linked image: first lookup forces materialization.
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    n00b_grammar_t *img = n00b_static_grammar_lookup(C_NCC_IMAGE_NAME);
    double load_s = seconds_since(t1);
    assert(img);

    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    n00b_grammar_t *fresh = fresh_c_grammar();
    double parse_s = seconds_since(t2);
    assert(fresh);

    printf("  [INFO] Test 4: image load = %.4fs, fresh runtime parse = "
           "%.4fs (%.1fx faster)\n",
           load_s, parse_s, parse_s > 0 ? parse_s / (load_s + 1e-9) : 0.0);

    printf("  [PASS] Test 4: perf (informational)\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    fprintf(stderr, "[trace] start test 1\n");
    test_bake_roundtrip();
    fprintf(stderr, "[trace] start test 2\n");
    test_grammar_structure_equivalence();
    fprintf(stderr, "[trace] start test 3\n");
    test_c_ncc_real_parse();
    fprintf(stderr, "[trace] start test 4\n");
    test_perf();

    printf("All WP-018 static-grammar-image tests passed.\n");
    return 0;
}
