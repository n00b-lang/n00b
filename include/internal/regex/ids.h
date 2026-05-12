/**
 * @file ids.h
 * @brief Shared integer-id newtypes for the regex engine.
 *
 * Single source of truth for the regex-internal id vocabulary.  Every
 * header that wants to talk about `NodeId`, `TRegexId`, `TSetId`,
 * `NullsId`, `MetadataId`, or `TSet` should include this file instead
 * of redeclaring the typedefs.
 *
 * Each id is a one-field struct wrapping a `uint32_t`, mirroring the
 * upstream Rust newtypes.  The wrapper makes ids non-interchangeable
 * at the type level: you cannot accidentally pass a `NodeId` where a
 * `TSetId` is expected.
 *
 * The structs are intentionally named (not anonymous) so forward
 * declarations like `typedef struct NodeId NodeId;` in other headers
 * compose with the complete definition here.
 *
 * These names stay un-prefixed (no `n00b_` prefix) because they form
 * the regex algorithmic vocabulary that tracks upstream Rust closely;
 * the header lives under `include/internal/regex/` and is not part of
 * the public n00b surface.
 */
#pragma once

#include <stdint.h>

/** Identifier for a node in the regex syntax / DFA graph. */
typedef struct NodeId     { uint32_t v; } NodeId;

/** Identifier for a transition regex (derivative class). */
typedef struct TRegexId   { uint32_t v; } TRegexId;

/** Identifier for a transition set (alphabet partition class). */
typedef struct TSetId     { uint32_t v; } TSetId;

/** Identifier for a nullability record. */
typedef struct NullsId    { uint32_t v; } NullsId;

/** Identifier for an auxiliary metadata record. */
typedef struct MetadataId { uint32_t v; } MetadataId;

/** 256-bit set, stored as 4 × `uint64_t` words. */
typedef struct TSet { uint64_t words[4]; } TSet;
