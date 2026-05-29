// naudit-grammar-bake.c — build-time grammar baking tool (WP-018).
//
// Usage:
//     naudit-grammar-bake <bnf_path> <start_nt> <output.c>
//                         [<symbol_prefix> [<grammar_name>]]
//
// Parses <bnf_path> with the BNF metagrammar (the same ~1.5s PWZ parse
// naudit pays at runtime today), finalizes the resulting grammar, then
// emits C source to <output.c> that reconstructs an identical grammar at
// program startup WITHOUT re-parsing — see `slay/grammar_image.h`.
//
// This tool is the Phase-1 stand-in for an ncc literal form (e.g.
// `bnf"path"`): the ncc xform piece is deferred to a follow-up ncc-repo
// PR (WP-018 DF-EA), so the bake step is wired explicitly via a meson
// `custom_target` rather than triggered by a source-level literal.
//
// Like `n00b-static-init-helper`, this is a build-time HOST tool: it
// links libn00b for the grammar machinery but uses libc for argv/file
// I/O. It is NOT compiled into the shipping binary, so its libc use does
// not fall under the runtime n00b-api-guidelines bans.

#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "slay/grammar.h"
#include "slay/bnf.h"
#include "slay/grammar_image.h"

extern void n00b_init_simple(int argc, char *argv[]);
extern void n00b_shutdown(void);

static char *
slurp_file(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

    *len_out = (long)got;
    return buf;
}

int
main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <bnf_path> <start_nt> <output.c> "
                "[<symbol_prefix> [<grammar_name>]]\n",
                argv[0]);
        return 2;
    }

    const char *bnf_path      = argv[1];
    const char *start_nt      = argv[2];
    const char *output_path   = argv[3];
    const char *symbol_prefix = (argc > 4) ? argv[4]
                                           : "__naudit_static_grammar";
    const char *grammar_name  = (argc > 5) ? argv[5] : start_nt;

    // The baked grammar is for PWZ-only consumers (naudit). PWZ never
    // reads the LR0 tables and tolerates absent first-sets, and naudit's
    // runtime sets this same gate — so skip the heavy (and, on
    // c_ncc.bnf, multi-minute) first-set / left-corner / LR0 analysis in
    // finalize. The reconstructed grammar finalizes under the same gate,
    // keeping the build-time and runtime structures identical (WP-018
    // DF-EB / DF-EC).
    setenv("N00B_SLAY_SKIP_FINALIZE_ANALYSIS", "1", 1);

    long  bnf_len = 0;
    char *bnf_src = slurp_file(bnf_path, &bnf_len);
    if (!bnf_src) {
        fprintf(stderr, "naudit-grammar-bake: cannot read '%s'\n", bnf_path);
        return 3;
    }

    n00b_init_simple(argc, argv);

    n00b_string_t  *bnf_text = n00b_string_from_raw(bnf_src, (int64_t)bnf_len);
    n00b_string_t  *start_s  = n00b_string_from_cstr(start_nt);
    n00b_grammar_t *g        = n00b_grammar_new();

    if (!n00b_bnf_load(bnf_text, start_s, g)) {
        fprintf(stderr, "naudit-grammar-bake: failed to parse '%s'\n",
                bnf_path);
        n00b_shutdown();
        free(bnf_src);
        return 4;
    }

    // Finalize so the baked image captures the canonical post-finalize
    // structure (including error-recovery rules) that PWZ sees on the
    // first parse.
    n00b_grammar_finalize(g);

    n00b_result_t(n00b_string_t *) emit_r = n00b_grammar_image_emit(
        g,
        n00b_string_from_cstr(symbol_prefix),
        n00b_string_from_cstr(grammar_name));

    if (n00b_result_is_err(emit_r)) {
        n00b_string_t *why = n00b_grammar_image_emit_err_str(
            n00b_result_get_err(emit_r));
        fprintf(stderr, "naudit-grammar-bake: emit failed for '%s': %s\n",
                bnf_path, why->data);
        n00b_shutdown();
        free(bnf_src);
        return 5;
    }

    n00b_string_t *emitted = n00b_result_get(emit_r);

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "naudit-grammar-bake: cannot write '%s'\n",
                output_path);
        n00b_shutdown();
        free(bnf_src);
        return 6;
    }
    fwrite(emitted->data, 1, (size_t)emitted->u8_bytes, out);
    fclose(out);

    n00b_shutdown();
    free(bnf_src);
    return 0;
}
