#pragma once

#include <stdint.h>

// One type descriptor, shared by everything that carries a type: symbol table
// entries, parse tree nodes, and IR nodes all point at this same struct.
//
// The conceptual identity is a typehash type term, so the descriptor carries
// everything the typehash spec makes normative — primitives, named types,
// BitInt, indirections and their attributes, sequences, records with member
// names and tags, functions with argument kinds and parameter modes, binders,
// variables, sums and free vars — without restating any of it. Extending the
// descriptor means adding a field here, never widening `identity`.
//
// Only pointers are needed, so this header pulls in neither typehash nor the
// IR: a front end can build and pass descriptors without linking either.
typedef struct th_ctx  th_ctx;
typedef struct th_type th_type;

typedef struct tdesc_t {
    // Typehash §2-§4. Never a spelling, never a reduced enum: anything learned
    // about a type is expressed by building or refining this term.
    th_type *identity;

    // Finalized identity hash (typehash §6), or 0 while unfinalized. A cache
    // key for the identity, never a substitute for it.
    uint64_t hash;

    // Representational descriptor (typehash §12): target-scoped layout — size,
    // alignment, field order, per-member bit offset and width, indirection
    // representation, sequence stride. Null until a target is chosen; never
    // valid as an identity or a wire tag.
    void *layout;

    // Facts with no typehash counterpart: inference state, provenance,
    // language-specific qualifiers.
    void *ext;
} tdesc_t;
