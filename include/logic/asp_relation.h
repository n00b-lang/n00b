/**
 * @file asp_relation.h
 * @brief Relation storage and indexing for the Datalog engine.
 *
 * A relation stores ground tuples in a three-buffer scheme used by
 * semi-naive evaluation:
 * - **stable**: tuples from previous iterations (fully committed)
 * - **recent**: tuples derived in the last iteration
 * - **to_add**: newly derived tuples awaiting merge
 *
 * Per-column indexes map column values to tuple offsets for
 * efficient join lookups.
 */
#pragma once

#include "logic/asp_types.h"
#include "internal/logic/asp_containers.h"

/**
 * @brief A Datalog relation (predicate extension).
 */
typedef struct {
    n00b_dl_rel_id_t        id;
    int32_t                 arity;
    n00b_string_t           name;

    n00b_dl_sym_t          *stable_data;
    size_t                  stable_count;
    size_t                  stable_cap;

    n00b_dl_sym_t          *recent_data;
    size_t                  recent_count;
    size_t                  recent_cap;

    n00b_dl_sym_t          *to_add_data;
    size_t                  to_add_count;
    size_t                  to_add_cap;

    /** Deduplication: stores tuple hashes. */
    n00b_dl_u64_set_t       seen;

    /** Per-column indexes over stable + recent. */
    n00b_dl_i64_offsets_map_t *col_index;
    bool                       index_dirty;
} n00b_dl_relation_t;

/**
 * @brief Initialize a relation.
 *
 * @param rel   Relation to initialize.
 * @param id    Unique relation ID.
 * @param name  Relation name.
 * @param arity Number of columns.
 */
void n00b_dl_relation_init(n00b_dl_relation_t *rel, n00b_dl_rel_id_t id,
                             n00b_string_t name, int32_t arity);

/**
 * @brief Free all resources held by a relation.
 * @param rel Relation to free.
 */
void n00b_dl_relation_free(n00b_dl_relation_t *rel);

/**
 * @brief Insert a tuple into the to-add buffer.
 *
 * Deduplicates against all previously seen tuples. After a hash
 * match, verifies actual tuple equality (fixing the hash-only
 * dedup bug from the original implementation).
 *
 * @param rel   Relation to insert into.
 * @param tuple Array of `arity` symbol values.
 * @return `true` if the tuple was new, `false` if duplicate.
 */
bool n00b_dl_relation_insert(n00b_dl_relation_t *rel,
                               const n00b_dl_sym_t *tuple);

/**
 * @brief Merge buffers: recent -> stable, to_add -> recent.
 *
 * @param rel Relation to swap.
 * @return `true` if new facts appeared in recent.
 */
bool n00b_dl_relation_swap(n00b_dl_relation_t *rel);

/**
 * @brief Rebuild per-column indexes over stable + recent.
 * @param rel Relation whose index to rebuild.
 */
void n00b_dl_relation_rebuild_index(n00b_dl_relation_t *rel);

/**
 * @brief Get total tuple count (stable + recent).
 * @param rel Relation to count.
 */
size_t n00b_dl_relation_count(n00b_dl_relation_t *rel);

/**
 * @brief Hash a tuple using FNV-1a.
 *
 * @param tuple Array of symbol values.
 * @param arity Number of symbols.
 * @return 64-bit hash.
 */
uint64_t n00b_dl_tuple_hash(const n00b_dl_sym_t *tuple, int32_t arity);
