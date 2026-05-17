/**
 * @file stream.h
 * @brief Streaming and seeking entry points for the regex engine.
 *
 * Internal regex-engine header, not part of the public n00b surface.
 * Algorithmic names track upstream Rust `resharp::stream` / resharp-c's
 * `internal/stream.h` and stay un-prefixed (no `n00b_`) per the regex
 * port convention.
 *
 * Provides the eight internal entry points the public surface (Phase 9
 * `src/text/regex/public.c`) dispatches through:
 *
 *   - `regex_stream`           — whole-input shortest-match scan → list.
 *   - `regex_stream_with`      — whole-input shortest-match scan → callback.
 *   - `regex_stream_first`     — whole-input, stop at first match.
 *   - `regex_stream_chunk`     — resumable per-chunk feeder.
 *   - `regex_stream_ends`      — whole-input, end-only → list.
 *   - `regex_stream_ends_with` — whole-input, end-only → callback.
 *   - `regex_seek_fwd`         — forward cursor over a contiguous buffer.
 *   - `regex_seek_rev`         — reverse cursor over a contiguous buffer.
 *
 * The eagerly-precomputed `StreamInit`, lazy `StreamCache`, and the
 * `Regex` / `RegexInner` field layouts they embed into live in
 * `internal/regex/regex.h` (Phase 7); this header trusts those
 * declarations and adds only the opaque cursor state `StreamState` and
 * the streaming / seeking entry points listed above.
 *
 * Errors: fallible operations return `n00b_result_t(T)` whose `err`
 * side carries an `n00b_regex_engine_err_t` value cast to `int`
 * (per § 7.5 D14).  Non-fallible queries return their values directly.
 *
 * Companion source: `src/text/regex/stream/stream.c`.
 */
#pragma once

#include "n00b.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "adt/result.h"

#include "internal/regex/regex.h"
#include "internal/regex/accel.h" // Match

// ---------------------------------------------------------------------------
// StreamState — resumable cursor for `regex_stream_chunk`.
//
// Trivial-copy by value; field layout is opaque from outside this header
// (use the helpers below to construct / inspect).
// ---------------------------------------------------------------------------

/** @brief Resumable DFA cursor for chunked feeding. */
typedef struct StreamState {
    uint32_t state;
    size_t   pos;
} StreamState;

/**
 * @brief Sentinel value for the `state` argument of `regex_seek_fwd` /
 *        `regex_seek_rev`'s first call.  Mirrors upstream
 *        `pub const SEEK_INITIAL: u32 = 0;` — instructs the seek
 *        kernel to substitute the precomputed initial state id.
 */
constexpr uint32_t REGEX_SEEK_INITIAL = 0u;

// ---------------------------------------------------------------------------
// StreamState constructors / accessors.
// ---------------------------------------------------------------------------

/** @brief Zero-pos initial cursor. */
[[nodiscard]] StreamState stream_state_new(void);

/** @brief Initial cursor positioned at the given absolute offset. */
[[nodiscard]] StreamState stream_state_at(size_t pos);

/** @brief Current absolute offset. */
[[nodiscard]] size_t      stream_state_pos(const StreamState *s);

/** @brief Raw DFA state id. */
[[nodiscard]] uint32_t    stream_state_state(const StreamState *s);

/** @brief Build a cursor from a raw `(state, pos)` pair. */
[[nodiscard]] StreamState stream_state_from_raw(uint32_t state, size_t pos);

// ---------------------------------------------------------------------------
// Whole-input streaming entry points.
//
// `regex_stream` clears `*out` before populating it with `(start, end)`
// pairs (shortest-match, left-to-right; the DFA state resets after each
// match).  `regex_stream_with` invokes the callback once per match.
// `regex_stream_first` stops on the first match and reports it via
// out-params.
// ---------------------------------------------------------------------------

n00b_result_t(int) regex_stream(Regex *r, const uint8_t *input, size_t len,
                                 n00b_list_t(Match) *out);

n00b_result_t(int) regex_stream_with(Regex *r, const uint8_t *input, size_t len,
                                      void (*on_match)(void *ctx, Match m),
                                      void *ctx);

n00b_result_t(int) regex_stream_first(Regex *r, const uint8_t *input, size_t len,
                                       bool *found, Match *out_match);

// ---------------------------------------------------------------------------
// Resume-from-state chunk feeder.
//
// `in_state` is the state returned by the previous call (or
// `stream_state_new()` / `stream_state_at(off)` for the first chunk);
// `*out_state` receives the post-chunk state.  `on_end` is invoked once
// per shortest-match end found within the chunk, expressed in absolute
// offsets (chunk-local `end` plus `in_state.pos`).
// ---------------------------------------------------------------------------

n00b_result_t(int) regex_stream_chunk(Regex *r, const uint8_t *chunk,
                                       size_t chunk_len,
                                       StreamState in_state,
                                       StreamState *out_state,
                                       void (*on_end)(void *ctx, size_t end),
                                       void *ctx);

// ---------------------------------------------------------------------------
// Match-end-only streams (skip the reverse pass).
// ---------------------------------------------------------------------------

n00b_result_t(int) regex_stream_ends(Regex *r, const uint8_t *input, size_t len,
                                      n00b_list_t(size_t) *out);

n00b_result_t(int) regex_stream_ends_with(Regex *r, const uint8_t *input,
                                           size_t len,
                                           void (*on_end)(void *ctx, size_t end),
                                           void *ctx);

// ---------------------------------------------------------------------------
// Forward / reverse cursor seeks.
//
// First call: pass `state = REGEX_SEEK_INITIAL`, `pos = 0` (fwd) or
// `pos = len` (rev).  Subsequent calls pass the previously-returned
// `*out_state` and the prior end / start as the new `pos`.
// `*found` is set to whether a match was emitted; on `*found == true`,
// `*out_state` / `*out_end` (fwd) or `*out_start` (rev) are populated.
// ---------------------------------------------------------------------------

n00b_result_t(int) regex_seek_fwd(Regex *r, const uint8_t *input, size_t len,
                                   uint32_t state, size_t pos, bool *found,
                                   uint32_t *out_state, size_t *out_end);

n00b_result_t(int) regex_seek_rev(Regex *r, const uint8_t *input, size_t len,
                                   uint32_t state, size_t pos, bool *found,
                                   uint32_t *out_state, size_t *out_start);
