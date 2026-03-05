#include "text/regex/charset.h"
#include "core/alloc.h"
#include "core/hash.h"

// ============================================================================
// Internal helpers
// ============================================================================

/** Number of bits needed to represent a Unicode codepoint (0..0x10FFFF). */
#define BDD_MAX_BIT 20

static uint32_t
solver_add_node(n00b_regex_solver_t *s, uint32_t var, uint32_t lo, uint32_t hi)
{
    n00b_regex_bdd_node_t node = {.var = var, .lo = lo, .hi = hi};
    uint32_t              id   = (uint32_t)s->nodes.len;

    n00b_list_push(s->nodes, node);
    return id;
}

static uint32_t
make_node(n00b_regex_solver_t *s, uint32_t var, uint32_t lo, uint32_t hi)
{
    if (lo == hi) {
        return lo; // Both branches go to same place — skip this decision.
    }
    return solver_add_node(s, var, lo, hi);
}

// ============================================================================
// Solver creation
// ============================================================================

n00b_regex_solver_t
n00b_regex_solver_new(void)
{
    n00b_regex_solver_t s = {};

    s.nodes    = n00b_list_new_cap_private(n00b_regex_bdd_node_t, 256);
    s.memo_or  = n00b_alloc(n00b_dict_t(uint64_t, uint32_t));
    n00b_dict_init(s.memo_or, .hash = n00b_hash_word, .skip_obj_hash = true);
    s.memo_and = n00b_alloc(n00b_dict_t(uint64_t, uint32_t));
    n00b_dict_init(s.memo_and, .hash = n00b_hash_word, .skip_obj_hash = true);
    s.memo_not = n00b_alloc(n00b_dict_t(uint32_t, uint32_t));
    n00b_dict_init(s.memo_not, .hash = n00b_hash_word, .skip_obj_hash = true);

    // GC charset cache
    s.gc_cache = n00b_alloc(n00b_dict_t(uint32_t, uint32_t));
    n00b_dict_init(s.gc_cache, .hash = n00b_hash_word, .skip_obj_hash = true);

    // Sentinel nodes: FALSE (id=0), TRUE (id=1)
    n00b_regex_bdd_node_t false_node = {
        .var = N00B_REGEX_BDD_FALSE_VAR, .lo = 0, .hi = 0};
    n00b_regex_bdd_node_t true_node = {
        .var = N00B_REGEX_BDD_TRUE_VAR, .lo = 0, .hi = 0};

    n00b_list_push(s.nodes, false_node);
    n00b_list_push(s.nodes, true_node);

    s.false_id = 0;
    s.true_id  = 1;

    return s;
}

// ============================================================================
// Range construction
// ============================================================================

/**
 * Build a BDD for the range [lo, hi] using bits [0..bit].
 * This mirrors CharSetSolver.CreateBDDFromRangeImpl.
 */
static n00b_regex_charset_t
range_impl(n00b_regex_solver_t *s, uint32_t lo, uint32_t hi, int bit)
{
    uint32_t mask = 1u << bit;

    // Base case: single bit
    if (mask == 1) {
        if (hi == 0) {
            return make_node(s, 0, s->true_id, s->false_id);
        }
        if (lo == 1) {
            return make_node(s, 0, s->false_id, s->true_id);
        }
        return s->true_id; // Both 0 and 1 included.
    }

    // Covers full range for this bit?
    if (lo == 0 && hi == (mask << 1) - 1) {
        return s->true_id;
    }

    uint32_t lo_masked = lo & mask;
    uint32_t hi_masked = hi & mask;

    if (hi_masked == 0) {
        // hi doesn't have this bit set — one branch is empty
        uint32_t zero = range_impl(s, lo, hi, bit - 1);
        return make_node(s, (uint32_t)bit, zero, s->false_id);
    }
    else if (lo_masked == mask) {
        // lo has this bit set — zero branch is empty
        uint32_t one = range_impl(s, lo & ~mask, hi & ~mask, bit - 1);
        return make_node(s, (uint32_t)bit, s->false_id, one);
    }
    else {
        // Range straddles the bit boundary
        uint32_t zero = range_impl(s, lo, mask - 1, bit - 1);
        uint32_t one  = range_impl(s, 0, hi & ~mask, bit - 1);
        return make_node(s, (uint32_t)bit, zero, one);
    }
}

n00b_regex_charset_t
n00b_regex_charset_range(n00b_regex_solver_t *s, uint32_t lo, uint32_t hi)
{
    if (hi < lo) {
        return s->false_id;
    }
    if (lo == hi) {
        return n00b_regex_charset_single(s, lo);
    }
    return range_impl(s, lo, hi, BDD_MAX_BIT);
}

n00b_regex_charset_t
n00b_regex_charset_single(n00b_regex_solver_t *s, n00b_codepoint_t cp)
{
    // Build BDD for a single codepoint: each bit must match exactly.
    uint32_t node = s->true_id;
    for (int k = 0; k <= BDD_MAX_BIT; k++) {
        if (cp & (1u << k)) {
            node = make_node(s, (uint32_t)k, s->false_id, node);
        }
        else {
            node = make_node(s, (uint32_t)k, node, s->false_id);
        }
    }
    return node;
}

// ============================================================================
// Boolean operations
// ============================================================================

static inline bool
is_leaf(n00b_regex_solver_t *s, uint32_t id)
{
    return s->nodes.data[id].var >= N00B_REGEX_BDD_FALSE_VAR;
}

static inline uint64_t
pack2(uint32_t a, uint32_t b)
{
    return ((uint64_t)a << 32) | (uint64_t)b;
}

n00b_regex_charset_t
n00b_regex_charset_or(n00b_regex_solver_t *s,
                      n00b_regex_charset_t a,
                      n00b_regex_charset_t b)
{
    // Base cases
    if (a == s->false_id) return b;
    if (b == s->false_id) return a;
    if (a == s->true_id || b == s->true_id) return s->true_id;
    if (a == b) return a;

    // Order for cache symmetry
    if (a > b) {
        uint32_t tmp = a;
        a = b;
        b = tmp;
    }

    uint64_t key = pack2(a, b);
    bool     found;
    uint32_t cached = n00b_dict_get(s->memo_or, key, &found);
    if (found) {
        return cached;
    }

    n00b_regex_bdd_node_t *na = &s->nodes.data[a];
    n00b_regex_bdd_node_t *nb = &s->nodes.data[b];

    uint32_t result;
    if (is_leaf(s, a) || (!is_leaf(s, b) && nb->var > na->var)) {
        uint32_t one = n00b_regex_charset_or(s, a, nb->hi);
        uint32_t two = n00b_regex_charset_or(s, a, nb->lo);
        result = make_node(s, nb->var, two, one);
    }
    else if (is_leaf(s, b) || na->var > nb->var) {
        uint32_t one = n00b_regex_charset_or(s, na->hi, b);
        uint32_t two = n00b_regex_charset_or(s, na->lo, b);
        result = make_node(s, na->var, two, one);
    }
    else {
        uint32_t one = n00b_regex_charset_or(s, na->hi, nb->hi);
        uint32_t two = n00b_regex_charset_or(s, na->lo, nb->lo);
        result = make_node(s, na->var, two, one);
    }

    n00b_dict_put(s->memo_or, key, result);
    return result;
}

n00b_regex_charset_t
n00b_regex_charset_and(n00b_regex_solver_t *s,
                       n00b_regex_charset_t a,
                       n00b_regex_charset_t b)
{
    if (a == s->true_id) return b;
    if (b == s->true_id) return a;
    if (a == s->false_id || b == s->false_id) return s->false_id;
    if (a == b) return a;

    if (a > b) {
        uint32_t tmp = a;
        a = b;
        b = tmp;
    }

    uint64_t key = pack2(a, b);
    bool     found;
    uint32_t cached = n00b_dict_get(s->memo_and, key, &found);
    if (found) {
        return cached;
    }

    n00b_regex_bdd_node_t *na = &s->nodes.data[a];
    n00b_regex_bdd_node_t *nb = &s->nodes.data[b];

    uint32_t result;
    if (is_leaf(s, a) || (!is_leaf(s, b) && nb->var > na->var)) {
        uint32_t one = n00b_regex_charset_and(s, a, nb->hi);
        uint32_t two = n00b_regex_charset_and(s, a, nb->lo);
        result = make_node(s, nb->var, two, one);
    }
    else if (is_leaf(s, b) || na->var > nb->var) {
        uint32_t one = n00b_regex_charset_and(s, na->hi, b);
        uint32_t two = n00b_regex_charset_and(s, na->lo, b);
        result = make_node(s, na->var, two, one);
    }
    else {
        uint32_t one = n00b_regex_charset_and(s, na->hi, nb->hi);
        uint32_t two = n00b_regex_charset_and(s, na->lo, nb->lo);
        result = make_node(s, na->var, two, one);
    }

    n00b_dict_put(s->memo_and, key, result);
    return result;
}

n00b_regex_charset_t
n00b_regex_charset_not(n00b_regex_solver_t *s, n00b_regex_charset_t a)
{
    if (a == s->false_id) return s->true_id;
    if (a == s->true_id) return s->false_id;

    bool     found;
    uint32_t cached = n00b_dict_get(s->memo_not, a, &found);
    if (found) {
        return cached;
    }

    n00b_regex_bdd_node_t *na = &s->nodes.data[a];
    uint32_t one = n00b_regex_charset_not(s, na->hi);
    uint32_t two = n00b_regex_charset_not(s, na->lo);
    uint32_t result = make_node(s, na->var, two, one);

    n00b_dict_put(s->memo_not, a, result);
    return result;
}

// ============================================================================
// Membership test
// ============================================================================

bool
n00b_regex_charset_contains(n00b_regex_solver_t *s,
                            n00b_regex_charset_t cs,
                            n00b_codepoint_t     cp)
{
    uint32_t cur = cs;
    while (!is_leaf(s, cur)) {
        n00b_regex_bdd_node_t *n = &s->nodes.data[cur];
        if (cp & (1u << n->var)) {
            cur = n->hi;
        }
        else {
            cur = n->lo;
        }
    }
    return cur == s->true_id;
}

bool
n00b_regex_charset_intersects_minterm(n00b_regex_solver_t *s,
                                     n00b_regex_charset_t cs,
                                     n00b_regex_charset_t mt)
{
    n00b_regex_charset_t inter = n00b_regex_charset_and(s, cs, mt);
    return !n00b_regex_charset_is_empty(s, inter);
}

// ============================================================================
// Unicode general category → BDD (with caching)
// ============================================================================

n00b_regex_charset_t
n00b_regex_charset_from_gc(n00b_regex_solver_t *s, n00b_unicode_gc_t gc)
{
    // Check cache first
    uint32_t gc_key = (uint32_t)gc;
    bool     found;
    uint32_t cached = n00b_dict_get(s->gc_cache, gc_key, &found);
    if (found) {
        return cached;
    }

    // Build union of all codepoint ranges that belong to this GC.
    // We scan the Unicode range in blocks and merge contiguous runs.
    n00b_regex_charset_t result = s->false_id;

    uint32_t run_start = UINT32_MAX;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        bool in_gc = (n00b_unicode_general_category(cp) == gc);
        if (in_gc && run_start == UINT32_MAX) {
            run_start = cp;
        }
        else if (!in_gc && run_start != UINT32_MAX) {
            n00b_regex_charset_t range = n00b_regex_charset_range(s, run_start, cp - 1);
            result    = n00b_regex_charset_or(s, result, range);
            run_start = UINT32_MAX;
        }
    }
    if (run_start != UINT32_MAX) {
        n00b_regex_charset_t range = n00b_regex_charset_range(s, run_start, 0x10FFFF);
        result = n00b_regex_charset_or(s, result, range);
    }

    n00b_dict_put(s->gc_cache, gc_key, result);
    return result;
}

// ============================================================================
// ============================================================================
// BDD-to-ranges conversion for BMP LUT construction
//
// Walks the BDD recursively, accumulating the accepted codepoint ranges
// and filling them directly into the LUT. This matches resharp's
// BDDRangeConverter.ToRanges + slice.Fill approach.
// ============================================================================

/**
 * Recursively enumerate accepted ranges in [lo_bound, hi_bound] for the BDD
 * rooted at @p node, and fill them into the LUT with minterm ID @p mt_id.
 * Only fills entries within 0..65535 (BMP).
 */
static void
bdd_fill_ranges(n00b_regex_solver_t *s, uint32_t node,
                uint32_t lo_bound, uint32_t hi_bound,
                n00b_regex_minterm_id_t mt_id,
                n00b_regex_minterm_id_t *lut)
{
    // Clamp to BMP
    if (lo_bound > 0xFFFF) return;
    if (hi_bound > 0xFFFF) hi_bound = 0xFFFF;

    if (node == s->false_id) return;

    if (node == s->true_id) {
        // Fill [lo_bound, hi_bound] in the LUT
        for (uint32_t cp = lo_bound; cp <= hi_bound; cp++) {
            lut[cp] = mt_id;
        }
        return;
    }

    n00b_regex_bdd_node_t *n = &s->nodes.data[node];
    uint32_t bit = n->var;
    uint32_t mask = 1u << bit;

    // Split the range at this bit position
    // lo branch: bit=0, range [lo_bound, min(hi_bound, mask-1 combined with upper bits)]
    // hi branch: bit=1, range [max(lo_bound, mask), hi_bound]

    // Compute the boundary: all values with this bit = 0 have codepoints < mask (for this bit)
    // We need to split based on whether the bit is 0 or 1 in the codepoint.
    //
    // lo child: codepoints where bit `var` = 0 → clear bit in bounds
    // hi child: codepoints where bit `var` = 1 → set bit in bounds

    uint32_t lo_hi_start = lo_bound | mask;  // first value with this bit set
    uint32_t lo_lo_end   = hi_bound & ~mask; // last value with this bit clear

    // lo branch: bit = 0
    if (lo_bound <= lo_lo_end) {
        uint32_t eff_lo = lo_bound;
        uint32_t eff_hi = (lo_lo_end < hi_bound) ? lo_lo_end : hi_bound;
        if (!(eff_lo & mask)) { // lo_bound has bit clear — valid for lo branch
            bdd_fill_ranges(s, n->lo, eff_lo, eff_hi, mt_id, lut);
        }
    }

    // hi branch: bit = 1
    if (lo_hi_start <= hi_bound) {
        uint32_t eff_lo = (lo_hi_start > lo_bound) ? lo_hi_start : lo_bound;
        uint32_t eff_hi = hi_bound;
        if (eff_lo & mask) { // has bit set — valid for hi branch
            bdd_fill_ranges(s, n->hi, eff_lo, eff_hi, mt_id, lut);
        }
    }
}

/**
 * Fill BMP LUT entries for a single minterm's BDD.
 */
static void
bdd_fill_bmp_lut(n00b_regex_solver_t *s, n00b_regex_charset_t bdd,
                 n00b_regex_minterm_id_t mt_id,
                 n00b_regex_minterm_id_t *lut)
{
    bdd_fill_ranges(s, bdd, 0, 0xFFFF, mt_id, lut);
}

// Minterm computation — partition-tree algorithm
// ============================================================================

typedef struct ptree_t {
    n00b_regex_charset_t set;
    struct ptree_t      *left;
    struct ptree_t      *right;
} ptree_t;

static ptree_t *
ptree_new(n00b_regex_charset_t set)
{
    ptree_t *t = n00b_alloc(ptree_t);
    t->set   = set;
    t->left  = nullptr;
    t->right = nullptr;
    return t;
}

static void
ptree_refine(n00b_regex_solver_t *s, ptree_t *tree, n00b_regex_charset_t other)
{
    n00b_regex_charset_t this_and_other = n00b_regex_charset_and(s, tree->set, other);
    if (n00b_regex_charset_is_empty(s, this_and_other)) {
        return;
    }

    n00b_regex_charset_t not_other       = n00b_regex_charset_not(s, other);
    n00b_regex_charset_t this_minus_other = n00b_regex_charset_and(s, tree->set, not_other);
    if (n00b_regex_charset_is_empty(s, this_minus_other)) {
        return; // tree->set is entirely contained in other
    }

    // Need to split
    if (tree->left == nullptr) {
        tree->left  = ptree_new(this_and_other);
        tree->right = ptree_new(this_minus_other);
    }
    else {
        ptree_refine(s, tree->left, other);
        ptree_refine(s, tree->right, other);
    }
}

static void
ptree_collect_leaves(ptree_t *tree, n00b_regex_charset_t **out, uint16_t *count)
{
    if (tree->left == nullptr && tree->right == nullptr) {
        (*out)[*count] = tree->set;
        (*count)++;
        return;
    }
    if (tree->left) {
        ptree_collect_leaves(tree->left, out, count);
    }
    if (tree->right) {
        ptree_collect_leaves(tree->right, out, count);
    }
}

n00b_regex_minterm_table_t *
n00b_regex_compute_minterms(n00b_regex_solver_t  *s,
                            n00b_regex_charset_t *preds,
                            uint32_t              n)
{
    ptree_t *root = ptree_new(s->true_id);

    for (uint32_t i = 0; i < n; i++) {
        ptree_refine(s, root, preds[i]);
    }

    // Count leaves first (upper bound is 2^n but typically much smaller)
    uint32_t max_minterms = (n + 1) * 2 + 1;
    if (max_minterms < 16) max_minterms = 16;

    n00b_regex_charset_t *mts = n00b_alloc_array(n00b_regex_charset_t, max_minterms);
    uint16_t count = 0;

    ptree_collect_leaves(root, &mts, &count);

    n00b_regex_minterm_table_t *mt = n00b_alloc(n00b_regex_minterm_table_t);
    mt->minterms = mts;
    mt->solver   = s;
    mt->count    = count;

    // Build BMP lookup table (65536 entries) matching resharp's createLookupUtf16.
    // Uses BDD-to-ranges conversion for each minterm, then fills ranges in the LUT.
    // This is O(minterms * ranges_per_minterm) — typically very fast.
    mt->bmp_lut = n00b_alloc_array(n00b_regex_minterm_id_t, 65536);

    // Initialize all to minterm 0
    for (uint32_t cp = 0; cp < 65536; cp++) {
        mt->bmp_lut[cp] = 0;
    }

    // For each non-zero minterm, convert BDD to ranges and fill
    for (uint16_t i = 1; i < count; i++) {
        bdd_fill_bmp_lut(s, mts[i], i, mt->bmp_lut);
    }

    // Build ASCII byte LUT from BMP LUT (bytes 0x00-0x7F map directly to codepoints)
    for (uint32_t b = 0; b < 128; b++) {
        mt->byte_lut[b] = (uint8_t)mt->bmp_lut[b];
    }
    // Bytes 0x80-0xFF are UTF-8 continuation/lead bytes — sentinel for slow path
    for (uint32_t b = 128; b < 256; b++) {
        mt->byte_lut[b] = N00B_RE_BYTE_LUT_MULTI;
    }

    return mt;
}

n00b_regex_minterm_id_t
n00b_regex_minterm_classify(n00b_regex_minterm_table_t *mt,
                            n00b_codepoint_t            cp)
{
    // Fast path: BMP codepoints (0-65535) use precomputed lookup table — O(1)
    if (cp < 65536) {
        return mt->bmp_lut[cp];
    }

    // Slow path: supplementary planes (above U+FFFF) walk BDDs
    for (uint16_t i = 0; i < mt->count; i++) {
        if (n00b_regex_charset_contains(mt->solver, mt->minterms[i], cp)) {
            return i;
        }
    }
    return 0;
}
