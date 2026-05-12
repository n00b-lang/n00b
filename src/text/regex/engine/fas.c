// fas.c — Forward Active-Set DFA: cached transition tables on top of LDFA.
//
// Per § 0a-Z + Phase 7 follow-up: every growable Vec uses
// `n00b_list_new_private(T)` (unlocked).  The state-vector intern dict and
// the action intern dict use custom `n00b_hash_fn`s (per `dict.h:51,177`)
// that walk the n00b_list contents and hash the element bytes.
//
//   resharp-c alloc helpers      -> n00b_alloc / n00b_alloc_array.
//   HashMap<Vec<u16>, u32>       -> n00b_dict_t(fas_list_u16 *, uint32_t)
//                                    with custom `fn` over the list's
//                                    element bytes.
//   HashMap<FwdAction, u32>      -> n00b_dict_t(FwdAction *, uint32_t)
//                                    with custom `fn` over the composite
//                                    content tuple, walking the inner lists.
//   require / bounds-check / ffi -> n00b_require (always-on, fatal).
//   `Error *` returns            -> n00b_result_t(...) with int err
//                                    (D14 int-err idiom).
//
// Cross-TU references to `engine_LDFA_*` and `matches_push` are declared
// extern in `internal/regex/fas.h`; sibling Phase 7 ports land them.

#include "n00b.h"
#include "core/alloc.h"
#include "core/hash.h"
#include "adt/dict.h"
#include "adt/list.h"
#include "adt/result.h"
#include "util/assert.h"
#include "util/panic.h"

#include "internal/regex/fas.h"
#include "internal/regex/algebra.h"
#include "internal/regex/nulls.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <string.h> // memcpy / memcmp (D13)

// ===========================================================================
// Small helpers
// ===========================================================================

static inline size_t safe_mul_sz(size_t a, size_t b)
{
    size_t r;
    if (ckd_mul(&r, a, b)) {
        n00b_panic("fas.c: capacity overflow");
    }
    return r;
}

static inline size_t safe_add_sz(size_t a, size_t b)
{
    size_t r;
    if (ckd_add(&r, a, b)) {
        n00b_panic("fas.c: capacity overflow");
    }
    return r;
}

// ===========================================================================
// List resize-fill helpers
//
// `n00b_list_t(T)` exposes push/get/set but no public resize-fill; the FAS
// scan kernel needs both an "extend with constant fill" pattern (for the
// trans / max / linker buffers) and direct tail writes after a resize.
// We emit per-element-type wrappers that go through `_n00b_list_ensure_cap`
// (the same path `n00b_list_push` uses internally).
// ===========================================================================

#define FAS_LIST_RESIZE_IMPL(NAME, ETY)                                       \
    static void NAME##_resize_fill(n00b_list_t(ETY) *v, size_t new_len,       \
                                    ETY fill)                                 \
    {                                                                         \
        _n00b_list_write_lock(v);                                             \
        _n00b_list_ensure_cap(v, new_len);                                    \
        for (size_t _i = v->len; _i < new_len; ++_i) v->data[_i] = fill;      \
        v->len = new_len;                                                     \
        _n00b_list_unlock(v);                                                 \
    }

FAS_LIST_RESIZE_IMPL(fas_list_u32_,    uint32_t)
FAS_LIST_RESIZE_IMPL(fas_list_usize_,  size_t)

// `fas_list_u16_clone` — deep-copies a `fas_list_u16`'s contents into a new
// private list.  Used by the state map to take ownership of inserted keys.
static fas_list_u16 fas_list_u16_clone(const fas_list_u16 *src)
{
    fas_list_u16 out = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    if (src->len > 0) {
        _n00b_list_write_lock(&out);
        _n00b_list_ensure_cap(&out, src->len);
        memcpy(out.data, src->data, safe_mul_sz(src->len, sizeof(uint16_t)));
        out.len = src->len;
        _n00b_list_unlock(&out);
    }
    return out;
}

static fas_list_u32 fas_list_u32_clone(const fas_list_u32 *src)
{
    fas_list_u32 out = n00b_list_new_private(uint32_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    if (src->len > 0) {
        _n00b_list_write_lock(&out);
        _n00b_list_ensure_cap(&out, src->len);
        memcpy(out.data, src->data, safe_mul_sz(src->len, sizeof(uint32_t)));
        out.len = src->len;
        _n00b_list_unlock(&out);
    }
    return out;
}

// ===========================================================================
// `fas_slot_list` helpers
// ===========================================================================

static void fas_slot_list_clear_each(fas_slot_list *v)
{
    for (size_t i = 0; i < v->len; ++i) SlotEntries_clear(&v->data[i]);
}

static void fas_slot_list_resize_default(fas_slot_list *v, size_t n)
{
    _n00b_list_write_lock(v);
    _n00b_list_ensure_cap(v, n);
    if (n < v->len) {
        for (size_t i = n; i < v->len; ++i) SlotEntries_clear(&v->data[i]);
    }
    else {
        for (size_t i = v->len; i < n; ++i) v->data[i] = SlotEntries_default();
    }
    v->len = n;
    _n00b_list_unlock(v);
}

// ===========================================================================
// FwdAction helpers
// ===========================================================================

static FwdAction FwdAction_clone(const FwdAction *src)
{
    return (FwdAction){
        .next_asid   = src->next_asid,
        .old_acts    = fas_list_u16_clone(&src->old_acts),
        .new_end_rel = fas_list_u32_clone(&src->new_end_rel),
        .spawn       = src->spawn,
    };
}

// ===========================================================================
// fas_state_map: n00b_dict_t(fas_list_u16 *, uint32_t)
//
// Custom `fn` derefs the stored slot to a `fas_list_u16 *` and hashes the
// underlying content (data[0..len] bytes plus the length, so {[]}, {[0]},
// {[0,0]} hash distinctly).  The map owns its key copies (deep-cloned via
// `fas_list_u16_clone`) and frees them on destroy.
// ===========================================================================

static n00b_hash_value_t fas_state_key_hash(void *opaque)
{
    const fas_list_u16 *k = (const fas_list_u16 *)opaque;
    if (!k || k->len == 0) {
        // Hash the length explicitly so the empty-key case still
        // discriminates against single-element keys with value 0.
        size_t z = 0;
        return n00b_hash_raw(&z, sizeof z);
    }
    // Concatenate `len` and the data bytes into a small heap buffer for a
    // single n00b_hash_raw call.  Length prefix prevents {[0]} vs {[0,0]}
    // collisions.  Walk via n00b_list_get on the read-locked list (lock is
    // null for private lists, so this is just a bounds-check in practice).
    size_t   nelem  = n00b_list_len(*k);
    size_t   nbytes = safe_add_sz(sizeof(size_t),
                                  safe_mul_sz(nelem, sizeof(uint16_t)));
    uint8_t *buf    = n00b_alloc_array(uint8_t, nbytes);
    memcpy(buf, &nelem, sizeof(size_t));
    for (size_t i = 0; i < nelem; ++i) {
        uint16_t v = n00b_list_get(*((fas_list_u16 *)k), i);
        memcpy(buf + sizeof(size_t) + i * sizeof(uint16_t), &v, sizeof(uint16_t));
    }
    n00b_hash_value_t h = n00b_hash_raw(buf, nbytes);
    n00b_free(buf);
    return h;
}

struct fas_state_map {
    n00b_dict_t(fas_list_u16 *, uint32_t) *m;
    // Track owned key clones so we can free their lists on destroy.
    n00b_list_t(fas_list_u16 *) owned_keys;
};

static fas_state_map *fas_state_map_new(void)
{
    fas_state_map *m = n00b_alloc(fas_state_map);
    m->m             = n00b_alloc(n00b_dict_t(fas_list_u16 *, uint32_t));
    n00b_dict_init(m->m,
                   .hash          = fas_state_key_hash,
                   .skip_obj_hash = false);
    m->owned_keys = n00b_list_new_private(fas_list_u16 *);
    return m;
}

static void fas_state_map_free(fas_state_map *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->owned_keys.len; ++i) {
        fas_list_u16 *k = m->owned_keys.data[i];
        if (k) {
            n00b_list_free(*k);
            n00b_free(k);
        }
    }
    n00b_list_free(m->owned_keys);
    n00b_free(m);
}

static void fas_state_map_insert(fas_state_map *m,
                                 const fas_list_u16 *key, uint32_t value)
{
    fas_list_u16 *kc = n00b_alloc(fas_list_u16);
    *kc              = fas_list_u16_clone(key);
    n00b_dict_put(m->m, kc, value);
    n00b_list_push(m->owned_keys, kc);
}

static bool fas_state_map_get(const fas_state_map *m,
                              const fas_list_u16 *key, uint32_t *out)
{
    bool          found;
    fas_list_u16 *k = (fas_list_u16 *)key;
    uint32_t      v = n00b_dict_get(m->m, k, &found);
    if (found && out) *out = v;
    return found;
}

static bool fas_state_map_contains(const fas_state_map *m,
                                   const fas_list_u16 *key)
{
    bool          found;
    fas_list_u16 *k = (fas_list_u16 *)key;
    (void)n00b_dict_get(m->m, k, &found);
    return found;
}

// ===========================================================================
// fas_action_map: n00b_dict_t(FwdAction *, uint32_t)
//
// Composite content hash on (next_asid, old_acts elements, new_end_rel
// elements, spawn).  Owns its key copies (deep-cloned old_acts and
// new_end_rel) and frees them on destroy.
// ===========================================================================

static n00b_hash_value_t fas_action_key_hash(void *opaque)
{
    const FwdAction *k = (const FwdAction *)opaque;
    if (!k) {
        size_t z = 0;
        return n00b_hash_raw(&z, sizeof z);
    }
    size_t oa_n     = n00b_list_len(*((fas_list_u16 *)&k->old_acts));
    size_t nr_n     = n00b_list_len(*((fas_list_u32 *)&k->new_end_rel));
    size_t oa_bytes = safe_mul_sz(oa_n, sizeof(uint16_t));
    size_t nr_bytes = safe_mul_sz(nr_n, sizeof(uint32_t));
    size_t nbytes   = safe_add_sz(sizeof(uint32_t),  // next_asid
                       safe_add_sz(sizeof(size_t),   // len(old_acts)
                       safe_add_sz(oa_bytes,
                       safe_add_sz(sizeof(size_t),   // len(new_end_rel)
                       safe_add_sz(nr_bytes,
                                   sizeof(uint16_t)))))); // spawn
    uint8_t *buf = n00b_alloc_array(uint8_t, nbytes);
    size_t   off = 0;
    memcpy(buf + off, &k->next_asid, sizeof(uint32_t));
    off += sizeof(uint32_t);
    memcpy(buf + off, &oa_n, sizeof(size_t));
    off += sizeof(size_t);
    for (size_t i = 0; i < oa_n; ++i) {
        uint16_t v = n00b_list_get(*((fas_list_u16 *)&k->old_acts), i);
        memcpy(buf + off, &v, sizeof(uint16_t));
        off += sizeof(uint16_t);
    }
    memcpy(buf + off, &nr_n, sizeof(size_t));
    off += sizeof(size_t);
    for (size_t i = 0; i < nr_n; ++i) {
        uint32_t v = n00b_list_get(*((fas_list_u32 *)&k->new_end_rel), i);
        memcpy(buf + off, &v, sizeof(uint32_t));
        off += sizeof(uint32_t);
    }
    memcpy(buf + off, &k->spawn, sizeof(uint16_t));
    off += sizeof(uint16_t);
    n00b_hash_value_t h = n00b_hash_raw(buf, nbytes);
    n00b_free(buf);
    return h;
}

struct fas_action_map {
    n00b_dict_t(FwdAction *, uint32_t) *m;
    n00b_list_t(FwdAction *) owned_keys;
};

static fas_action_map *fas_action_map_new(void)
{
    fas_action_map *m = n00b_alloc(fas_action_map);
    m->m              = n00b_alloc(n00b_dict_t(FwdAction *, uint32_t));
    n00b_dict_init(m->m,
                   .hash          = fas_action_key_hash,
                   .skip_obj_hash = false);
    m->owned_keys = n00b_list_new_private(FwdAction *);
    return m;
}

static void fas_action_map_free(fas_action_map *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->owned_keys.len; ++i) {
        FwdAction *k = m->owned_keys.data[i];
        if (k) {
            n00b_list_free(k->old_acts);
            n00b_list_free(k->new_end_rel);
            n00b_free(k);
        }
    }
    n00b_list_free(m->owned_keys);
    n00b_free(m);
}

static void fas_action_map_insert(fas_action_map *m,
                                  const FwdAction *key, uint32_t value)
{
    FwdAction *kc = n00b_alloc(FwdAction);
    *kc           = FwdAction_clone(key);
    n00b_dict_put(m->m, kc, value);
    n00b_list_push(m->owned_keys, kc);
}

static bool fas_action_map_get(const fas_action_map *m,
                               const FwdAction *key, uint32_t *out)
{
    bool       found;
    FwdAction *k = (FwdAction *)key;
    uint32_t   v = n00b_dict_get(m->m, k, &found);
    if (found && out) *out = v;
    return found;
}

// ===========================================================================
// SpawnKind
// ===========================================================================

typedef enum SpawnKindTag {
    SPAWN_KIND_DEAD,
    SPAWN_KIND_LOW_PRIORITY,
    SPAWN_KIND_NEW_SLOT,
} SpawnKindTag;

typedef struct SpawnKind {
    SpawnKindTag tag;
    size_t       idx; // valid for LOW_PRIORITY and NEW_SLOT
} SpawnKind;

// ===========================================================================
// SlotEntries
// ===========================================================================

SlotEntries SlotEntries_default(void)
{
    return (SlotEntries){.head = SLOT_NIL, .tail = SLOT_NIL, .max_e = 0};
}

[[gnu::always_inline]] inline void SlotEntries_clear(SlotEntries *self)
{
    self->head  = SLOT_NIL;
    self->tail  = SLOT_NIL;
    self->max_e = 0;
}

[[gnu::always_inline]] inline bool SlotEntries_is_empty(const SlotEntries *self)
{
    return self->head == SLOT_NIL;
}

[[gnu::always_inline]] static inline
void SlotEntries_push_spawn(SlotEntries *self, uint32_t *linker,
                            uint32_t idx, size_t e)
{
    linker[(size_t)idx] = SLOT_NIL;
    if (SlotEntries_is_empty(self)) {
        self->head  = idx;
        self->tail  = idx;
        self->max_e = e;
    }
    else {
        linker[(size_t)self->tail] = idx;
        self->tail                 = idx;
        if (e > self->max_e) self->max_e = e;
    }
}

[[gnu::always_inline]] inline void SlotEntries_extend_e(SlotEntries *self,
                                                        size_t ce)
{
    if (!SlotEntries_is_empty(self) && ce > self->max_e) {
        self->max_e = ce;
    }
}

// merge_from: ce is Option<usize>; we represent that as a (have_ce, ce) pair.
static void SlotEntries_merge_from(SlotEntries *self, SlotEntries *other,
                                   bool have_ce, size_t ce, uint32_t *linker)
{
    if (have_ce) {
        SlotEntries_extend_e(other, ce);
    }
    if (SlotEntries_is_empty(self)) {
        // std::mem::swap(self, other);
        SlotEntries tmp = *self;
        *self           = *other;
        *other          = tmp;
        return;
    }
    if (SlotEntries_is_empty(other)) return;
    linker[(size_t)self->tail] = other->head;
    self->tail                 = other->tail;
    if (other->max_e > self->max_e) self->max_e = other->max_e;
    SlotEntries_clear(other);
}

void SlotEntries_drain_to_max(SlotEntries *self, const uint32_t *linker,
                              size_t *max,
                              [[maybe_unused]] size_t max_len)
{
    size_t   e   = self->max_e;
    uint32_t cur = self->head;
    while (cur != SLOT_NIL) {
        size_t i = (size_t)cur;
        if (e >= i && e > max[i]) max[i] = e;
        cur = linker[i];
    }
    SlotEntries_clear(self);
}

// ===========================================================================
// FwdDFA
// ===========================================================================

FwdDFA *fas_FwdDFA_new(const LDFA *ldfa, bool keep_spawn_on_merge)
{
    n00b_require(ldfa != nullptr, "fas_FwdDFA_new: ldfa must be non-null");
    FwdDFA *fas        = n00b_alloc(FwdDFA);
    fas->states        = n00b_list_new_private(fas_list_u16);
    fas->trans         = n00b_list_new_private(uint32_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    fas->actions       = n00b_list_new_private(FwdAction);
    fas->max           = n00b_list_new_private(size_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    fas->linker        = n00b_list_new_private(uint32_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    fas->regs          = n00b_list_new_private(SlotEntries);
    fas->new_regs      = n00b_list_new_private(SlotEntries);
    fas->stride        = (size_t)1u << engine_LDFA_mt_log(ldfa);
    fas->initial_asid  = 0;
    // always_nullable: ldfa.effects_id[engine_DFA_INITIAL] as u32 == EID_ALWAYS0
    fas->always_nullable =
        ((uint32_t)engine_LDFA_effects_id_at(ldfa, (size_t)engine_DFA_INITIAL)
         == EID_ALWAYS0);
    fas->keep_spawn_on_merge = keep_spawn_on_merge;
    fas->state_map           = fas_state_map_new();
    fas->action_map          = fas_action_map_new();

    // state 0 = empty set
    fas_list_u16 empty = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    {
        n00b_list_push(fas->states, empty);
        size_t new_len = safe_mul_sz(safe_add_sz((size_t)0, 1), fas->stride);
        if (fas->trans.len < new_len) {
            fas_list_u32__resize_fill(&fas->trans, new_len, FAS_ACTION_MISSING);
        }
        fas_state_map_insert(fas->state_map, &fas->states.data[0], 0);
    }
    // initial_asid = register(vec![engine_DFA_INITIAL])
    {
        fas_list_u16 init = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
        n00b_list_push(init, (uint16_t)engine_DFA_INITIAL);
        uint32_t existing = 0;
        if (fas_state_map_get(fas->state_map, &init, &existing)) {
            fas->initial_asid = existing;
            n00b_list_free(init);
        }
        else {
            uint32_t id = (uint32_t)fas->states.len;
            n00b_list_push(fas->states, init);
            size_t new_len = safe_mul_sz(safe_add_sz((size_t)id, 1), fas->stride);
            if (fas->trans.len < new_len) {
                fas_list_u32__resize_fill(&fas->trans, new_len, FAS_ACTION_MISSING);
            }
            fas_state_map_insert(fas->state_map, &fas->states.data[id], id);
            fas->initial_asid = id;
        }
    }
    return fas;
}

void fas_FwdDFA_free(FwdDFA *self)
{
    if (!self) return;
    for (size_t i = 0; i < self->states.len; ++i) {
        n00b_list_free(self->states.data[i]);
    }
    n00b_list_free(self->states);
    for (size_t i = 0; i < self->actions.len; ++i) {
        n00b_list_free(self->actions.data[i].old_acts);
        n00b_list_free(self->actions.data[i].new_end_rel);
    }
    n00b_list_free(self->actions);
    n00b_list_free(self->trans);
    n00b_list_free(self->max);
    n00b_list_free(self->linker);
    n00b_list_free(self->regs);
    n00b_list_free(self->new_regs);
    // The maps own their key copies; their `_free` walks `owned_keys` and
    // frees each cloned key's inner list buffers.
    if (self->state_map)  fas_state_map_free(self->state_map);
    if (self->action_map) fas_action_map_free(self->action_map);
    n00b_free(self);
}

static uint32_t FwdDFA_register(FwdDFA *self, fas_list_u16 set)
{
    uint32_t existing = 0;
    if (fas_state_map_get(self->state_map, &set, &existing)) {
        n00b_list_free(set);
        return existing;
    }
    // states[] takes ownership of `set`; the map deep-clones the key
    // internally (no external pre-clone needed).
    uint32_t id = (uint32_t)self->states.len;
    n00b_list_push(self->states, set);
    fas_state_map_insert(self->state_map, &self->states.data[id], id);
    size_t new_len = safe_mul_sz(safe_add_sz((size_t)id, 1), self->stride);
    if (self->trans.len < new_len) {
        fas_list_u32__resize_fill(&self->trans, new_len, FAS_ACTION_MISSING);
    }
    return id;
}

static uint32_t FwdDFA_intern_action(FwdDFA *self, FwdAction action)
{
    uint32_t existing = 0;
    if (fas_action_map_get(self->action_map, &action, &existing)) {
        n00b_list_free(action.old_acts);
        n00b_list_free(action.new_end_rel);
        return existing;
    }
    uint32_t id = (uint32_t)self->actions.len;
    n00b_list_push(self->actions, action);
    fas_action_map_insert(self->action_map, &self->actions.data[id], id);
    return id;
}

// Helper: position of `target` in canonical[], or SIZE_MAX.
static size_t fas_position_u16(const fas_list_u16 *v, uint16_t target)
{
    for (size_t i = 0; i < v->len; ++i) {
        if (v->data[i] == target) return i;
    }
    return SIZE_MAX;
}

// FwdDFA::compute_action — returns ok(action_id) or err(engine err).
static n00b_result_t(uint32_t)
    FwdDFA_compute_action(FwdDFA *self, RegexBuilder *b, LDFA *ldfa,
                          uint32_t asid, uint32_t mt)
{
    n00b_require(asid < self->states.len, "FwdDFA.states OOB");
    fas_list_u16 source       = fas_list_u16_clone(&self->states.data[asid]);
    fas_list_u16 next_targets = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    fas_list_u16 old_acts     = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);

    for (size_t i = 0; i < source.len; ++i) {
        uint16_t s = source.data[i];
        n00b_result_t(uint32_t) tr =
            engine_LDFA_lazy_transition(ldfa, b, (uint32_t)s, mt);
        if (n00b_result_is_err(tr)) {
            n00b_list_free(source);
            n00b_list_free(next_targets);
            n00b_list_free(old_acts);
            return n00b_result_err(uint32_t, n00b_result_get_err(tr));
        }
        uint16_t target = (uint16_t)n00b_result_get(tr);
        if ((uint32_t)target <= engine_DFA_DEAD) {
            n00b_list_push(old_acts, (uint16_t)FAS_DIED);
        }
        else {
            size_t pos = fas_position_u16(&next_targets, target);
            if (pos != SIZE_MAX) {
                n00b_list_push(old_acts, (uint16_t)((uint16_t)pos | FAS_LOW_BIT));
            }
            else {
                n00b_list_push(old_acts, (uint16_t)next_targets.len);
                n00b_list_push(next_targets, target);
            }
        }
    }

    uint16_t spawn_target = 0;
    {
        n00b_result_t(uint32_t) tr =
            engine_LDFA_lazy_transition(ldfa, b, engine_DFA_INITIAL, mt);
        if (n00b_result_is_err(tr)) {
            n00b_list_free(source);
            n00b_list_free(next_targets);
            n00b_list_free(old_acts);
            return n00b_result_err(uint32_t, n00b_result_get_err(tr));
        }
        spawn_target = (uint16_t)n00b_result_get(tr);
    }
    SpawnKind spawn_kind;
    if ((uint32_t)spawn_target <= engine_DFA_DEAD) {
        spawn_kind = (SpawnKind){.tag = SPAWN_KIND_DEAD, .idx = 0};
    }
    else {
        size_t pos = fas_position_u16(&next_targets, spawn_target);
        if (pos != SIZE_MAX) {
            spawn_kind = (SpawnKind){.tag = SPAWN_KIND_LOW_PRIORITY, .idx = pos};
        }
        else {
            size_t p = next_targets.len;
            n00b_list_push(next_targets, spawn_target);
            spawn_kind = (SpawnKind){.tag = SPAWN_KIND_NEW_SLOT, .idx = p};
        }
    }

    fas_list_u16 canonical = next_targets; // move

    if (self->states.len > FAS_STATE_CAP
        && !fas_state_map_contains(self->state_map, &canonical)) {
        n00b_list_free(source);
        n00b_list_free(canonical);
        n00b_list_free(old_acts);
        return n00b_result_err(uint32_t,
                               (int)N00B_REGEX_ENGINE_ERR_CAPACITY_EXCEEDED);
    }

    fas_list_u32 new_end_rel = n00b_list_new_private(uint32_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
    for (size_t i = 0; i < canonical.len; ++i) {
        uint16_t s   = canonical.data[i];
        uint32_t eid = (uint32_t)engine_LDFA_effects_id_at(ldfa, (size_t)s);
        uint32_t rel;
        if (eid == EID_NONE || eid == EID_BEGIN0 || eid == EID_END0) {
            rel = FAS_NOT_NULLABLE;
        }
        else if (eid == EID_CENTER0 || eid == EID_ALWAYS0) {
            rel = 0;
        }
        else {
            rel        = FAS_NOT_NULLABLE;
            size_t n   = engine_LDFA_effects_len(ldfa, eid);
            // .iter().rev().find(|n| n.mask.has(Nullability::CENTER))
            for (size_t j = n; j-- > 0;) {
                NullState ns;
                engine_LDFA_effects_get(ldfa, eid, j, &ns);
                if (nullability_mask_has_center(ns.mask)) {
                    rel = ns.rel;
                    break;
                }
            }
        }
        n00b_list_push(new_end_rel, rel);
    }

    uint16_t spawn;
    switch (spawn_kind.tag) {
    case SPAWN_KIND_DEAD:
        spawn = FAS_SPAWN_DEAD;
        break;
    case SPAWN_KIND_LOW_PRIORITY:
        spawn = (uint16_t)spawn_kind.idx | FAS_LOW_BIT;
        break;
    case SPAWN_KIND_NEW_SLOT:
    default:
        spawn = (uint16_t)spawn_kind.idx;
        break;
    }

    // NOTE: #[cfg(feature = "debug")] block elided.

    uint32_t  next_asid = FwdDFA_register(self, canonical);
    FwdAction action    = {
           .next_asid   = next_asid,
           .old_acts    = old_acts,
           .new_end_rel = new_end_rel,
           .spawn       = spawn,
    };
    uint32_t out_id = FwdDFA_intern_action(self, action);

    n00b_list_free(source);
    return n00b_result_ok(uint32_t, out_id);
}

// ===========================================================================
// fas_apply
// ===========================================================================

[[gnu::always_inline]] static inline
void fas_apply(const FwdAction *act,
               SlotEntries     *regs,
               size_t           regs_len,
               fas_slot_list   *new_regs,
               uint32_t        *linker,
               size_t          *max,
               size_t           pos,
               bool             init_contributes_pos,
               bool             keep_spawn_on_merge,
               bool             spawn_allowed)
{
    // Invariant from the FAS state machine: act->old_acts.len records the
    // slot count of the source `asid`, which is also the live extent of the
    // caller's `regs`.  A corrupted/stale cached action would silently OOB-
    // read past `regs`; assert defensively so the failure is loud.
    n00b_require(act->old_acts.len <= regs_len,
                 "fas_apply: old_acts.len exceeds regs_len");
    fas_slot_list_resize_default(new_regs, act->new_end_rel.len);

    size_t next_pos = safe_add_sz(pos, 1);

    for (size_t slot = 0; slot < act->old_acts.len; ++slot) {
        uint16_t code = act->old_acts.data[slot];
        if (code == FAS_DIED) {
            SlotEntries_drain_to_max(&regs[slot], linker, max, /*ignored*/ 0);
            continue;
        }
        size_t idx = (size_t)(code & 0x7FFF);
        n00b_require(idx < act->new_end_rel.len, "fas_apply.new_end_rel OOB");
        n00b_require(idx < new_regs->len,        "fas_apply.new_regs OOB");
        uint32_t rel     = act->new_end_rel.data[idx];
        bool     have_ce = false;
        size_t   ce      = 0;
        if (rel != FAS_NOT_NULLABLE) {
            // checked_sub(rel)
            if (next_pos >= (size_t)rel) {
                ce      = next_pos - (size_t)rel;
                have_ce = true;
            }
        }
        SlotEntries_merge_from(&new_regs->data[idx], &regs[slot],
                               have_ce, ce, linker);
    }

    if (spawn_allowed) {
        uint16_t s_code = act->spawn;
        if (s_code == FAS_SPAWN_NONE) {
            // nothing
        }
        else if (s_code == FAS_SPAWN_DEAD) {
            if (init_contributes_pos && max[pos] < pos) {
                max[pos] = pos;
            }
        }
        else if ((s_code & FAS_LOW_BIT) != 0) {
            if (keep_spawn_on_merge) {
                size_t idx = (size_t)(s_code & 0x7FFF);
                n00b_require(idx < act->new_end_rel.len, "fas_apply.new_end_rel OOB");
                n00b_require(idx < new_regs->len,        "fas_apply.new_regs OOB");
                uint32_t rel     = act->new_end_rel.data[idx];
                size_t   init_me = init_contributes_pos ? pos : 0;
                size_t   candidate_end;
                if (rel != FAS_NOT_NULLABLE) {
                    candidate_end = (next_pos >= (size_t)rel)
                                      ? (next_pos - (size_t)rel)
                                      : 0;
                }
                else {
                    candidate_end = 0;
                }
                size_t me = candidate_end > init_me ? candidate_end : init_me;
                n00b_require(pos <= UINT32_MAX,
                             "fas_apply: pos exceeds UINT32_MAX");
                SlotEntries_push_spawn(&new_regs->data[idx], linker,
                                       (uint32_t)pos, me);
            }
        }
        else {
            size_t idx = (size_t)s_code;
            n00b_require(idx < act->new_end_rel.len, "fas_apply.new_end_rel OOB");
            n00b_require(idx < new_regs->len,        "fas_apply.new_regs OOB");
            uint32_t rel     = act->new_end_rel.data[idx];
            size_t   init_me = init_contributes_pos ? pos : 0;
            size_t   candidate_end;
            if (rel != FAS_NOT_NULLABLE) {
                candidate_end = (next_pos >= (size_t)rel)
                                  ? (next_pos - (size_t)rel)
                                  : 0;
            }
            else {
                candidate_end = 0;
            }
            size_t me = candidate_end > init_me ? candidate_end : init_me;
            n00b_require(pos <= UINT32_MAX,
                         "fas_apply: pos exceeds UINT32_MAX");
            SlotEntries_push_spawn(&new_regs->data[idx], linker,
                                   (uint32_t)pos, me);
        }
    }
}

// ===========================================================================
// LDFA::scan_fwd_active_set
//
// Implemented as a single static helper parameterised by ALWAYS_NULLABLE,
// then exposed via two extern wrappers (matching the const-generic Rust
// monomorphisations).
// ===========================================================================

static n00b_result_t(int)
    scan_fwd_active_set_impl(LDFA          *self,
                             RegexBuilder  *b,
                             FwdDFA        *fas,
                             const uint8_t *data,
                             size_t         data_end,
                             const size_t  *nulls,
                             size_t         nulls_len,
                             void          *matches,
                             bool           ALWAYS_NULLABLE)
{
    if (data_end == 0) return n00b_result_ok(int, 0);
    // Reject SIZE_MAX before computing data_end + 1 below: the wrap to 0
    // produces zero-length buffers and the per-byte writes into linker.data /
    // max.data become heap OOB writes.
    if (data_end == SIZE_MAX) {
        return n00b_result_err(int,
                               (int)N00B_REGEX_ENGINE_ERR_CAPACITY_EXCEEDED);
    }

    size_t ni = nulls_len;

    // Take fas's owned vectors out (mem::take parity).
    fas_list_usize  max      = fas->max;       fas->max      = (fas_list_usize){};
    fas_list_u32    linker   = fas->linker;    fas->linker   = (fas_list_u32){};
    fas_slot_list   regs     = fas->regs;      fas->regs     = (fas_slot_list){};
    fas_slot_list   new_regs = fas->new_regs;  fas->new_regs = (fas_slot_list){};

    max.len = 0;
    fas_list_usize__resize_fill(&max, safe_add_sz(data_end, 1), 0);
    // linker resize-clear:
    linker.len = 0;
    fas_list_u32__resize_fill(&linker, safe_add_sz(data_end, 1), SLOT_NIL);
    regs.len     = 0;
    new_regs.len = 0;

    uint32_t asid = 0;
    size_t   pos;

    // one-off begin step
    {
        bool spawn_allowed_0 =
            ALWAYS_NULLABLE || (ni > 0 && nulls[ni - 1] == 0);
        if (spawn_allowed_0) {
            uint32_t mt0 = (uint32_t)engine_LDFA_mt_lookup_at(self, (size_t)data[0]);
            uint16_t bs  = engine_LDFA_begin_table_at(self, (size_t)mt0);
            if ((uint32_t)bs > engine_DFA_DEAD) {
                uint32_t bs32 = (uint32_t)bs;
                uint32_t eid  = (uint32_t)engine_LDFA_effects_id_at(self, (size_t)bs32);
                uint32_t rel;
                if (eid == EID_NONE) {
                    rel = FAS_NOT_NULLABLE;
                }
                else if (eid == EID_CENTER0 || eid == EID_ALWAYS0) {
                    rel = 0;
                }
                else {
                    rel      = FAS_NOT_NULLABLE;
                    size_t n = engine_LDFA_effects_len(self, eid);
                    for (size_t j = n; j-- > 0;) {
                        NullState ns;
                        engine_LDFA_effects_get(self, eid, j, &ns);
                        if (nullability_mask_has_center(ns.mask)) {
                            rel = ns.rel;
                            break;
                        }
                    }
                }
                size_t candidate_end;
                if (rel != FAS_NOT_NULLABLE) {
                    // 1usize.saturating_sub(rel)
                    candidate_end = (1u >= (size_t)rel) ? (1u - (size_t)rel) : 0u;
                }
                else {
                    candidate_end = 0;
                }
                size_t me = candidate_end;
                regs.len = 0;
                SlotEntries s0 = SlotEntries_default();
                SlotEntries_push_spawn(&s0, linker.data, 0u, me);
                n00b_list_push(regs, s0);

                fas_list_u16 init = n00b_list_new_private(uint16_t, .scan_kind = N00B_GC_SCAN_KIND_NONE);
                n00b_list_push(init, (uint16_t)bs32);
                asid = FwdDFA_register(fas, init);
            }
        }
        pos = 1;
    }

    const size_t *nulls_ptr = nulls;

    // phase 1: there's still matches that haven't spawned
    while (pos < data_end) {
        bool spawn_allowed;
        bool break_phase1 = false;
        if (ALWAYS_NULLABLE) {
            spawn_allowed = true;
        }
        else {
            while (ni > 0 && nulls_ptr[ni - 1] < pos) {
                ni -= 1;
            }
            if (ni == 0) {
                break_phase1 = true;
                break;
            }
            spawn_allowed = (nulls_ptr[ni - 1] == pos);
        }
        if (break_phase1) break; // unreachable but keeps parity

        if (!ALWAYS_NULLABLE && !spawn_allowed) {
            bool all_empty = true;
            for (size_t i = 0; i < regs.len; ++i) {
                if (!SlotEntries_is_empty(&regs.data[i])) {
                    all_empty = false;
                    break;
                }
            }
            if (all_empty) {
                pos  = nulls_ptr[ni - 1];
                asid = 0;
                fas_slot_list_clear_each(&regs);
                continue;
            }
        }

        uint32_t mt = (uint32_t)engine_LDFA_mt_lookup_at(self, (size_t)data[pos]);
        n00b_require((size_t)mt < fas->stride, "scan_fwd: mt exceeds stride");
        size_t trans_idx = ((size_t)asid) * fas->stride | (size_t)mt;
        n00b_require(trans_idx < fas->trans.len, "FwdDFA.trans OOB");
        uint32_t cached = fas->trans.data[trans_idx];
        uint32_t action_id;
        if (cached != FAS_ACTION_MISSING) {
            action_id = cached;
        }
        else {
            n00b_result_t(uint32_t) ar =
                FwdDFA_compute_action(fas, b, self, asid, mt);
            if (n00b_result_is_err(ar)) {
                fas->max      = max;
                fas->linker   = linker;
                fas->regs     = regs;
                fas->new_regs = new_regs;
                return n00b_result_err(int, n00b_result_get_err(ar));
            }
            action_id = n00b_result_get(ar);
            // compute_action may have grown self->trans; re-check length.
            n00b_require(trans_idx < fas->trans.len, "FwdDFA.trans OOB");
            fas->trans.data[trans_idx] = action_id;
        }
        n00b_require(action_id < fas->actions.len, "FwdDFA.actions OOB");
        const FwdAction *act = &fas->actions.data[action_id];
        fas_apply(act, regs.data, regs.len, &new_regs,
                  linker.data, max.data, pos,
                  fas->always_nullable, fas->keep_spawn_on_merge,
                  spawn_allowed);
        // std::mem::swap(&mut regs, &mut new_regs);
        {
            fas_slot_list tmp = regs;
            regs              = new_regs;
            new_regs          = tmp;
        }
        asid = act->next_asid;
        pos += 1;
    }

    // phase 2: process remaining bytes only as long as some slot is alive
    if (!ALWAYS_NULLABLE) {
        while (pos < data_end) {
            bool all_empty = true;
            for (size_t i = 0; i < regs.len; ++i) {
                if (!SlotEntries_is_empty(&regs.data[i])) {
                    all_empty = false;
                    break;
                }
            }
            if (all_empty) break;
            uint32_t mt = (uint32_t)engine_LDFA_mt_lookup_at(self, (size_t)data[pos]);
            n00b_require((size_t)mt < fas->stride, "scan_fwd: mt exceeds stride");
            size_t trans_idx = ((size_t)asid) * fas->stride | (size_t)mt;
            n00b_require(trans_idx < fas->trans.len, "FwdDFA.trans OOB");
            uint32_t cached = fas->trans.data[trans_idx];
            uint32_t action_id;
            if (cached != FAS_ACTION_MISSING) {
                action_id = cached;
            }
            else {
                n00b_result_t(uint32_t) ar =
                    FwdDFA_compute_action(fas, b, self, asid, mt);
                if (n00b_result_is_err(ar)) {
                    fas->max      = max;
                    fas->linker   = linker;
                    fas->regs     = regs;
                    fas->new_regs = new_regs;
                    return n00b_result_err(int, n00b_result_get_err(ar));
                }
                action_id = n00b_result_get(ar);
                n00b_require(trans_idx < fas->trans.len, "FwdDFA.trans OOB");
                fas->trans.data[trans_idx] = action_id;
            }
            n00b_require(action_id < fas->actions.len, "FwdDFA.actions OOB");
            const FwdAction *act = &fas->actions.data[action_id];
            fas_apply(act, regs.data, regs.len, &new_regs,
                      linker.data, max.data, pos,
                      fas->always_nullable, fas->keep_spawn_on_merge,
                      false /* spawn_allowed: ni == 0 */);
            {
                fas_slot_list tmp = regs;
                regs              = new_regs;
                new_regs          = tmp;
            }
            asid = act->next_asid;
            pos += 1;
        }
    }

    // end
    {
        n00b_require(asid < fas->states.len, "FwdDFA.states OOB");
        const fas_list_u16 *states = &fas->states.data[asid];
        for (size_t slot = 0; slot < states->len; ++slot) {
            uint16_t sid = states->data[slot];
            if (SlotEntries_is_empty(&regs.data[slot])) continue;
            uint32_t eid     = (uint32_t)engine_LDFA_effects_id_at(self, (size_t)sid);
            bool     have_ce = false;
            size_t   cand_end = 0;
            if (eid == EID_NONE || eid == EID_CENTER0 || eid == EID_BEGIN0) {
                continue;
            }
            else if (eid == EID_ALWAYS0 || eid == EID_END0) {
                cand_end = data_end;
                have_ce  = true;
            }
            else {
                size_t n = engine_LDFA_effects_len(self, eid);
                for (size_t j = n; j-- > 0;) {
                    NullState ns;
                    engine_LDFA_effects_get(self, eid, j, &ns);
                    if (nullability_mask_has_end(ns.mask)) {
                        cand_end = (data_end >= (size_t)ns.rel)
                                     ? (data_end - (size_t)ns.rel)
                                     : 0;
                        have_ce  = true;
                        break;
                    }
                }
            }
            if (have_ce) {
                SlotEntries_extend_e(&regs.data[slot], cand_end);
            }
        }
    }

    for (size_t i = 0; i < regs.len; ++i) {
        SlotEntries_drain_to_max(&regs.data[i], linker.data, max.data, max.len);
    }

    // Emit matches
    size_t skip_until = 0;
    if (ALWAYS_NULLABLE) {
        for (size_t i = 0; i <= data_end; ++i) {
            if (i < skip_until) continue;
            size_t e = max.data[i] > i ? max.data[i] : i;
            matches_push(matches, i, e);
            skip_until = (e > i) ? e : (i + 1);
        }
    }
    else {
        for (size_t k = nulls_len; k-- > 0;) {
            size_t i = nulls[k];
            if (i < skip_until || max.data[i] == 0) continue;
            size_t e = max.data[i];
            matches_push(matches, i, e);
            skip_until = (e > i) ? e : (i + 1);
        }
    }

    // Put borrowed buffers back.
    fas->max      = max;
    fas->linker   = linker;
    fas->regs     = regs;
    fas->new_regs = new_regs;
    return n00b_result_ok(int, 0);
}

n00b_result_t(int) LDFA_scan_fwd_active_set_always_nullable(
    LDFA *self, RegexBuilder *b, FwdDFA *fas,
    const uint8_t *data, size_t data_len,
    const size_t *nulls, size_t nulls_len,
    void *matches)
{
    n00b_require(self    != nullptr, "scan_fwd_active_set: self must be non-null");
    n00b_require(fas     != nullptr, "scan_fwd_active_set: fas must be non-null");
    n00b_require(matches != nullptr, "scan_fwd_active_set: matches must be non-null");
    n00b_require(!(data == nullptr && data_len > 0),
                 "scan_fwd_active_set: data null with non-zero data_len");
    n00b_require(!(nulls == nullptr && nulls_len > 0),
                 "scan_fwd_active_set: nulls null with non-zero nulls_len");
    return scan_fwd_active_set_impl(self, b, fas, data, data_len,
                                    nulls, nulls_len, matches,
                                    /*ALWAYS_NULLABLE=*/true);
}

n00b_result_t(int) LDFA_scan_fwd_active_set_general(
    LDFA *self, RegexBuilder *b, FwdDFA *fas,
    const uint8_t *data, size_t data_len,
    const size_t *nulls, size_t nulls_len,
    void *matches)
{
    n00b_require(self    != nullptr, "scan_fwd_active_set: self must be non-null");
    n00b_require(fas     != nullptr, "scan_fwd_active_set: fas must be non-null");
    n00b_require(matches != nullptr, "scan_fwd_active_set: matches must be non-null");
    n00b_require(!(data == nullptr && data_len > 0),
                 "scan_fwd_active_set: data null with non-zero data_len");
    n00b_require(!(nulls == nullptr && nulls_len > 0),
                 "scan_fwd_active_set: nulls null with non-zero nulls_len");
    return scan_fwd_active_set_impl(self, b, fas, data, data_len,
                                    nulls, nulls_len, matches,
                                    /*ALWAYS_NULLABLE=*/false);
}
