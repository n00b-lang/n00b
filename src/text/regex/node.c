#include "text/regex/node.h"
#include "text/regex/parse.h"
#include "core/alloc.h"
#include "core/hash.h"

#include <limits.h>

// ============================================================================
// Internal helpers
// ============================================================================

static inline int32_t
incr_len(int32_t a, int32_t b)
{
    if (a == INT32_MAX || b == INT32_MAX) return INT32_MAX;
    int64_t sum = (int64_t)a + (int64_t)b;
    return sum > INT32_MAX ? INT32_MAX : (int32_t)sum;
}

static inline int32_t
mul_len(int32_t a, int32_t b)
{
    if (a == INT32_MAX || b == INT32_MAX) return INT32_MAX;
    if (a == 0 || b == 0) return 0;
    int64_t prod = (int64_t)a * (int64_t)b;
    return prod > INT32_MAX ? INT32_MAX : (int32_t)prod;
}

static uint64_t
node_hash(n00b_regex_node_kind_t kind, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t buf[4] = {(uint64_t)kind, a, b, c};
    n00b_uint128_t h = n00b_hash_raw(buf, sizeof(buf));
    return (uint64_t)h;
}

static uint32_t
register_node(n00b_regex_builder_t *bld, n00b_regex_node_t node)
{
    uint32_t id = (uint32_t)bld->nodes.len;
    node.id     = id;
    n00b_list_push(bld->nodes, node);
    return id;
}

// Helper: test if a node is Loop(Singleton(pred), 0, MAX) — "PredStar".
// Returns the predicate charset, or solver->false_id if not a PredStar.
static n00b_regex_charset_t
get_pred_star(n00b_regex_builder_t *b, uint32_t id)
{
    if (id == N00B_RE_ID_DOTSTAR) return b->solver->true_id;
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    if (n->kind == N00B_RE_LOOP && n->loop.lo == 0 && n->loop.hi == INT32_MAX) {
        n00b_regex_node_t *body = n00b_regex_node_get(b, n->loop.body);
        if (body->kind == N00B_RE_SINGLETON) return body->singleton.set;
    }
    return b->solver->false_id;
}

// Helper: test if a node is Loop(Singleton(pred), lo, hi) — "PredLoop".
// Returns true and fills out_pred, out_lo, out_hi if so.
static bool
get_pred_loop(n00b_regex_builder_t *b, uint32_t id,
              n00b_regex_charset_t *out_pred, int32_t *out_lo, int32_t *out_hi)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    if (n->kind == N00B_RE_LOOP) {
        n00b_regex_node_t *body = n00b_regex_node_get(b, n->loop.body);
        if (body->kind == N00B_RE_SINGLETON) {
            *out_pred = body->singleton.set;
            *out_lo   = n->loop.lo;
            *out_hi   = n->loop.hi;
            return true;
        }
    }
    else if (n->kind == N00B_RE_SINGLETON) {
        *out_pred = n->singleton.set;
        *out_lo   = 1;
        *out_hi   = 1;
        return true;
    }
    return false;
}

// Helper: PredStarHead — matches PredStar directly, or Concat whose head is PredStar.
// Returns the predicate charset, or solver->false_id if not a match.
static n00b_regex_charset_t
get_pred_star_head(n00b_regex_builder_t *b, uint32_t id)
{
    n00b_regex_charset_t ps = get_pred_star(b, id);
    if (!n00b_regex_charset_is_empty(b->solver, ps)) return ps;
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    if (n->kind == N00B_RE_CONCAT) {
        return get_pred_star(b, n->concat.head);
    }
    return b->solver->false_id;
}

// Helper: ConcatSuffix — walk concat tail chain to find the rightmost non-Concat node.
static uint32_t
get_concat_suffix(n00b_regex_builder_t *b, uint32_t id)
{
    uint32_t cur = id;
    for (int depth = 0; depth < 128; depth++) {
        n00b_regex_node_t *n = n00b_regex_node_get(b, cur);
        if (n->kind != N00B_RE_CONCAT) break;
        cur = n->concat.tail;
    }
    return cur;
}

// Helper: check if a node ends with DOTSTAR (for lookahead body normalization).
static bool
ends_with_dotstar(n00b_regex_builder_t *b, uint32_t id)
{
    if (id == N00B_RE_ID_DOTSTAR) return true;
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    if (n->kind == N00B_RE_CONCAT) {
        return ends_with_dotstar(b, n->concat.tail);
    }
    return false;
}

// Helper: check if a node starts with DOTSTAR (for AND-head TrueStar).
static bool
starts_with_dotstar(n00b_regex_builder_t *b, uint32_t id)
{
    if (id == N00B_RE_ID_DOTSTAR) return true;
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    if (n->kind == N00B_RE_CONCAT) {
        return starts_with_dotstar(b, n->concat.head);
    }
    return false;
}

static void
compute_node_flags(n00b_regex_builder_t *bld, n00b_regex_node_t *node)
{
    n00b_regex_solver_t *s = bld->solver;

    switch (node->kind) {
    case N00B_RE_SINGLETON:
        node->is_always_nullable    = false;
        node->can_be_nullable       = false;
        node->depends_on_anchor     = false;
        node->contains_lookaround   = false;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = node->singleton.set;
        node->subsumed_by           = node->singleton.set;
        node->min_length            = 1;
        node->max_length            = 1;
        break;
    case N00B_RE_CONCAT: {
        n00b_regex_node_t *h = n00b_regex_node_get(bld, node->concat.head);
        n00b_regex_node_t *t = n00b_regex_node_get(bld, node->concat.tail);
        node->is_always_nullable  = h->is_always_nullable && t->is_always_nullable;
        node->can_be_nullable     = h->can_be_nullable && t->can_be_nullable;
        node->depends_on_anchor   = h->depends_on_anchor || (h->can_be_nullable && t->depends_on_anchor);
        node->contains_lookaround = h->contains_lookaround || t->contains_lookaround;
        node->start_set           = h->start_set;
        node->subsumed_by         = n00b_regex_charset_or(s, h->subsumed_by, t->subsumed_by);
        node->min_length          = incr_len(h->min_length, t->min_length);
        node->max_length          = incr_len(h->max_length, t->max_length);
        // Prefix lookbehind: propagate from head, or head IS a lookbehind
        node->has_prefix_lookbehind = h->has_prefix_lookbehind
            || (h->kind == N00B_RE_LOOKAROUND && h->lookaround.look_back);
        // Suffix lookahead: propagate from tail, or tail IS a lookahead
        node->has_suffix_lookahead = t->has_suffix_lookahead
            || (t->kind == N00B_RE_LOOKAROUND && !t->lookaround.look_back);
        break;
    }
    case N00B_RE_OR: {
        node->is_always_nullable    = false;
        node->can_be_nullable       = false;
        node->depends_on_anchor     = false;
        node->contains_lookaround   = false;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = s->false_id;
        node->subsumed_by           = s->false_id;
        node->min_length            = INT32_MAX;
        node->max_length            = 0;
        for (uint32_t i = 0; i < node->multi.count; i++) {
            n00b_regex_node_t *c = n00b_regex_node_get(bld, node->multi.children[i]);
            if (c->is_always_nullable) node->is_always_nullable = true;
            if (c->can_be_nullable) node->can_be_nullable = true;
            if (c->depends_on_anchor) node->depends_on_anchor = true;
            if (c->contains_lookaround) node->contains_lookaround = true;
            node->start_set   = n00b_regex_charset_or(s, node->start_set, c->start_set);
            node->subsumed_by = n00b_regex_charset_or(s, node->subsumed_by, c->subsumed_by);
            if (c->min_length < node->min_length) node->min_length = c->min_length;
            if (c->max_length > node->max_length) node->max_length = c->max_length;
        }
        break;
    }
    case N00B_RE_AND: {
        node->is_always_nullable    = true;
        node->can_be_nullable       = true;
        node->depends_on_anchor     = false;
        node->contains_lookaround   = false;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = s->true_id;
        node->subsumed_by           = s->true_id;
        node->min_length            = 0;
        node->max_length            = INT32_MAX;
        for (uint32_t i = 0; i < node->multi.count; i++) {
            n00b_regex_node_t *c = n00b_regex_node_get(bld, node->multi.children[i]);
            if (!c->is_always_nullable) node->is_always_nullable = false;
            if (!c->can_be_nullable) node->can_be_nullable = false;
            if (c->depends_on_anchor) node->depends_on_anchor = true;
            if (c->contains_lookaround) node->contains_lookaround = true;
            node->start_set   = n00b_regex_charset_and(s, node->start_set, c->start_set);
            node->subsumed_by = n00b_regex_charset_and(s, node->subsumed_by, c->subsumed_by);
            if (c->min_length > node->min_length) node->min_length = c->min_length;
            if (c->max_length < node->max_length) node->max_length = c->max_length;
        }
        break;
    }
    case N00B_RE_NOT: {
        n00b_regex_node_t *inner = n00b_regex_node_get(bld, node->not_.inner);
        node->is_always_nullable    = !inner->can_be_nullable;
        node->can_be_nullable       = !inner->is_always_nullable;
        node->depends_on_anchor     = inner->depends_on_anchor;
        node->contains_lookaround   = inner->contains_lookaround;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = s->true_id;
        node->subsumed_by           = s->true_id;
        node->min_length            = 0;
        node->max_length            = INT32_MAX;
        break;
    }
    case N00B_RE_LOOP: {
        n00b_regex_node_t *body = n00b_regex_node_get(bld, node->loop.body);
        node->is_always_nullable    = (node->loop.lo == 0) || body->is_always_nullable;
        node->can_be_nullable       = (node->loop.lo == 0) || body->can_be_nullable;
        node->depends_on_anchor     = body->depends_on_anchor;
        node->contains_lookaround   = body->contains_lookaround;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = body->start_set;
        node->subsumed_by         = (node->loop.lo > 0) ? body->subsumed_by : s->true_id;
        node->min_length          = mul_len(node->loop.lo, body->min_length);
        node->max_length          = mul_len(node->loop.hi, body->max_length);
        break;
    }
    case N00B_RE_LOOKAROUND: {
        n00b_regex_node_t *body = n00b_regex_node_get(bld, node->lookaround.body);
        node->is_always_nullable    = body->is_always_nullable;
        node->can_be_nullable       = body->can_be_nullable;
        node->depends_on_anchor     = body->depends_on_anchor;
        node->contains_lookaround   = true;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = s->true_id;
        node->subsumed_by           = body->subsumed_by;
        node->min_length            = 0;
        node->max_length            = 0;
        break;
    }
    case N00B_RE_BEGIN:
    case N00B_RE_END:
        node->is_always_nullable    = false;
        node->can_be_nullable       = true;
        node->depends_on_anchor     = true;
        node->contains_lookaround   = false;
        node->has_prefix_lookbehind = false;
        node->has_suffix_lookahead  = false;
        node->start_set             = s->true_id;
        node->subsumed_by           = s->false_id;
        node->min_length            = 0;
        node->max_length            = 0;
        break;
    }
}

// ============================================================================
// Builder construction
// ============================================================================

n00b_regex_builder_t
n00b_regex_builder_new(n00b_regex_solver_t *solver)
{
    n00b_regex_builder_t b = {};

    b.nodes  = n00b_list_new_cap_private(n00b_regex_node_t, 256);
    b.dedup  = n00b_alloc(n00b_dict_t(uint64_t, uint32_t));
    n00b_dict_init(b.dedup, .hash = n00b_hash_word, .skip_obj_hash = true);
    b.solver = solver;

    // Register sentinel nodes in the required order (IDs 0..6).
    // 0: NOTHING = Singleton(empty set)
    n00b_regex_node_t nothing = {
        .kind = N00B_RE_SINGLETON,
        .singleton.set = solver->false_id,
    };
    register_node(&b, nothing);
    compute_node_flags(&b, &b.nodes.data[N00B_RE_ID_NOTHING]);

    // 1: EPSILON = Loop(NOTHING, 0, INT32_MAX) — always nullable
    n00b_regex_node_t eps = {
        .kind    = N00B_RE_LOOP,
        .loop    = {.body = N00B_RE_ID_NOTHING, .lo = 0, .hi = INT32_MAX},
    };
    register_node(&b, eps);
    b.nodes.data[N00B_RE_ID_EPSILON].is_always_nullable = true;
    b.nodes.data[N00B_RE_ID_EPSILON].can_be_nullable    = true;
    b.nodes.data[N00B_RE_ID_EPSILON].start_set          = solver->true_id;
    b.nodes.data[N00B_RE_ID_EPSILON].subsumed_by        = solver->false_id;
    b.nodes.data[N00B_RE_ID_EPSILON].min_length          = 0;
    b.nodes.data[N00B_RE_ID_EPSILON].max_length          = 0;

    // 2: ANY = Singleton(full set)
    n00b_regex_node_t any = {
        .kind = N00B_RE_SINGLETON,
        .singleton.set = solver->true_id,
    };
    register_node(&b, any);
    compute_node_flags(&b, &b.nodes.data[N00B_RE_ID_ANY]);

    // 3: DOTSTAR = Loop(ANY, 0, INT32_MAX)
    n00b_regex_node_t dotstar = {
        .kind = N00B_RE_LOOP,
        .loop = {.body = N00B_RE_ID_ANY, .lo = 0, .hi = INT32_MAX},
    };
    register_node(&b, dotstar);
    b.nodes.data[N00B_RE_ID_DOTSTAR].is_always_nullable = true;
    b.nodes.data[N00B_RE_ID_DOTSTAR].can_be_nullable    = true;
    b.nodes.data[N00B_RE_ID_DOTSTAR].start_set          = solver->true_id;
    b.nodes.data[N00B_RE_ID_DOTSTAR].subsumed_by        = solver->true_id;
    b.nodes.data[N00B_RE_ID_DOTSTAR].min_length          = 0;
    b.nodes.data[N00B_RE_ID_DOTSTAR].max_length          = INT32_MAX;

    // 4: ANYPLUS = Loop(ANY, 1, INT32_MAX)
    n00b_regex_node_t anyplus = {
        .kind = N00B_RE_LOOP,
        .loop = {.body = N00B_RE_ID_ANY, .lo = 1, .hi = INT32_MAX},
    };
    register_node(&b, anyplus);
    compute_node_flags(&b, &b.nodes.data[N00B_RE_ID_ANYPLUS]);

    // 5: END
    n00b_regex_node_t end = {.kind = N00B_RE_END};
    register_node(&b, end);
    compute_node_flags(&b, &b.nodes.data[N00B_RE_ID_END]);

    // 6: BEGIN
    n00b_regex_node_t begin = {.kind = N00B_RE_BEGIN};
    register_node(&b, begin);
    compute_node_flags(&b, &b.nodes.data[N00B_RE_ID_BEGIN]);

    return b;
}

// ============================================================================
// Insertion sort helper (used by mk_or and mk_and)
// ============================================================================

static void
sort_u32(uint32_t *arr, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = arr[i];
        uint32_t j   = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

// Binary search in sorted array; returns true if found.
static bool
sorted_contains(uint32_t *arr, uint32_t n, uint32_t val)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (arr[mid] < val) lo = mid + 1;
        else if (arr[mid] > val) hi = mid;
        else return true;
    }
    return false;
}

// ============================================================================
// Flat buffer helpers
// ============================================================================

// Growable stack-allocated-first buffer for collecting node IDs.
typedef struct {
    uint32_t *data;
    uint32_t  len;
    uint32_t  cap;
    uint32_t  stack_buf[64];
} flat_buf_t;

static inline void
flat_init(flat_buf_t *fb)
{
    fb->data = fb->stack_buf;
    fb->cap  = 64;
    fb->len  = 0;
}

static inline void
flat_push(flat_buf_t *fb, uint32_t val)
{
    if (fb->len >= fb->cap) {
        uint32_t new_cap = fb->cap * 2;
        uint32_t *nd = n00b_alloc_array(uint32_t, new_cap);
        memcpy(nd, fb->data, fb->len * sizeof(uint32_t));
        if (fb->data != fb->stack_buf) n00b_free(fb->data);
        fb->data = nd;
        fb->cap  = new_cap;
    }
    fb->data[fb->len++] = val;
}

static inline void
flat_free(flat_buf_t *fb)
{
    if (fb->data != fb->stack_buf) n00b_free(fb->data);
}

static inline bool
flat_contains(flat_buf_t *fb, uint32_t val)
{
    for (uint32_t i = 0; i < fb->len; i++) {
        if (fb->data[i] == val) return true;
    }
    return false;
}

static inline void
flat_remove(flat_buf_t *fb, uint32_t val)
{
    for (uint32_t i = 0; i < fb->len; i++) {
        if (fb->data[i] == val) {
            fb->data[i] = fb->data[--fb->len];
            return;
        }
    }
}

// ============================================================================
// SplitTail: decompose right-associative Concat chain into (heads[], tail)
// ============================================================================

// Walks Concat(h1, Concat(h2, ... Concat(hN, tail))) and stores h1..hN
// in *heads, returning the final non-Concat tail.
static uint32_t
split_tail(n00b_regex_builder_t *b, uint32_t id, flat_buf_t *heads)
{
    uint32_t cur = id;
    for (int depth = 0; depth < 128; depth++) {
        n00b_regex_node_t *n = n00b_regex_node_get(b, cur);
        if (n->kind != N00B_RE_CONCAT) break;
        flat_push(heads, n->concat.head);
        cur = n->concat.tail;
    }
    return cur;
}

// Rebuild a right-associative Concat from a heads array:
// mkConcat(heads[0], mkConcat(heads[1], ... mkConcat(heads[n-1], EPS)))
static uint32_t
rebuild_concat_from_heads(n00b_regex_builder_t *b, uint32_t *heads, uint32_t n)
{
    if (n == 0) return N00B_RE_ID_EPSILON;
    uint32_t result = heads[n - 1];
    for (int32_t i = (int32_t)n - 2; i >= 0; i--) {
        result = n00b_regex_mk_concat(b, heads[i], result);
    }
    return result;
}

// Get fixed length of a node, or -1 if not fixed.
static inline int32_t
get_fixed_length(n00b_regex_builder_t *b, uint32_t id)
{
    n00b_regex_node_t *n = n00b_regex_node_get(b, id);
    return (n->min_length == n->max_length && n->min_length >= 0)
         ? n->min_length : -1;
}

// ============================================================================
// Range union: merge overlapping/adjacent (lo,hi) pairs
// ============================================================================

typedef struct {
    int32_t lo;
    int32_t hi;
} range_pair_t;

static int
range_cmp(const void *a, const void *b)
{
    const range_pair_t *ra = a, *rb = b;
    if (ra->lo != rb->lo) return (ra->lo < rb->lo) ? -1 : 1;
    return (ra->hi < rb->hi) ? -1 : (ra->hi > rb->hi) ? 1 : 0;
}

// Merge overlapping/adjacent ranges in-place. Returns new count.
static uint32_t
range_union(range_pair_t *ranges, uint32_t n)
{
    if (n <= 1) return n;
    qsort(ranges, n, sizeof(range_pair_t), range_cmp);
    uint32_t out = 0;
    for (uint32_t i = 1; i < n; i++) {
        // Adjacent or overlapping: ranges[out].hi >= ranges[i].lo - 1
        if (ranges[out].hi >= ranges[i].lo - 1) {
            if (ranges[i].hi > ranges[out].hi)
                ranges[out].hi = ranges[i].hi;
        }
        else {
            ranges[++out] = ranges[i];
        }
    }
    return out + 1;
}

// ============================================================================
// mk_or finalization helper: register an Or node
// ============================================================================

static uint32_t
finalize_or(n00b_regex_builder_t *b, uint32_t *flat, uint32_t flat_count)
{
    // Hash for dedup
    uint64_t key = node_hash(N00B_RE_OR, flat[0], flat[flat_count - 1], flat_count);
    for (uint32_t i = 1; i < flat_count; i++) {
        key ^= (uint64_t)flat[i] * 0x9E3779B97F4A7C15ULL;
    }
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return cached;

    uint32_t *owned = n00b_alloc_array(uint32_t, flat_count);
    memcpy(owned, flat, flat_count * sizeof(uint32_t));

    n00b_regex_node_t node = {
        .kind  = N00B_RE_OR,
        .multi = {.children = owned, .count = flat_count},
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return id;
}

static uint32_t
finalize_and(n00b_regex_builder_t *b, uint32_t *flat, uint32_t flat_count)
{
    uint64_t key = node_hash(N00B_RE_AND, flat[0], flat[flat_count - 1], flat_count);
    for (uint32_t i = 1; i < flat_count; i++) {
        key ^= (uint64_t)flat[i] * 0x9E3779B97F4A7C15ULL;
    }
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return cached;

    uint32_t *owned = n00b_alloc_array(uint32_t, flat_count);
    memcpy(owned, flat, flat_count * sizeof(uint32_t));

    n00b_regex_node_t node = {
        .kind  = N00B_RE_AND,
        .multi = {.children = owned, .count = flat_count},
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return id;
}

// ============================================================================
// Node constructors with dedup + simplification
// ============================================================================

uint32_t
n00b_regex_mk_singleton(n00b_regex_builder_t *b, n00b_regex_charset_t set)
{
    if (n00b_regex_charset_is_empty(b->solver, set)) return N00B_RE_ID_NOTHING;
    if (n00b_regex_charset_is_full(b->solver, set)) return N00B_RE_ID_ANY;

    uint64_t key = node_hash(N00B_RE_SINGLETON, set, 0, 0);
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return cached;

    n00b_regex_node_t node = {
        .kind = N00B_RE_SINGLETON,
        .singleton.set = set,
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return id;
}

// ============================================================================
// mk_concat — Phase 5: Full resharp concat optimizations
// ============================================================================

uint32_t
n00b_regex_mk_concat(n00b_regex_builder_t *b, uint32_t head, uint32_t tail)
{
    // Identity / annihilator
    if (head == N00B_RE_ID_EPSILON) return tail;
    if (tail == N00B_RE_ID_EPSILON) return head;
    if (head == N00B_RE_ID_NOTHING) return N00B_RE_ID_NOTHING;
    if (tail == N00B_RE_ID_NOTHING) return N00B_RE_ID_NOTHING;

    // DOTSTAR · DOTSTAR = DOTSTAR
    if (head == N00B_RE_ID_DOTSTAR && tail == N00B_RE_ID_DOTSTAR) {
        return N00B_RE_ID_DOTSTAR;
    }

    n00b_regex_node_t *nh = n00b_regex_node_get(b, head);
    n00b_regex_node_t *nt = n00b_regex_node_get(b, tail);

    // DOTSTAR · And(all-start-with-DOTSTAR) = tail
    if (head == N00B_RE_ID_DOTSTAR && nt->kind == N00B_RE_AND) {
        bool all_start = true;
        for (uint32_t i = 0; i < nt->multi.count; i++) {
            if (!starts_with_dotstar(b, nt->multi.children[i])) {
                all_start = false;
                break;
            }
        }
        if (all_start) return tail;
    }
    // And(all-end-with-DOTSTAR) · DOTSTAR = head
    if (tail == N00B_RE_ID_DOTSTAR && nh->kind == N00B_RE_AND) {
        bool all_end = true;
        for (uint32_t i = 0; i < nh->multi.count; i++) {
            if (!ends_with_dotstar(b, nh->multi.children[i])) {
                all_end = false;
                break;
            }
        }
        if (all_end) return head;
    }

    // Loop concatenation merge: Loop(x,a,b) · Loop(x,c,d) → Loop(x, a+c, b+d)
    if (nh->kind == N00B_RE_LOOP && nt->kind == N00B_RE_LOOP
        && nh->loop.body == nt->loop.body) {
        return n00b_regex_mk_loop(b, nh->loop.body,
                                   incr_len(nh->loop.lo, nt->loop.lo),
                                   incr_len(nh->loop.hi, nt->loop.hi));
    }

    // Loop body absorption: Loop(x,a,b) · x → Loop(x, a+1, b+1)
    if (nh->kind == N00B_RE_LOOP && nh->loop.body == tail) {
        return n00b_regex_mk_loop(b, tail,
                                   incr_len(nh->loop.lo, 1),
                                   incr_len(nh->loop.hi, 1));
    }

    // x · Loop(x,a,b) → Loop(x, a+1, b+1)
    if (nt->kind == N00B_RE_LOOP && nt->loop.body == head) {
        return n00b_regex_mk_loop(b, head,
                                   incr_len(nt->loop.lo, 1),
                                   incr_len(nt->loop.hi, 1));
    }

    // When tail is Concat(Loop(x,c,d), rest) and head is Loop(x,a,b) or head==x
    if (nt->kind == N00B_RE_CONCAT) {
        n00b_regex_node_t *ch = n00b_regex_node_get(b, nt->concat.head);
        if (ch->kind == N00B_RE_LOOP && ch->loop.body == head) {
            uint32_t merged = n00b_regex_mk_loop(b, head,
                                                   incr_len(ch->loop.lo, 1),
                                                   incr_len(ch->loop.hi, 1));
            return n00b_regex_mk_concat(b, merged, nt->concat.tail);
        }
        if (nh->kind == N00B_RE_LOOP && ch->kind == N00B_RE_LOOP
            && nh->loop.body == ch->loop.body) {
            uint32_t merged = n00b_regex_mk_loop(b, nh->loop.body,
                                                   incr_len(nh->loop.lo, ch->loop.lo),
                                                   incr_len(nh->loop.hi, ch->loop.hi));
            return n00b_regex_mk_concat(b, merged, nt->concat.tail);
        }

        // SUB03-06: repeating concat-to-loop patterns
        // n1 = head, n2 = concatHead, tailNode = tail2
        uint32_t n2       = nt->concat.head;
        uint32_t tailNode = nt->concat.tail;
        uint32_t inner    = n00b_regex_mk_concat(b, head, n2);

        // Re-read after potential pool growth
        nh = n00b_regex_node_get(b, head);
        nt = n00b_regex_node_get(b, tail);

        // SUB06: if inner is PredStar, flatten
        n00b_regex_charset_t ps_inner = get_pred_star(b, inner);
        if (!n00b_regex_charset_is_empty(b->solver, ps_inner)) {
            return n00b_regex_mk_concat(b, inner, tailNode);
        }

        n00b_regex_node_t *tn = n00b_regex_node_get(b, tailNode);

        // SUB03: tailNode is Concat(t1,t2) where head==t1 && n2==t2
        if (tn->kind == N00B_RE_CONCAT
            && head == tn->concat.head && n2 == tn->concat.tail) {
            return n00b_regex_mk_loop(b, tailNode, 2, 2);
        }

        // SUB04: tailNode is Concat(t1, Concat(t2, t3)) where head==t1 && n2==t2
        if (tn->kind == N00B_RE_CONCAT) {
            n00b_regex_node_t *tt = n00b_regex_node_get(b, tn->concat.tail);
            if (tt->kind == N00B_RE_CONCAT
                && head == tn->concat.head && n2 == tt->concat.head) {
                uint32_t loop = n00b_regex_mk_loop(b, inner, 2, 2);
                return n00b_regex_mk_concat(b, loop, tt->concat.tail);
            }

            // SUB05: tailNode head is Loop(lnode, lo, hi) where lnode==inner
            n00b_regex_node_t *t1n = n00b_regex_node_get(b, tn->concat.head);
            if (t1n->kind == N00B_RE_LOOP && t1n->loop.body == inner) {
                uint32_t new_loop = n00b_regex_mk_loop(b, inner,
                    incr_len(t1n->loop.lo, 1), incr_len(t1n->loop.hi, 1));
                return n00b_regex_mk_concat(b, new_loop, tn->concat.tail);
            }
        }

        // Re-read after SUB checks
        nh = n00b_regex_node_get(b, head);
        nt = n00b_regex_node_get(b, tail);
    }

    // PredStar subsumption: Singleton(p1)* · Singleton(p2)* → keep superset
    {
        n00b_regex_charset_t ps1 = get_pred_star(b, head);
        n00b_regex_charset_t ps2 = get_pred_star(b, tail);
        if (!n00b_regex_charset_is_empty(b->solver, ps1)
            && !n00b_regex_charset_is_empty(b->solver, ps2)) {
            if (n00b_regex_charset_contains_set(b->solver, ps1, ps2)) return head;
            if (n00b_regex_charset_contains_set(b->solver, ps2, ps1)) return tail;
        }

        // PredStar · Concat(PredStar, tail2) — subsume in concat-tail context
        if (!n00b_regex_charset_is_empty(b->solver, ps1) && nt->kind == N00B_RE_CONCAT) {
            n00b_regex_node_t *ch = n00b_regex_node_get(b, nt->concat.head);
            n00b_regex_charset_t ps_ch = get_pred_star(b, nt->concat.head);
            if (!n00b_regex_charset_is_empty(b->solver, ps_ch)) {
                // head=PredStar(p1), concatHead=PredStar(p2)
                if (n00b_regex_charset_contains_set(b->solver, ps1, ps_ch))
                    return n00b_regex_mk_concat(b, head, nt->concat.tail);
                if (n00b_regex_charset_contains_set(b->solver, ps_ch, ps1))
                    return n00b_regex_mk_concat(b, nt->concat.head, nt->concat.tail);
            }
            // head=PredStar(p1), concatHead=Loop(x,0,1) where p1 ⊇ x.subsumed_by
            // and ConcatSuffix(loopBody) == ConcatSuffix(concat.tail)
            // → skip concatHead
            if (ch->kind == N00B_RE_LOOP && ch->loop.lo == 0 && ch->loop.hi == 1) {
                n00b_regex_node_t *opt_body = n00b_regex_node_get(b, ch->loop.body);
                if (n00b_regex_charset_contains_set(b->solver, ps1, opt_body->subsumed_by)
                    && get_concat_suffix(b, ch->loop.body) == get_concat_suffix(b, nt->concat.tail)) {
                    return n00b_regex_mk_concat(b, head, nt->concat.tail);
                }
            }
        }

        // Loop(x,0,N) · PredStar(p2) where p2 ⊇ head.subsumed_by → return tail
        if (!n00b_regex_charset_is_empty(b->solver, ps2)
            && nh->kind == N00B_RE_LOOP && nh->loop.lo == 0) {
            if (n00b_regex_charset_contains_set(b->solver, ps2, nh->subsumed_by)) {
                return tail;
            }
        }
    }

    // OR-head subsumption: Or(xs) · PredStar → replace always-nullable subsumed
    // children with EPS
    if (nh->kind == N00B_RE_OR) {
        // Extract PredStar from tail (direct or via Concat head)
        n00b_regex_charset_t ps_tail = get_pred_star(b, tail);
        if (n00b_regex_charset_is_empty(b->solver, ps_tail) && nt->kind == N00B_RE_CONCAT) {
            ps_tail = get_pred_star(b, nt->concat.head);
        }
        if (!n00b_regex_charset_is_empty(b->solver, ps_tail)) {
            // Re-read nh in case pool grew
            nh = n00b_regex_node_get(b, head);
            flat_buf_t new_children;
            flat_init(&new_children);
            bool any_changed = false;
            for (uint32_t i = 0; i < nh->multi.count; i++) {
                uint32_t child = nh->multi.children[i];
                if (child == N00B_RE_ID_EPSILON) {
                    flat_push(&new_children, child);
                    continue;
                }
                n00b_regex_node_t *cn = n00b_regex_node_get(b, child);
                if (cn->is_always_nullable
                    && n00b_regex_charset_contains_set(b->solver, ps_tail,
                                                       cn->subsumed_by)) {
                    flat_push(&new_children, N00B_RE_ID_EPSILON);
                    any_changed = true;
                }
                else {
                    flat_push(&new_children, child);
                }
            }
            if (any_changed) {
                sort_u32(new_children.data, new_children.len);
                uint32_t new_or = n00b_regex_mk_or(b, new_children.data, new_children.len);
                flat_free(&new_children);
                return n00b_regex_mk_concat(b, new_or, tail);
            }
            flat_free(&new_children);
            // Re-read after potential pool growth
            nh = n00b_regex_node_get(b, head);
            nt = n00b_regex_node_get(b, tail);
        }
    }

    // Lookbehind merge: LB(body1) · LB(body2) → LB(And(.*body1, .*body2))
    if (nh->kind == N00B_RE_LOOKAROUND && nh->lookaround.look_back
        && nt->kind == N00B_RE_LOOKAROUND && nt->lookaround.look_back) {
        uint32_t c1 = n00b_regex_mk_concat(b, N00B_RE_ID_DOTSTAR, nh->lookaround.body);
        uint32_t c2 = n00b_regex_mk_concat(b, N00B_RE_ID_DOTSTAR, nt->lookaround.body);
        uint32_t both[2];
        if (c1 <= c2) { both[0] = c1; both[1] = c2; }
        else          { both[0] = c2; both[1] = c1; }
        uint32_t combined = n00b_regex_mk_and(b, both, 2);
        return n00b_regex_mk_lookaround(b, combined, true, 0, nullptr, 0);
    }

    // Lookbehind then concat: LB(body1) · Concat(LB(body2), rest) → merge lookbehinds
    if (nh->kind == N00B_RE_LOOKAROUND && nh->lookaround.look_back
        && nt->kind == N00B_RE_CONCAT) {
        n00b_regex_node_t *tc = n00b_regex_node_get(b, nt->concat.head);
        if (tc->kind == N00B_RE_LOOKAROUND && tc->lookaround.look_back) {
            uint32_t c1 = n00b_regex_mk_concat(b, N00B_RE_ID_DOTSTAR, nh->lookaround.body);
            uint32_t c2 = n00b_regex_mk_concat(b, N00B_RE_ID_DOTSTAR, tc->lookaround.body);
            uint32_t both[2];
            if (c1 <= c2) { both[0] = c1; both[1] = c2; }
            else          { both[0] = c2; both[1] = c1; }
            uint32_t combined = n00b_regex_mk_and(b, both, 2);
            uint32_t look = n00b_regex_mk_lookaround(b, combined, true, 0, nullptr, 0);
            return n00b_regex_mk_concat(b, look, nt->concat.tail);
        }
    }

    // Lookbehind with PredStar body subsumption:
    // LB(PredStar(p)) · tail where p ⊇ tail.subsumed_by → tail
    if (nh->kind == N00B_RE_LOOKAROUND && nh->lookaround.look_back) {
        n00b_regex_charset_t lb_ps = get_pred_star(b, nh->lookaround.body);
        if (!n00b_regex_charset_is_empty(b->solver, lb_ps)
            && n00b_regex_charset_contains_set(b->solver, lb_ps, nt->subsumed_by)) {
            return tail;
        }
    }

    // Lookahead merge: LA(body1) · LA(body2) → LA(And(body1·.*, body2·.*))
    // Preserves pending nullables from the first lookahead (second must have none).
    if (nh->kind == N00B_RE_LOOKAROUND && !nh->lookaround.look_back
        && nt->kind == N00B_RE_LOOKAROUND && !nt->lookaround.look_back) {
        uint32_t c1 = n00b_regex_mk_concat(b, nh->lookaround.body, N00B_RE_ID_DOTSTAR);
        uint32_t c2 = n00b_regex_mk_concat(b, nt->lookaround.body, N00B_RE_ID_DOTSTAR);
        uint32_t both[2];
        if (c1 <= c2) { both[0] = c1; both[1] = c2; }
        else          { both[0] = c2; both[1] = c1; }
        uint32_t combined = n00b_regex_mk_and(b, both, 2);
        // Re-read nh after pool growth from mk_concat/mk_and calls
        nh = n00b_regex_node_get(b, head);
        return n00b_regex_mk_lookaround(b, combined, false,
                                         nh->lookaround.relative_to,
                                         nh->lookaround.pending_nullable_pos,
                                         nh->lookaround.n_pending);
    }

    // Nullable-head · PredStarHead subsumption:
    // head · PredStarHead(p) where head always-nullable and p ⊇ head.subsumed_by → tail
    {
        n00b_regex_charset_t ps = get_pred_star_head(b, tail);
        if (!n00b_regex_charset_is_empty(b->solver, ps)
            && nh->is_always_nullable
            && n00b_regex_charset_contains_set(b->solver, ps, nh->subsumed_by)) {
            return tail;
        }
    }

    // Nested concat flattening: Concat(h1, h2) · tail → Concat(h1, Concat(h2, tail))
    if (nh->kind == N00B_RE_CONCAT) {
        uint32_t new_tail = n00b_regex_mk_concat(b, nh->concat.tail, tail);
        // Re-read nh since the pool may have grown
        nh = n00b_regex_node_get(b, head);
        head = nh->concat.head;
        tail = new_tail;
    }

    // Dedup / register
    uint64_t key = node_hash(N00B_RE_CONCAT, head, tail, 0);
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return cached;

    n00b_regex_node_t node = {
        .kind   = N00B_RE_CONCAT,
        .concat = {.head = head, .tail = tail},
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return id;
}

// ============================================================================
// mk_or2 — Phase 2: Full resharp two-child Or optimizations
// ============================================================================

uint32_t
n00b_regex_mk_or2(n00b_regex_builder_t *b, uint32_t a, uint32_t b_)
{
    if (a == b_) return a;
    if (a == N00B_RE_ID_NOTHING) return b_;
    if (b_ == N00B_RE_ID_NOTHING) return a;
    if (a == N00B_RE_ID_DOTSTAR || b_ == N00B_RE_ID_DOTSTAR) return N00B_RE_ID_DOTSTAR;

    n00b_regex_node_t *na = n00b_regex_node_get(b, a);
    n00b_regex_node_t *nb = n00b_regex_node_get(b, b_);

    // Complement law: Not(x) | x = DOTSTAR
    if (na->kind == N00B_RE_NOT && na->not_.inner == b_) return N00B_RE_ID_DOTSTAR;
    if (nb->kind == N00B_RE_NOT && nb->not_.inner == a) return N00B_RE_ID_DOTSTAR;

    // Singleton merge: Singleton(p1) | Singleton(p2) → Singleton(p1 | p2)
    if (na->kind == N00B_RE_SINGLETON && nb->kind == N00B_RE_SINGLETON) {
        return n00b_regex_mk_singleton(b,
            n00b_regex_charset_or(b->solver, na->singleton.set, nb->singleton.set));
    }

    // Epsilon normalization: EPS | x → Loop(x, 0, 1) (and symmetric)
    if (a == N00B_RE_ID_EPSILON) return n00b_regex_mk_loop(b, b_, 0, 1);
    if (b_ == N00B_RE_ID_EPSILON) return n00b_regex_mk_loop(b, a, 0, 1);

    // Re-read after possible pool growth
    na = n00b_regex_node_get(b, a);
    nb = n00b_regex_node_get(b, b_);

    // Loop range merge: Loop(body,lo1,hi1) | Loop(body,lo2,hi2) same body
    if (na->kind == N00B_RE_LOOP && nb->kind == N00B_RE_LOOP
        && na->loop.body == nb->loop.body) {
        int32_t lo = na->loop.lo < nb->loop.lo ? na->loop.lo : nb->loop.lo;
        int32_t hi = na->loop.hi > nb->loop.hi ? na->loop.hi : nb->loop.hi;
        return n00b_regex_mk_loop(b, na->loop.body, lo, hi);
    }

    // Loop body absorption: Loop(body,lo,hi) | body
    if (na->kind == N00B_RE_LOOP && na->loop.body == b_) {
        if (na->loop.lo >= 2) {
            return n00b_regex_mk_loop(b, b_, 1, na->loop.hi);
        }
        if (na->loop.lo == 0 && na->loop.hi == 1) return a; // x? | x = x?
    }
    if (nb->kind == N00B_RE_LOOP && nb->loop.body == a) {
        if (nb->loop.lo >= 2) {
            return n00b_regex_mk_loop(b, a, 1, nb->loop.hi);
        }
        if (nb->loop.lo == 0 && nb->loop.hi == 1) return b_; // x | x? = x?
    }

    // Re-read
    na = n00b_regex_node_get(b, a);
    nb = n00b_regex_node_get(b, b_);

    // Nested Or flatten + membership check
    if (na->kind == N00B_RE_OR) {
        if (sorted_contains(na->multi.children, na->multi.count, b_)) return a;
        // Merge into existing Or
        flat_buf_t fb;
        flat_init(&fb);
        for (uint32_t i = 0; i < na->multi.count; i++) flat_push(&fb, na->multi.children[i]);
        flat_push(&fb, b_);
        sort_u32(fb.data, fb.len);
        uint32_t result = n00b_regex_mk_or(b, fb.data, fb.len);
        flat_free(&fb);
        return result;
    }
    if (nb->kind == N00B_RE_OR) {
        if (sorted_contains(nb->multi.children, nb->multi.count, a)) return b_;
        flat_buf_t fb;
        flat_init(&fb);
        for (uint32_t i = 0; i < nb->multi.count; i++) flat_push(&fb, nb->multi.children[i]);
        flat_push(&fb, a);
        sort_u32(fb.data, fb.len);
        uint32_t result = n00b_regex_mk_or(b, fb.data, fb.len);
        flat_free(&fb);
        return result;
    }

    // Concat head factoring: Concat(h,t1) | Concat(h,t2) → Concat(h, Or(t1,t2))
    if (na->kind == N00B_RE_CONCAT && nb->kind == N00B_RE_CONCAT
        && na->concat.head == nb->concat.head) {
        uint32_t new_tail = n00b_regex_mk_or2(b, na->concat.tail, nb->concat.tail);
        return n00b_regex_mk_concat(b, na->concat.head, new_tail);
    }

    // Tail factoring: Concat(h1,t) | Concat(h2,t) where same tail → Concat(Or(h1,h2), t)
    if (na->kind == N00B_RE_CONCAT && nb->kind == N00B_RE_CONCAT) {
        flat_buf_t heads_a, heads_b;
        flat_init(&heads_a);
        flat_init(&heads_b);
        uint32_t tail_a = split_tail(b, a, &heads_a);
        uint32_t tail_b = split_tail(b, b_, &heads_b);
        if (tail_a == tail_b && heads_a.len > 0 && heads_b.len > 0) {
            uint32_t ha = rebuild_concat_from_heads(b, heads_a.data, heads_a.len);
            uint32_t hb = rebuild_concat_from_heads(b, heads_b.data, heads_b.len);
            uint32_t new_head = n00b_regex_mk_or2(b, ha, hb);
            flat_free(&heads_a);
            flat_free(&heads_b);
            return n00b_regex_mk_concat(b, new_head, tail_a);
        }
        flat_free(&heads_a);
        flat_free(&heads_b);
    }

    // Re-read after possible pool growth
    na = n00b_regex_node_get(b, a);
    nb = n00b_regex_node_get(b, b_);

    // Anchor + lookaround merge
    if ((na->kind == N00B_RE_BEGIN || na->kind == N00B_RE_END)
        && nb->kind == N00B_RE_LOOKAROUND) {
        uint32_t new_body = n00b_regex_mk_or2(b, a, nb->lookaround.body);
        return n00b_regex_mk_lookaround(b, new_body, nb->lookaround.look_back,
                                         nb->lookaround.relative_to,
                                         nb->lookaround.pending_nullable_pos,
                                         nb->lookaround.n_pending);
    }
    if ((nb->kind == N00B_RE_BEGIN || nb->kind == N00B_RE_END)
        && na->kind == N00B_RE_LOOKAROUND) {
        uint32_t new_body = n00b_regex_mk_or2(b, b_, na->lookaround.body);
        return n00b_regex_mk_lookaround(b, new_body, na->lookaround.look_back,
                                         na->lookaround.relative_to,
                                         na->lookaround.pending_nullable_pos,
                                         na->lookaround.n_pending);
    }

    // Re-read
    na = n00b_regex_node_get(b, a);
    nb = n00b_regex_node_get(b, b_);

    // PredStar subsumption: two PredStars
    {
        n00b_regex_charset_t ps_a = get_pred_star(b, a);
        n00b_regex_charset_t ps_b = get_pred_star(b, b_);
        if (!n00b_regex_charset_is_empty(b->solver, ps_a)
            && !n00b_regex_charset_is_empty(b->solver, ps_b)) {
            if (n00b_regex_charset_contains_set(b->solver, ps_a, ps_b)) return a;
            if (n00b_regex_charset_contains_set(b->solver, ps_b, ps_a)) return b_;
        }
    }

    // Loop subsumption via MinLength/MaxLength:
    // Loop(Singleton(pred), lo, hi) | other where pred ⊇ other.subsumed_by
    // and lo ≤ other.min_length and hi ≥ other.max_length → return the Loop
    na = n00b_regex_node_get(b, a);
    nb = n00b_regex_node_get(b, b_);
    {
        n00b_regex_charset_t pred;
        int32_t lo, hi;
        if (get_pred_loop(b, a, &pred, &lo, &hi)) {
            if (n00b_regex_charset_contains_set(b->solver, pred, nb->subsumed_by)
                && lo <= nb->min_length && hi >= nb->max_length) {
                return a;
            }
        }
        if (get_pred_loop(b, b_, &pred, &lo, &hi)) {
            if (n00b_regex_charset_contains_set(b->solver, pred, na->subsumed_by)
                && lo <= na->min_length && hi >= na->max_length) {
                return b_;
            }
        }
    }

    // Default: sort and delegate to mk_or
    uint32_t children[2];
    if (a <= b_) {
        children[0] = a;
        children[1] = b_;
    }
    else {
        children[0] = b_;
        children[1] = a;
    }
    return n00b_regex_mk_or(b, children, 2);
}

// ============================================================================
// mk_or — Phase 3: Full resharp multi-child Or optimizations
// ============================================================================

uint32_t
n00b_regex_mk_or(n00b_regex_builder_t *b, uint32_t *children, uint32_t count)
{
    if (count == 0) return N00B_RE_ID_NOTHING;
    if (count == 1) return children[0];

    // --- Phase 1: Flatten + collect ---
    flat_buf_t fb;
    flat_init(&fb);

    bool has_epsilon        = false;
    bool has_dotstar        = false;

    // Flatten stack
    uint32_t stack[64];
    int sp = 0;
    for (uint32_t i = 0; i < count && sp < 64; i++) {
        stack[sp++] = children[i];
    }

    while (sp > 0) {
        uint32_t c = stack[--sp];
        if (c == N00B_RE_ID_NOTHING) continue;
        if (c == N00B_RE_ID_DOTSTAR) { has_dotstar = true; break; }
        if (c == N00B_RE_ID_EPSILON) { has_epsilon = true; continue; }

        n00b_regex_node_t *cn = n00b_regex_node_get(b, c);
        if (cn->kind == N00B_RE_OR) {
            for (uint32_t i = 0; i < cn->multi.count && sp < 64; i++) {
                stack[sp++] = cn->multi.children[i];
            }
            continue;
        }

        if (!flat_contains(&fb, c)) {
            flat_push(&fb, c);
        }
    }

    if (has_dotstar) {
        flat_free(&fb);
        return N00B_RE_ID_DOTSTAR;
    }

    // --- Epsilon gating: only add EPS if no child is already always-nullable ---
    if (has_epsilon) {
        bool any_always_nullable = false;
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (cn->is_always_nullable) { any_always_nullable = true; break; }
        }
        if (!any_always_nullable) {
            flat_push(&fb, N00B_RE_ID_EPSILON);
        }
    }

    // --- Singleton merging ---
    {
        n00b_regex_charset_t merged_set = b->solver->false_id;
        uint32_t n_singletons = 0;
        // First pass: collect and remove singletons
        for (uint32_t i = 0; i < fb.len; ) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (cn->kind == N00B_RE_SINGLETON) {
                merged_set = n00b_regex_charset_or(b->solver, merged_set, cn->singleton.set);
                n_singletons++;
                fb.data[i] = fb.data[--fb.len]; // Remove by swap-last
            }
            else {
                i++;
            }
        }
        if (n_singletons > 0) {
            uint32_t merged = n00b_regex_mk_singleton(b, merged_set);
            flat_push(&fb, merged);
        }
    }

    // --- Singleton-star subsumption ---
    // For each Loop(Singleton(pred), 0, MAX), remove children whose subsumed_by ⊆ pred
    {
        // Collect PredStar children
        uint32_t pstar_buf[16];
        n00b_regex_charset_t pstar_pred_buf[16];
        uint32_t n_pstars = 0;
        for (uint32_t i = 0; i < fb.len && n_pstars < 16; i++) {
            n00b_regex_charset_t ps = get_pred_star(b, fb.data[i]);
            if (!n00b_regex_charset_is_empty(b->solver, ps)) {
                pstar_buf[n_pstars] = fb.data[i];
                pstar_pred_buf[n_pstars] = ps;
                n_pstars++;
            }
        }
        for (uint32_t si = 0; si < n_pstars; si++) {
            for (uint32_t i = 0; i < fb.len; ) {
                if (fb.data[i] == pstar_buf[si]) { i++; continue; }
                n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
                if (n00b_regex_charset_contains_set(b->solver, pstar_pred_buf[si], cn->subsumed_by)) {
                    fb.data[i] = fb.data[--fb.len]; // Remove subsumed
                }
                else {
                    i++;
                }
            }
        }
    }

    // --- mergeOrIntersections: remove subsumed And nodes ---
    // And(xs) is subsumed by And(ys) in Or context when ys ⊂ xs
    // (the broader And with fewer constraints wins)
    {
        uint32_t n_ands = 0;
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (cn->kind == N00B_RE_AND) n_ands++;
        }
        if (n_ands >= 2) {
            for (uint32_t i = 0; i < fb.len; ) {
                n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
                if (ni->kind != N00B_RE_AND) { i++; continue; }
                bool subsumed = false;
                for (uint32_t j = 0; j < fb.len && !subsumed; j++) {
                    if (i == j) continue;
                    n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                    if (nj->kind != N00B_RE_AND) continue;
                    if (nj->multi.count >= ni->multi.count) continue;
                    // Check if every child of nj is in ni (nj ⊂ ni → ni subsumed)
                    bool all_in = true;
                    for (uint32_t k = 0; k < nj->multi.count && all_in; k++) {
                        if (!sorted_contains(ni->multi.children, ni->multi.count,
                                             nj->multi.children[k])) {
                            all_in = false;
                        }
                    }
                    if (all_in) subsumed = true;
                }
                if (subsumed) {
                    fb.data[i] = fb.data[--fb.len];
                }
                else {
                    i++;
                }
            }
        }
    }

    // --- mergeOrLookaheads: merge lookaheads with same body ---
    {
        // Find lookahead pairs with the same body and merge their pending arrays
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
            if (ni->kind != N00B_RE_LOOKAROUND || ni->lookaround.look_back) continue;

            for (uint32_t j = i + 1; j < fb.len; ) {
                n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                if (nj->kind != N00B_RE_LOOKAROUND || nj->lookaround.look_back
                    || nj->lookaround.body != ni->lookaround.body) {
                    j++;
                    continue;
                }
                // Same body: merge pending nullable positions with min relative_to
                int32_t min_rel = ni->lookaround.relative_to < nj->lookaround.relative_to
                                ? ni->lookaround.relative_to : nj->lookaround.relative_to;
                // Collect all pending positions, offset-adjusted
                int32_t merged_pending[128];
                uint32_t mp = 0;
                int32_t off_i = ni->lookaround.relative_to - min_rel;
                for (uint16_t k = 0; k < ni->lookaround.n_pending && mp < 128; k++) {
                    merged_pending[mp++] = ni->lookaround.pending_nullable_pos[k] + off_i;
                }
                int32_t off_j = nj->lookaround.relative_to - min_rel;
                for (uint16_t k = 0; k < nj->lookaround.n_pending && mp < 128; k++) {
                    merged_pending[mp++] = nj->lookaround.pending_nullable_pos[k] + off_j;
                }
                // Sort and deduplicate
                if (mp > 1) {
                    for (uint32_t a = 1; a < mp; a++) {
                        int32_t key = merged_pending[a];
                        uint32_t p = a;
                        while (p > 0 && merged_pending[p - 1] > key) {
                            merged_pending[p] = merged_pending[p - 1]; p--;
                        }
                        merged_pending[p] = key;
                    }
                    uint32_t w = 1;
                    for (uint32_t r = 1; r < mp; r++) {
                        if (merged_pending[r] != merged_pending[w - 1])
                            merged_pending[w++] = merged_pending[r];
                    }
                    mp = w;
                }
                uint32_t merged = n00b_regex_mk_lookaround(b, ni->lookaround.body, false,
                                                            min_rel, merged_pending, (uint16_t)mp);
                fb.data[i] = merged;
                fb.data[j] = fb.data[--fb.len];
                // Re-read ni since pool may have grown
                ni = n00b_regex_node_get(b, fb.data[i]);
                // Don't increment j — re-check swapped element
            }
        }
    }

    // --- mergeOrNonZeroLoops: merge all loops by (body, tail) with range union ---
    // Generalizes the old zero-loop merge to handle any (lo, hi) ranges.
    // Groups by (body, tail) key, collects ranges, merges via range_union.
    {
        // Extract (body, tail, lo, hi) from each eligible node.
        // We do O(n²) grouping since n is typically small.
        typedef struct { uint32_t body; uint32_t tail; } bt_key_t;
        bt_key_t keys[256];
        range_pair_t ranges[256];
        uint32_t fb_idx[256]; // which fb index this came from
        uint32_t n_entries = 0;

        for (uint32_t i = 0; i < fb.len && n_entries < 256; i++) {
            n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
            uint32_t body, tail;
            int32_t lo, hi;
            bool eligible = false;

            if (ni->kind == N00B_RE_LOOP && ni->loop.hi != INT32_MAX) {
                body = ni->loop.body; tail = N00B_RE_ID_EPSILON;
                lo = ni->loop.lo; hi = ni->loop.hi;
                eligible = true;
            }
            else if (ni->kind == N00B_RE_SINGLETON) {
                body = fb.data[i]; tail = N00B_RE_ID_EPSILON;
                lo = 1; hi = 1;
                eligible = true;
            }
            else if (ni->kind == N00B_RE_CONCAT) {
                n00b_regex_node_t *ch = n00b_regex_node_get(b, ni->concat.head);
                if (ch->kind == N00B_RE_LOOP && ch->loop.hi != INT32_MAX) {
                    body = ch->loop.body; tail = ni->concat.tail;
                    lo = ch->loop.lo; hi = ch->loop.hi;
                    eligible = true;
                }
                else if (ch->kind != N00B_RE_LOOP) {
                    // Non-loop head in concat: treat as {1,1}
                    body = ni->concat.head; tail = ni->concat.tail;
                    lo = 1; hi = 1;
                    eligible = true;
                }
            }
            if (!eligible) continue;

            keys[n_entries]   = (bt_key_t){body, tail};
            ranges[n_entries] = (range_pair_t){lo, hi};
            fb_idx[n_entries] = i;
            n_entries++;
        }

        // Group by (body, tail) and merge
        bool used[256] = {};
        bool any_merged = false;

        for (uint32_t i = 0; i < n_entries; i++) {
            if (used[i]) continue;
            // Collect all entries with same (body, tail)
            range_pair_t group_ranges[64];
            uint32_t group_fb[64];
            uint32_t gn = 0;
            group_ranges[gn] = ranges[i];
            group_fb[gn] = fb_idx[i];
            gn++;
            used[i] = true;

            for (uint32_t j = i + 1; j < n_entries && gn < 64; j++) {
                if (used[j]) continue;
                if (keys[j].body == keys[i].body && keys[j].tail == keys[i].tail) {
                    group_ranges[gn] = ranges[j];
                    group_fb[gn] = fb_idx[j];
                    gn++;
                    used[j] = true;
                }
            }
            if (gn < 2) continue;

            // Merge ranges
            uint32_t merged_n = range_union(group_ranges, gn);

            // Mark all original fb entries for removal except first
            // We'll rebuild into the first slot and remove the rest
            any_merged = true;

            // Build new nodes from merged ranges
            flat_buf_t new_nodes;
            flat_init(&new_nodes);
            for (uint32_t r = 0; r < merged_n; r++) {
                uint32_t loop_node = n00b_regex_mk_loop(b, keys[i].body,
                                                         group_ranges[r].lo,
                                                         group_ranges[r].hi);
                uint32_t node = (keys[i].tail == N00B_RE_ID_EPSILON)
                              ? loop_node
                              : n00b_regex_mk_concat(b, loop_node, keys[i].tail);
                flat_push(&new_nodes, node);
            }

            // Replace first entry, remove rest
            fb.data[group_fb[0]] = new_nodes.len > 0 ? new_nodes.data[0] : N00B_RE_ID_NOTHING;
            // Add extra merged ranges
            for (uint32_t r = 1; r < new_nodes.len; r++) {
                flat_push(&fb, new_nodes.data[r]);
            }
            // Remove other original entries (iterate backwards to keep indices valid)
            for (int32_t g = (int32_t)gn - 1; g >= 1; g--) {
                uint32_t idx = group_fb[g];
                if (idx < fb.len) {
                    fb.data[idx] = fb.data[--fb.len];
                }
            }
            flat_free(&new_nodes);
        }
    }

    // --- Head factoring (mergeOrGroupedHeads) ---
    // Group Concat children by head, factor out common head
    {
        bool any_grouped = false;
        for (uint32_t i = 0; i < fb.len && !any_grouped; i++) {
            n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
            if (ni->kind != N00B_RE_CONCAT && ni->kind != N00B_RE_SINGLETON) continue;
            uint32_t i_head = (ni->kind == N00B_RE_CONCAT) ? ni->concat.head : fb.data[i];
            for (uint32_t j = i + 1; j < fb.len; j++) {
                n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                uint32_t j_head;
                if (nj->kind == N00B_RE_CONCAT) j_head = nj->concat.head;
                else if (nj->kind == N00B_RE_SINGLETON) j_head = fb.data[j];
                else continue;
                if (i_head == j_head) { any_grouped = true; break; }
            }
        }

        if (any_grouped) {
            flat_buf_t out;
            flat_init(&out);
            bool visited[256] = {};
            uint32_t len = fb.len < 256 ? fb.len : 256;

            for (uint32_t i = 0; i < len; i++) {
                if (visited[i]) continue;
                n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
                if (ni->kind != N00B_RE_CONCAT && ni->kind != N00B_RE_SINGLETON) {
                    flat_push(&out, fb.data[i]);
                    continue;
                }
                uint32_t i_head = (ni->kind == N00B_RE_CONCAT) ? ni->concat.head : fb.data[i];
                uint32_t i_tail = (ni->kind == N00B_RE_CONCAT) ? ni->concat.tail : N00B_RE_ID_EPSILON;

                // Collect all tails sharing this head
                flat_buf_t tails;
                flat_init(&tails);
                flat_push(&tails, i_tail);
                visited[i] = true;

                for (uint32_t j = i + 1; j < len; j++) {
                    if (visited[j]) continue;
                    n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                    uint32_t j_head;
                    if (nj->kind == N00B_RE_CONCAT) j_head = nj->concat.head;
                    else if (nj->kind == N00B_RE_SINGLETON) j_head = fb.data[j];
                    else continue;
                    if (j_head != i_head) continue;
                    uint32_t j_tail = (nj->kind == N00B_RE_CONCAT) ? nj->concat.tail : N00B_RE_ID_EPSILON;
                    flat_push(&tails, j_tail);
                    visited[j] = true;
                }

                if (tails.len == 1) {
                    flat_push(&out, fb.data[i]);
                }
                else {
                    sort_u32(tails.data, tails.len);
                    uint32_t new_tail = n00b_regex_mk_or(b, tails.data, tails.len);
                    uint32_t factored = n00b_regex_mk_concat(b, i_head, new_tail);
                    flat_push(&out, factored);
                }
                flat_free(&tails);
            }
            // Copy ungrouped items from beyond 256
            for (uint32_t i = len; i < fb.len; i++) {
                flat_push(&out, fb.data[i]);
            }

            flat_free(&fb);
            fb = out;
        }
    }

    // --- Tail factoring (mergeOrGroupedTails) ---
    // Group Concat nodes by tail (via split_tail), factor common suffix
    {
        bool any_tail_grouped = false;
        // Quick scan: do any two Concat nodes share a tail?
        for (uint32_t i = 0; i < fb.len && !any_tail_grouped; i++) {
            n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
            if (ni->kind != N00B_RE_CONCAT) continue;
            flat_buf_t hi;
            flat_init(&hi);
            uint32_t ti = split_tail(b, fb.data[i], &hi);
            flat_free(&hi);
            if (ti == N00B_RE_ID_EPSILON) continue;
            for (uint32_t j = i + 1; j < fb.len; j++) {
                n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                if (nj->kind != N00B_RE_CONCAT) continue;
                flat_buf_t hj;
                flat_init(&hj);
                uint32_t tj = split_tail(b, fb.data[j], &hj);
                flat_free(&hj);
                if (ti == tj) { any_tail_grouped = true; break; }
            }
        }

        if (any_tail_grouped) {
            flat_buf_t out;
            flat_init(&out);
            bool visited[256] = {};
            uint32_t len = fb.len < 256 ? fb.len : 256;

            for (uint32_t i = 0; i < len; i++) {
                if (visited[i]) continue;
                n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
                if (ni->kind != N00B_RE_CONCAT) {
                    flat_push(&out, fb.data[i]);
                    continue;
                }
                flat_buf_t hi;
                flat_init(&hi);
                uint32_t ti = split_tail(b, fb.data[i], &hi);
                if (ti == N00B_RE_ID_EPSILON || hi.len == 0) {
                    flat_free(&hi);
                    flat_push(&out, fb.data[i]);
                    continue;
                }

                // Collect all heads sharing this tail
                flat_buf_t heads_list;
                flat_init(&heads_list);
                uint32_t rebuilt_i = rebuild_concat_from_heads(b, hi.data, hi.len);
                flat_push(&heads_list, rebuilt_i);
                flat_free(&hi);
                visited[i] = true;

                for (uint32_t j = i + 1; j < len; j++) {
                    if (visited[j]) continue;
                    n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                    if (nj->kind != N00B_RE_CONCAT) continue;
                    flat_buf_t hj;
                    flat_init(&hj);
                    uint32_t tj = split_tail(b, fb.data[j], &hj);
                    if (tj != ti || hj.len == 0) {
                        flat_free(&hj);
                        continue;
                    }
                    uint32_t rebuilt_j = rebuild_concat_from_heads(b, hj.data, hj.len);
                    flat_push(&heads_list, rebuilt_j);
                    flat_free(&hj);
                    visited[j] = true;
                }

                if (heads_list.len == 1) {
                    flat_push(&out, fb.data[i]);
                }
                else {
                    sort_u32(heads_list.data, heads_list.len);
                    uint32_t new_head = n00b_regex_mk_or(b, heads_list.data, heads_list.len);
                    uint32_t factored = n00b_regex_mk_concat(b, new_head, ti);
                    flat_push(&out, factored);
                }
                flat_free(&heads_list);
            }
            for (uint32_t i = len; i < fb.len; i++) {
                flat_push(&out, fb.data[i]);
            }
            flat_free(&fb);
            fb = out;
        }
    }

    // --- Final result ---
    if (fb.len == 0) {
        flat_free(&fb);
        return N00B_RE_ID_NOTHING;
    }
    if (fb.len == 1) {
        uint32_t result = fb.data[0];
        flat_free(&fb);
        return result;
    }

    sort_u32(fb.data, fb.len);

    uint32_t result = finalize_or(b, fb.data, fb.len);
    flat_free(&fb);
    return result;
}

// ============================================================================
// mk_and — Phase 4: Full resharp AND optimizations
// ============================================================================

uint32_t
n00b_regex_mk_and(n00b_regex_builder_t *b, uint32_t *children, uint32_t count)
{
    if (count == 0) return N00B_RE_ID_DOTSTAR;
    if (count == 1) return children[0];

    // --- Two-child fast path (Gap E) ---
    if (count == 2) {
        uint32_t a = children[0], b_ = children[1];
        if (a == b_) return a;
        if (a == N00B_RE_ID_NOTHING || b_ == N00B_RE_ID_NOTHING) return N00B_RE_ID_NOTHING;
        if (a == N00B_RE_ID_DOTSTAR) return b_;
        if (b_ == N00B_RE_ID_DOTSTAR) return a;

        n00b_regex_node_t *na = n00b_regex_node_get(b, a);
        n00b_regex_node_t *nb = n00b_regex_node_get(b, b_);

        // Complement: Not(x) & x = NOTHING
        if (na->kind == N00B_RE_NOT && na->not_.inner == b_) return N00B_RE_ID_NOTHING;
        if (nb->kind == N00B_RE_NOT && nb->not_.inner == a) return N00B_RE_ID_NOTHING;

        // Singleton merge
        if (na->kind == N00B_RE_SINGLETON && nb->kind == N00B_RE_SINGLETON) {
            return n00b_regex_mk_singleton(b,
                n00b_regex_charset_and(b->solver, na->singleton.set, nb->singleton.set));
        }

        // Epsilon handling
        if (a == N00B_RE_ID_EPSILON) {
            return nb->can_be_nullable ? N00B_RE_ID_EPSILON : N00B_RE_ID_NOTHING;
        }
        if (b_ == N00B_RE_ID_EPSILON) {
            return na->can_be_nullable ? N00B_RE_ID_EPSILON : N00B_RE_ID_NOTHING;
        }

        // And-flattening: And(xs) & y where y ∈ xs → And(xs)
        na = n00b_regex_node_get(b, a);
        if (na->kind == N00B_RE_AND) {
            if (sorted_contains(na->multi.children, na->multi.count, b_)) return a;
        }
        nb = n00b_regex_node_get(b, b_);
        if (nb->kind == N00B_RE_AND) {
            if (sorted_contains(nb->multi.children, nb->multi.count, a)) return b_;
        }

        // PredLoop subsumption
        na = n00b_regex_node_get(b, a);
        nb = n00b_regex_node_get(b, b_);
        {
            n00b_regex_charset_t p1, p2;
            int32_t lo1, hi1, lo2, hi2;
            bool is_pl1 = get_pred_loop(b, a, &p1, &lo1, &hi1);
            bool is_pl2 = get_pred_loop(b, b_, &p2, &lo2, &hi2);
            if (is_pl1 && is_pl2) {
                bool p1_large = n00b_regex_charset_contains_set(b->solver, p1, p2);
                bool p2_large = n00b_regex_charset_contains_set(b->solver, p2, p1);
                bool r1_large = (lo1 <= lo2 && hi1 >= hi2);
                bool r2_large = (lo2 <= lo1 && hi2 >= hi1);
                if ((p1_large || p2_large) && (r1_large || r2_large)) {
                    n00b_regex_charset_t sm_pred = p1_large ? p2 : p1;
                    int32_t sm_lo = r1_large ? lo2 : lo1;
                    int32_t sm_hi = r1_large ? hi2 : hi1;
                    uint32_t sm_sing = n00b_regex_mk_singleton(b, sm_pred);
                    return n00b_regex_mk_loop(b, sm_sing, sm_lo, sm_hi);
                }
            }
        }

        // PredStar subsumption
        na = n00b_regex_node_get(b, a);
        nb = n00b_regex_node_get(b, b_);
        {
            n00b_regex_charset_t ps_a = get_pred_star(b, a);
            if (!n00b_regex_charset_is_empty(b->solver, ps_a)
                && n00b_regex_charset_contains_set(b->solver, ps_a, nb->subsumed_by)) {
                return b_;
            }
            n00b_regex_charset_t ps_b = get_pred_star(b, b_);
            na = n00b_regex_node_get(b, a);
            if (!n00b_regex_charset_is_empty(b->solver, ps_b)
                && n00b_regex_charset_contains_set(b->solver, ps_b, na->subsumed_by)) {
                return a;
            }
        }

        // Fall through to general N-child path
    }

    // --- Flatten + collect ---
    flat_buf_t fb;
    flat_init(&fb);
    flat_buf_t complements;
    flat_init(&complements);

    bool has_nothing  = false;
    bool has_epsilon  = false;

    uint32_t stack[64];
    int sp = 0;
    for (uint32_t i = 0; i < count && sp < 64; i++) {
        stack[sp++] = children[i];
    }

    while (sp > 0) {
        uint32_t c = stack[--sp];
        if (c == N00B_RE_ID_NOTHING) { has_nothing = true; break; }
        if (c == N00B_RE_ID_DOTSTAR) continue;
        if (c == N00B_RE_ID_EPSILON) { has_epsilon = true; continue; }

        n00b_regex_node_t *cn = n00b_regex_node_get(b, c);
        if (cn->kind == N00B_RE_AND) {
            for (uint32_t i = 0; i < cn->multi.count && sp < 64; i++) {
                stack[sp++] = cn->multi.children[i];
            }
            continue;
        }

        // Collect complements for De Morgan merge
        if (cn->kind == N00B_RE_NOT) {
            flat_push(&complements, cn->not_.inner);
            continue;
        }

        if (!flat_contains(&fb, c)) {
            flat_push(&fb, c);
        }
    }

    if (has_nothing) {
        flat_free(&fb);
        flat_free(&complements);
        return N00B_RE_ID_NOTHING;
    }

    // --- EPS + non-nullable → NOTHING ---
    if (has_epsilon) {
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (!cn->can_be_nullable) {
                flat_free(&fb);
                flat_free(&complements);
                return N00B_RE_ID_NOTHING;
            }
        }
        flat_push(&fb, N00B_RE_ID_EPSILON);
    }

    // --- De Morgan: merge all Not(x) into Not(Or(all x)) ---
    if (complements.len > 0) {
        sort_u32(complements.data, complements.len);
        uint32_t or_inner = n00b_regex_mk_or(b, complements.data, complements.len);
        auto not_result = n00b_regex_mk_not(b, or_inner);
        if (n00b_result_is_ok(not_result)) {
            uint32_t merged_not = n00b_result_get(not_result);
            if (!flat_contains(&fb, merged_not)) {
                flat_push(&fb, merged_not);
            }
        }
        else {
            // De Morgan merge failed (unsupported inner) — keep individual Not nodes
            for (uint32_t i = 0; i < complements.len; i++) {
                auto individual = n00b_regex_mk_not(b, complements.data[i]);
                if (n00b_result_is_ok(individual)) {
                    uint32_t nid = n00b_result_get(individual);
                    if (!flat_contains(&fb, nid)) {
                        flat_push(&fb, nid);
                    }
                }
            }
        }
    }
    flat_free(&complements);

    // --- Singleton-star subsumption ---
    {
        uint32_t pstar_buf[16];
        n00b_regex_charset_t pstar_pred[16];
        uint32_t n_pstars = 0;
        for (uint32_t i = 0; i < fb.len && n_pstars < 16; i++) {
            n00b_regex_charset_t ps = get_pred_star(b, fb.data[i]);
            if (!n00b_regex_charset_is_empty(b->solver, ps)) {
                pstar_buf[n_pstars] = fb.data[i];
                pstar_pred[n_pstars] = ps;
                n_pstars++;
            }
        }
        // Remove PredStar if some other node is fully subsumed by it
        for (uint32_t si = 0; si < n_pstars; si++) {
            bool subsumes_something = false;
            for (uint32_t i = 0; i < fb.len; i++) {
                if (fb.data[i] == pstar_buf[si]) continue;
                n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
                if (n00b_regex_charset_contains_set(b->solver, pstar_pred[si], cn->subsumed_by)) {
                    subsumes_something = true;
                    break;
                }
            }
            if (subsumes_something) {
                flat_remove(&fb, pstar_buf[si]);
            }
        }
    }

    // --- All-singleton collapse ---
    {
        bool all_sing = true;
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (cn->kind != N00B_RE_SINGLETON) { all_sing = false; break; }
        }
        if (all_sing && fb.len > 0) {
            n00b_regex_charset_t merged = b->solver->true_id;
            for (uint32_t i = 0; i < fb.len; i++) {
                n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
                merged = n00b_regex_charset_and(b->solver, merged, cn->singleton.set);
            }
            flat_free(&fb);
            return n00b_regex_mk_singleton(b, merged);
        }
    }

    // --- PredLoop subsumption: N-child pairwise ---
    // For each pair of PredLoop children, if one subsumes the other
    // (pred containment + range containment), remove the subsumed one.
    {
        n00b_regex_charset_t pl_preds[64];
        int32_t pl_lo[64], pl_hi[64];
        uint32_t pl_idx[64];
        uint32_t n_pl = 0;

        for (uint32_t i = 0; i < fb.len && n_pl < 64; i++) {
            n00b_regex_charset_t p;
            int32_t lo, hi;
            if (get_pred_loop(b, fb.data[i], &p, &lo, &hi)) {
                pl_preds[n_pl] = p;
                pl_lo[n_pl]    = lo;
                pl_hi[n_pl]    = hi;
                pl_idx[n_pl]   = i;
                n_pl++;
            }
        }

        if (n_pl >= 2) {
            bool removed[64] = {};
            for (uint32_t i = 0; i < n_pl; i++) {
                if (removed[i]) continue;
                for (uint32_t j = i + 1; j < n_pl; j++) {
                    if (removed[j]) continue;
                    bool i_pred_large = n00b_regex_charset_contains_set(b->solver, pl_preds[i], pl_preds[j]);
                    bool j_pred_large = n00b_regex_charset_contains_set(b->solver, pl_preds[j], pl_preds[i]);
                    bool i_range_large = (pl_lo[i] <= pl_lo[j] && pl_hi[i] >= pl_hi[j]);
                    bool j_range_large = (pl_lo[j] <= pl_lo[i] && pl_hi[j] >= pl_hi[i]);

                    if ((i_pred_large || j_pred_large) && (i_range_large || j_range_large)) {
                        // Keep the smaller (more restrictive) of the two
                        // Smaller pred = the one NOT large; smaller range = the one NOT large
                        n00b_regex_charset_t sm_pred = i_pred_large ? pl_preds[j] : pl_preds[i];
                        int32_t sm_lo = i_range_large ? pl_lo[j] : pl_lo[i];
                        int32_t sm_hi = i_range_large ? pl_hi[j] : pl_hi[i];
                        uint32_t sm_sing = n00b_regex_mk_singleton(b, sm_pred);
                        uint32_t sm_node = n00b_regex_mk_loop(b, sm_sing, sm_lo, sm_hi);

                        // Replace one, remove the other
                        fb.data[pl_idx[i]] = sm_node;
                        removed[j] = true;
                        // Update i's info for subsequent comparisons
                        pl_preds[i] = sm_pred;
                        pl_lo[i] = sm_lo;
                        pl_hi[i] = sm_hi;
                    }
                }
            }
            // Remove marked entries (backwards to preserve indices)
            for (int32_t i = (int32_t)n_pl - 1; i >= 0; i--) {
                if (removed[i]) {
                    uint32_t idx = pl_idx[i];
                    if (idx < fb.len) {
                        fb.data[idx] = fb.data[--fb.len];
                    }
                }
            }
        }
    }

    // --- Grouped heads by fixed length ---
    // And(Concat(h1,t1), Concat(h2,t2)) where |h1|=|h2|=fixed
    // → Concat(And(h1,h2), And(t1,t2))
    if (fb.len >= 2) {
        flat_buf_t out;
        flat_init(&out);
        bool visited[256] = {};
        uint32_t len = fb.len < 256 ? fb.len : 256;
        bool did_group = false;

        for (uint32_t i = 0; i < len; i++) {
            if (visited[i]) continue;
            n00b_regex_node_t *ni = n00b_regex_node_get(b, fb.data[i]);
            if (ni->kind != N00B_RE_CONCAT) {
                flat_push(&out, fb.data[i]);
                continue;
            }
            int32_t fixlen = get_fixed_length(b, ni->concat.head);
            if (fixlen <= 0 || ni->concat.tail == N00B_RE_ID_EPSILON) {
                flat_push(&out, fb.data[i]);
                continue;
            }

            // Collect all Concat children with same head fixed-length
            flat_buf_t heads, tails;
            flat_init(&heads);
            flat_init(&tails);
            flat_push(&heads, ni->concat.head);
            flat_push(&tails, ni->concat.tail);
            visited[i] = true;

            for (uint32_t j = i + 1; j < len; j++) {
                if (visited[j]) continue;
                n00b_regex_node_t *nj = n00b_regex_node_get(b, fb.data[j]);
                if (nj->kind != N00B_RE_CONCAT) continue;
                if (nj->concat.tail == N00B_RE_ID_EPSILON) continue;
                int32_t fj = get_fixed_length(b, nj->concat.head);
                if (fj != fixlen) continue;
                flat_push(&heads, nj->concat.head);
                flat_push(&tails, nj->concat.tail);
                visited[j] = true;
            }

            if (heads.len == 1) {
                flat_push(&out, fb.data[i]);
            }
            else {
                did_group = true;
                uint32_t new_head = n00b_regex_mk_and(b, heads.data, heads.len);
                uint32_t new_tail = n00b_regex_mk_and(b, tails.data, tails.len);
                uint32_t merged = n00b_regex_mk_concat(b, new_head, new_tail);
                flat_push(&out, merged);
            }
            flat_free(&heads);
            flat_free(&tails);
        }
        for (uint32_t i = len; i < fb.len; i++) {
            flat_push(&out, fb.data[i]);
        }
        if (did_group) {
            flat_free(&fb);
            fb = out;
        }
        else {
            flat_free(&out);
        }
    }

    // --- Grouped tails by fixed length ---
    // And(Concat(h1,t), Concat(h2,t)) where |t|=fixed
    // → Concat(And(h1,h2), And(t1,t2))
    if (fb.len >= 2) {
        flat_buf_t out;
        flat_init(&out);
        bool visited[256] = {};
        uint32_t len = fb.len < 256 ? fb.len : 256;
        bool did_group = false;

        for (uint32_t i = 0; i < len; i++) {
            if (visited[i]) continue;
            // Decompose via split_tail
            flat_buf_t hi;
            flat_init(&hi);
            uint32_t ti = split_tail(b, fb.data[i], &hi);
            int32_t fixlen = get_fixed_length(b, ti);
            if (fixlen <= 0 || hi.len == 0) {
                flat_free(&hi);
                flat_push(&out, fb.data[i]);
                continue;
            }

            // Collect nodes with same tail fixed-length
            flat_buf_t head_nodes, tail_nodes;
            flat_init(&head_nodes);
            flat_init(&tail_nodes);
            uint32_t rebuilt_i = rebuild_concat_from_heads(b, hi.data, hi.len);
            flat_push(&head_nodes, rebuilt_i);
            flat_push(&tail_nodes, ti);
            flat_free(&hi);
            visited[i] = true;

            for (uint32_t j = i + 1; j < len; j++) {
                if (visited[j]) continue;
                flat_buf_t hj;
                flat_init(&hj);
                uint32_t tj = split_tail(b, fb.data[j], &hj);
                int32_t fj = get_fixed_length(b, tj);
                if (fj != fixlen || hj.len == 0) {
                    flat_free(&hj);
                    continue;
                }
                uint32_t rebuilt_j = rebuild_concat_from_heads(b, hj.data, hj.len);
                flat_push(&head_nodes, rebuilt_j);
                flat_push(&tail_nodes, tj);
                flat_free(&hj);
                visited[j] = true;
            }

            if (head_nodes.len == 1) {
                flat_push(&out, fb.data[i]);
            }
            else {
                did_group = true;
                uint32_t new_head = n00b_regex_mk_and(b, head_nodes.data, head_nodes.len);
                uint32_t new_tail = n00b_regex_mk_and(b, tail_nodes.data, tail_nodes.len);
                uint32_t merged = n00b_regex_mk_concat(b, new_head, new_tail);
                flat_push(&out, merged);
            }
            flat_free(&head_nodes);
            flat_free(&tail_nodes);
        }
        for (uint32_t i = len; i < fb.len; i++) {
            flat_push(&out, fb.data[i]);
        }
        if (did_group) {
            flat_free(&fb);
            fb = out;
        }
        else {
            flat_free(&out);
        }
    }

    // --- mergeAndPrefixSuffix: extract lookbehind prefixes / lookahead suffixes ---
    // If any child has prefix lookbehind or suffix lookahead, strip them all out,
    // collect them, and reconstruct: prefixes · And(inner nodes) · suffixes
    {
        bool any_has_lookaround_boundary = false;
        for (uint32_t i = 0; i < fb.len; i++) {
            n00b_regex_node_t *cn = n00b_regex_node_get(b, fb.data[i]);
            if (cn->has_prefix_lookbehind || cn->has_suffix_lookahead) {
                any_has_lookaround_boundary = true;
                break;
            }
        }
        if (any_has_lookaround_boundary) {
            flat_buf_t prefixes, suffixes, inner;
            flat_init(&prefixes);
            flat_init(&suffixes);
            flat_init(&inner);

            for (uint32_t i = 0; i < fb.len; i++) {
                uint32_t cur = fb.data[i];
                // Strip prefix lookbehinds
                for (;;) {
                    n00b_regex_node_t *cn = n00b_regex_node_get(b, cur);
                    if (cn->kind == N00B_RE_CONCAT) {
                        n00b_regex_node_t *ch = n00b_regex_node_get(b, cn->concat.head);
                        if (ch->kind == N00B_RE_LOOKAROUND && ch->lookaround.look_back) {
                            flat_push(&prefixes, cn->concat.head);
                            cur = cn->concat.tail;
                            continue;
                        }
                    }
                    break;
                }
                // Strip suffix lookaheads (walk to find last concat tail)
                // We need to decompose via split_tail, check suffix
                flat_buf_t heads;
                flat_init(&heads);
                uint32_t tail_id = split_tail(b, cur, &heads);
                n00b_regex_node_t *tn = n00b_regex_node_get(b, tail_id);
                if (tn->kind == N00B_RE_LOOKAROUND && !tn->lookaround.look_back) {
                    flat_push(&suffixes, tail_id);
                    uint32_t rebuilt = (heads.len > 0)
                        ? rebuild_concat_from_heads(b, heads.data, heads.len)
                        : N00B_RE_ID_DOTSTAR;
                    if (rebuilt != N00B_RE_ID_DOTSTAR) {
                        flat_push(&inner, rebuilt);
                    }
                }
                else {
                    if (cur != N00B_RE_ID_DOTSTAR) {
                        flat_push(&inner, cur);
                    }
                }
                flat_free(&heads);
            }

            if (prefixes.len > 0 || suffixes.len > 0) {
                // Build: prefixes · And(inner) · suffixes
                uint32_t and_node;
                if (inner.len == 0) {
                    and_node = N00B_RE_ID_DOTSTAR;
                }
                else {
                    sort_u32(inner.data, inner.len);
                    and_node = (inner.len == 1) ? inner.data[0]
                             : finalize_and(b, inner.data, inner.len);
                }

                // Concat all prefixes
                uint32_t prefix_chain = N00B_RE_ID_EPSILON;
                for (uint32_t i = 0; i < prefixes.len; i++) {
                    prefix_chain = n00b_regex_mk_concat(b, prefix_chain, prefixes.data[i]);
                }
                // Concat all suffixes
                uint32_t suffix_chain = N00B_RE_ID_EPSILON;
                for (uint32_t i = 0; i < suffixes.len; i++) {
                    suffix_chain = n00b_regex_mk_concat(b, suffix_chain, suffixes.data[i]);
                }

                uint32_t result = n00b_regex_mk_concat(b, prefix_chain,
                                    n00b_regex_mk_concat(b, and_node, suffix_chain));
                flat_free(&prefixes);
                flat_free(&suffixes);
                flat_free(&inner);
                flat_free(&fb);
                return result;
            }
            flat_free(&prefixes);
            flat_free(&suffixes);
            flat_free(&inner);
        }
    }

    // --- Final result ---
    if (fb.len == 0) {
        flat_free(&fb);
        return N00B_RE_ID_DOTSTAR;
    }
    if (fb.len == 1) {
        uint32_t result = fb.data[0];
        flat_free(&fb);
        return result;
    }

    sort_u32(fb.data, fb.len);

    uint32_t result = finalize_and(b, fb.data, fb.len);
    flat_free(&fb);
    return result;
}

// ============================================================================
// mk_not
// ============================================================================

n00b_result_t(uint32_t)
n00b_regex_mk_not(n00b_regex_builder_t *b, uint32_t inner)
{
    // Not(Not(x)) = x
    n00b_regex_node_t *n = n00b_regex_node_get(b, inner);
    if (n->kind == N00B_RE_NOT) return n00b_result_ok(uint32_t, n->not_.inner);

    // Not(NOTHING) = DOTSTAR, Not(DOTSTAR) = NOTHING
    if (inner == N00B_RE_ID_NOTHING) return n00b_result_ok(uint32_t, N00B_RE_ID_DOTSTAR);
    if (inner == N00B_RE_ID_DOTSTAR) return n00b_result_ok(uint32_t, N00B_RE_ID_NOTHING);
    // Not(EPSILON) = ANYPLUS
    if (inner == N00B_RE_ID_EPSILON) return n00b_result_ok(uint32_t, N00B_RE_ID_ANYPLUS);

    // Reject lookarounds and anchors inside complement (unsupported by derivative engine)
    n = n00b_regex_node_get(b, inner);
    if (n->contains_lookaround) {
        return n00b_result_err(uint32_t, (n00b_err_t)N00B_RE_PARSE_NOT_LOOKAROUND);
    }
    if (n->depends_on_anchor) {
        return n00b_result_err(uint32_t, (n00b_err_t)N00B_RE_PARSE_NOT_ANCHOR);
    }

    uint64_t key = node_hash(N00B_RE_NOT, inner, 0, 0);
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return n00b_result_ok(uint32_t, cached);

    n00b_regex_node_t node = {
        .kind  = N00B_RE_NOT,
        .not_  = {.inner = inner},
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return n00b_result_ok(uint32_t, id);
}

// ============================================================================
// mk_loop — Phase 6: Full resharp loop optimizations
// ============================================================================

uint32_t
n00b_regex_mk_loop(n00b_regex_builder_t *b,
                    uint32_t body, int32_t lo, int32_t hi)
{
    // Basic simplifications
    if (lo == 0 && hi == 0) return N00B_RE_ID_EPSILON;
    if (lo == 1 && hi == 1) return body;
    if (body == N00B_RE_ID_NOTHING && lo == 0) return N00B_RE_ID_EPSILON;
    if (body == N00B_RE_ID_EPSILON) return N00B_RE_ID_EPSILON;

    // Well-known sentinel loops
    if (body == N00B_RE_ID_NOTHING && lo == 0 && hi == INT32_MAX) return N00B_RE_ID_EPSILON;
    if (body == N00B_RE_ID_ANY && lo == 0 && hi == INT32_MAX) return N00B_RE_ID_DOTSTAR;
    if (body == N00B_RE_ID_ANY && lo == 1 && hi == INT32_MAX) return N00B_RE_ID_ANYPLUS;

    n00b_regex_node_t *nb = n00b_regex_node_get(b, body);

    // Nested loop collapse: Loop(Loop(inner, 0, MAX), 0, _) → body (star absorbs)
    if (nb->kind == N00B_RE_LOOP && nb->loop.lo == 0 && nb->loop.hi == INT32_MAX && lo == 0) {
        return body;
    }

    // Optional of optional: Loop(Loop(inner, 0, 1), 0, 1) → Loop(inner, 0, 1)
    if (nb->kind == N00B_RE_LOOP && nb->loop.lo == 0 && nb->loop.hi == 1
        && lo == 0 && hi == 1) {
        return n00b_regex_mk_loop(b, nb->loop.body, 0, 1);
    }

    // Lookbehind loop: Loop(Lookbehind, 0, _) → EPS (zero-width)
    if (nb->kind == N00B_RE_LOOKAROUND && nb->lookaround.look_back && lo == 0) {
        return N00B_RE_ID_EPSILON;
    }
    // Lookbehind loop nonzero: Loop(Lookbehind, lo>0, _) → body
    if (nb->kind == N00B_RE_LOOKAROUND && nb->lookaround.look_back) {
        return body;
    }

    // Concat-head/tail star subsumption:
    // Loop(Concat(PredStar(p), tail), lo, hi) where p ⊇ tail.subsumed_by
    // → {1,MAX} identity: return body; else {lo>0, hi<MAX}: extend to Loop(body, lo, MAX)
    if (nb->kind == N00B_RE_CONCAT) {
        n00b_regex_charset_t pstar = get_pred_star(b, nb->concat.head);
        if (!n00b_regex_charset_is_empty(b->solver, pstar)) {
            n00b_regex_node_t *tail_node = n00b_regex_node_get(b, nb->concat.tail);
            if (n00b_regex_charset_contains_set(b->solver, pstar, tail_node->subsumed_by)) {
                if (lo == 1 && hi == INT32_MAX) return body;
                if (lo > 0 && hi != INT32_MAX) {
                    return n00b_regex_mk_loop(b, body, lo, INT32_MAX);
                }
            }
        }
        // Also check tail being the PredStar
        pstar = get_pred_star(b, nb->concat.tail);
        if (!n00b_regex_charset_is_empty(b->solver, pstar)) {
            n00b_regex_node_t *head_node = n00b_regex_node_get(b, nb->concat.head);
            if (n00b_regex_charset_contains_set(b->solver, pstar, head_node->subsumed_by)) {
                if (lo == 1 && hi == INT32_MAX) return body;
                if (lo > 0 && hi != INT32_MAX) {
                    return n00b_regex_mk_loop(b, body, lo, INT32_MAX);
                }
            }
        }
    }

    uint64_t key = node_hash(N00B_RE_LOOP, body, (uint64_t)lo, (uint64_t)hi);
    bool     found;
    uint32_t cached = n00b_dict_get(b->dedup, key, &found);
    if (found) return cached;

    n00b_regex_node_t node = {
        .kind = N00B_RE_LOOP,
        .loop = {.body = body, .lo = lo, .hi = hi},
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    n00b_dict_put(b->dedup, key, id);
    return id;
}

// ============================================================================
// mk_lookaround — Phase 7: Body normalization
// ============================================================================

uint32_t
n00b_regex_mk_lookaround(n00b_regex_builder_t *b,
                          uint32_t body, bool look_back,
                          int32_t relative_to,
                          int32_t *pending, uint16_t n_pending)
{
    // Trivial lookbehind cases
    if (look_back) {
        if (body == N00B_RE_ID_EPSILON) return N00B_RE_ID_EPSILON;
    }

    // Dead body
    if (body == N00B_RE_ID_NOTHING) return N00B_RE_ID_NOTHING;

    // Lookahead body normalization
    if (!look_back) {
        // Trivial: LA(EPS) → EPS (with pending)
        // Trivial: LA(DOTSTAR) → create cached node (always-nullable)

        n00b_regex_node_t *bn = n00b_regex_node_get(b, body);
        // Always-nullable body → reduce body to EPS (pass pending through)
        if (bn->is_always_nullable) {
            body = N00B_RE_ID_EPSILON;
        }
        // Concat(DOTSTAR, anchor) where anchor is BEGIN/END → EPS
        else if (bn->kind == N00B_RE_CONCAT
                 && bn->concat.head == N00B_RE_ID_DOTSTAR
                 && (bn->concat.tail == N00B_RE_ID_END
                     || bn->concat.tail == N00B_RE_ID_BEGIN)) {
            body = N00B_RE_ID_EPSILON;
        }
        // Anchor-dependent Concat: only append DOTSTAR if tail doesn't depend on anchor
        else if (bn->depends_on_anchor && bn->kind == N00B_RE_CONCAT) {
            flat_buf_t heads;
            flat_init(&heads);
            uint32_t tail_id = split_tail(b, body, &heads);
            flat_free(&heads);
            n00b_regex_node_t *tail_node = n00b_regex_node_get(b, tail_id);
            if (!tail_node->depends_on_anchor && !ends_with_dotstar(b, body)) {
                body = n00b_regex_mk_concat(b, body, N00B_RE_ID_DOTSTAR);
            }
            // else: tail depends on anchor, leave body as-is
        }
        else if (body != N00B_RE_ID_EPSILON && body != N00B_RE_ID_DOTSTAR
                 && !ends_with_dotstar(b, body)) {
            // Append DOTSTAR to lookahead body for normalization
            body = n00b_regex_mk_concat(b, body, N00B_RE_ID_DOTSTAR);
        }
    }

    // Dedup freshly-parsed lookarounds
    if (relative_to == 0 && n_pending == 0) {
        uint64_t key = node_hash(N00B_RE_LOOKAROUND, body, look_back, 0);
        bool     found;
        uint32_t cached = n00b_dict_get(b->dedup, key, &found);
        if (found) return cached;

        n00b_regex_node_t node = {
            .kind       = N00B_RE_LOOKAROUND,
            .lookaround = {
                .body        = body,
                .look_back   = look_back,
                .relative_to = 0,
                .pending_nullable_pos = nullptr,
                .n_pending   = 0,
            },
        };
        uint32_t id = register_node(b, node);
        compute_node_flags(b, &b->nodes.data[id]);
        n00b_dict_put(b->dedup, key, id);
        return id;
    }

    // Derivative-time: no dedup, always create new node.
    int32_t *pending_copy = nullptr;
    if (n_pending > 0 && pending != nullptr) {
        pending_copy = n00b_alloc_array(int32_t, n_pending);
        for (uint16_t i = 0; i < n_pending; i++) {
            pending_copy[i] = pending[i];
        }
    }

    n00b_regex_node_t node = {
        .kind       = N00B_RE_LOOKAROUND,
        .lookaround = {
            .body        = body,
            .look_back   = look_back,
            .relative_to = relative_to,
            .pending_nullable_pos = pending_copy,
            .n_pending   = n_pending,
        },
    };
    uint32_t id = register_node(b, node);
    compute_node_flags(b, &b->nodes.data[id]);
    return id;
}
