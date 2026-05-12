// Phase 11 typed translation of resharp-c/tests/fuzz_compare.c.
//
// Runs the regex engine (multiline mode) against the fuzz corpus and writes
// data/fuzz/regex-crate/*-regex-crate.json for use by the upstream regex-crate
// fuzz target. Per § 7.5 + § 19a: types are translated (regex_t ->
// n00b_regex_t, match_range_t -> n00b_regex_match_t); data values are
// byte-identical oracles and must not be paraphrased.  External harness
// symbols (filesystem, JSON, bounded-thread runner, xalloc) remain forward-
// declared as `extern` pending the API sweep in a later phase.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/stat.h>

#include "test_regex_fuzz_compare.h"

#include "n00b.h"
#include "text/regex/regex.h"

// ---------------------------------------------------------------------------
// Externally-provided helpers (defined in sibling translation units / harness)
// ---------------------------------------------------------------------------

// Build a multiline regex from a NUL-terminated pattern. Returns nullptr on
// compile failure. Caller owns the returned handle and must free with
// regex_free().
extern n00b_regex_t *regex_build_multiline(const char *pattern);
extern void regex_free(n00b_regex_t *re);

// Iterate over all non-overlapping matches in `input` (byte view, length
// `input_len`). Allocates an array of `n00b_regex_match_t` of length
// `*out_count` (zero is allowed). Returns nullptr on UTF-8 decode failure or
// other error. Caller owns the returned buffer and must free() it.
extern n00b_regex_match_t *regex_find_iter(n00b_regex_t *re,
                                           const uint8_t *input,
                                           size_t input_len,
                                           size_t *out_count);

// Validate that `input`/`input_len` is well-formed UTF-8.
extern bool utf8_validate(const uint8_t *input, size_t input_len);

// JSON helpers — mirror serde_json::from_str / to_string for the two
// shapes used here.
extern fuzz_entry_t *fuzz_entries_from_json(const char *json,
                                            size_t json_len,
                                            size_t *out_count);
extern void fuzz_entries_free(fuzz_entry_t *entries, size_t count);

extern char *regex_crate_entries_to_json(const regex_crate_entry_t *entries,
                                         size_t count,
                                         size_t *out_len);

// Bounded execution: run `fn(ctx)` on a worker thread with an 8MiB stack and a
// 5-second timeout. On success stores the matches array into *out_matches /
// *out_count and returns true. On timeout, panic, or fn() returning nullptr,
// returns false and leaves *out_matches == nullptr / *out_count == 0.
// NOTE: The Rust code uses panic::catch_unwind + mpsc::recv_timeout. The C
// equivalent will be implemented atop pthreads in phase 2; the signature
// captures the same contract.
typedef n00b_regex_match_t *(*bounded_fn_t)(void *ctx, size_t *out_count);
extern bool bounded_run(bounded_fn_t fn,
                        void *ctx,
                        n00b_regex_match_t **out_matches,
                        size_t *out_count);

// Filesystem + alloc helpers. These mirror the resharp-c common/ surface and
// remain extern pending the API sweep onto n00b's runtime.
extern char *read_file_to_cstr(const char *path, size_t *out_len);
extern bool write_file(const char *path, const char *data, size_t len);
extern bool path_exists(const char *path);
extern bool create_dir_all(const char *path);
extern const char *path_basename(const char *path);
extern char *path_join(const char *a, const char *b);
extern void *xmalloc(size_t n);
extern void xfree(void *p);
extern char *xstrdup(const char *s);
extern size_t ckd_add_sz(size_t a, size_t b);
extern size_t ckd_add3_sz(size_t a, size_t b, size_t c);

#define PANIC_FMT(fmt, ...)                                                   \
    do {                                                                       \
        fprintf(stderr, "panic: " fmt "\n", __VA_ARGS__);                      \
        abort();                                                               \
    } while (0)

// CARGO_MANIFEST_DIR was provided by Cargo at compile time. Its C analogue is
// supplied by the build system as a -D define or extern global.
#ifndef CARGO_MANIFEST_DIR
extern const char *const CARGO_MANIFEST_DIR;
#endif

// ---------------------------------------------------------------------------
// regex_find_multiline
// ---------------------------------------------------------------------------

// Returns a heap-allocated array of match ranges (length *out_count), or
// nullptr on failure.Option<Vec<[usize; 2]>>: nullptr means
// "None", a non-null pointer (possibly with *out_count == 0) means "Some".
n00b_regex_match_t *regex_find_multiline(const char *pattern,
                                         const uint8_t *input,
                                         size_t input_len,
                                         size_t *out_count)
{
    *out_count = 0;
    n00b_regex_t *re = regex_build_multiline(pattern);
    if (re == nullptr) {
        return nullptr;
    }
    if (!utf8_validate(input, input_len)) {
        regex_free(re);
        return nullptr;
    }
    n00b_regex_match_t *matches = regex_find_iter(re, input, input_len, out_count);
    regex_free(re);
    return matches;
}

// ---------------------------------------------------------------------------
// bounded_run helper closure for record_regex_crate
// ---------------------------------------------------------------------------

typedef struct {
    char *pattern;          // owned, NUL-terminated
    uint8_t *input;         // owned, length = input_len
    size_t input_len;
} find_ctx_t;

static n00b_regex_match_t *find_thunk(void *vctx, size_t *out_count)
{
    find_ctx_t *ctx = (find_ctx_t *)vctx;
    return regex_find_multiline(ctx->pattern, ctx->input, ctx->input_len, out_count);
}

// ---------------------------------------------------------------------------
// fuzz_dir: <CARGO_MANIFEST_DIR>/../data/fuzz
// ---------------------------------------------------------------------------

// Returns a heap-allocated NUL-terminated path string. Caller owns it.
static char *fuzz_dir(void)
{
    const char *manifest = CARGO_MANIFEST_DIR;

    // Compute the parent of `manifest` by locating its basename and trimming.
    // path_basename returns a pointer into `manifest` past the final '/'.
    const char *base = path_basename(manifest);
    size_t parent_len = (size_t)(base - manifest);
    if (parent_len > 0 && manifest[parent_len - 1] == '/') {
        parent_len--; // drop the trailing slash
    }

    // Build a NUL-terminated parent string, then path_join("data/fuzz") onto it.
    char *parent = (char *)xmalloc(ckd_add_sz(parent_len, 1));
    memcpy(parent, manifest, parent_len);
    parent[parent_len] = '\0';

    char *out = path_join(parent, "data/fuzz");
    xfree(parent);
    return out;
}

// ---------------------------------------------------------------------------
// record_regex_crate
// ---------------------------------------------------------------------------

static char *strip_suffix_or_dup(const char *s, const char *suffix)
{
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (slen >= suflen && memcmp(s + slen - suflen, suffix, suflen) == 0) {
        size_t keep = slen - suflen;
        char *out = (char *)xmalloc(ckd_add_sz(keep, 1));
        memcpy(out, s, keep);
        out[keep] = '\0';
        return out;
    }
    return xstrdup(s);
}

void record_regex_crate(const char *filename)
{
    char *base = fuzz_dir();
    char *path = path_join(base, filename);

    if (!path_exists(path)) {
        fprintf(stderr, "skip: %s not found\n", path);
        xfree(path);
        xfree(base);
        return;
    }

    char *stem = strip_suffix_or_dup(filename, ".json");
    char *out_dir = path_join(base, "regex-crate");
    if (!create_dir_all(out_dir)) {
        PANIC_FMT("create_dir_all failed for %s", out_dir);
    }

    // format!("{}-regex-crate.json", stem)
    size_t stem_len = strlen(stem);
    static const char *const tail = "-regex-crate.json";
    size_t tail_len = strlen(tail);
    size_t fname_size = ckd_add3_sz(stem_len, tail_len, 1);
    char *fname = (char *)xmalloc(fname_size);
    memcpy(fname, stem, stem_len);
    memcpy(fname + stem_len, tail, tail_len);
    fname[stem_len + tail_len] = '\0';
    char *out_path = path_join(out_dir, fname);
    xfree(fname);

    size_t content_len = 0;
    char *content = read_file_to_cstr(path, &content_len);

    size_t entry_count = 0;
    fuzz_entry_t *entries = fuzz_entries_from_json(content, content_len, &entry_count);
    if (entries == nullptr) {
        PANIC_FMT("fuzz_entries_from_json failed for %s", path);
    }
    xfree(content);

    // calloc(0, ...) is allowed to return NULL or a unique pointer per ISO C.
    // The `&& entry_count != 0` clause below makes either outcome correct: an
    // empty corpus yields a NULL `results` pointer and the for-loop iterates
    // zero times. Do NOT "tighten" this to a plain NULL check — that would
    // spuriously panic on a legitimately empty corpus.
    regex_crate_entry_t *results =
        (regex_crate_entry_t *)calloc(entry_count, sizeof(regex_crate_entry_t));
    if (results == nullptr && entry_count != 0) {
        PANIC_FMT("calloc failed for %zu regex_crate_entry_t", entry_count);
    }
    size_t result_count = 0;
    size_t null_count = 0;

    for (size_t i = 0; i < entry_count; ++i) {
        const fuzz_entry_t *entry = &entries[i];

        // Move pattern + input bytes into a context owned by the worker thread.
        find_ctx_t *ctx = (find_ctx_t *)xmalloc(sizeof(find_ctx_t));
        size_t plen = strlen(entry->pattern);
        size_t pat_size = ckd_add_sz(plen, 1);
        ctx->pattern = (char *)xmalloc(pat_size);
        memcpy(ctx->pattern, entry->pattern, plen + 1);

        ctx->input_len = entry->input_len;
        // xmalloc rounds 0 up to 1, so we don't need a separate >0 branch.
        ctx->input = (uint8_t *)xmalloc(ctx->input_len);
        if (ctx->input_len > 0) {
            memcpy(ctx->input, entry->input, ctx->input_len);
        }

        n00b_regex_match_t *matches = nullptr;
        size_t match_count = 0;
        bool got = bounded_run(find_thunk, ctx, &matches, &match_count);

        // bounded_run takes ownership of ctx (it frees pattern/input/ctx).
        // NOTE: phase-2 must guarantee that ownership transfer.

        if (!got || matches == nullptr) {
            null_count += 1;
        }

        // Clone pattern + input into the result entry (Rust does entry.pattern.clone()).
        regex_crate_entry_t *r = &results[result_count++];
        r->pattern = (char *)xmalloc(pat_size);
        memcpy(r->pattern, entry->pattern, plen + 1);

        // Checked-add guards against an adversarial entry->input_len of
        // SIZE_MAX wrapping (size + 1) to 0 — a NUL-terminator OOB write
        // / heap-corruption primitive prior to this fix.
        r->input_len = entry->input_len;
        size_t in_size = ckd_add_sz(r->input_len, 1);
        r->input = (char *)xmalloc(in_size);
        // Mirror the line-271 guard: memcpy with NULL src is UB even when
        // n == 0, and fuzz_entries_from_json may legitimately return
        // entry->input == NULL for an empty-string input.
        if (r->input_len > 0) {
            memcpy(r->input, entry->input, r->input_len);
        }
        r->input[r->input_len] = '\0';

        r->has_matches = got && (matches != nullptr);
        r->matches = matches;
        r->match_count = match_count;

        if (((i + 1) % 5000) == 0) {
            fprintf(stderr, "  [%zu/%zu] null=%zu\n",
                    i + 1, entry_count, null_count);
        }
    }

    size_t json_len = 0;
    char *json = regex_crate_entries_to_json(results, result_count, &json_len);
    if (json == nullptr) {
        PANIC_FMT("regex_crate_entries_to_json failed for %s", out_path);
    }
    if (!write_file(out_path, json, json_len)) {
        PANIC_FMT("write_file failed for %s", out_path);
    }
    fprintf(stderr, "wrote %s (%zu entries, %zu null)\n",
            out_path, result_count, null_count);

    // Cleanup.
    xfree(json);
    for (size_t i = 0; i < result_count; ++i) {
        xfree(results[i].pattern);
        xfree(results[i].input);
        xfree(results[i].matches);
    }
    xfree(results);
    fuzz_entries_free(entries, entry_count);
    xfree(out_path);
    xfree(out_dir);
    xfree(stem);
    xfree(path);
    xfree(base);
}

// ---------------------------------------------------------------------------
// Test entry points (Rust: #[test] #[ignore] generated by fuzz_test! macro).
// NOTE: Rust's #[ignore] gate has no direct C analogue. These functions are
// exposed for whatever harness phase 2 plugs in (CTest, a custom main, etc.).
// ---------------------------------------------------------------------------

void fuzz_npm(void)            { record_regex_crate("npm-uniquePatterns.json"); }
void fuzz_pypi(void)           { record_regex_crate("pypi-uniquePatterns.json"); }
void fuzz_regexlib(void)       { record_regex_crate("internetSources-regExLib.json"); }
void fuzz_stackoverflow(void)  { record_regex_crate("internetSources-stackoverflow.json"); }
void fuzz_uniq(void)           { record_regex_crate("uniq-regexes-8.json"); }
