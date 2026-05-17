/**
 * @file regex.h
 * @brief Engine-internal `Regex` type — the algebra-level compiled regex.
 *
 * Internal regex-engine header, not part of the public n00b surface.
 * The names here track the upstream Rust resharp engine closely; the
 * algorithmic vocabulary (`Regex`, `RegexInner`, `RegexOptions`, etc.)
 * stays un-prefixed because it is the regex-internal vocabulary.
 *
 * This is the **engine-level** `Regex` struct that lives behind n00b's
 * public-facing `n00b_regex_t` (Phase 9, `include/text/regex/regex.h`).
 * Both files keep their distinct names per § 6 of the port plan.
 *
 * The compiled regex owns:
 *   - a `RegexBuilder` (the algebra DAG and metadata caches);
 *   - one or more `LDFA` lazy DFAs (forward, ts-prefixed forward, ts-prefixed
 *     reverse, and a lazily-initialised `rev`);
 *   - an optional `BDFA` bounded DFA;
 *   - an optional `FwdDFA` follow-set automaton (the "FAS" hardened path);
 *   - an optional `PrefixKind` accelerator;
 *   - precomputed seek-state ids in `StreamInit`, lazy stream caches in
 *     `StreamCache`.
 *
 * Concurrency: the inner state is guarded by an `n00b_mutex_t`; the lazy
 * once-flags inside `StreamCache` use `n00b_atomic_*` primitives.
 *
 * Companion source: `src/text/regex/engine/regex.c`.
 */
#pragma once

#include "n00b.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h> // _Atomic(bool) once-flag fields (accessed via n00b_atomic_*)

#include "adt/list.h"
#include "adt/option.h"
#include "adt/result.h"

#include "core/mutex.h"
#include "core/pool.h"

#include "internal/regex/ids.h"
#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"
#include "internal/regex/accel.h"
#include "internal/regex/fas.h"

// ---------------------------------------------------------------------------
// Forward decls of opaque sibling-engine types (Phase 7 parallel).  Their
// real layouts live in their own headers; we only need pointer-typed
// references here for the `RegexInner` field declarations below.
// ---------------------------------------------------------------------------

typedef struct LDFA   LDFA;
typedef struct BDFA   BDFA;
typedef struct FwdDFA FwdDFA;

typedef struct PrefixKind PrefixKind;

// ---------------------------------------------------------------------------
// Regex error vocabulary.
//
// resharp-c had a tagged-union `Error *` over `ParseError`, `ResharpError`,
// capacity-exceeded, pattern-too-large, serialize.  In the n00b port the
// algebra and parser sub-systems already report fallibles via
// `n00b_result_t(...)`; the engine tracks its own errno-style return code
// (`n00b_regex_engine_err_t`, cast to `int` to live in `n00b_result_t.err`).
// ---------------------------------------------------------------------------

// `n00b_regex_engine_err_t` is the canonical enum defined in
// `internal/regex/fas.h` (included above).

/** @brief Static human-readable description for an engine err kind. */
[[nodiscard]] const char *n00b_regex_engine_err_str(int kind);

// ---------------------------------------------------------------------------
// UnicodeMode — same set as upstream.
// ---------------------------------------------------------------------------

typedef enum UnicodeMode {
    UNICODE_MODE_ASCII,
    UNICODE_MODE_DEFAULT,
    UNICODE_MODE_FULL,
    UNICODE_MODE_JAVASCRIPT,
} UnicodeMode;

// ---------------------------------------------------------------------------
// RegexOptions — engine-level options passed to `regex_with_options`.
// ---------------------------------------------------------------------------

typedef struct RegexOptions {
    size_t      max_dfa_capacity;
    uint32_t    lookahead_context_max;
    UnicodeMode unicode;
    bool        case_insensitive;
    bool        dot_matches_new_line;
    bool        multiline;
    bool        ignore_whitespace;
    bool        hardened;
    bool        unbounded_size;
} RegexOptions;

RegexOptions regex_options_default(void);
RegexOptions regex_options_unicode(RegexOptions self, UnicodeMode mode);
RegexOptions regex_options_case_insensitive(RegexOptions self, bool yes);
RegexOptions regex_options_dot_matches_new_line(RegexOptions self, bool yes);
RegexOptions regex_options_multiline(RegexOptions self, bool yes);
RegexOptions regex_options_ignore_whitespace(RegexOptions self, bool yes);
RegexOptions regex_options_hardened(RegexOptions self, bool yes);
RegexOptions regex_options_unbounded_size(RegexOptions self, bool yes);

// ---------------------------------------------------------------------------
// Streaming / seeking eagerly-precomputed init state.
//
// The full streaming surface (the seven `regex_stream_*` / `regex_seek_*`
// entry points) is owned by the `stream/` sibling subdir; this header
// only declares the cache shapes that are embedded into `RegexInner`
// and `Regex` so consumers in this TU and others can reference them
// without including the full streaming header.
// ---------------------------------------------------------------------------

typedef struct StreamInit {
    NodeId   start_node;
    uint32_t seek_fwd; /**< initial state id for fwd seek, post prune_begin_eps. */
    uint32_t seek_rev; /**< initial state id for rev seek, post prune_begin_eps. */
} StreamInit;

/**
 * @brief Lazy once-init caches for the streaming entry points.
 *
 * The `_inited` fields are atomic flags accessed via `n00b_atomic_*`
 * macros (acquire/release ordering); the slow-path lazy init takes the
 * `Regex.inner_lock` mutex.
 */
typedef struct StreamCache {
    _Atomic(bool)    fwd_prefix_inited;
    FwdPrefixSearch *fwd_prefix; /**< may be nullptr after init. */
    _Atomic(bool)    rev_inited;
} StreamCache;

/**
 * @brief Release any heap state owned by @p cache.
 *
 * Safe on a default-init `StreamCache` (everything zero/false).
 */
void stream_cache_drop(StreamCache *cache);

// ---------------------------------------------------------------------------
// RegexInner — guarded mutable state.
//
// Owned by `Regex` and reached only via `Regex.inner_lock`.  All scan
// kernels go through this struct; the lock serialises concurrent FFI
// callers per upstream's `Mutex<RegexInner>` contract.
// ---------------------------------------------------------------------------

typedef struct RegexInner {
    RegexBuilder       *b;
    LDFA               *fwd;
    LDFA               *fwd_ts;     /**< eagerly built TS-prefixed forward LDFA. */
    LDFA               *rev;        /**< lazily inited via stream_cache.rev_inited. */
    LDFA               *rev_ts;
    StreamInit          stream;
    n00b_list_t(size_t) *nulls;     /**< `Vec<usize>` per upstream. */
    n00b_list_t(Match)  *matches;   /**< `Vec<Match>` per upstream. */
    BDFA               *bounded;    /**< optional. */
    FwdDFA             *fas;        /**< optional. */
} RegexInner;

// ---------------------------------------------------------------------------
// Regex — engine-level compiled regex.
//
// The mutex is embedded by value (per D4: `n00b_mutex_t` for the inner
// state lock).  Once-flag fields in `stream_cache` use `n00b_atomic_*`.
// ---------------------------------------------------------------------------

typedef struct Regex {
    n00b_mutex_t    inner_lock;
    RegexInner     *inner;
    PrefixKind     *prefix;             /**< optional. */
    bool            has_fixed_length;
    uint32_t        fixed_length;
    bool            empty_nullable;
    bool            always_nullable;
    bool            is_empty_lang;
    bool            fwd_begin_anchored;
    bool            rev_trivial;
    Nullability     initial_nullability;
    bool            fwd_end_nullable;
    bool            hardened;
    bool            has_bounded_prefix;
    bool            has_bounded;
    uint8_t         lb_check_bytes;
    bool            fwd_lb_begin_nullable;
    bool            has_anchors;
    StreamCache     stream_cache;
    /**
     * Per-regex pool allocator.  When `pool_owned` is true, every
     * allocation made during compile/match for this Regex goes through
     * the pool instead of the runtime default arena.  This eliminates
     * GC triggers from regex compile/match (the AWS-keys 100k stress
     * path), avoiding the LTO-induced register-aliasing forwarding bug.
     *
     * The pool struct lives in the pinned `rt->system_pool` (NOT inline
     * in Regex) so its address is stable across GC compactions of
     * `Regex *r`.  Every regex-internal callee caches the pool
     * `n00b_allocator_t *` as a parameter; an inline pool would have
     * its address change every time the (movable) Regex relocates,
     * leaving those cached pointers dangling.
     */
    n00b_pool_t    *pool;
    bool            pool_owned;
} Regex;

// ---------------------------------------------------------------------------
// Compile / drop.
//
// `regex_new` and `regex_with_options` return nullptr on failure; the
// caller does not get the underlying error (use the FFI surface for that).
// `regex_from_node` reports a structured error in the result.
// ---------------------------------------------------------------------------

Regex *regex_new(const char *pattern);
Regex *regex_with_options(const char *pattern, RegexOptions opts);

/**
 * @brief Build a `Regex` over an already-parsed node, into caller-owned
 *        storage.  On error, returns the engine errno; on success, returns
 *        `N00B_REGEX_ENGINE_ERR_NONE` and writes the result through @p out.
 */
n00b_regex_engine_err_t regex_from_node(RegexBuilder *b, NodeId node,
                                         RegexOptions opts, Regex *out);

void regex_free(Regex *r);

// ---------------------------------------------------------------------------
// Read-only queries.
// ---------------------------------------------------------------------------

bool        has_simd(void);

/**
 * @brief Return a heap-owned escaped copy of @p text.  Buffer is GC-managed.
 */
char       *resharp_escape(const char *text);

/**
 * @brief Append the escaped form of @p text to a caller-owned growable buffer.
 *        Storage grows via the n00b allocator.
 */
void        resharp_escape_into(const char *text, char **out_buf,
                                size_t *out_len, size_t *out_cap);

bool        regex_is_hardened(const Regex *r);
bool        regex_is_fwd_begin_anchored(const Regex *r);
bool        regex_has_fwd_prefix(const Regex *r);

/**
 * @brief Return a static string identifying the selected `PrefixKind`
 *        variant ("AnchoredFwd" / "AnchoredFwdLb" / "AnchoredRev" /
 *        "PotentialStart"), or nullptr if no prefix accelerator was
 *        selected.
 */
const char *regex_prefix_kind_name(const Regex *r);

/**
 * @brief Report whether the compiled `Regex` has prefix-skip accelerators
 *        on the forward and/or reverse search.
 */
void        regex_has_accel(const Regex *r, bool *fwd_accel, bool *rev_accel);

/** @brief True iff the BDFA hot-path is configured for @p r. */
bool        regex_bdfa_stats_is_some(const Regex *r);

/**
 * @brief Heap-clone the reverse-null-state vector produced by `collect_rev`
 *        over @p input.  The returned list is GC-managed.
 */
n00b_list_t(size_t) *regex_collect_rev_nulls_debug(const Regex *r,
                                                    const uint8_t *input,
                                                    size_t input_len);

// ---------------------------------------------------------------------------
// Match-driving entry points.
//
// All of these are FFI-reachable: the `r` and `out_*` arguments are
// validated with `n00b_require` at the boundary.  They return an
// engine-errno code (cast to `n00b_regex_engine_err_t`); on success, the
// returned code is `N00B_REGEX_ENGINE_ERR_NONE`.
// ---------------------------------------------------------------------------

/**
 * @brief Fill @p out with every match in @p input.  @p out is cleared
 *        before being populated.
 */
n00b_regex_engine_err_t regex_find_all(Regex *r, const uint8_t *input,
                                        size_t input_len,
                                        n00b_list_t(Match) *out);

/** @brief Like `regex_find_all` but only writes the count to @p *out_count. */
n00b_regex_engine_err_t regex_count_all(Regex *r, const uint8_t *input,
                                         size_t input_len,
                                         size_t *out_count);

/** @brief Like `regex_find_all` but only writes the sum of match lengths. */
n00b_regex_engine_err_t regex_total_span(Regex *r, const uint8_t *input,
                                          size_t input_len,
                                          size_t *out_total);

/**
 * @brief Anchored find: returns at most one match starting at offset 0.
 *        On success, writes whether a match was found and the match itself.
 */
n00b_regex_engine_err_t regex_find_anchored(Regex *r, const uint8_t *input,
                                             size_t input_len,
                                             bool *found, Match *out_match);

/** @brief Boolean is-match.  Sets @p *out and returns the engine errno. */
n00b_regex_engine_err_t regex_is_match(Regex *r, const uint8_t *input,
                                        size_t input_len, bool *out);

// ---------------------------------------------------------------------------
// Internal callback used by the FAS scan (`fas.c`) to push matches into a
// type-erased `void *matches` parameter.  The payload is always a
// `n00b_list_t(Match) *`.
// ---------------------------------------------------------------------------

void matches_push(void *matches, size_t start, size_t end);
