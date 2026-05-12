/*
 * src/text/regex/stream/stream.c
 *
 * Faithful per-file translation of resharp-c's `src/engine/stream.c`
 * (which is itself the C port of upstream Rust `resharp::stream`), with
 * primitives migrated to n00b idioms:
 *
 *   - allocation: not used in this TU; the matches / ends containers
 *     are caller-managed `n00b_list_t(...)` per § 7.5.
 *   - vectors:   resharp-c's `MatchVec *` → `n00b_list_t(Match) *`;
 *                `UsizeVec *` → `n00b_list_t(size_t) *`.  Both lists are
 *                cleared in place at entry and pushed-to (private list
 *                ownership stays with the caller; the regex API created
 *                them with `n00b_list_new_private(...)` upstream).
 *   - mutex:     resharp-c's `RegexMutex *` wrapping `RegexInner *` →
 *                `n00b_mutex_t inner_lock` embedded by value on `Regex`,
 *                plus `Regex.inner` pointing at `RegexInner` directly.
 *   - atomics:   `_Atomic(bool)` once-flag fields on `StreamCache`
 *                accessed via `n00b_atomic_load` (acquire) /
 *                `n00b_atomic_store` (release) macros.
 *   - errors:    `Error *` chains → `n00b_result_t(int)` whose `err`
 *                side carries an `n00b_regex_engine_err_t` value cast to
 *                int.  Algebra fallibles funnel `n00b_regex_algebra_err_t`
 *                through the same int slot — converted to engine errno
 *                at the boundary via `algebra_err_to_engine`.
 *   - require:   resharp-c REQUIRE-the-macro → `n00b_require(...)` (D8).
 *
 * Engine-side const-generics (`<const STOP: bool, const REV: bool,
 * const PREFIX: bool>` in upstream Rust) are translated to runtime bool
 * flags forwarded into `stream_feed_loop` / `stream_general` /
 * `stream_anchored_fwd`.  Each branch is preserved exactly; no variant
 * elision.
 */

#include "n00b.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h> // memcpy / memset (D13) — not used directly below but kept for parity.

#include "core/thread.h" // for n00b_thread_id used by core/lock_common.h
#include "core/mutex.h"
#include "core/atomic.h"

#include "adt/list.h"
#include "adt/option.h"
#include "adt/result.h"

#include "util/assert.h"

#include "internal/regex/stream.h"
#include "internal/regex/regex.h"
#include "internal/regex/engine.h"
#include "internal/regex/accel.h"
#include "internal/regex/prefix.h"
#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"

// ---------------------------------------------------------------------------
// Internal callback type — start/end pair.
//
// The upstream Rust passed `Box<dyn FnMut(Match)>`; resharp-c translated
// it to a function-pointer + opaque ctx.  We keep that shape; n00b has
// no closure type at the C surface.
// ---------------------------------------------------------------------------

typedef void (*EmitMatchFn)(void *ctx, size_t start, size_t end);

// ---------------------------------------------------------------------------
// Funnel an algebra-side err code into an engine errno.  The result-int
// slot is shared between algebra and engine errs; the translation point
// is the same one used by the rest of the engine TUs.
// ---------------------------------------------------------------------------

static inline n00b_regex_engine_err_t algebra_err_to_engine(int e)
{
    switch ((n00b_regex_algebra_err_t)e) {
    case N00B_REGEX_ALGEBRA_ERR_NONE:                  return N00B_REGEX_ENGINE_ERR_NONE;
    case N00B_REGEX_ALGEBRA_ERR_ANCHOR_LIMIT:          return N00B_REGEX_ENGINE_ERR_UNSUPPORTED_PATTERN;
    case N00B_REGEX_ALGEBRA_ERR_STATE_SPACE_EXPLOSION: return N00B_REGEX_ENGINE_ERR_CAPACITY_EXCEEDED;
    case N00B_REGEX_ALGEBRA_ERR_UNSUPPORTED_PATTERN:   return N00B_REGEX_ENGINE_ERR_UNSUPPORTED_PATTERN;
    }
    return N00B_REGEX_ENGINE_ERR_ALGEBRA;
}

// ===========================================================================
// StreamState constructors / accessors
// ===========================================================================

StreamState stream_state_new(void)
{
    return (StreamState){ .state = (uint32_t)engine_DFA_INITIAL, .pos = 0 };
}

StreamState stream_state_at(size_t pos)
{
    return (StreamState){ .state = (uint32_t)engine_DFA_INITIAL, .pos = pos };
}

size_t stream_state_pos(const StreamState *s)
{
    return s->pos;
}

uint32_t stream_state_state(const StreamState *s)
{
    return s->state;
}

StreamState stream_state_from_raw(uint32_t state, size_t pos)
{
    return (StreamState){ .state = state, .pos = pos };
}

// ===========================================================================
// init helpers (lazy fwd_prefix / lazy rev LDFA) — once-flag pattern
//
// Both init paths take Regex.inner_lock on first call; subsequent calls
// short-circuit on the inited flags.  Outer (lock-free) reads use
// `n00b_atomic_load` (acquire) so observing `inited == true` synchronises
// with the release-store the initialiser performs while holding the
// inner mutex — pairing with Rust OnceLock's release/acquire semantics.
// ===========================================================================

static n00b_result_t(int) init_stream_fwd_only(Regex *r)
{
    if (n00b_atomic_load(&r->stream_cache.fwd_prefix_inited)) {
        return n00b_result_ok(int, 0);
    }
    n00b_mutex_lock(&r->inner_lock);
    // Re-check under the lock to avoid double-init on contended first call.
    if (n00b_atomic_load(&r->stream_cache.fwd_prefix_inited)) {
        n00b_mutex_unlock(&r->inner_lock);
        return n00b_result_ok(int, 0);
    }
    NodeId start_node = r->inner->stream.start_node;
    n00b_result_t(OptFwdPrefix) res = build_fwd_prefix(r->inner->b, start_node);
    if (n00b_result_is_err(res)) {
        n00b_mutex_unlock(&r->inner_lock);
        return n00b_result_err(int,
            (int)algebra_err_to_engine(n00b_result_get_err(res)));
    }
    OptFwdPrefix opt = n00b_result_get(res);
    r->stream_cache.fwd_prefix = opt.has_value ? opt.value : nullptr;
    n00b_atomic_store(&r->stream_cache.fwd_prefix_inited, true);
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(int, 0);
}

static n00b_result_t(int) init_stream(Regex *r)
{
    if (n00b_atomic_load(&r->stream_cache.fwd_prefix_inited)
        && n00b_atomic_load(&r->stream_cache.rev_inited)) {
        return n00b_result_ok(int, 0);
    }
    n00b_mutex_lock(&r->inner_lock);
    if (!n00b_atomic_load(&r->stream_cache.fwd_prefix_inited)) {
        NodeId start_node = r->inner->stream.start_node;
        n00b_result_t(OptFwdPrefix) res = build_fwd_prefix(r->inner->b, start_node);
        if (n00b_result_is_err(res)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(res)));
        }
        OptFwdPrefix opt = n00b_result_get(res);
        r->stream_cache.fwd_prefix = opt.has_value ? opt.value : nullptr;
        n00b_atomic_store(&r->stream_cache.fwd_prefix_inited, true);
    }
    if (!n00b_atomic_load(&r->stream_cache.rev_inited)) {
        NodeId start_node = r->inner->stream.start_node;

        n00b_result_t(NodeId) rev_r = regex_builder_reverse(r->inner->b, start_node);
        if (n00b_result_is_err(rev_r)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(rev_r)));
        }
        NodeId rev = n00b_result_get(rev_r);

        n00b_result_t(NodeId) strip_r = regex_builder_strip_lb(r->inner->b, rev);
        if (n00b_result_is_err(strip_r)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(strip_r)));
        }
        rev = n00b_result_get(strip_r);

        n00b_result_t(NodeId) norm_r = regex_builder_normalize_rev(r->inner->b, rev);
        if (n00b_result_is_err(norm_r)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(norm_r)));
        }
        rev = n00b_result_get(norm_r);

        size_t max_cap = r->inner->fwd->max_capacity;
        n00b_result_t(LDFA *) ldfa_r = engine_LDFA_new(r->inner->b, rev, max_cap);
        if (n00b_result_is_err(ldfa_r)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(ldfa_r)));
        }
        LDFA *rev_dfa = n00b_result_get(ldfa_r);

        // Upstream Rust holds the inner mutex across this assignment; a
        // racing init_stream observes the populated `inner.rev` after
        // re-acquiring.  We do the same.
        n00b_require(r->inner->rev == nullptr,
                     "init_stream: rev already set despite rev_inited=false");
        r->inner->rev = rev_dfa;
        n00b_atomic_store(&r->stream_cache.rev_inited, true);
    }
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// resolve_emit
//
// Returns true with *out_end populated when the (state, pos, hit_null)
// tuple resolves to a match; false otherwise.
// ===========================================================================

static bool resolve_emit(const LDFA *fwd, uint32_t state, size_t pos,
                         size_t end, bool hit_null, size_t *out_end)
{
    if (state <= (uint32_t)engine_DFA_DEAD) return false;
    Nullability mask;
    if (pos == end) {
        mask = NULLABILITY_END;
    } else if (hit_null) {
        mask = NULLABILITY_CENTER;
    } else {
        return false;
    }
    if (!engine_has_any_null(&fwd->effects_id, &fwd->effects, state, mask)) {
        return false;
    }
    size_t match_end = 0;
    engine_collect_max_fwd_pub(&fwd->effects_id, &fwd->effects, state, pos, mask,
                               &match_end);
    *out_end = (match_end == 0) ? pos : match_end;
    return true;
}

// ===========================================================================
// begin_search_start
//
// Used only for the AnchoredFwd path (no leading lookbehind): probe
// whether input[0..] already lies inside the language and, if so, return
// the shortest implied end position.
// ===========================================================================

static n00b_result_t(size_t) begin_search_start(Regex *r,
                                                 const uint8_t *input,
                                                 size_t input_len)
{
    n00b_mutex_lock(&r->inner_lock);
    LDFA *fwd = r->inner->fwd;
    uint8_t  mt  = fwd->mt_lookup[input[0]];
    uint32_t st0 = (uint32_t)fwd->begin_table.data[mt];
    if (st0 == (uint32_t)fwd->pruned || st0 <= (uint32_t)engine_DFA_DEAD) {
        n00b_mutex_unlock(&r->inner_lock);
        return n00b_result_ok(size_t, 0);
    }
    size_t end = input_len;
    n00b_result_t(EngineFirstNullOut) sr =
        engine_LDFA_scan_fwd_first_null_from(fwd, r->inner->b, st0, 1,
                                             input, input_len);
    if (n00b_result_is_err(sr)) {
        n00b_mutex_unlock(&r->inner_lock);
        return n00b_result_err(size_t,
            (int)algebra_err_to_engine(n00b_result_get_err(sr)));
    }
    EngineFirstNullOut sn = n00b_result_get(sr);
    size_t out;
    size_t emit_end;
    if (resolve_emit(fwd, sn.state, sn.pos, end, sn.hit_null, &emit_end)) {
        // Special case: if emit_end == 0, treat as 1 to ensure forward
        // progress (upstream-Rust quirk preserved).
        out = (emit_end == 0) ? 1 : emit_end;
    } else {
        out = 0;
    }
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(size_t, out);
}

// ===========================================================================
// stream_anchored_fwd
//
// Uses the AnchoredFwd[Lb] prefix accelerator to seek candidate
// positions, walks the prefix bytes through the forward LDFA to land in
// `body_state`, then emits matches via `scan_fwd_first_null` +
// `resolve_emit`.
// ===========================================================================

static n00b_result_t(int) stream_anchored_fwd(Regex *r,
                                               FwdPrefixSearch *fwd_prefix,
                                               size_t lb_len,
                                               size_t search_start,
                                               const uint8_t *input,
                                               size_t input_len,
                                               bool stop,
                                               EmitMatchFn emit, void *ctx,
                                               bool *out_stopped)
{
    n00b_mutex_lock(&r->inner_lock);
    LDFA  *fwd        = r->inner->fwd;
    size_t end        = input_len;
    size_t prefix_len = fwd_prefix_search_len(fwd_prefix);

    if (out_stopped) *out_stopped = false;

    for (;;) {
        n00b_option_t(size_t) fr = fwd_prefix_search_find_fwd(fwd_prefix,
                                                              input, input_len,
                                                              search_start);
        if (!n00b_option_is_set(fr)) break;
        size_t candidate = n00b_option_get(fr);

        uint32_t body_state;
        size_t   body_pos;
        if (lb_len > 0) {
            body_state = (uint32_t)engine_DFA_INITIAL;
            body_pos   = candidate + lb_len;
        } else {
            n00b_result_t(uint32_t) wr =
                engine_LDFA_walk_input(fwd, r->inner->b, candidate, prefix_len,
                                       input, input_len);
            if (n00b_result_is_err(wr)) {
                n00b_mutex_unlock(&r->inner_lock);
                return n00b_result_err(int,
                    (int)algebra_err_to_engine(n00b_result_get_err(wr)));
            }
            body_state = n00b_result_get(wr);
            body_pos   = candidate + prefix_len;
        }
        if (body_state == 0) {
            search_start = candidate + 1;
            continue;
        }

        uint32_t state   = body_state;
        size_t   pos     = body_pos;
        size_t   end_pos = 0;
        bool     emitted = false;

        for (;;) {
            n00b_result_t(EngineFirstNullOut) sr =
                engine_LDFA_scan_fwd_first_null_from(fwd, r->inner->b, state, pos,
                                                     input, input_len);
            if (n00b_result_is_err(sr)) {
                n00b_mutex_unlock(&r->inner_lock);
                return n00b_result_err(int,
                    (int)algebra_err_to_engine(n00b_result_get_err(sr)));
            }
            EngineFirstNullOut sn = n00b_result_get(sr);
            size_t got;
            if (resolve_emit(fwd, sn.state, sn.pos, end, sn.hit_null, &got)) {
                end_pos = got;
                emitted = true;
                break;
            }
            if (sn.hit_null && sn.pos < end) {
                uint32_t mt = (uint32_t)fwd->mt_lookup[input[sn.pos]];
                n00b_result_t(uint32_t) lt =
                    engine_LDFA_lazy_transition(fwd, r->inner->b,
                                                (uint32_t)sn.state, mt);
                if (n00b_result_is_err(lt)) {
                    n00b_mutex_unlock(&r->inner_lock);
                    return n00b_result_err(int,
                        (int)algebra_err_to_engine(n00b_result_get_err(lt)));
                }
                uint32_t nxt = n00b_result_get(lt);
                if (nxt <= (uint32_t)engine_DFA_DEAD) break;
                state = nxt;
                pos   = sn.pos + 1;
                continue;
            }
            break;
        }

        if (emitted) {
            size_t m_start = candidate + lb_len;
            emit(ctx, m_start, end_pos);
            if (stop) {
                if (out_stopped) *out_stopped = true;
                n00b_mutex_unlock(&r->inner_lock);
                return n00b_result_ok(int, 0);
            }
            search_start = (end_pos == m_start) ? (m_start + 1) : end_pos;
        } else {
            search_start = candidate + 1;
        }
    }
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// try_emit_step
//
// Returns ok with *out_emitted set when (mask, state) lands on a match
// and emit() was invoked.  REV=true triggers a reverse-LDFA scan to
// recover the match start.
// ===========================================================================

static n00b_result_t(int) try_emit_step(RegexInner *inner, bool REV,
                                         const uint8_t *input, size_t input_len,
                                         size_t pos, Nullability mask,
                                         uint32_t state,
                                         size_t *last_match_end,
                                         EmitMatchFn emit, void *ctx,
                                         bool *out_emitted)
{
    *out_emitted = false;
    LDFA *dfa = inner->fwd_ts;
    if (!engine_has_any_null(&dfa->effects_id, &dfa->effects, state, mask)) {
        return n00b_result_ok(int, 0);
    }
    size_t match_end = 0;
    engine_collect_max_fwd_pub(&dfa->effects_id, &dfa->effects, state, pos, mask,
                               &match_end);
    if (match_end == 0) match_end = pos;

    size_t m_start;
    if (REV) {
        n00b_require(inner->rev != nullptr,
                     "try_emit_step: REV path without rev LDFA inited");
        n00b_result_t(size_t) rr =
            engine_LDFA_scan_rev_from(inner->rev, inner->b, match_end,
                                      *last_match_end, input, input_len);
        if (n00b_result_is_err(rr)) {
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(rr)));
        }
        size_t s = n00b_result_get(rr);
        *last_match_end = match_end;
        m_start = (s == engine_NO_MATCH) ? match_end : s;
    } else {
        m_start = match_end;
    }
    emit(ctx, m_start, match_end);
    *out_emitted = true;
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// stream_feed_loop
//
// Upstream Rust const-generics `<const REV: bool, const PREFIX: bool,
// const STOP: bool>` → runtime flags.  Returns the resume state via
// *out_resume.
// ===========================================================================

static n00b_result_t(int) stream_feed_loop(RegexInner *inner,
                                            bool REV, bool PREFIX, bool STOP,
                                            FwdPrefixSearch *fwd_prefix,
                                            const uint8_t *input,
                                            size_t input_len,
                                            size_t pos_in, uint32_t init_state,
                                            size_t *last_match_end,
                                            EmitMatchFn emit, void *ctx,
                                            uint32_t *out_resume)
{
    size_t end = input_len;
    uint32_t state = init_state;
    size_t pos = pos_in;
    LDFA *dfa = inner->fwd_ts;

    while (pos < end) {
        if (PREFIX && state == (uint32_t)engine_DFA_INITIAL) {
            if (fwd_prefix != nullptr) {
                n00b_option_t(size_t) fr =
                    fwd_prefix_search_find_fwd(fwd_prefix, input, input_len, pos);
                if (n00b_option_is_set(fr)) {
                    pos = n00b_option_get(fr);
                } else {
                    break;
                }
            }
        }
        if (!PREFIX) {
            uint8_t sid = (state < dfa->skip_ids.len)
                              ? dfa->skip_ids.data[state]
                              : 0;
            if (sid != 0) {
                const MintermSearchValue *searcher =
                    &dfa->skip_searchers.data[sid - 1];
                n00b_option_t(size_t) msr =
                    minterm_search_value_find_fwd(searcher, input + pos,
                                                  end - pos);
                if (n00b_option_is_set(msr)) {
                    pos += n00b_option_get(msr);
                } else {
                    break;
                }
                if (pos >= end) break;
            }
        }

        uint32_t mt = (uint32_t)dfa->mt_lookup[input[pos]];
        n00b_result_t(uint32_t) lt =
            engine_LDFA_lazy_transition(dfa, inner->b, state, mt);
        if (n00b_result_is_err(lt)) {
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(lt)));
        }
        uint32_t next = n00b_result_get(lt);
        pos += 1;
        if (next == (uint32_t)engine_DFA_DEAD) {
            state = (uint32_t)engine_DFA_INITIAL;
            continue;
        }
        Nullability mask = (pos < end) ? NULLABILITY_CENTER : NULLABILITY_END;
        bool emitted;
        n00b_result_t(int) er =
            try_emit_step(inner, REV, input, input_len, pos, mask, next,
                          last_match_end, emit, ctx, &emitted);
        if (n00b_result_is_err(er)) return er;
        if (STOP && emitted) {
            *out_resume = (uint32_t)engine_DFA_INITIAL;
            return n00b_result_ok(int, 0);
        }
        state = emitted ? (uint32_t)engine_DFA_INITIAL : next;
    }
    *out_resume = state;
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// stream_general
//
// One-shot driver: handles the first byte (which uses begin_table rather
// than lazy_transition), then enters stream_feed_loop with the resulting
// state.
// ===========================================================================

static n00b_result_t(int) stream_general(RegexInner *inner, bool REV, bool STOP,
                                          FwdPrefixSearch *fwd_prefix,
                                          const uint8_t *input,
                                          size_t input_len,
                                          EmitMatchFn emit, void *ctx)
{
    size_t end = input_len;
    LDFA  *dfa = inner->fwd_ts;
    uint32_t mt0   = (uint32_t)dfa->mt_lookup[input[0]];
    uint32_t first = (uint32_t)dfa->begin_table.data[mt0];
    size_t pos = 1;
    Nullability mask = (pos < end) ? NULLABILITY_CENTER : NULLABILITY_END;
    size_t last_match_end = 0;
    bool   emitted;

    n00b_result_t(int) er =
        try_emit_step(inner, REV, input, input_len, pos, mask, first,
                      &last_match_end, emit, ctx, &emitted);
    if (n00b_result_is_err(er)) return er;
    if (STOP && emitted) return n00b_result_ok(int, 0);
    uint32_t state = emitted ? (uint32_t)engine_DFA_INITIAL : first;
    uint32_t resume_unused;
    return stream_feed_loop(inner, REV, /*PREFIX=*/true, STOP,
                            fwd_prefix, input, input_len, pos, state,
                            &last_match_end, emit, ctx, &resume_unused);
}

// ===========================================================================
// stream_with_inner — common driver for stream / stream_with / stream_first
// ===========================================================================

static n00b_result_t(int) stream_with_inner(Regex *r,
                                             const uint8_t *input,
                                             size_t input_len,
                                             bool STOP,
                                             EmitMatchFn emit, void *ctx)
{
    if (input_len == 0) {
        if (r->empty_nullable) emit(ctx, 0, 0);
        return n00b_result_ok(int, 0);
    }
    n00b_result_t(int) ir = init_stream(r);
    if (n00b_result_is_err(ir)) return ir;

    if (r->prefix) {
        PrefixKindTag t = prefix_kind_tag(r->prefix);
        if (t == PREFIX_KIND_ANCHORED_FWD) {
            FwdPrefixSearch *fp = prefix_kind_fwd_search(r->prefix);
            n00b_require(fp != nullptr,
                         "stream_with_inner: AnchoredFwd missing fwd search");
            n00b_result_t(size_t) sr = begin_search_start(r, input, input_len);
            if (n00b_result_is_err(sr)) {
                return n00b_result_err(int, n00b_result_get_err(sr));
            }
            size_t search_start = n00b_result_get(sr);
            if (search_start > 0) {
                emit(ctx, 0, search_start);
                if (STOP) return n00b_result_ok(int, 0);
            }
            bool stopped = false;
            return stream_anchored_fwd(r, fp, /*lb_len=*/0, search_start,
                                       input, input_len, STOP, emit, ctx,
                                       &stopped);
        }
        if (t == PREFIX_KIND_ANCHORED_FWD_LB) {
            if (!r->fwd_lb_begin_nullable) {
                FwdPrefixSearch *fp = prefix_kind_fwd_search(r->prefix);
                n00b_require(fp != nullptr,
                             "stream_with_inner: AnchoredFwdLb missing fwd search");
                size_t lb_len = (size_t)r->lb_check_bytes;
                bool stopped = false;
                return stream_anchored_fwd(r, fp, lb_len, 0,
                                           input, input_len, STOP, emit, ctx,
                                           &stopped);
            }
            // begin-nullable LBs fall through to stream_general.
        }
        // ANCHORED_REV / POTENTIAL_START fall through.
    }
    FwdPrefixSearch *fwd_prefix = r->stream_cache.fwd_prefix;
    n00b_mutex_lock(&r->inner_lock);
    n00b_result_t(int) res = stream_general(r->inner, /*REV=*/true, STOP,
                                             fwd_prefix, input, input_len,
                                             emit, ctx);
    n00b_mutex_unlock(&r->inner_lock);
    return res;
}

// ===========================================================================
// regex_stream / regex_stream_with / regex_stream_first
// ===========================================================================

typedef struct PushMatchCtx {
    n00b_list_t(Match) *vec;
} PushMatchCtx;

static void push_match_cb(void *ctx, size_t start, size_t end)
{
    PushMatchCtx *pc = (PushMatchCtx *)ctx;
    n00b_list_push(*pc->vec, ((Match){ .start = start, .end = end }));
}

n00b_result_t(int) regex_stream(Regex *r, const uint8_t *input, size_t len,
                                 n00b_list_t(Match) *out)
{
    n00b_require(out != nullptr, "regex_stream: out must not be NULL");
    n00b_list_clear(*out);
    PushMatchCtx pc = { .vec = out };
    return stream_with_inner(r, input, len, /*STOP=*/false,
                             push_match_cb, &pc);
}

typedef struct UserMatchCtx {
    void (*on_match)(void *ctx, Match m);
    void *user_ctx;
} UserMatchCtx;

static void user_match_cb(void *ctx, size_t start, size_t end)
{
    UserMatchCtx *uc = (UserMatchCtx *)ctx;
    uc->on_match(uc->user_ctx, (Match){ .start = start, .end = end });
}

n00b_result_t(int) regex_stream_with(Regex *r, const uint8_t *input, size_t len,
                                      void (*on_match)(void *ctx, Match m),
                                      void *ctx)
{
    UserMatchCtx uc = { .on_match = on_match, .user_ctx = ctx };
    return stream_with_inner(r, input, len, /*STOP=*/false,
                             user_match_cb, &uc);
}

typedef struct FirstMatchCtx {
    bool   found;
    Match  m;
} FirstMatchCtx;

static void first_match_cb(void *ctx, size_t start, size_t end)
{
    FirstMatchCtx *fc = (FirstMatchCtx *)ctx;
    if (!fc->found) {
        fc->found = true;
        fc->m = (Match){ .start = start, .end = end };
    }
}

n00b_result_t(int) regex_stream_first(Regex *r, const uint8_t *input, size_t len,
                                       bool *found, Match *out_match)
{
    n00b_require(found     != nullptr, "regex_stream_first: found must not be NULL");
    n00b_require(out_match != nullptr, "regex_stream_first: out_match must not be NULL");
    FirstMatchCtx fc = { .found = false, .m = {} };
    n00b_result_t(int) er =
        stream_with_inner(r, input, len, /*STOP=*/true, first_match_cb, &fc);
    if (n00b_result_is_err(er)) return er;
    *found = fc.found;
    if (fc.found) *out_match = fc.m;
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// regex_stream_ends / regex_stream_ends_with
// ===========================================================================

typedef struct EndOnlyCtx {
    void (*on_end)(void *ctx, size_t end);
    void *user_ctx;
} EndOnlyCtx;

static void end_only_cb(void *ctx, size_t start, size_t end)
{
    (void)start;
    EndOnlyCtx *ec = (EndOnlyCtx *)ctx;
    ec->on_end(ec->user_ctx, end);
}

static n00b_result_t(int) stream_ends_with_inner(Regex *r,
                                                  const uint8_t *input,
                                                  size_t input_len,
                                                  EmitMatchFn emit, void *ctx)
{
    if (input_len == 0) {
        if (r->empty_nullable) emit(ctx, 0, 0);
        return n00b_result_ok(int, 0);
    }
    n00b_result_t(int) ir = init_stream_fwd_only(r);
    if (n00b_result_is_err(ir)) return ir;

    if (r->prefix) {
        PrefixKindTag t = prefix_kind_tag(r->prefix);
        if (t == PREFIX_KIND_ANCHORED_FWD) {
            FwdPrefixSearch *fp = prefix_kind_fwd_search(r->prefix);
            n00b_require(fp != nullptr,
                         "stream_ends_with_inner: AnchoredFwd missing fwd search");
            n00b_result_t(size_t) sr = begin_search_start(r, input, input_len);
            if (n00b_result_is_err(sr)) {
                return n00b_result_err(int, n00b_result_get_err(sr));
            }
            size_t search_start = n00b_result_get(sr);
            if (search_start > 0) emit(ctx, 0, search_start);
            bool stopped = false;
            return stream_anchored_fwd(r, fp, /*lb_len=*/0, search_start,
                                       input, input_len, /*stop=*/false,
                                       emit, ctx, &stopped);
        }
        if (t == PREFIX_KIND_ANCHORED_FWD_LB) {
            if (!r->fwd_lb_begin_nullable) {
                FwdPrefixSearch *fp = prefix_kind_fwd_search(r->prefix);
                n00b_require(fp != nullptr,
                             "stream_ends_with_inner: AnchoredFwdLb missing fwd search");
                size_t lb_len = (size_t)r->lb_check_bytes;
                bool stopped = false;
                return stream_anchored_fwd(r, fp, lb_len, 0,
                                           input, input_len, /*stop=*/false,
                                           emit, ctx, &stopped);
            }
        }
    }
    FwdPrefixSearch *fwd_prefix = r->stream_cache.fwd_prefix;
    n00b_mutex_lock(&r->inner_lock);
    n00b_result_t(int) res = stream_general(r->inner, /*REV=*/false, /*STOP=*/false,
                                             fwd_prefix, input, input_len,
                                             emit, ctx);
    n00b_mutex_unlock(&r->inner_lock);
    return res;
}

typedef struct PushUsizeCtx {
    n00b_list_t(size_t) *vec;
} PushUsizeCtx;

static void push_usize_match_cb(void *ctx, size_t start, size_t end)
{
    (void)start;
    PushUsizeCtx *pc = (PushUsizeCtx *)ctx;
    n00b_list_push(*pc->vec, end);
}

n00b_result_t(int) regex_stream_ends(Regex *r, const uint8_t *input, size_t len,
                                      n00b_list_t(size_t) *out)
{
    n00b_require(out != nullptr, "regex_stream_ends: out must not be NULL");
    n00b_list_clear(*out);
    PushUsizeCtx pc = { .vec = out };
    return stream_ends_with_inner(r, input, len, push_usize_match_cb, &pc);
}

n00b_result_t(int) regex_stream_ends_with(Regex *r, const uint8_t *input, size_t len,
                                           void (*on_end)(void *ctx, size_t end),
                                           void *ctx)
{
    EndOnlyCtx ec = { .on_end = on_end, .user_ctx = ctx };
    return stream_ends_with_inner(r, input, len, end_only_cb, &ec);
}

// ===========================================================================
// regex_stream_chunk
// ===========================================================================

typedef struct ChunkCtx {
    void (*on_end)(void *ctx, size_t end);
    void *user_ctx;
    size_t offset;
} ChunkCtx;

static void chunk_emit_cb(void *ctx, size_t start, size_t end)
{
    (void)start;
    ChunkCtx *cc = (ChunkCtx *)ctx;
    cc->on_end(cc->user_ctx, cc->offset + end);
}

n00b_result_t(int) regex_stream_chunk(Regex *r, const uint8_t *chunk,
                                       size_t chunk_len,
                                       StreamState in_state,
                                       StreamState *out_state,
                                       void (*on_end)(void *ctx, size_t end),
                                       void *ctx)
{
    n00b_require(out_state != nullptr, "regex_stream_chunk: out_state must not be NULL");
    n00b_result_t(int) ir = init_stream_fwd_only(r);
    if (n00b_result_is_err(ir)) return ir;

    FwdPrefixSearch *fwd_prefix = r->stream_cache.fwd_prefix;
    n00b_mutex_lock(&r->inner_lock);
    size_t offset = in_state.pos;
    ChunkCtx cc = { .on_end = on_end, .user_ctx = ctx, .offset = offset };
    size_t   last_match_end = 0;
    uint32_t resume = (uint32_t)engine_DFA_INITIAL;
    n00b_result_t(int) res =
        stream_feed_loop(r->inner, /*REV=*/false, /*PREFIX=*/false,
                         /*STOP=*/false,
                         fwd_prefix, chunk, chunk_len, 0,
                         in_state.state, &last_match_end,
                         chunk_emit_cb, &cc, &resume);
    n00b_mutex_unlock(&r->inner_lock);
    if (n00b_result_is_err(res)) return res;
    *out_state = (StreamState){ .state = resume, .pos = offset + chunk_len };
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// regex_seek_fwd
// ===========================================================================

n00b_result_t(int) regex_seek_fwd(Regex *r, const uint8_t *input, size_t len,
                                   uint32_t state, size_t pos, bool *found,
                                   uint32_t *out_state, size_t *out_end)
{
    n00b_require(found     != nullptr, "regex_seek_fwd: found must not be NULL");
    n00b_require(out_state != nullptr, "regex_seek_fwd: out_state must not be NULL");
    n00b_require(out_end   != nullptr, "regex_seek_fwd: out_end must not be NULL");
    *found = false;
    n00b_result_t(int) ir = init_stream_fwd_only(r);
    if (n00b_result_is_err(ir)) return ir;

    n00b_mutex_lock(&r->inner_lock);
    LDFA *dfa = r->inner->fwd_ts;
    uint32_t fwd_initial_state = r->inner->stream.seek_fwd;
    if (state == REGEX_SEEK_INITIAL) state = fwd_initial_state;
    size_t end = len;
    bool transitioned = false;
    while (pos < end) {
        uint8_t sid = (state < dfa->skip_ids.len)
                          ? dfa->skip_ids.data[state]
                          : 0;
        if (sid != 0) {
            const MintermSearchValue *searcher = &dfa->skip_searchers.data[sid - 1];
            n00b_option_t(size_t) msr =
                minterm_search_value_find_fwd(searcher, input + pos, end - pos);
            if (n00b_option_is_set(msr)) {
                pos += n00b_option_get(msr);
            } else {
                n00b_mutex_unlock(&r->inner_lock);
                return n00b_result_ok(int, 0); // *found stays false
            }
            if (pos >= end) break;
        }
        uint32_t mt = (uint32_t)dfa->mt_lookup[input[pos]];
        n00b_result_t(uint32_t) lt =
            engine_LDFA_lazy_transition(dfa, r->inner->b, state, mt);
        if (n00b_result_is_err(lt)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(lt)));
        }
        uint32_t next = n00b_result_get(lt);
        pos += 1;
        if (next == (uint32_t)engine_DFA_DEAD) {
            state = fwd_initial_state;
            transitioned = false;
            continue;
        }
        state = next;
        transitioned = true;
        if (pos == end) break;
        if (engine_has_any_null(&dfa->effects_id, &dfa->effects, state,
                                NULLABILITY_CENTER)) {
            size_t match_end = 0;
            engine_collect_max_fwd_pub(&dfa->effects_id, &dfa->effects, state,
                                       pos, NULLABILITY_CENTER, &match_end);
            if (match_end == 0) match_end = pos;
            *found     = true;
            *out_state = fwd_initial_state;
            *out_end   = match_end;
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_ok(int, 0);
        }
    }
    if (transitioned && pos == end) {
        if (engine_has_any_null(&dfa->effects_id, &dfa->effects, state,
                                NULLABILITY_END)) {
            size_t match_end = 0;
            engine_collect_max_fwd_pub(&dfa->effects_id, &dfa->effects, state,
                                       end, NULLABILITY_END, &match_end);
            if (match_end == 0) match_end = end;
            *found     = true;
            *out_state = fwd_initial_state;
            *out_end   = match_end;
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_ok(int, 0);
        }
    }
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(int, 0);
}

// ===========================================================================
// regex_seek_rev
// ===========================================================================

n00b_result_t(int) regex_seek_rev(Regex *r, const uint8_t *input, size_t len,
                                   uint32_t state, size_t pos, bool *found,
                                   uint32_t *out_state, size_t *out_start)
{
    (void)len;
    n00b_require(found     != nullptr, "regex_seek_rev: found must not be NULL");
    n00b_require(out_state != nullptr, "regex_seek_rev: out_state must not be NULL");
    n00b_require(out_start != nullptr, "regex_seek_rev: out_start must not be NULL");
    *found = false;
    n00b_mutex_lock(&r->inner_lock);
    LDFA *dfa = r->inner->rev_ts;
    uint32_t rev_initial_state = r->inner->stream.seek_rev;
    if (state == REGEX_SEEK_INITIAL) state = rev_initial_state;
    bool transitioned = false;
    while (pos > 0) {
        uint8_t sid = (state < dfa->skip_ids.len)
                          ? dfa->skip_ids.data[state]
                          : 0;
        if (sid != 0) {
            const MintermSearchValue *searcher = &dfa->skip_searchers.data[sid - 1];
            n00b_option_t(size_t) msr =
                minterm_search_value_find_rev(searcher, input, pos);
            if (n00b_option_is_set(msr)) {
                pos = n00b_option_get(msr) + 1;
            } else {
                n00b_mutex_unlock(&r->inner_lock);
                return n00b_result_ok(int, 0);
            }
        }
        pos -= 1;
        uint32_t mt = (uint32_t)dfa->mt_lookup[input[pos]];
        n00b_result_t(uint32_t) lt =
            engine_LDFA_lazy_transition(dfa, r->inner->b, state, mt);
        if (n00b_result_is_err(lt)) {
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_err(int,
                (int)algebra_err_to_engine(n00b_result_get_err(lt)));
        }
        uint32_t next = n00b_result_get(lt);
        if (next == (uint32_t)engine_DFA_DEAD) {
            state = rev_initial_state;
            transitioned = false;
            continue;
        }
        state = next;
        transitioned = true;
        if (pos == 0) break;
        if (engine_has_any_null(&dfa->effects_id, &dfa->effects, state,
                                NULLABILITY_CENTER)) {
            size_t match_start = engine_NO_MATCH;
            engine_collect_max_rev_pub(&dfa->effects_id, &dfa->effects, state,
                                       pos, NULLABILITY_CENTER, &match_start);
            if (match_start == engine_NO_MATCH) match_start = pos;
            *found     = true;
            *out_state = rev_initial_state;
            *out_start = match_start;
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_ok(int, 0);
        }
    }
    if (transitioned && pos == 0) {
        if (engine_has_any_null(&dfa->effects_id, &dfa->effects, state,
                                NULLABILITY_END)) {
            size_t match_start = engine_NO_MATCH;
            engine_collect_max_rev_pub(&dfa->effects_id, &dfa->effects, state,
                                       0, NULLABILITY_END, &match_start);
            if (match_start == engine_NO_MATCH) match_start = 0;
            *found     = true;
            *out_state = rev_initial_state;
            *out_start = match_start;
            n00b_mutex_unlock(&r->inner_lock);
            return n00b_result_ok(int, 0);
        }
    }
    n00b_mutex_unlock(&r->inner_lock);
    return n00b_result_ok(int, 0);
}
