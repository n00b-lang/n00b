#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/layout.h"
#include "ir/operands.h"
#include "ir/summary.h"
#include "ir/text.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(ir_sfield_t);
core_list_decl(ir_splace_t);
typedef core_list_t(ir_splace_t) place_list_t;
core_list_decl(ir_strap_site_t);
core_list_decl(ir_switness_t);
core_list_decl(ir_sstore_t);
core_list_decl(ir_sinput_t);
core_list_decl(ir_soutput_t);
core_list_decl(uint8_t);
typedef ir_func_t *summary_func_ptr_t;
core_list_decl(summary_func_ptr_t);
typedef const ir_instr_t *summary_instr_ptr_t;
core_list_decl(summary_instr_ptr_t);
typedef ir_block_t *summary_block_ptr_t;
core_list_decl(summary_block_ptr_t);
typedef ir_summary_t *summary_ptr_t;
core_list_decl(summary_ptr_t);

struct ir_summary_t {
    const ir_func_t           *function;
    uint32_t                   flags;
    core_list_t(ir_sinput_t)   inputs;
    core_list_t(ir_soutput_t)  outputs;
    core_list_t(uint32_t)      dependences;
    bool                       results_known;
    bool                       returns_external;
    core_list_t(ir_sstore_t)   results;
    core_list_t(ir_sstore_t)   stores;
    uint8_t                    traps;
    core_list_t(ir_strap_site_t) trap_sites;
    core_list_t(ir_switness_t) witnesses;
    core_list_t(ir_sfield_t)   fields;
    core_list_t(core_string_t) effects;
    core_list_t(ir_spath_t)    paths;
};

static uint8_t
trap_kind(const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_TRAP:
        return IR_STRAP_EXPLICIT;
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_SET:
        return IR_STRAP_BOUNDS;
    case IR_OP_OPTION_GET:
        return IR_STRAP_UNWRAP;
    default:
        return IR_STRAP_ARITHMETIC;
    }
}

struct ir_summaries_t {
    const ir_summary_hooks_t *hooks;
    core_list_t(summary_ptr_t) owned;
    core_dict_t               by_function;
    core_dict_t               names;
    core_dict_t               type_memos;
    bool                      escapes_only;
};

bool
ir_splace_eq(const ir_splace_t *a, const ir_splace_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case IR_SPLACE_PARAM:
        return a->param == b->param;
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return a->global == b->global;
    case IR_SPLACE_HEAP:
        return a->type == b->type
            || (a->type && b->type && th_resolve(a->type) == th_resolve(b->type));
    case IR_SPLACE_UNKNOWN:
        return true;
    }
    return false;
}

ir_summary_t *
ir_summary_new(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = core_alloc(ir_summary_t);
    summary->function    = function;
    summary->inputs      = core_list_new(ir_sinput_t);
    summary->outputs     = core_list_new(ir_soutput_t);
    summary->dependences = core_list_new(uint32_t);
    summary->fields      = core_list_new(ir_sfield_t);
    summary->trap_sites  = core_list_new(ir_strap_site_t);
    summary->witnesses   = core_list_new(ir_switness_t);
    summary->effects     = core_list_new(core_string_t);
    summary->paths       = core_list_new(ir_spath_t);
    summary->results     = core_list_new(ir_sstore_t);
    summary->stores      = core_list_new(ir_sstore_t);
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

static void
release_path(ir_spath_t *path)
{
    for (uint8_t s = 0; s < path->depth; s++) core_free(path->steps[s].data);
}

static void
release_paths(core_list_t(ir_spath_t) *paths)
{
    for (size_t i = 0; i < paths->len; i++) release_path(&paths->data[i]);
    paths->len = 0;
}

typedef core_list_t(ir_sstore_t) pair_list_t;

static void
release_pairs(pair_list_t *pairs)
{
    for (size_t i = 0; i < pairs->len; i++) {
        release_path(&pairs->data[i].to);
        release_path(&pairs->data[i].from);
    }
    pairs->len = 0;
}

static void
summary_free(ir_summary_t *summary)
{
    for (size_t i = 0; i < summary->effects.len; i++)
        core_free(summary->effects.data[i].data);
    core_list_free(summary->effects);
    release_paths(&summary->paths);
    core_list_free(summary->paths);
    release_pairs(&summary->results);
    core_list_free(summary->results);
    release_pairs(&summary->stores);
    core_list_free(summary->stores);
    core_list_free(summary->witnesses);
    core_list_free(summary->trap_sites);
    core_list_free(summary->inputs);
    core_list_free(summary->outputs);
    core_list_free(summary->dependences);
    core_list_free(summary->fields);
    core_free(summary);
}

void
ir_summary_set_flags(ir_summary_t *summary, uint32_t flags)
{
    summary->flags |= flags;
}

bool
ir_summary_add_effect(ir_summary_t *summary, core_string_t name)
{
    if (!summary || !name.u8_bytes) return false;
    size_t at = 0;
    while (at < summary->effects.len) {
        int order = core_string_cmp(summary->effects.data[at], name);
        if (order == 0) return true;
        if (order > 0) break;
        at++;
    }
    core_string_t copy = core_string_dup(name);
    if (!copy.data) return false;
    core_list_insert(summary->effects, at, copy);
    return true;
}

uint32_t
ir_summary_effect_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->effects.len : 0;
}

core_string_t
ir_summary_effect_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->effects.len
        ? summary->effects.data[index] : (core_string_t){0};
}

static const core_string_t flag_names[] = {
#define IR_EFFECT_FLAG(id, name, bit) [bit] = CORE_STRING_STATIC(#name),
#include "ir/summary_effects.def"
};

static const core_string_t trap_names[] = {
#define IR_EFFECT_TRAP(id, name, bit) [bit] = CORE_STRING_STATIC(#name),
#include "ir/summary_effects.def"
};

static const struct {
    core_string_t name;
    uint64_t      bits;
} effect_names[] = {
#define IR_EFFECT_FLAG(id, name, bit) \
    {CORE_STRING_STATIC(#name), UINT64_C(1) << bit},
#define IR_EFFECT_TRAP(id, name, bit) \
    {CORE_STRING_STATIC("traps_" #name), \
     UINT64_C(1) << (IR_SEFFECT_TRAPS_SHIFT + bit)},
#define IR_EFFECT_NONLOCAL(id, name, bit) \
    {CORE_STRING_STATIC(#name), UINT64_C(1) << bit},
#include "ir/summary_effects.def"
};

core_string_t
ir_summary_flag_name(uint32_t bit)
{
    return bit < sizeof(flag_names) / sizeof(*flag_names)
        ? flag_names[bit] : (core_string_t){0};
}

core_string_t
ir_summary_trap_name(uint32_t bit)
{
    return bit < sizeof(trap_names) / sizeof(*trap_names)
        ? trap_names[bit] : (core_string_t){0};
}

bool
ir_summary_effect_lookup(core_string_t name, uint64_t *bits)
{
    for (size_t i = 0; i < sizeof(effect_names) / sizeof(*effect_names); i++)
        if (core_string_eq(effect_names[i].name, name)) {
            *bits = effect_names[i].bits;
            return true;
        }
    return false;
}

bool
ir_summary_has_effect(const ir_summary_t *summary, core_string_t name)
{
    for (size_t i = 0; summary && i < summary->effects.len; i++)
        if (core_string_eq(summary->effects.data[i], name)) return true;
    return false;
}

static bool
same_effects(const ir_summary_t *a, const ir_summary_t *b)
{
    if (a->effects.len != b->effects.len) return false;
    for (size_t i = 0; i < a->effects.len; i++)
        if (!core_string_eq(a->effects.data[i], b->effects.data[i]))
            return false;
    return true;
}

static bool
merge_effects(ir_summary_t *into, const ir_summary_t *from)
{
    for (size_t i = 0; i < from->effects.len; i++)
        if (!ir_summary_add_effect(into, from->effects.data[i])) return false;
    return true;
}

enum { PATH_LIMIT = 16 };

static bool
named_root(ir_splace_kind_t root)
{
    return root == IR_SPLACE_GLOBAL || root == IR_SPLACE_THREAD;
}

static int
compare_path_roots(const ir_spath_t *a, const ir_spath_t *b)
{
    if (a->output != b->output) return a->output < b->output ? -1 : 1;
    if (a->root != b->root) return a->root < b->root ? -1 : 1;
    if (a->root == IR_SPLACE_PARAM && a->param != b->param)
        return a->param < b->param ? -1 : 1;
    if (!named_root(a->root) || a->global == b->global) return 0;
    if (!a->global || !b->global) return a->global ? 1 : -1;
    int order = core_string_cmp(a->global->name, b->global->name);
    if (order) return order;
    return (uintptr_t)a->global < (uintptr_t)b->global ? -1 : 1;
}

static bool
same_path_root(const ir_spath_t *a, const ir_spath_t *b)
{
    return compare_path_roots(a, b) == 0;
}

static bool
same_type(th_type *a, th_type *b)
{
    return a == b || (a && b && th_resolve(a) == th_resolve(b));
}

static th_type *
heap_type_of(const ir_spath_t *path)
{
    return path->root == IR_SPLACE_UNKNOWN && (path->flags & IR_SPATH_TYPED)
        ? path->type : nullptr;
}

static bool
same_access_kind(const ir_spath_t *a, const ir_spath_t *b)
{
    return ((a->flags ^ b->flags) & IR_SPATH_ROOT) == 0
        && same_type(heap_type_of(a), heap_type_of(b));
}

static bool
same_path_target(const ir_spath_t *a, const ir_spath_t *b)
{
    if (!same_path_root(a, b) || a->depth != b->depth
        || !same_access_kind(a, b))
        return false;
    for (uint8_t s = 0; s < a->depth; s++)
        if (!core_string_eq(a->steps[s], b->steps[s])) return false;
    return true;
}

typedef ir_spath_list_t path_list_t;

static void
root_range(const path_list_t *paths, const ir_spath_t *root, size_t *first,
           size_t *last)
{
    size_t lo = 0, hi = paths->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_path_roots(&paths->data[mid], root) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *first = lo;
    hi = paths->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (compare_path_roots(&paths->data[mid], root) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *last = lo;
}

static bool
merge_type(ir_spath_t *into, const ir_spath_t *from)
{
    bool typed = (into->flags & IR_SPATH_TYPED)
        && (from->flags & IR_SPATH_TYPED);
    if (!typed) into->type = nullptr;
    else if (!into->type) into->type = from->type;
    else if (from->type && !same_type(into->type, from->type)) {
        into->type = nullptr;
        typed      = false;
    }
    return typed;
}

static void
merge_path_into(ir_spath_t *into, const ir_spath_t *from)
{
    bool typed = merge_type(into, from);
    into->flags = (uint8_t)((into->flags | from->flags) & ~IR_SPATH_TYPED);
    if (typed) into->flags |= IR_SPATH_TYPED;
}

static size_t
compact_range(path_list_t *paths, size_t first, size_t last)
{
    size_t kept = first;
    for (size_t i = first; i < last; i++) {
        ir_spath_t path = paths->data[i];
        size_t j = first;
        while (j < kept && !same_path_target(&paths->data[j], &path))
            j++;
        if (j == kept) {
            paths->data[kept++] = path;
            continue;
        }
        merge_path_into(&paths->data[j], &path);
        release_path(&path);
    }
    size_t removed = last - kept;
    for (size_t i = last; removed && i < paths->len; i++)
        paths->data[i - removed] = paths->data[i];
    paths->len -= removed;
    return kept;
}

static void
widen_range(path_list_t *paths, size_t first, size_t last)
{
    while (last - first > PATH_LIMIT) {
        uint8_t deepest = 0;
        for (size_t i = first; i < last; i++)
            if (paths->data[i].depth > deepest)
                deepest = paths->data[i].depth;
        if (!deepest) return;
        for (size_t i = first; i < last; i++) {
            ir_spath_t *path = &paths->data[i];
            if (path->depth != deepest) continue;
            core_free(path->steps[--path->depth].data);
            path->flags |= IR_SPATH_DEEP;
        }
        last = compact_range(paths, first, last);
    }
}

static bool
steps_prefix(uint8_t depth, const core_string_t *steps, uint8_t other_depth,
             const core_string_t *other)
{
    if (depth > other_depth) return false;
    for (uint8_t s = 0; s < depth; s++)
        if (!core_string_eq(steps[s], other[s])) return false;
    return true;
}

static bool
steps_cover(uint8_t depth, bool deep, const core_string_t *steps,
            uint8_t other_depth, bool other_deep, const core_string_t *other)
{
    if ((depth < other_depth && !deep)
        || (depth == other_depth && other_deep && !deep)) return false;
    return steps_prefix(depth, steps, other_depth, other);
}

static bool
paths_overlap(const ir_spath_t *a, const ir_spath_t *b)
{
    return steps_prefix(a->depth, a->steps, b->depth, b->steps)
        || steps_prefix(b->depth, b->steps, a->depth, a->steps);
}

static bool
path_covers(const ir_spath_t *prefix, const ir_spath_t *path)
{
    th_type *outer = heap_type_of(prefix);
    if (prefix->depth > path->depth
        || (prefix->depth < path->depth && !(prefix->flags & IR_SPATH_DEEP))
        || ((prefix->flags ^ path->flags) & IR_SPATH_ROOT)
        || (outer && !same_type(outer, heap_type_of(path))))
        return false;
    for (uint8_t s = 0; s < prefix->depth; s++)
        if (!core_string_eq(prefix->steps[s], path->steps[s])) return false;
    return true;
}

static bool
path_absorbs(const ir_spath_t *into, const ir_spath_t *path)
{
    return path_covers(into, path)
        && (!(path->flags & ~into->flags & ~IR_SPATH_TYPED)
            || (into->depth == path->depth
                && !((into->flags ^ path->flags) & IR_SPATH_DEEP)));
}

static ir_spath_t
canonical_path(const ir_spath_t *path)
{
    ir_spath_t copy = *path;
    if (named_root(copy.root) && copy.global)
        copy.global = ir_global_identity((ir_global_t *)copy.global);
    return copy;
}

static void
add_path_to(path_list_t *paths, const ir_spath_t *path)
{
    ir_spath_t copy = canonical_path(path);
    size_t first, last;
    root_range(paths, &copy, &first, &last);
    for (size_t i = first; i < last; i++)
        if (path_absorbs(&paths->data[i], &copy)) {
            merge_path_into(&paths->data[i], &copy);
            return;
        }
    size_t kept = first;
    for (size_t i = first; i < last; i++) {
        ir_spath_t *old = &paths->data[i];
        if (path_covers(&copy, old)
            && !(old->flags & ~copy.flags & ~IR_SPATH_TYPED)) {
            if (!merge_type(&copy, old)) copy.flags &= ~IR_SPATH_TYPED;
            release_path(old);
        }
        else
            paths->data[kept++] = *old;
    }
    for (size_t i = last; kept < last && i < paths->len; i++)
        paths->data[kept + i - last] = paths->data[i];
    paths->len -= last - kept;
    last = kept;
    for (uint8_t s = 0; s < copy.depth; s++)
        copy.steps[s] = core_string_dup(path->steps[s]);
    core_list_insert(*paths, last, copy);
    widen_range(paths, first, last + 1);
}

static void
add_path(ir_summary_t *summary, const ir_spath_t *path)
{
    add_path_to(&summary->paths, path);
}

static bool
same_path_list(const path_list_t *a, const path_list_t *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; i++)
        if (!same_path_target(&a->data[i], &b->data[i])
            || a->data[i].flags != b->data[i].flags)
            return false;
    return true;
}

enum { STORE_LIMIT = 64 };

static int
compare_paths(const ir_spath_t *a, const ir_spath_t *b)
{
    int order = compare_path_roots(a, b);
    if (order) return order;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    for (uint8_t s = 0; s < a->depth; s++) {
        order = core_string_cmp(a->steps[s], b->steps[s]);
        if (order) return order;
    }
    return a->flags == b->flags ? 0 : a->flags < b->flags ? -1 : 1;
}

static int
compare_stores(const ir_sstore_t *a, const ir_sstore_t *b)
{
    int order = compare_paths(&a->to, &b->to);
    return order ? order : compare_paths(&a->from, &b->from);
}

static bool
spath_covers(const ir_spath_t *a, const ir_spath_t *b)
{
    return same_path_root(a, b)
        && !(b->flags & IR_SPATH_ROOT & ~a->flags)
        && steps_cover(a->depth, a->flags & IR_SPATH_DEEP, a->steps,
                       b->depth, b->flags & IR_SPATH_DEEP, b->steps);
}

static bool
pair_covers(const ir_sstore_t *a, const ir_sstore_t *b)
{
    return spath_covers(&a->to, &b->to) && spath_covers(&a->from, &b->from);
}

static void
add_pair(pair_list_t *pairs, const ir_spath_t *to, const ir_spath_t *from)
{
    ir_sstore_t pair = {.to = canonical_path(to), .from = canonical_path(from)};
    if (pairs->len >= STORE_LIMIT) {
        pair.to.depth    = 0;
        pair.to.flags   |= IR_SPATH_DEEP;
        pair.from.depth  = 0;
        pair.from.flags |= IR_SPATH_DEEP;
    }
    for (size_t i = 0; i < pairs->len; i++)
        if (pair_covers(&pairs->data[i], &pair)) return;
    size_t kept = 0;
    for (size_t i = 0; i < pairs->len; i++) {
        ir_sstore_t *old = &pairs->data[i];
        if (!pair_covers(&pair, old)) {
            pairs->data[kept++] = *old;
            continue;
        }
        release_path(&old->to);
        release_path(&old->from);
    }
    pairs->len = kept;
    for (uint8_t s = 0; s < pair.to.depth; s++)
        pair.to.steps[s] = core_string_dup(pair.to.steps[s]);
    for (uint8_t s = 0; s < pair.from.depth; s++)
        pair.from.steps[s] = core_string_dup(pair.from.steps[s]);
    size_t at = 0;
    while (at < pairs->len && compare_stores(&pairs->data[at], &pair) < 0)
        at++;
    core_list_insert(*pairs, at, pair);
}

static bool
same_pairs(const pair_list_t *a, const pair_list_t *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; i++)
        if (compare_stores(&a->data[i], &b->data[i])) return false;
    return true;
}

static bool
same_paths(const ir_summary_t *a, const ir_summary_t *b)
{
    return same_path_list(&a->paths, &b->paths)
        && same_pairs(&a->results, &b->results)
        && same_pairs(&a->stores, &b->stores)
        && a->results_known == b->results_known
        && a->returns_external == b->returns_external;
}

uint32_t
ir_summary_store_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->stores.len : 0;
}

const ir_sstore_t *
ir_summary_store_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->stores.len
        ? &summary->stores.data[index] : nullptr;
}

uint32_t
ir_summary_result_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->results.len : 0;
}

const ir_sstore_t *
ir_summary_result_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->results.len
        ? &summary->results.data[index] : nullptr;
}

void
ir_summary_add_result(ir_summary_t *summary, const ir_spath_t *to,
                      const ir_spath_t *from)
{
    add_pair(&summary->results, to, from);
    summary->results_known = true;
}

uint32_t
ir_summary_path_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->paths.len : 0;
}

const ir_spath_t *
ir_summary_path_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->paths.len
        ? &summary->paths.data[index] : nullptr;
}

static const core_string_t element_step = CORE_STRING_STATIC("[]");

static uint8_t
parse_query_path(core_string_t text, core_string_t *root,
                 core_string_t steps[IR_SPATH_STEPS])
{
    uint8_t depth = 0;
    size_t  start = 0;
    bool    named = false;
    for (size_t at = 0; at <= text.u8_bytes; at++) {
        char byte = at < text.u8_bytes ? text.data[at] : '.';
        if (byte != '.' && byte != '[') continue;
        core_string_t token = core_string_slice(text, start, at - start);
        if (token.u8_bytes) {
            if (!named) *root = token;
            else if (depth < IR_SPATH_STEPS) steps[depth++] = token;
            named = true;
        }
        if (byte == '[') {
            if (depth < IR_SPATH_STEPS)
                steps[depth++] = element_step;
            if (at + 1 < text.u8_bytes && text.data[at + 1] == ']') at++;
        }
        start = at + 1;
    }
    return depth;
}

static bool
path_root_named(const ir_func_t *function, const ir_spath_t *path,
                core_string_t name)
{
    if (path->root == IR_SPLACE_PARAM)
        return function && path->param < function->n_params
            && function->params[path->param]
            && core_string_eq(function->params[path->param]->name, name);
    return named_root(path->root)
        && (!path->global || core_string_eq(path->global->name, name));
}

bool
ir_summary_may_access(const ir_summary_t *summary, const ir_func_t *function,
                      core_string_t query, bool write)
{
    if (!summary) return true;
    uint8_t mode = write ? IR_SPATH_WRITE : IR_SPATH_READ;
    if (function && !function->n_blocks) {
        for (size_t i = 0; i < summary->paths.len; i++)
            if (summary->paths.data[i].flags & mode) return true;
        return false;
    }
    core_string_t root   = {0};
    ir_spath_t    wanted = {0};
    wanted.depth = parse_query_path(query, &root, wanted.steps);
    for (size_t i = 0; i < summary->paths.len; i++) {
        const ir_spath_t *path = &summary->paths.data[i];
        if (!(path->flags & mode)) continue;
        if (path->root == IR_SPLACE_UNKNOWN) return true;
        if (path_root_named(function, path, root)
            && paths_overlap(path, &wanted)) return true;
    }
    return false;
}

void
ir_summary_add_path(ir_summary_t *summary, const ir_spath_t *path)
{
    add_path(summary, path);
}

void
ir_summary_merged_paths(const ir_summary_t *summary, ir_spath_list_t *into)
{
    for (size_t i = 0; summary && i < summary->paths.len; i++) {
        ir_spath_t path = summary->paths.data[i];
        path.flags &= (uint8_t)~(IR_SPATH_ROOT | IR_SPATH_TYPED);
        path.type   = nullptr;
        add_path_to(into, &path);
    }
    bool *absorbed = into->len ? core_alloc_array(bool, into->len) : nullptr;
    for (size_t i = 0; i < into->len; i++)
        for (size_t j = 0; j < into->len; j++)
            if (j != i && !absorbed[j]
                && same_path_root(&into->data[j], &into->data[i])
                && path_covers(&into->data[j], &into->data[i])
                && !(into->data[i].flags & ~into->data[j].flags)) {
                absorbed[i] = true;
                break;
            }
    size_t kept = 0;
    for (size_t i = 0; i < into->len; i++)
        if (absorbed[i]) release_path(&into->data[i]);
        else into->data[kept++] = into->data[i];
    into->len = kept;
    core_free(absorbed);
}

void
ir_spath_list_release(ir_spath_list_t *paths)
{
    release_paths(paths);
    core_list_free(*paths);
}

static void
written_places(const ir_summary_t *summary, place_list_t *into)
{
    for (size_t i = 0; i < summary->paths.len; i++) {
        ir_splace_t places[2];
        uint32_t    count = summary->paths.data[i].flags & IR_SPATH_WRITE
            ? ir_summary_path_places(summary->function,
                                     &summary->paths.data[i], places)
            : 0;
        for (uint32_t p = 0; p < count; p++) {
            bool seen = false;
            for (size_t j = 0; !seen && j < into->len; j++)
                seen = ir_splace_eq(&into->data[j], &places[p]);
            if (!seen) core_list_push(*into, places[p]);
        }
    }
}

ir_summary_t *
ir_summary_conservative(ir_summaries_t *summaries, const ir_func_t *function)
{
    ir_summary_t *summary = ir_summary_new(summaries, function);
    summary->flags = IR_SUMMARY_MAY_FAIL | IR_SUMMARY_MAY_TRAP
        | IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_BLOCK
        | IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_IO
        | IR_SUMMARY_MAY_COLLECT | IR_SUMMARY_OBSERVES;
    add_path(summary, &(ir_spath_t){
        .root  = IR_SPLACE_UNKNOWN,
        .flags = IR_SPATH_ACCESS | IR_SPATH_DEEP});
    summary->traps = IR_STRAP_ALL;
    return summary;
}

static bool holds_pointers(const ir_func_t *f, th_type *type, uint32_t depth);

static th_type *
summary_callable(const ir_summary_t *summary)
{
    th_type *callable = summary->function
        ? th_resolve(summary->function->type) : nullptr;
    return callable && th_kind_of(callable) == TH_FUNCTION ? callable
                                                           : nullptr;
}

static bool
returns_pointers(const ir_summary_t *summary)
{
    th_type *callable = summary_callable(summary);
    return !callable
        || holds_pointers(summary->function, th_func_ret(callable), 0);
}

static uint8_t
escape_from(const ir_spath_t *from, uint32_t param, bool address)
{
    if (from->root != IR_SPLACE_PARAM || from->param != param) return 0;
    uint8_t escape = 0;
    if (address && !from->depth && (from->flags & IR_SPATH_ROOT))
        escape |= IR_SESCAPE_ADDRESS;
    if (from->depth || (from->flags & IR_SPATH_DEEP))
        escape |= IR_SESCAPE_REFERENT;
    return escape;
}

uint8_t
ir_summary_param_escape(const ir_summary_t *summary, uint32_t param)
{
    if (!summary || !summary->results_known)
        return IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT;
    uint8_t escape = 0;
    for (size_t i = 0; i < summary->stores.len; i++)
        escape |= escape_from(&summary->stores.data[i].from, param, true);
    th_type *callable = summary->function ? summary->function->type
                                          : nullptr;
    for (size_t i = 0; i < summary->results.len; i++) {
        const ir_spath_t *to     = &summary->results.data[i].to;
        bool              direct = !to->depth
            && !(to->flags & IR_SPATH_DEEP);
        bool              own    = to->output
            ? direct && callable
                && ir_call_output_param(callable, to->output - 1u) == param
            : returns_pointers(summary);
        escape |= escape_from(&summary->results.data[i].from, param, !own);
    }
    return escape;
}

static ir_sreturn_t
result_source(const ir_summary_t *summary, const ir_spath_t *from)
{
    bool own = !from->depth && (from->flags & IR_SPATH_ROOT)
        && !(from->flags & IR_SPATH_DEEP);
    switch (from->root) {
    case IR_SPLACE_PARAM:
        return own ? (ir_sreturn_t){.kind = IR_SRETURN_PARAM,
                                    .param = from->param}
                   : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return own && from->global
            ? (ir_sreturn_t){.kind = IR_SRETURN_GLOBAL,
                             .global = from->global}
            : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    case IR_SPLACE_HEAP:
        return (ir_sreturn_t){.kind = summary->returns_external
                                  ? IR_SRETURN_FRESH_EXTERNAL
                                  : IR_SRETURN_FRESH};
    case IR_SPLACE_UNKNOWN:
        break;
    }
    return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
}

ir_sreturn_t
ir_summary_return(const ir_summary_t *summary)
{
    if (!summary || !summary->results_known)
        return (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    ir_sreturn_t returned = {.kind = IR_SRETURN_NONE};
    if (!returns_pointers(summary)) return returned;
    for (size_t i = 0; i < summary->results.len; i++) {
        const ir_sstore_t *result = &summary->results.data[i];
        if (result->to.output || result->to.depth) continue;
        ir_sreturn_t source = result_source(summary, &result->from);
        bool same = source.kind == returned.kind
            && source.param == returned.param
            && source.global == returned.global;
        returned = returned.kind == IR_SRETURN_NONE || same
            ? source : (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN};
    }
    return returned;
}

bool
ir_summary_returns_fresh(const ir_summary_t *summary)
{
    if (!summary || !summary->results_known) return false;
    bool fresh = false;
    for (size_t i = 0; i < summary->results.len; i++) {
        const ir_sstore_t *result = &summary->results.data[i];
        if (result->to.output) continue;
        if (result->from.root != IR_SPLACE_HEAP) return false;
        fresh = true;
    }
    for (size_t i = 0; fresh && i < summary->stores.len; i++)
        if (summary->stores.data[i].to.root == IR_SPLACE_HEAP
            && summary->stores.data[i].from.root != IR_SPLACE_HEAP)
            return false;
    return fresh;
}

const ir_func_t *ir_summary_function(const ir_summary_t *s) { return s->function; }
bool ir_summary_results_known(const ir_summary_t *s)
{
    return s && s->results_known;
}
uint32_t ir_summary_flags(const ir_summary_t *s) { return s ? s->flags : 0; }
uint8_t ir_summary_trap_kinds(const ir_summary_t *s)
{
    return s ? s->traps : IR_STRAP_ALL;
}
void ir_summary_set_trap_kinds(ir_summary_t *s, uint8_t kinds)
{
    s->traps |= kinds & IR_STRAP_ALL;
}
uint32_t ir_summary_trap_site_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->trap_sites.len : 0;
}
const ir_strap_site_t *ir_summary_trap_site_at(const ir_summary_t *s,
                                               uint32_t i)
{
    return s && i < s->trap_sites.len ? &s->trap_sites.data[i] : nullptr;
}
const ir_switness_t *ir_summary_witness_for(const ir_summary_t *s,
                                            uint32_t flag)
{
    for (size_t i = 0; s && i < s->witnesses.len; i++)
        if (s->witnesses.data[i].flag == flag) return &s->witnesses.data[i];
    return nullptr;
}

void
ir_switness_callee_text(const ir_switness_t *witness, core_buffer_t *out)
{
    if (witness->module.u8_bytes) {
        core_buffer_append(out, witness->module.data, witness->module.u8_bytes);
        core_buffer_append(out, ".", 1);
    }
    int64_t overload = core_string_find_byte(witness->name, '#');
    core_buffer_append(out, witness->name.data,
                       overload >= 0 ? (size_t)overload
                                     : witness->name.u8_bytes);
}

static void
add_flags(ir_summary_t *summary, uint32_t flags, const ir_instr_t *in,
          ir_func_t *callee, core_string_t name)
{
    uint32_t added = flags & ~summary->flags;
    summary->flags |= flags;
    for (uint32_t b = 0; added && b < IR_SEFFECT_FLAG_BITS; b++) {
        uint32_t flag = 1u << b;
        if (!(added & flag) || ir_summary_witness_for(summary, flag))
            continue;
        core_list_push(summary->witnesses, ((ir_switness_t){
            .flag   = flag,
            .callee = callee,
            .module = callee ? ir_module_name(callee->owner)
                             : (core_string_t){0},
            .name   = callee ? callee->name : name,
            .line   = in ? in->span.start_line : 0,
            .column = in ? in->span.start_col : 0,
        }));
    }
}

static core_string_t
qualified_name(ir_summaries_t *summaries, ir_ctx_t *ctx,
               const ir_func_t *target)
{
    bool found = false;
    core_string_t *known = core_dict_get(&summaries->names, (void *)target,
                                         &found);
    if (found) return *known;
    core_buffer_t text   = {0};
    core_string_t module = ir_module_name(target->owner);
    core_buffer_append(&text, module.data, module.u8_bytes);
    core_buffer_append(&text, ".", 1);
    core_buffer_append(&text, target->name.data, target->name.u8_bytes);
    core_string_t *name = ir_alloc(ctx, sizeof(core_string_t));
    *name = ir_strdup(ctx, core_buffer_string(&text));
    core_buffer_release(&text);
    core_dict_put(&summaries->names, (void *)target, name);
    return *name;
}
uint32_t ir_summary_input_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->inputs.len : 0;
}
const ir_sinput_t *ir_summary_input_at(const ir_summary_t *s, uint32_t i)
{
    return s && i < s->inputs.len ? &s->inputs.data[i] : nullptr;
}
uint32_t ir_summary_output_count(const ir_summary_t *s)
{
    return s ? (uint32_t)s->outputs.len : 0;
}
const ir_soutput_t *ir_summary_output_at(const ir_summary_t *s, uint32_t i)
{
    return s && i < s->outputs.len ? &s->outputs.data[i] : nullptr;
}
const uint32_t *ir_summary_dependences(const ir_summary_t *s)
{
    return s ? s->dependences.data : nullptr;
}

ir_summary_t *
ir_summary_of(ir_summaries_t *summaries, const ir_func_t *function)
{
    bool found = false;
    void *summary = summaries
        ? core_dict_get(&summaries->by_function, (void *)function, &found)
        : nullptr;
    return found ? summary : nullptr;
}

static bool
portable_place(const ir_func_t *f, const ir_place_t *place, ir_splace_t *out)
{
    switch (place->kind) {
    case IR_PLACE_LOCAL:
        return false;
    case IR_PLACE_PARAM:
        for (uint32_t i = 0; i < f->n_params; i++)
            if (f->params[i] == place->root) {
                *out = (ir_splace_t){.kind = IR_SPLACE_PARAM, .param = i,
                                     .type = place->type};
                return true;
            }
        *out = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
        return true;
    case IR_PLACE_GLOBAL:
    case IR_PLACE_THREAD: {
        const ir_global_t *global = place->root && place->root->global
            ? ir_global_identity(place->root->global) : nullptr;
        *out = (ir_splace_t){
            .kind = place->kind == IR_PLACE_THREAD ? IR_SPLACE_THREAD
                                                    : IR_SPLACE_GLOBAL,
            .global = global, .type = place->type};
        return global != nullptr;
    }
    case IR_PLACE_HEAP:
        *out = (ir_splace_t){.kind = IR_SPLACE_HEAP, .type = place->type};
        return true;
    case IR_PLACE_UNKNOWN:
        *out = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
        return true;
    }
    return false;
}

static bool
addressable(const ir_func_t *f, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    if (!type) return false;
    if (th_kind_of(type) == TH_INDIRECTION || th_kind_of(type) == TH_SEQUENCE)
        return true;
    if (th_kind_of(type) == TH_NAMED && f && f->owner) {
        th_type *definition = ir_type_definition(f->owner, type);
        definition = definition ? th_resolve(definition) : nullptr;
        return definition && (th_kind_of(definition) == TH_INDIRECTION
                              || th_kind_of(definition) == TH_SEQUENCE);
    }
    return th_kind_of(type) == TH_VAR;
}

static bool
argument_places(ir_places_t *places, const ir_func_t *f, const ir_value_t *arg,
                const uint32_t **ids, uint32_t *count)
{
    *ids   = nullptr;
    *count = 0;
    return arg && addressable(f, arg) && ir_places_of(places, arg, ids, count);
}

typedef struct {
    ir_summaries_t                *summaries;
    core_list_t(summary_func_ptr_t) targets;
    bool                           unknown;
} call_targets_t;

static void
collect_target(void *sink, ir_func_t *target)
{
    call_targets_t *targets = sink;
    core_list_push(targets->targets, target);
}

static bool
resolve_targets(ir_summaries_t *summaries, const ir_instr_t *call,
                call_targets_t *out)
{
    out->summaries = summaries;
    out->targets   = core_list_new(summary_func_ptr_t);
    out->unknown   = false;
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->call_targets
        && hooks->call_targets(hooks->context, call, collect_target, out))
        return true;
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands
        ? ir_operand_value(&call->operands[0]) : nullptr;
    if (callee && callee->kind == IR_VAL_FUNC && callee->owner) {
        core_list_push(out->targets, callee->owner);
        return true;
    }
    out->unknown = true;
    return true;
}

static void
adjust_flags(ir_summaries_t *summaries, ir_summary_t *summary)
{
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->adjust)
        summary->flags = hooks->adjust(hooks->context, summary->function,
                                       summary->flags);
}

static ir_summary_t *
target_summary(ir_summaries_t *summaries, ir_func_t *target)
{
    ir_summary_t *summary = ir_summary_of(summaries, target);
    if (summary) return summary;
    const ir_summary_hooks_t *hooks = summaries->hooks;
    if (hooks && hooks->axiom) summary = hooks->axiom(hooks->context,
                                                      summaries, target);
    if (!summary) summary = ir_summary_conservative(summaries, target);
    adjust_flags(summaries, summary);
    core_dict_put(&summaries->by_function, target, summary);
    return summary;
}

ir_summary_t *
ir_summary_for(ir_summaries_t *summaries, ir_func_t *function)
{
    return function ? target_summary(summaries, function)
                    : ir_summary_conservative(summaries, nullptr);
}

static bool is_call(const ir_instr_t *in);
static th_type *record_of(const ir_func_t *f, th_type *type);
static bool returns_value(const ir_func_t *f);

static const ir_value_t *
referenced(const ir_instr_t *in, uint32_t operand)
{
    return operand < in->n_operands
        ? ir_operand_referenced_value(&in->operands[operand]) : nullptr;
}

static bool
holds_pointers(const ir_func_t *f, th_type *type, uint32_t depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 8) return true;
    switch (ir_machine_of(f->owner, type).class) {
    case IR_CLASS_SINT:
    case IR_CLASS_UINT:
    case IR_CLASS_FLOAT:
    case IR_CLASS_BOOL:
    case IR_CLASS_VOID:
        return false;
    default:
        break;
    }
    uint64_t extent = 0;
    switch (th_kind_of(type)) {
    case TH_BITINT:
        return false;
    case TH_SEQUENCE:
        return !th_seq_extent(type, &extent)
            || holds_pointers(f, th_seq_elem(type), depth + 1);
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || holds_pointers(f, member->type, depth + 1))
                return true;
        }
        return false;
    case TH_SUM:
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (holds_pointers(f, th_sum_arm(type, i), depth + 1))
                return true;
        return false;
    case TH_NAMED: {
        th_type *definition = ir_type_definition(f->owner, type);
        return !definition || definition == type
            || holds_pointers(f, definition, depth + 1);
    }
    default:
        return true;
    }
}

static bool
inline_member(const ir_func_t *f, th_type *record, core_string_t name,
              bool without_layout)
{
    bool known  = false;
    bool inside = ir_layout_member_inline(f->owner, record, name, &known);
    return known ? inside : without_layout;
}

static bool
inline_field(const ir_func_t *f, const ir_value_t *base, bool without_layout)
{
    const ir_instr_t *field = base && base->kind == IR_VAL_INSTR
        ? base->def : nullptr;
    if (!field || field->op != IR_OP_FIELD || field->n_operands != 2
        || field->operands[0].kind != IR_OPD_VALUE
        || field->operands[1].kind != IR_OPD_NAME
        || !field->operands[0].value) return false;
    return inline_member(f, th_ind_referent(field->operands[0].value->type),
                         field->operands[1].name, without_layout);
}

static void
add_unknown_path(ir_summary_t *summary, uint8_t flags)
{
    ir_spath_t path = {.root = IR_SPLACE_UNKNOWN,
                       .flags = flags | IR_SPATH_DEEP};
    add_path(summary, &path);
}

static void
add_function_path(ir_summary_t *summary, const ir_func_t *f,
                  ir_spath_t *path)
{
    th_type *type = path->root == IR_SPLACE_PARAM && path->param < f->n_params
            && f->params[path->param]->type
        ? th_resolve(f->params[path->param]->type) : nullptr;
    th_kind kind = type ? th_kind_of(type) : TH_NAMED;
    if (kind == TH_VAR || kind == TH_BOUND || kind == TH_BINDER
        || kind == TH_TRAIT) {
        path->depth  = 0;
        path->flags |= IR_SPATH_DEEP;
    }
    add_path(summary, path);
}

uint64_t
ir_summary_effect_bits(const ir_summary_t *summary)
{
    uint64_t bits = summary->flags & IR_SEFFECT_FLAG_MASK;
    bits |= (uint64_t)summary->traps << IR_SEFFECT_TRAPS_SHIFT;
    for (size_t i = 0; i < summary->paths.len; i++) {
        const ir_spath_t *path = &summary->paths.data[i];
        bool read  = path->flags & IR_SPATH_READ;
        bool write = path->flags & IR_SPATH_WRITE;
        if (path->root == IR_SPLACE_HEAP) continue;
        if (path->root != IR_SPLACE_PARAM) {
            if (read) bits |= IR_SEFFECT_READS_NONLOCAL;
            if (write) bits |= IR_SEFFECT_WRITES_NONLOCAL;
            continue;
        }
        uint32_t param = path->param < 16 ? path->param : 15;
        if (read) bits |= UINT64_C(1) << (IR_SEFFECT_READS_PARAM_SHIFT + param);
        if (write)
            bits |= UINT64_C(1) << (IR_SEFFECT_WRITES_PARAM_SHIFT + param);
    }
    return bits;
}

uint8_t
ir_summary_call_escape(ir_summaries_t *summaries, const ir_instr_t *call,
                       uint32_t param)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    uint8_t flags = targets.unknown
        ? IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT : 0;
    for (size_t t = 0; !targets.unknown && t < targets.targets.len; t++)
        flags |= ir_summary_param_escape(
            target_summary(summaries, targets.targets.data[t]), param);
    core_list_free(targets.targets);
    return flags;
}

bool
ir_summary_call_diverges(ir_summaries_t *summaries, const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    bool diverges = !targets.unknown && targets.targets.len;
    for (size_t t = 0; diverges && t < targets.targets.len; t++)
        diverges = (target_summary(summaries, targets.targets.data[t])->flags
                    & IR_SUMMARY_DIVERGES) != 0;
    core_list_free(targets.targets);
    return diverges;
}

const ir_summary_t *
ir_summary_entries(ir_summaries_t *summaries, uint32_t flag)
{
    const ir_summary_hooks_t *hooks = summaries ? summaries->hooks : nullptr;
    return hooks && hooks->entries
        ? hooks->entries(hooks->context, summaries, flag) : nullptr;
}

static bool
unseen_reaches(ir_summaries_t *summaries, const ir_splace_t *place)
{
    if (place->kind != IR_SPLACE_GLOBAL && place->kind != IR_SPLACE_THREAD)
        return true;
    const ir_global_t *global = place->global
        ? ir_global_identity(place->global) : nullptr;
    const ir_summary_hooks_t *hooks = summaries ? summaries->hooks : nullptr;
    return !global || !hooks || !hooks->global_reachable
        || hooks->global_reachable(hooks->context, global);
}

static bool
access_touches(ir_summaries_t *summaries, const ir_splace_t *access,
               const ir_splace_t *place)
{
    return access->kind == IR_SPLACE_UNKNOWN
        ? unseen_reaches(summaries, place)
        : ir_splace_eq(access, place);
}

bool
ir_summary_place_unseen(ir_summaries_t *summaries, const ir_splace_t *place)
{
    return unseen_reaches(summaries, place);
}

bool
ir_summary_call_callees(ir_summaries_t *summaries, const ir_instr_t *call,
                        ir_summary_callee_fn visit, void *context)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    for (size_t t = 0; t < targets.targets.len; t++)
        visit(context, targets.targets.data[t],
              target_summary(summaries, targets.targets.data[t]));
    core_list_free(targets.targets);
    return !targets.unknown;
}

static void
check_pure_callee(void *context, const ir_func_t *target,
                  const ir_summary_t *summary)
{
    (void)target;
    const uint32_t effects = IR_SUMMARY_MAY_FAIL | IR_SUMMARY_MAY_BLOCK
        | IR_SUMMARY_DIVERGES | IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_IO
        | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS
        | IR_SUMMARY_OBSERVES;
    bool *pure = context;
    *pure = *pure && !(summary->flags & effects);
    for (size_t i = 0; *pure && i < summary->paths.len; i++)
        *pure = summary->paths.data[i].root == IR_SPLACE_HEAP
            || !(summary->paths.data[i].flags & IR_SPATH_WRITE);
}

bool
ir_summary_call_pure(ir_summaries_t *summaries, const ir_instr_t *call)
{
    bool pure = summaries != nullptr;
    return pure
        && ir_summary_call_callees(summaries, call, check_pure_callee, &pure)
        && pure;
}

static bool
place_meets(ir_module_t *module, const ir_place_t *place,
            const ir_splace_t *other)
{
    if (place->kind == IR_PLACE_LOCAL)
        return place->exposed && other->kind == IR_SPLACE_UNKNOWN;
    if (place->kind == IR_PLACE_UNKNOWN || other->kind == IR_SPLACE_UNKNOWN)
        return true;
    bool named = place->kind == IR_PLACE_GLOBAL
        || place->kind == IR_PLACE_THREAD;
    bool other_named = other->kind == IR_SPLACE_GLOBAL
        || other->kind == IR_SPLACE_THREAD;
    if (named && other_named) {
        const ir_global_t *global = place->root && place->root->global
            ? ir_global_identity(place->root->global) : nullptr;
        return !global || !other->global
            || global == ir_global_identity(other->global);
    }
    if (place->kind == IR_PLACE_HEAP && place->root
        && other->kind != IR_SPLACE_HEAP) return false;
    return !place->type || !other->type
        || ir_places_types_overlap(module, place->type, other->type);
}

typedef struct {
    ir_summaries_t   *summaries;
    const ir_func_t  *f;
    ir_places_t      *places;
    const ir_instr_t *call;
    const uint32_t   *ids;
    uint32_t          count;
} write_query_t;

static bool
access_writes(write_query_t *q, const ir_splace_t *place, bool arguments)
{
    if (place->kind == IR_SPLACE_PARAM && arguments) {
        uint32_t operand = place->param + 1;
        const ir_value_t *arg = operand < q->call->n_operands
            ? ir_operand_value(&q->call->operands[operand]) : nullptr;
        if (!arg) return true;
        if (!addressable(q->f, arg)) return false;
        const uint32_t *written = nullptr;
        uint32_t        n       = 0;
        if (!argument_places(q->places, q->f, arg, &written, &n)) return true;
        for (uint32_t w = 0; w < n; w++)
            for (uint32_t i = 0; i < q->count; i++)
                if (ir_places_may_alias(q->places, written[w], q->ids[i]))
                    return true;
        return false;
    }
    ir_splace_t other = place->kind == IR_SPLACE_PARAM
        ? (ir_splace_t){.kind = IR_SPLACE_UNKNOWN} : *place;
    for (uint32_t i = 0; i < q->count; i++)
        if (place_meets(q->f->owner, ir_place_at(q->places, q->ids[i]),
                        &other))
            return true;
    return false;
}

static bool
summary_writes(write_query_t *q, const ir_summary_t *summary, bool follow)
{
    if (!summary) return true;
    for (size_t i = 0; i < summary->paths.len; i++) {
        const ir_spath_t *path = &summary->paths.data[i];
        ir_splace_t       places[2];
        uint32_t          count = path->flags & IR_SPATH_WRITE
            ? ir_summary_path_places(summary->function, path, places) : 0;
        for (uint32_t p = 0; p < count; p++)
            if (access_writes(q, &places[p], follow)) return true;
    }
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; follow && e < sizeof(entries) / sizeof(*entries); e++)
        if ((summary->flags & entries[e])
            && summary_writes(q, ir_summary_entries(q->summaries, entries[e]),
                              false))
            return true;
    return false;
}

bool
ir_summary_call_writes(ir_summaries_t *summaries, const ir_func_t *f,
                       ir_places_t *places, const ir_instr_t *call,
                       uint32_t place)
{
    if (!summaries || !f || !places || !call) return true;
    if (call->op != IR_OP_CALL && call->op != IR_OP_CALLBACK_INVOKE)
        return true;
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    write_query_t q = {
        .summaries = summaries, .f = f, .places = places, .call = call,
        .ids = &place, .count = 1,
    };
    bool writes = targets.unknown;
    for (size_t t = 0; !writes && t < targets.targets.len; t++)
        writes = summary_writes(&q, target_summary(summaries,
                                                   targets.targets.data[t]),
                                true);
    core_list_free(targets.targets);
    return writes;
}

bool
ir_summary_argument_addressable(const ir_func_t *function,
                                const ir_value_t *argument)
{
    return addressable(function, argument);
}

bool
ir_summary_place_of(const ir_func_t *function, const ir_place_t *place,
                    ir_splace_t *out)
{
    return portable_place(function, place, out);
}

static bool
returns_parameter_address(const ir_summary_t *summary, uint32_t param)
{
    if (!summary || !summary->results_known) return true;
    for (size_t i = 0; i < summary->results.len; i++) {
        const ir_sstore_t *result = &summary->results.data[i];
        if (result->to.output) continue;
        if (result->from.root == IR_SPLACE_UNKNOWN
            || escape_from(&result->from, param, true)
                & IR_SESCAPE_ADDRESS)
            return true;
    }
    return false;
}

static bool
call_returns_parameter(ir_summaries_t *summaries, const ir_instr_t *call,
                       uint32_t param)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    bool returns = targets.unknown;
    for (size_t t = 0; !returns && t < targets.targets.len; t++)
        returns = returns_parameter_address(
            target_summary(summaries, targets.targets.data[t]), param);
    core_list_free(targets.targets);
    return returns;
}

typedef struct {
    const ir_value_t *value;
    bool              storage;
} escape_value_t;
core_list_decl(escape_value_t);

typedef struct {
    ir_summaries_t              *summaries;
    const ir_func_t             *function;
    bool                         returns_escape;
    bool                         returned;
    bool                         referent;
    uint32_t                     param;
    core_list_t(escape_value_t)  values;
} escape_walk_t;

static bool holds_reference(const ir_func_t *f, const ir_value_t *value);

static bool
tracked(const escape_walk_t *walk, const ir_value_t *value, bool storage)
{
    for (size_t i = 0; i < walk->values.len; i++)
        if (walk->values.data[i].value == value
            && walk->values.data[i].storage == storage) return true;
    return false;
}

static bool
derived(const escape_walk_t *walk, const ir_value_t *value)
{
    return tracked(walk, value, false);
}

static void
track(escape_walk_t *walk, const ir_value_t *value, bool storage)
{
    if (!value || tracked(walk, value, storage)) return;
    if (!storage && !holds_reference(walk->function, value)) return;
    core_list_push(walk->values,
                   ((escape_value_t){.value = value, .storage = storage}));
}

static void
derive(escape_walk_t *walk, const ir_value_t *value)
{
    track(walk, value, false);
}

static bool
inline_elements(const ir_func_t *f, const ir_value_t *base)
{
    return inline_field(f, base, true);
}

static bool
positional_argument(const ir_instr_t *call, uint32_t index)
{
    return index && ir_call_arguments_positional(call, index - 1);
}

static bool
returns_value(const ir_func_t *f)
{
    return ir_callable_returns_value(f->owner, f->type);
}

static bool
return_operand_escapes(escape_walk_t *walk, uint32_t index)
{
    const ir_func_t *f = walk->function;
    uint32_t first_output = returns_value(f) ? 1 : 0;
    if (index < first_output) {
        walk->returned = true;
        return walk->returns_escape;
    }
    uint32_t output = first_output;
    for (size_t i = 0; i < th_func_nparams(f->type); i++) {
        const th_param *formal = th_func_param(f->type, i);
        if (!formal || !th_mode_produces_output(formal->mode)) continue;
        if (output++ == index)
            return walk->returns_escape || i != walk->param;
    }
    return true;
}

static bool
hold_in_local(escape_walk_t *walk, const ir_value_t *target)
{
    for (unsigned depth = 0; target && depth < 32; depth++) {
        const ir_instr_t *def = target->kind == IR_VAL_INSTR ? target->def
                                                             : nullptr;
        if (!def) return false;
        switch (def->op) {
        case IR_OP_FIELD:
        case IR_OP_INDEX:
        case IR_OP_OFFSET:
        case IR_OP_BITCAST:
        case IR_OP_COPY:
            target = referenced(def, 0);
            continue;
        case IR_OP_ADDRESS_OF: {
            const ir_value_t *local = referenced(def, 0);
            if (!local || (local->kind != IR_VAL_LOCAL
                           && local->kind != IR_VAL_PARAM)) return false;
            if (!derived(walk, local))
                core_list_push(walk->values,
                    ((escape_value_t){.value = local, .storage = false}));
            return true;
        }
        case IR_OP_ALLOCA:
        case IR_OP_ALLOC:
            track(walk, target, true);
            return true;
        default:
            return false;
        }
    }
    return false;
}


static bool
use_escapes(escape_walk_t *walk, const ir_instr_t *use, uint32_t index)
{
    const ir_operand_t *operand = &use->operands[index];
    switch (use->op) {
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_LENGTH:
    case IR_OP_SEQUENCE_SHAPE_CHECK:
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_LT:
    case IR_OP_LE:
    case IR_OP_GT:
    case IR_OP_GE:
    case IR_OP_TYPE_OF:
    case IR_OP_TYPE_TEST:
    case IR_OP_OPTION_IS_NIL:
    case IR_OP_VARIANT_TAG:
    case IR_OP_PREFETCH:
    case IR_OP_MEMCMP:
    case IR_OP_BRANCH:
    case IR_OP_SWITCH:
        return false;
    case IR_OP_STORE:
        return index != 0 && !hold_in_local(walk, referenced(use, 0));
    case IR_OP_SEQUENCE_SET:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
        return index != 0;
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
        if (index == 1) walk->referent = true;
        return index > 1;
    case IR_OP_MEMSET:
        return index > 1;
    case IR_OP_INDEX:
        if (index == 0 && use->dest && operand->value
            && th_kind_of(th_resolve(operand->value->type)) == TH_INDIRECTION
            && th_kind_of(th_resolve(th_ind_referent(operand->value->type)))
                == TH_SEQUENCE
            && !inline_elements(walk->function, operand->value)) {
            if (ir_address_escapes(walk->summaries, walk->function,
                                   use->dest))
                walk->referent = true;
            return false;
        }
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_FIELD:
    case IR_OP_OFFSET:
    case IR_OP_BITCAST:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_PHI:
    case IR_OP_SELECT:
    case IR_OP_EXTRACT:
    case IR_OP_EXTRACT_INDEX:
    case IR_OP_INSERT:
    case IR_OP_AGGREGATE:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_OPTION_GET_OR_ELSE:
    case IR_OP_VARIANT_MAKE:
    case IR_OP_VARIANT_PAYLOAD:
    case IR_OP_TYPE_NARROW:
    case IR_OP_BOX:
    case IR_OP_CONSUME:
        if (use->op == IR_OP_SELECT && index == 0) return false;
        if (use->op == IR_OP_CONSUME && index != 0) return false;
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_COPY:
        if (use->dest_local) {
            if (!use->dest_local->value) return true;
            derive(walk, use->dest_local->value);
            return false;
        }
        if (!use->dest) return true;
        derive(walk, use->dest);
        return false;
    case IR_OP_ADDRESS_OF:
        if (!use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_PARAM_PRESENT:
    case IR_OP_ENUM_VALUE:
        return false;
    case IR_OP_CAST:
        return !operand->value || ir_machine_of(walk->function->owner,
            operand->value->type).class == IR_CLASS_PTR;
    case IR_OP_CALL:
        if (!positional_argument(use, index)) return true;
        uint8_t escape = ir_summary_call_escape(walk->summaries, use,
                                                index - 1);
        if (escape & IR_SESCAPE_ADDRESS) return true;
        if (escape & IR_SESCAPE_REFERENT) walk->referent = true;
        if (use->dest
            && call_returns_parameter(walk->summaries, use, index - 1))
            derive(walk, use->dest);
        return false;
    case IR_OP_RETURN:
        return return_operand_escapes(walk, index);
    default:
        return true;
    }
}

static bool
storage_use_escapes(escape_walk_t *walk, const ir_instr_t *use,
                    uint32_t index)
{
    switch (use->op) {
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET:
        if (use->dest) derive(walk, use->dest);
        return false;
    case IR_OP_STORE:
    case IR_OP_SEQUENCE_SET:
    case IR_OP_MEMSET:
        return index != 0;
    case IR_OP_SEQUENCE_LENGTH:
    case IR_OP_SEQUENCE_SHAPE_CHECK:
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_PREFETCH:
    case IR_OP_MEMCMP:
        return false;
    case IR_OP_CONSUME:
        return index == 0;
    case IR_OP_FIELD:
    case IR_OP_INDEX:
    case IR_OP_OFFSET:
    case IR_OP_BITCAST:
    case IR_OP_PHI:
    case IR_OP_SELECT:
        if (use->op == IR_OP_SELECT && index == 0) return false;
        if (!use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_COPY:
        if (use->dest_local || !use->dest) return true;
        track(walk, use->dest, true);
        return false;
    case IR_OP_CALL:
        if (!positional_argument(use, index)) return true;
        if (ir_summary_call_escape(walk->summaries, use, index - 1))
            return true;
        if (use->dest
            && call_returns_parameter(walk->summaries, use, index - 1))
            track(walk, use->dest, true);
        return false;
    default:
        return true;
    }
}

static bool
walk_escapes(escape_walk_t *walk, const ir_value_t *start)
{
    derive(walk, start);
    const ir_func_t *f = walk->function;
    for (size_t cursor = 0; cursor < walk->values.len; cursor++) {
        const ir_value_t *value = walk->values.data[cursor].value;
        bool storage = walk->values.data[cursor].storage;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            const ir_block_t *block = f->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                const ir_instr_t *use = block->instrs[i];
                for (uint32_t k = 0; k < use->n_operands; k++)
                    if (referenced(use, k) == value
                        && (storage ? storage_use_escapes(walk, use, k)
                                    : use_escapes(walk, use, k)))
                        return true;
            }
        }
    }
    return false;
}

bool
ir_address_escapes(ir_summaries_t *summaries, const ir_func_t *function,
                   const ir_value_t *address)
{
    escape_walk_t walk = {
        .summaries = summaries, .function = function, .returns_escape = true,
        .param = UINT32_MAX, .values = core_list_new(escape_value_t),
    };
    bool escapes = walk_escapes(&walk, address);
    core_list_free(walk.values);
    return escapes;
}

static bool
holds_reference(const ir_func_t *f, const ir_value_t *value)
{
    if (!value || !value->type) return true;
    switch (ir_machine_of(f->owner, value->type).class) {
    case IR_CLASS_SINT:
    case IR_CLASS_UINT:
    case IR_CLASS_FLOAT:
    case IR_CLASS_BOOL:
    case IR_CLASS_VOID:
        return false;
    default:
        return true;
    }
}

void
ir_summary_set_param_escape(ir_summary_t *summary, uint32_t param,
                            uint8_t flags)
{
    const ir_spath_t anywhere = {.root  = IR_SPLACE_UNKNOWN,
                                 .flags = IR_SPATH_DEEP};
    ir_spath_t from = {
        .root  = IR_SPLACE_PARAM, .param = param,
        .flags = (flags & IR_SESCAPE_ADDRESS ? IR_SPATH_ROOT : 0)
            | (flags & IR_SESCAPE_REFERENT ? IR_SPATH_DEEP : 0)};
    if (from.flags) add_pair(&summary->stores, &anywhere, &from);
    summary->results_known = true;
}

void
ir_summary_set_return(ir_summary_t *summary, ir_sreturn_t returned)
{
    ir_spath_t from = {.root = IR_SPLACE_UNKNOWN, .flags = IR_SPATH_DEEP};
    switch (returned.kind) {
    case IR_SRETURN_NONE:
        summary->results_known = true;
        return;
    case IR_SRETURN_PARAM:
        from = (ir_spath_t){.root = IR_SPLACE_PARAM, .param = returned.param,
                            .flags = IR_SPATH_ROOT};
        break;
    case IR_SRETURN_GLOBAL:
        from = ir_spath_of_place(&(ir_splace_t){
            .kind = IR_SPLACE_GLOBAL, .global = returned.global}, 0);
        break;
    case IR_SRETURN_FRESH_EXTERNAL:
        summary->returns_external = true;
        from = (ir_spath_t){.root = IR_SPLACE_HEAP};
        break;
    case IR_SRETURN_FRESH:
        from = (ir_spath_t){.root = IR_SPLACE_HEAP};
        break;
    case IR_SRETURN_UNKNOWN:
        break;
    }
    add_pair(&summary->results, &(ir_spath_t){0}, &from);
    summary->results_known = true;
}


static bool
is_call(const ir_instr_t *in)
{
    return in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE;
}

enum { FIELD_LEAVES = 64 };

static th_type *
definition_in(ir_module_t *module, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (type && th_kind_of(type) == TH_NAMED) {
        th_type *definition = module ? ir_type_definition(module, type)
                                     : nullptr;
        type = definition && definition != type ? th_resolve(definition)
                                                : nullptr;
    }
    return type;
}

static th_type *
defined_type(const ir_func_t *f, th_type *type)
{
    return definition_in(f->owner, type);
}

static th_type *
record_of(const ir_func_t *f, th_type *type)
{
    type = defined_type(f, type);
    return type && th_kind_of(type) == TH_RECORD && th_record_len(type)
        ? type : nullptr;
}

static bool
collect_leaves(const ir_func_t *f, th_type *type, ir_spath_t prefix,
               path_list_t *out)
{
    th_type *record = prefix.depth < IR_SPATH_STEPS ? record_of(f, type)
                                                    : nullptr;
    if (!record) {
        if (out->len >= FIELD_LEAVES) return false;
        core_list_push(*out, prefix);
        return true;
    }
    for (size_t i = 0; i < th_record_len(record); i++) {
        const th_member *member = th_record_member(record, i);
        ir_spath_t       next   = prefix;
        next.steps[next.depth++] = member->name;
        if (!collect_leaves(f, member->type, next, out)) return false;
    }
    return true;
}

static path_list_t
place_leaves(const ir_func_t *f, const ir_splace_t *place)
{
    path_list_t leaves = core_list_new(ir_spath_t);
    if (!collect_leaves(f, place->type, (ir_spath_t){0}, &leaves)) {
        leaves.len = 0;
        core_list_push(leaves, ((ir_spath_t){0}));
    }
    return leaves;
}

static const ir_value_t *
field_chain(const ir_value_t *address, ir_op_t step, ir_spath_t *path)
{
    core_string_t names[IR_SPATH_STEPS];
    uint8_t       depth = 0;
    while (address && address->kind == IR_VAL_INSTR && address->def
           && address->def->op == step && address->def->n_operands == 2
           && address->def->operands[1].kind == IR_OPD_NAME) {
        if (depth + path->depth == IR_SPATH_STEPS) return nullptr;
        names[depth++] = address->def->operands[1].name;
        address = ir_operand_value(&address->def->operands[0]);
    }
    for (uint8_t i = path->depth; i > 0; i--)
        path->steps[i - 1 + depth] = path->steps[i - 1];
    for (uint8_t i = 0; i < depth; i++)
        path->steps[i] = names[depth - 1 - i];
    path->depth += depth;
    return address;
}

typedef struct {
    ir_summaries_t    *summaries;
    ir_func_t         *f;
    ir_places_t       *places;
    const ir_splace_t *target;
    const ir_spath_t  *leaf;
    core_dict_t       *path_places;
} flow_query_t;

typedef struct {
    uint8_t     *counts;
    ir_splace_t *places;
} path_places_t;

static uint32_t
query_path_places(flow_query_t *q, const ir_summary_t *s, size_t index,
                  ir_splace_t places[2])
{
    if (!q->path_places)
        return ir_summary_path_places(s->function, &s->paths.data[index],
                                      places);
    bool           found  = false;
    path_places_t *cached = core_dict_get(q->path_places, (void *)s, &found);
    if (!found) {
        size_t count   = s->paths.len;
        cached         = core_alloc(path_places_t);
        cached->counts = core_alloc_array(uint8_t, count ? count : 1);
        cached->places = core_alloc_array(ir_splace_t, 2 * (count ? count
                                                                  : 1));
        for (size_t i = 0; i < count; i++)
            cached->counts[i] = (uint8_t)ir_summary_path_places(s->function,
                &s->paths.data[i], &cached->places[2 * i]);
        core_dict_put(q->path_places, (void *)s, cached);
    }
    places[0] = cached->places[2 * index];
    places[1] = cached->places[2 * index + 1];
    return cached->counts[index];
}

static void
path_places_free(core_dict_t *cache)
{
    core_dict_foreach(cache, item) {
        path_places_t *cached = item->value;
        core_free(cached->counts);
        core_free(cached->places);
        core_free(cached);
    }
    core_dict_free(cache);
}

static bool
argument_is(flow_query_t *q, const ir_instr_t *call, uint32_t param,
            const ir_splace_t *want)
{
    uint32_t operand = param + 1;
    if (operand >= call->n_operands) return false;
    const ir_value_t *arg = ir_operand_value(&call->operands[operand]);
    const uint32_t   *ids = nullptr;
    uint32_t          n   = 0;
    if (!argument_places(q->places, q->f, arg, &ids, &n) || n != 1)
        return false;
    ir_splace_t mapped;
    return portable_place(q->f, ir_place_at(q->places, ids[0]), &mapped)
        && mapped.kind != IR_SPLACE_HEAP && mapped.kind != IR_SPLACE_UNKNOWN
        && ir_splace_eq(&mapped, want);
}

static bool
named_place(const ir_splace_t *place)
{
    return place->kind != IR_SPLACE_HEAP && place->kind != IR_SPLACE_UNKNOWN;
}

static uint8_t
target_access_flags(flow_query_t *q, const ir_instr_t *call,
                    const ir_summary_t *s)
{
    uint8_t flags = 0;
    for (size_t i = 0; q->target && i < s->paths.len; i++) {
        const ir_spath_t *path = &s->paths.data[i];
        ir_splace_t       places[2];
        uint32_t          count = query_path_places(q, s, i, places);
        for (uint32_t p = 0; p < count; p++) {
            const ir_splace_t *place = &places[p];
            bool matches = place->kind == IR_SPLACE_PARAM
                ? argument_is(q, call, place->param, q->target)
                : place->kind == IR_SPLACE_UNKNOWN
                ? unseen_reaches(q->summaries, q->target)
                : named_place(place) && ir_splace_eq(place, q->target);
            if (matches)
                flags |= named_place(place) ? path->flags
                                            : path->flags & IR_SPATH_ACCESS;
        }
    }
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; q->target && e < sizeof(entries) / sizeof(*entries);
         e++) {
        if (!(s->flags & entries[e])) continue;
        const ir_summary_t *entry = ir_summary_entries(q->summaries,
                                                       entries[e]);
        if (!entry) {
            if (unseen_reaches(q->summaries, q->target))
                flags |= IR_SPATH_ACCESS;
            continue;
        }
        for (size_t i = 0; i < entry->paths.len; i++) {
            const ir_spath_t *path = &entry->paths.data[i];
            ir_splace_t       places[2];
            uint32_t          count = path->root == IR_SPLACE_PARAM ? 0
                : query_path_places(q, entry, i, places);
            for (uint32_t p = 0; p < count; p++)
                if (access_touches(q->summaries, &places[p], q->target))
                    flags |= path->flags & IR_SPATH_ACCESS;
        }
    }
    return flags;
}

static uint8_t
written(uint8_t flags, uint8_t must)
{
    if (flags & must) return IR_PATH_PASSED;
    if (flags & IR_SPATH_WRITE) return IR_PATH_UNPASSED | IR_PATH_PASSED;
    return IR_PATH_UNPASSED;
}

static uint8_t
callee_effect(const ir_summary_t *s, uint8_t flags, ir_path_outcome_t outcome)
{
    bool diverges = (s->flags & IR_SUMMARY_DIVERGES) != 0;
    uint8_t success = diverges || (s->flags & IR_SUMMARY_NO_RETURN)
        ? 0 : written(flags, IR_SPATH_MUST_SUCCESS);
    uint8_t failure = !diverges && (s->flags & IR_SUMMARY_MAY_FAIL)
        ? written(flags, IR_SPATH_MUST_FAILURE) : 0;
    switch (outcome) {
    case IR_PATH_SUCCESS:
        return success;
    case IR_PATH_FAILURE:
        return failure;
    case IR_PATH_ANY:
        break;
    }
    return success | failure;
}

static uint8_t
call_effect(flow_query_t *q, const ir_instr_t *call, ir_path_outcome_t outcome)
{
    call_targets_t targets;
    resolve_targets(q->summaries, call, &targets);
    uint8_t effect = targets.unknown || !targets.targets.len
        ? IR_PATH_UNPASSED : 0;
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *s = target_summary(q->summaries, targets.targets.data[t]);
        effect |= callee_effect(s, target_access_flags(q, call, s), outcome);
    }
    core_list_free(targets.targets);
    return effect;
}

static bool
stores_target(flow_query_t *q, const ir_instr_t *in)
{
    uint32_t first = 0;
    uint32_t count = q->target ? ir_places_accesses_of(q->places, in, &first)
                               : 0;
    for (uint32_t a = first; a < first + count; a++) {
        const ir_access_t *access = ir_places_access_at(q->places, a);
        if (access->kind != IR_ACCESS_STORE
            || !access->whole || access->count != 1) continue;
        ir_splace_t mapped;
        if (portable_place(q->f, ir_place_at(q->places,
                ir_places_access_places(q->places, access)[0]), &mapped)
            && named_place(&mapped) && ir_splace_eq(&mapped, q->target))
            return true;
    }
    return false;
}

static bool
field_store_covers(flow_query_t *q, const ir_instr_t *in)
{
    if (!q->target || !q->leaf || !q->leaf->depth || in->op != IR_OP_STORE
        || !in->n_operands) return false;
    ir_spath_t        path    = {0};
    const ir_value_t *address = field_chain(
        ir_operand_value(&in->operands[0]), IR_OP_FIELD, &path);
    if (!path.depth || !steps_prefix(path.depth, path.steps, q->leaf->depth,
                                     q->leaf->steps)) return false;
    const uint32_t *ids = nullptr;
    uint32_t        n   = 0;
    ir_splace_t     mapped;
    return address && ir_places_of(q->places, address, &ids, &n) && n == 1
        && portable_place(q->f, ir_place_at(q->places, ids[0]), &mapped)
        && named_place(&mapped) && ir_splace_eq(&mapped, q->target);
}

static bool
writes_target(flow_query_t *q, const ir_instr_t *in)
{
    return stores_target(q, in) || field_store_covers(q, in);
}

static uint8_t
flow_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    flow_query_t *q = context;
    if (is_call(in)) return call_effect(q, in, outcome);
    return writes_target(q, in) ? IR_PATH_PASSED : IR_PATH_UNPASSED;
}

static ir_spath_t
read_path(const ir_splace_t *place, const ir_instr_t *in)
{
    ir_spath_t path = {0};
    if ((in->op != IR_OP_LOAD && in->op != IR_OP_EXTRACT) || !in->n_operands)
        return path;
    if (in->op == IR_OP_EXTRACT) {
        if (in->n_operands != 2 || in->operands[1].kind != IR_OPD_NAME)
            return path;
        path.steps[path.depth++] = in->operands[1].name;
    }
    const ir_value_t *address = field_chain(
        ir_operand_value(&in->operands[0]),
        in->op == IR_OP_LOAD ? IR_OP_FIELD : IR_OP_EXTRACT, &path);
    if (!path.depth || !address || address->kind != IR_VAL_GLOBAL
        || !address->global
        || ir_global_identity(address->global) != place->global)
        return (ir_spath_t){0};
    return path;
}

static uint8_t
combine_leaf(uint8_t combined, uint8_t leaf, bool first)
{
    if (first) return leaf;
    if (!combined || !leaf) return 0;
    uint8_t passed = combined & leaf & IR_PATH_PASSED;
    return ((combined | leaf) & IR_PATH_UNPASSED) | passed;
}

uint8_t
ir_summary_effect(const ir_summary_t *summary, const ir_splace_t *place,
                  ir_path_outcome_t outcome)
{
    if (!summary) return IR_PATH_UNPASSED;
    uint8_t flags = 0;
    for (size_t i = 0; i < summary->paths.len; i++) {
        ir_splace_t places[2];
        uint32_t    count = ir_summary_path_places(summary->function,
                                                   &summary->paths.data[i],
                                                   places);
        for (uint32_t p = 0; p < count; p++)
            if (named_place(&places[p]) && ir_splace_eq(&places[p], place))
                flags |= summary->paths.data[i].flags;
    }
    return callee_effect(summary, flags, outcome);
}

static bool
arm_dominates(ir_doms_t *doms, ir_block_t *arm, ir_block_t *from)
{
    return arm && ir_pred_count(doms, arm) == 1 && ir_dominates(doms, arm, from);
}

static ir_path_outcome_t
call_arm(ir_doms_t *doms, const ir_instr_t *call, ir_block_t *from)
{
    ir_instr_t *terminator      = ir_block_terminator(call->block);
    bool        true_is_success = false;
    if (ir_call_outcome_branch(terminator, &true_is_success) != call)
        return IR_PATH_ANY;
    ir_block_t *on_true  = ir_succ_of(&terminator->operands[1]);
    ir_block_t *on_false = ir_succ_of(&terminator->operands[2]);
    ir_block_t *success  = true_is_success ? on_true : on_false;
    ir_block_t *failure  = true_is_success ? on_false : on_true;
    if (success == failure) return IR_PATH_ANY;
    if (arm_dominates(doms, success, from)) return IR_PATH_SUCCESS;
    if (arm_dominates(doms, failure, from)) return IR_PATH_FAILURE;
    return IR_PATH_ANY;
}

typedef struct {
    uint32_t    def;
    uint8_t     effect;
    ir_block_t *from;
} walk_item_t;
core_list_decl(walk_item_t);

static uint8_t state_before_leaf(flow_query_t *q, ir_memssa_t *memssa,
                                 ir_doms_t *doms, uint8_t entry,
                                 const ir_instr_t *in);

uint8_t
ir_summary_state_before(ir_summaries_t *summaries, ir_func_t *f,
                        const ir_splace_t *place, uint8_t entry,
                        const ir_instr_t *in)
{
    size_t       scope  = ir_analysis_scope_begin();
    ir_memssa_t *memssa = ir_func_memssa(f);
    ir_doms_t   *doms   = ir_func_doms(f);
    core_dict_t  cache;
    core_dict_init(&cache, nullptr, nullptr);
    flow_query_t q      = {.summaries = summaries, .f = f,
                           .places = ir_func_places(f), .target = place,
                           .path_places = &cache};
    uint8_t state = memssa && doms && q.places
        ? state_before_leaf(&q, memssa, doms, entry, in) : 0;
    if (state & IR_PATH_UNPASSED) {
        ir_spath_t  read   = read_path(place, in);
        path_list_t leaves = place_leaves(f, place);
        bool first = true;
        for (size_t l = 0; leaves.data[0].depth && l < leaves.len; l++) {
            if (!paths_overlap(&leaves.data[l], &read)) continue;
            q.leaf = &leaves.data[l];
            state  = combine_leaf(state,
                state_before_leaf(&q, memssa, doms, entry, in), first);
            first  = false;
        }
        core_list_free(leaves);
    }
    path_places_free(&cache);
    ir_analysis_scope_end(scope);
    return state;
}

static uint8_t
state_before_leaf(flow_query_t *q, ir_memssa_t *memssa, ir_doms_t *doms,
                  uint8_t entry, const ir_instr_t *in)
{
    uint32_t start = ir_memssa_def_before(memssa, in);
    uint8_t  state = 0;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_t(walk_item_t) work = core_list_new(walk_item_t);
    if (start != UINT32_MAX)
        core_list_push(work, ((walk_item_t){.def = start,
                                            .effect = IR_PATH_UNPASSED,
                                            .from = in->block}));
    while (work.len) {
        walk_item_t item = work.data[--work.len];
        uintptr_t key = ((uintptr_t)item.def << 34)
            | ((uintptr_t)item.from->index << 2) | item.effect;
        if (!core_dict_add(&seen, (void *)(key + 1), (void *)(key + 1)))
            continue;
        const ir_memdef_t *def    = ir_memssa_def(memssa, item.def);
        uint8_t            effect = item.effect;
        switch (def->kind) {
        case IR_MEMDEF_ENTRY:
            state |= ir_path_apply(entry, effect);
            continue;
        case IR_MEMDEF_PHI:
            for (uint32_t i = 0; i < ir_memssa_phi_count(memssa, item.def);
                 i++) {
                ir_block_t *pred = ir_pred_at(doms, def->block, i);
                core_list_push(work, ((walk_item_t){
                    .def    = ir_memssa_phi_incoming(memssa, item.def, i),
                    .effect = effect,
                    .from   = pred ? pred : def->block}));
            }
            continue;
        case IR_MEMDEF_STORE:
            if (writes_target(q, def->instr)) effect = IR_PATH_PASSED;
            break;
        case IR_MEMDEF_CALL:
            if (is_call(def->instr))
                effect = ir_path_apply(call_effect(q, def->instr,
                    call_arm(doms, def->instr, item.from)), effect);
            break;
        case IR_MEMDEF_ELEMENT_PHI:
            continue;
        }
        if (effect == IR_PATH_PASSED) state |= IR_PATH_PASSED;
        else if (effect)
            core_list_push(work, ((walk_item_t){
                .def = ir_memssa_def_prev(memssa, item.def),
                .effect = effect, .from = item.from}));
    }
    core_list_free(work);
    core_dict_free(&seen);
    return state;
}

ir_splace_t
ir_splace_global(const ir_global_t *global)
{
    bool thread = ir_global_storage_duration(global)
        == IR_STORAGE_DURATION_THREAD;
    return (ir_splace_t){
        .kind   = thread ? IR_SPLACE_THREAD : IR_SPLACE_GLOBAL,
        .global = ir_global_identity(global),
        .type   = global->type};
}

static bool
call_unobserved(const ir_instr_t *call)
{
    for (uint32_t a = 0; a < call->annots.n; a++)
        if (call->annots.a[a].kind == IR_ANNOT_BOOL && call->annots.a[a].b
            && core_string_eq(call->annots.a[a].key,
                              CORE_STRING_STATIC("ir.unobserved")))
            return true;
    return false;
}

static void
compose_call(ir_summaries_t *summaries, ir_summary_t *summary,
             const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(summaries, call, &targets);
    uint32_t observes = call_unobserved(call) ? 0 : IR_SUMMARY_OBSERVES;
    if (call->op == IR_OP_CALLBACK_INVOKE)
        add_flags(summary, IR_SUMMARY_CALLS_CALLBACKS, call, nullptr,
                  (core_string_t){0});
    if (targets.unknown) {
        add_flags(summary, IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
                      | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
                      | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
                      | observes,
                  call, nullptr, CORE_STRING_STATIC("?"));
        summary->traps |= IR_STRAP_ALL;
        core_list_free(targets.targets);
        return;
    }
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(summaries,
                                              targets.targets.data[t]);
        summary->traps |= callee->traps;
        uint32_t flags = callee->flags
            & (IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
               | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_CALLS_UNKNOWN
               | IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_COLLECT
               | IR_SUMMARY_ALLOCATES_DIRECT | IR_SUMMARY_CALLS_FINALIZERS
               | IR_SUMMARY_STARTS_THREADS | IR_SUMMARY_REINTERPRETS
               | IR_SUMMARY_CALLS_CALLBACKS | observes);
        if (flags & ~summary->flags)
            add_flags(summary, flags, call, targets.targets.data[t],
                      (core_string_t){0});
        (void)merge_effects(summary, callee);
    }
    core_list_free(targets.targets);
}

static uint32_t
allocation_flags(const ir_func_t *f, const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_ALLOC:
        return ir_allocation_has_only_local_uses(f, in)
            ? 0
            : IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_COLLECT
                | IR_SUMMARY_ALLOCATES_DIRECT;
    case IR_OP_ARRAY_NEW:
        return in->dest && th_is_closed(ir_module_types(f->owner),
                                        in->dest->type)
            ? IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_COLLECT
                | IR_SUMMARY_ALLOCATES_DIRECT
            : IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_ALLOC_USING:
    case IR_OP_ARRAY_NEW_USING:
    case IR_OP_BOX:
        return IR_SUMMARY_MAY_ALLOCATE;
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_BUFFER_BORROW_END:
        return IR_SUMMARY_MAY_COLLECT;
    default:
        return 0;
    }
}


static void summarize_dependences(ir_summaries_t *summaries,
                                  ir_summary_t *summary, ir_func_t *f);

enum {
    UNPASSED_RETURN = 1u << 0,
    UNPASSED_FAIL   = 1u << 1,
};

static uint8_t
exit_unpassed(const ir_path_exits_t *exits)
{
    return ((exits->returned & IR_PATH_UNPASSED) ? UNPASSED_RETURN : 0)
        | ((exits->failed & IR_PATH_UNPASSED) ? UNPASSED_FAIL : 0);
}

static uint8_t
unpassed_exits(flow_query_t *q, ir_func_t *f, const ir_path_exits_t *whole)
{
    uint8_t unpassed = exit_unpassed(whole);
    if (!unpassed) return 0;
    path_list_t leaves = place_leaves(f, q->target);
    if (leaves.data[0].depth) {
        unpassed = 0;
        for (size_t l = 0; l < leaves.len; l++) {
            ir_path_exits_t exits;
            q->leaf = &leaves.data[l];
            unpassed |= ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, q, &exits)
                ? exit_unpassed(&exits) : UNPASSED_RETURN | UNPASSED_FAIL;
        }
        q->leaf = nullptr;
    }
    core_list_free(leaves);
    return unpassed;
}

static void
summarize_function(ir_summaries_t *summaries, ir_summary_t *summary,
                   ir_func_t *f)
{
    summary->flags = ir_reinterpretations_note_function(nullptr, f)
        ? IR_SUMMARY_REINTERPRETS : 0;
    for (uint32_t e = 0;; e++) {
        core_string_t effect = ir_func_effect_at(f, e);
        if (!effect.u8_bytes) break;
        (void)ir_summary_add_effect(summary, effect);
    }
    summary->witnesses.len = 0;
    ir_places_t *places = ir_func_places(f);
    ir_sites_t  *sites  = ir_func_sites(f);
    ir_ranges_t *prove_ranges = (!summaries->hooks
                                 || !summaries->hooks->defer_trap_proofs)
        && (ir_module_props(f->owner) & IR_PROP_SSA)
        ? ir_func_ranges(f) : nullptr;
    for (uint32_t s = 0; sites && s < ir_sites_count(sites); s++) {
        const ir_site_t *site = ir_site_at(sites, s);
        if (site->kind != IR_SITE_TRAP
            || (prove_ranges && ir_range_trap_proven(prove_ranges,
                                                     site->instr))) continue;
        uint8_t kind = trap_kind(site->instr);
        add_flags(summary, IR_SUMMARY_MAY_TRAP, site->instr, nullptr,
                  (core_string_t){0});
        summary->traps |= kind;
        core_list_push(summary->trap_sites, ((ir_strap_site_t){
            .kind   = kind,
            .line   = site->instr->span.start_line,
            .column = site->instr->span.start_col,
        }));
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (is_call(in)) compose_call(summaries, summary, in);
            add_flags(summary, allocation_flags(f, in), in, nullptr,
                      (core_string_t){0});
        }
    }
    flow_query_t    q = {.summaries = summaries, .f = f, .places = places};
    ir_path_exits_t exits;
    if (places && ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, &q, &exits)) {
        if (!exits.returned) summary->flags |= IR_SUMMARY_NO_RETURN;
        if (exits.failed) summary->flags |= IR_SUMMARY_MAY_FAIL;
        if (!exits.returned && !exits.failed)
            summary->flags |= IR_SUMMARY_DIVERGES;
    }
    else summary->flags |= IR_SUMMARY_MAY_FAIL;
    adjust_flags(summaries, summary);
}

static bool
path_touches(flow_query_t *q, const ir_summary_t *s, size_t index,
             const ir_splace_t *place)
{
    ir_splace_t places[2];
    uint32_t    count = query_path_places(q, s, index, places);
    for (uint32_t p = 0; p < count; p++)
        if (named_place(&places[p]) && ir_splace_eq(&places[p], place))
            return true;
    return false;
}

static uint8_t
must_write(flow_query_t *q, ir_func_t *f, const ir_splace_t *target)
{
    ir_path_exits_t exits;
    q->target = target;
    q->leaf   = nullptr;
    if (!ir_paths_flow(f, IR_PATH_UNPASSED, flow_step, q, &exits)) return 0;
    uint8_t unpassed = unpassed_exits(q, f, &exits);
    return (unpassed & UNPASSED_RETURN ? 0 : IR_SPATH_MUST_SUCCESS)
        | (unpassed & UNPASSED_FAIL ? 0 : IR_SPATH_MUST_FAILURE);
}

static void
summarize_writes(ir_summaries_t *summaries, ir_summary_t *summary,
                 ir_func_t *f)
{
    ir_places_t *places = ir_func_places(f);
    core_dict_t  cache;
    core_dict_init(&cache, nullptr, nullptr);
    flow_query_t q = {.summaries = summaries, .f = f, .places = places,
                      .path_places = &cache};
    for (size_t i = 0; places && i < summary->paths.len; i++) {
        ir_splace_t targets[2];
        uint32_t    count = summary->paths.data[i].flags & IR_SPATH_WRITE
            ? query_path_places(&q, summary, i, targets) : 0;
        for (uint32_t t = 0; t < count; t++) {
            bool seen = !named_place(&targets[t]);
            for (size_t j = 0; !seen && j < i; j++)
                seen = (summary->paths.data[j].flags & IR_SPATH_WRITE)
                    && path_touches(&q, summary, j, &targets[t]);
            uint8_t must = seen ? 0 : must_write(&q, f, &targets[t]);
            for (size_t j = i; must && j < summary->paths.len; j++)
                if ((summary->paths.data[j].flags & IR_SPATH_WRITE)
                    && path_touches(&q, summary, j, &targets[t]))
                    summary->paths.data[j].flags |= must;
        }
    }
    path_places_free(&cache);
}

typedef struct {
    core_list_t(uint32_t) bits;
    core_list_t(uint32_t) rows;
    core_list_t(uint32_t) stores;
    core_list_t(uint32_t) loads;
    core_list_t(uint32_t) parts;
    bool                  built;
} recipe_t;

typedef recipe_t *recipe_ptr_t;
core_list_decl(recipe_ptr_t);

typedef struct {
    ir_splace_t place;
    uint32_t    part;
} place_part_t;
core_list_decl(place_part_t);

typedef struct {
    ir_splace_t   place;
    ir_summary_t *callee;
    int32_t       output;
    uint32_t      owner;
    uint32_t      part;
} call_output_t;
core_list_decl(call_output_t);

typedef struct {
    ir_summaries_t *summaries;
    ir_summary_t   *summary;
    ir_func_t      *f;
    ir_places_t    *places;
    ir_memssa_t    *memssa;
    ir_pdg_t       *pdg;
    core_dict_t     index;
    uint32_t        instrs;
    uint32_t        words;
    uint32_t       *access_first;
    uint32_t       *access_count;
    uint64_t       *data;
    uint64_t       *control;
    uint64_t       *place_stores;
    uint64_t       *loads;
    uint32_t       *place_bits;
    recipe_t       *call_recipes;
    uint32_t       *slot_of;
    uint32_t       *slot_places;
    uint32_t        slots;
    uint32_t        defs;
    uint32_t       *order;
    uint32_t       *call_slots;
    uint64_t       *state;
    core_list_t(recipe_ptr_t) parts;
    core_dict_t     call_parts;
    core_dict_t     inputs;
    core_dict_t     input_parts;
    core_list_t(place_part_t) place_parts;
    core_list_t(call_output_t) *call_outputs;
    bool            changed;
} deps_t;

static uint64_t *
row(deps_t *d, uint64_t *table, uint32_t index)
{
    return table + (size_t)index * d->words;
}

static bool
merge(deps_t *d, uint64_t *into, const uint64_t *from)
{
    bool grew = false;
    for (uint32_t w = 0; w < d->words; w++) {
        uint64_t next = into[w] | from[w];
        grew |= next != into[w];
        into[w] = next;
    }
    return grew;
}

static void
set_bit(deps_t *d, uint64_t *into, uint32_t bit)
{
    uint64_t mask = 1ull << (bit % 64);
    if (!(into[bit / 64] & mask)) {
        into[bit / 64] |= mask;
        d->changed = true;
    }
}

static uint32_t
input_of(ir_summary_t *summary, ir_sinput_t input)
{
    for (size_t i = 0; i < summary->inputs.len; i++) {
        ir_sinput_t *have = &summary->inputs.data[i];
        if (have->kind != input.kind) continue;
        if ((input.kind == IR_SINPUT_VALUE || input.kind == IR_SINPUT_REFERENT)
                ? have->param == input.param
                : input.kind == IR_SINPUT_CALL
                ? core_string_eq(have->callee, input.callee)
                : input.kind == IR_SINPUT_UNKNOWN
                      || ir_splace_eq(&have->place, &input.place))
            return (uint32_t)i;
    }
    core_list_push(summary->inputs, input);
    return (uint32_t)(summary->inputs.len - 1);
}

static uint32_t
instr_index(deps_t *d, const ir_instr_t *in)
{
    bool found = false;
    uint32_t index = (uint32_t)(uintptr_t)core_dict_get(&d->index, (void *)in,
                                                        &found);
    return found ? index - 1 : UINT32_MAX;
}

static bool same_input(const ir_sinput_t *a, const ir_sinput_t *b);

static void *
input_key(const ir_sinput_t *input)
{
    uint64_t key = (uint64_t)input->kind + 1;
    switch (input->kind) {
    case IR_SINPUT_VALUE:
    case IR_SINPUT_REFERENT:
        key |= (uint64_t)(input->param + 1) << 8;
        break;
    case IR_SINPUT_PLACE: {
        const void *identity = input->place.kind == IR_SPLACE_HEAP
            ? (input->place.type ? (void *)th_resolve(input->place.type)
                                 : nullptr)
            : (const void *)input->place.global;
        key ^= ((uint64_t)input->place.kind << 4)
            ^ ((uint64_t)(uintptr_t)identity * 0x9e3779b97f4a7c15ull);
        break;
    }
    case IR_SINPUT_CALL:
        key ^= (uint64_t)(uintptr_t)input->callee.data * 0x9e3779b97f4a7c15ull;
        break;
    case IR_SINPUT_UNKNOWN:
        break;
    }
    return (void *)(uintptr_t)(key ? key : 1);
}

static uint32_t
deps_input(deps_t *d, ir_sinput_t input)
{
    void     *key   = input_key(&input);
    uintptr_t known = (uintptr_t)core_dict_get(&d->inputs, key, nullptr);
    if (known && same_input(&d->summary->inputs.data[known - 1], &input))
        return (uint32_t)(known - 1);
    uint32_t index = input_of(d->summary, input);
    if (!known) core_dict_put(&d->inputs, key, (void *)(uintptr_t)(index + 1));
    return index;
}

static uint32_t
entry_input(deps_t *d, uint32_t place)
{
    if (d->place_bits) return d->place_bits[place];
    ir_splace_t mapped;
    if (!portable_place(d->f, ir_place_at(d->places, place), &mapped))
        return UINT32_MAX;
    if (mapped.kind == IR_SPLACE_PARAM)
        return deps_input(d, (ir_sinput_t){
            .kind = IR_SINPUT_REFERENT, .param = mapped.param});
    if (mapped.kind == IR_SPLACE_UNKNOWN)
        return deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    return deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_PLACE,
                                       .place = mapped});
}


static void
recipe_free(recipe_t *r)
{
    core_list_free(r->bits);
    core_list_free(r->rows);
    core_list_free(r->stores);
    core_list_free(r->loads);
    core_list_free(r->parts);
}

static void
recipe_add(core_list_t(uint32_t) *list, uint32_t value)
{
    if (!list->data) *list = core_list_new(uint32_t);
    if (list->len && list->data[list->len - 1] == value) return;
    core_list_push(*list, value);
}

static void
apply_recipe(deps_t *d, const recipe_t *r, uint64_t *into)
{
    for (size_t i = 0; i < r->bits.len; i++) set_bit(d, into, r->bits.data[i]);
    for (size_t i = 0; i < r->rows.len; i++)
        if (merge(d, into, row(d, d->data, r->rows.data[i]))) d->changed = true;
    for (size_t i = 0; i < r->stores.len; i++)
        if (merge(d, into, row(d, d->place_stores, r->stores.data[i])))
            d->changed = true;
    for (size_t i = 0; i < r->loads.len; i++)
        if (merge(d, into, row(d, d->loads, r->loads.data[i])))
            d->changed = true;
    for (size_t i = 0; i < r->parts.len; i++)
        apply_recipe(d, d->parts.data[r->parts.data[i]], into);
}

static uint32_t
new_part(deps_t *d)
{
    core_list_push(d->parts, core_alloc(recipe_t));
    return (uint32_t)(d->parts.len - 1);
}

static void
set_input(deps_t *d, recipe_t *into, ir_sinput_t input)
{
    uint32_t bit = deps_input(d, input);
    if (bit >= d->words * 64)
        bit = deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    recipe_add(&into->bits, bit);
}

static void
place_dependences(deps_t *d, uint32_t place, recipe_t *into)
{
    uint32_t bit = entry_input(d, place);
    if (bit != UINT32_MAX && bit < d->words * 64) recipe_add(&into->bits, bit);
    recipe_add(&into->stores, place);
}

static void
storage_operand_dependences(deps_t *d, const ir_instr_t *call,
                            const ir_value_t *storage, recipe_t *into)
{
    uint32_t index = instr_index(d, call);
    if (index == UINT32_MAX) return;
    uint32_t end = d->access_first[index] + d->access_count[index];
    for (uint32_t a = d->access_first[index]; a < end; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        const uint32_t    *ids    = ir_places_access_places(d->places, access);
        if (access->kind == IR_ACCESS_LOAD && access->direct
            && access->count == 1
            && ir_place_at(d->places, ids[0])->root == storage)
            recipe_add(&into->loads, a);
    }
}

static void
value_dependences(deps_t *d, const ir_instr_t *call, const ir_value_t *value,
                  recipe_t *into)
{
    if (!value) return;
    if (value->kind == IR_VAL_LOCAL || value->kind == IR_VAL_GLOBAL)
        storage_operand_dependences(d, call, value, into);
    if (value->kind == IR_VAL_PARAM)
        for (uint32_t i = 0; i < d->f->n_params; i++)
            if (d->f->params[i] == value)
                set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_VALUE,
                                                 .param = i});
    if (value->kind == IR_VAL_INSTR && value->def) {
        uint32_t index = instr_index(d, value->def);
        if (index != UINT32_MAX) recipe_add(&into->rows, index);
    }
}

static void
argument_dependences(deps_t *d, const ir_instr_t *call, recipe_t *into)
{
    for (uint32_t o = 1; o < call->n_operands; o++) {
        ir_value_t *value = ir_operand_value(&call->operands[o]);
        value_dependences(d, call, value, into);
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument_places(d->places, d->f, value, &ids, &n))
            for (uint32_t p = 0; p < n; p++)
                place_dependences(d, ids[p], into);
    }
}

static bool
opaque_summary(const ir_summary_t *callee)
{
    return !callee->outputs.len && !callee->inputs.len
        && (!callee->function || !callee->function->n_blocks);
}

static bool
conservative_summary(const ir_summary_t *callee)
{
    return opaque_summary(callee)
        && (callee->flags & (IR_SUMMARY_CALLS_UNKNOWN
                             | IR_SUMMARY_CALLS_FINALIZERS
                             | IR_SUMMARY_STARTS_THREADS));
}

static core_string_t
callee_name(deps_t *d, const ir_func_t *target)
{
    return qualified_name(d->summaries, d->f->owner->ctx, target);
}

static void
opaque_dependences(deps_t *d, const ir_instr_t *call, ir_summary_t *callee,
                   recipe_t *into)
{
    argument_dependences(d, call, into);
    if (conservative_summary(callee) || !callee->function)
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    else
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_CALL,
            .callee = callee_name(d, callee->function)});
}

static bool
splace_matches(ir_module_t *module, const ir_splace_t *a,
               const ir_splace_t *b)
{
    if (a->kind == IR_SPLACE_UNKNOWN || b->kind == IR_SPLACE_UNKNOWN)
        return true;
    if (a->kind != b->kind) return false;
    if (a->kind == IR_SPLACE_HEAP)
        return !a->type || !b->type
            || ir_places_types_overlap(module, a->type, b->type);
    return ir_splace_eq(a, b);
}

static uint32_t
place_input_part(deps_t *d, const ir_splace_t *place)
{
    for (size_t i = 0; i < d->place_parts.len; i++)
        if (ir_splace_eq(&d->place_parts.data[i].place, place))
            return d->place_parts.data[i].part;
    uint32_t part = new_part(d);
    for (uint32_t p = 0; p < ir_places_count(d->places); p++) {
        ir_splace_t mapped;
        if (portable_place(d->f, ir_place_at(d->places, p), &mapped)
            && splace_matches(d->f->owner, &mapped, place))
            place_dependences(d, p, d->parts.data[part]);
    }
    core_list_push(d->place_parts, ((place_part_t){*place, part}));
    return part;
}

static void
map_callee_input(deps_t *d, const ir_instr_t *call,
                 const ir_sinput_t *input, recipe_t *into)
{
    ir_value_t *argument = input->param + 1 < call->n_operands
        ? ir_operand_value(&call->operands[input->param + 1]) : nullptr;
    switch (input->kind) {
    case IR_SINPUT_VALUE: {
        value_dependences(d, call, argument, into);
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument && argument->type
            && th_kind_of(th_resolve(argument->type)) == TH_INDIRECTION
            && argument_places(d->places, d->f, argument, &ids, &n))
            for (uint32_t p = 0; p < n; p++) place_dependences(d, ids[p], into);
        break;
    }
    case IR_SINPUT_REFERENT: {
        const uint32_t *ids = nullptr;
        uint32_t        n   = 0;
        if (argument_places(d->places, d->f, argument, &ids, &n))
            for (uint32_t p = 0; p < n; p++) place_dependences(d, ids[p], into);
        else if (argument && addressable(d->f, argument))
            set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
        break;
    }
    case IR_SINPUT_PLACE:
        set_input(d, into, *input);
        recipe_add(&into->parts, place_input_part(d, &input->place));
        break;
    case IR_SINPUT_CALL:
        set_input(d, into, *input);
        break;
    case IR_SINPUT_UNKNOWN:
        set_input(d, into, *input);
        break;
    }
}

static void
map_callee_output(deps_t *d, const ir_instr_t *call, ir_summary_t *callee,
                  const ir_soutput_t *output, recipe_t *into)
{
    core_dict_t *mapped = core_dict_get(&d->input_parts, callee, nullptr);
    if (!mapped) {
        mapped = core_alloc(core_dict_t);
        core_dict_init(mapped, nullptr, nullptr);
        core_dict_put(&d->input_parts, callee, mapped);
    }
    uint64_t call_key = (uint64_t)(instr_index(d, call) + 1) << 32;
    const uint32_t ranges[2][2] = {
        {output->data_first, output->data_count},
        {output->control_first, output->control_count},
    };
    for (int r = 0; r < 2; r++)
        for (uint32_t i = ranges[r][0]; i < ranges[r][0] + ranges[r][1]; i++) {
            uint32_t input = callee->dependences.data[i];
            if (input >= callee->inputs.len) continue;
            void     *key   = (void *)(uintptr_t)(call_key | input);
            uintptr_t known = (uintptr_t)core_dict_get(mapped, key, nullptr);
            if (!known) {
                uint32_t part = new_part(d);
                map_callee_input(d, call, &callee->inputs.data[input],
                                 d->parts.data[part]);
                known = part + 1;
                core_dict_put(mapped, key, (void *)known);
            }
            recipe_add(&into->parts, (uint32_t)(known - 1));
        }
}

static bool
output_writes(deps_t *d, const ir_instr_t *call, const ir_splace_t *written,
              const ir_splace_t *target, const uint32_t *ids, uint32_t n)
{
    if (written->kind == IR_SPLACE_UNKNOWN) return true;
    if (written->kind != IR_SPLACE_PARAM) {
        if (target && splace_matches(d->f->owner, written, target))
            return true;
        for (uint32_t p = 0; p < n; p++) {
            ir_splace_t mapped;
            if (portable_place(d->f, ir_place_at(d->places, ids[p]), &mapped)
                && splace_matches(d->f->owner, written, &mapped))
                return true;
        }
        return false;
    }
    ir_value_t *argument = written->param + 1 < call->n_operands
        ? ir_operand_value(&call->operands[written->param + 1]) : nullptr;
    const uint32_t *argument_ids = nullptr;
    uint32_t        count        = 0;
    if (!argument_places(d->places, d->f, argument, &argument_ids, &count))
        return argument && addressable(d->f, argument);
    for (uint32_t a = 0; a < count; a++) {
        for (uint32_t p = 0; p < n; p++)
            if (argument_ids[a] == ids[p]) return true;
        ir_splace_t mapped;
        if (target && portable_place(d->f, ir_place_at(d->places,
                                                       argument_ids[a]),
                                     &mapped)
            && splace_matches(d->f->owner, &mapped, target))
            return true;
    }
    return false;
}

enum { CALL_OUTPUT_UNKNOWN = -2, CALL_OUTPUT_OPAQUE = -1 };

static void
add_call_output(core_list_t(call_output_t) *entries, ir_splace_t written,
                ir_summary_t *callee, int32_t output, uint32_t owner)
{
    core_list_push(*entries, ((call_output_t){.place = written,
        .callee = callee, .output = output, .owner = owner}));
}

static core_list_t(call_output_t) *
call_output_entries(deps_t *d, const ir_instr_t *call)
{
    core_list_t(call_output_t) *entries =
        &d->call_outputs[instr_index(d, call)];
    if (entries->data) return entries;
    *entries = core_list_new(call_output_t);
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    if (targets.unknown)
        add_call_output(entries, (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                        nullptr, CALL_OUTPUT_UNKNOWN,
                        (uint32_t)entries->len);
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        if (opaque_summary(callee)) {
            uint32_t owner = (uint32_t)entries->len;
            if (conservative_summary(callee))
                add_call_output(entries,
                                (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                                callee, CALL_OUTPUT_OPAQUE, owner);
            place_list_t written = core_list_new(ir_splace_t);
            written_places(callee, &written);
            for (size_t w = 0; w < written.len; w++)
                add_call_output(entries, written.data[w], callee,
                                CALL_OUTPUT_OPAQUE, owner);
            core_list_free(written);
            continue;
        }
        for (size_t o = 0; o < callee->outputs.len; o++)
            if (callee->outputs.data[o].kind == IR_SOUTPUT_PLACE)
                add_call_output(entries, callee->outputs.data[o].place,
                                callee, (int32_t)o, (uint32_t)entries->len);
    }
    core_list_free(targets.targets);
    return entries;
}

static uint32_t
call_output_part(deps_t *d, const ir_instr_t *call,
                 core_list_t(call_output_t) *entries, size_t e)
{
    call_output_t *owner = &entries->data[entries->data[e].owner];
    if (owner->part) return owner->part - 1;
    uint32_t  part   = new_part(d);
    recipe_t *recipe = d->parts.data[part];
    if (owner->output == CALL_OUTPUT_UNKNOWN) {
        argument_dependences(d, call, recipe);
        set_input(d, recipe, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    }
    else if (owner->output == CALL_OUTPUT_OPAQUE)
        opaque_dependences(d, call, owner->callee, recipe);
    else
        map_callee_output(d, call, owner->callee,
                          &owner->callee->outputs.data[owner->output],
                          recipe);
    owner = &entries->data[entries->data[e].owner];
    owner->part = part + 1;
    return part;
}

static void
call_place_dependences(deps_t *d, const ir_instr_t *call,
                       const ir_splace_t *target, const uint32_t *ids,
                       uint32_t n, recipe_t *into)
{
    core_list_t(call_output_t) *entries = call_output_entries(d, call);
    for (size_t e = 0; e < entries->len; e++)
        if (output_writes(d, call, &entries->data[e].place, target, ids, n))
            recipe_add(&into->parts, call_output_part(d, call, entries, e));
}

static uint32_t
call_place_part(deps_t *d, const ir_instr_t *call, uint32_t place)
{
    uint32_t index = instr_index(d, call);
    void    *key   = (void *)(uintptr_t)((uint64_t)(index + 1)
        * (ir_places_count(d->places) + 1) + place + 1);
    bool     found = false;
    uintptr_t known = (uintptr_t)core_dict_get(&d->call_parts, key, &found);
    if (found) return (uint32_t)known;
    uint32_t part = new_part(d);
    core_dict_put(&d->call_parts, key, (void *)(uintptr_t)part);
    call_place_dependences(d, call, nullptr, &place, 1, d->parts.data[part]);
    return part;
}

static uint64_t *
state_row(deps_t *d, uint32_t def, uint32_t slot)
{
    return d->state + ((size_t)def * d->slots + slot) * d->words;
}

static bool
store_touches(deps_t *d, const ir_access_t *store, uint32_t place)
{
    const uint32_t *ids = ir_places_access_places(d->places, store);
    for (uint32_t i = 0; i < store->count; i++)
        if (ir_places_may_alias(d->places, ids[i], place)) return true;
    return false;
}

static bool
store_covers(deps_t *d, const ir_access_t *store, uint32_t place)
{
    if (!store->whole || store->count != 1
        || ir_places_access_places(d->places, store)[0] != place)
        return false;
    ir_place_kind_t kind = ir_place_at(d->places, place)->kind;
    return kind != IR_PLACE_HEAP && kind != IR_PLACE_UNKNOWN;
}

static bool
private_slot(deps_t *d, uint32_t slot)
{
    const ir_place_t *place = ir_place_at(d->places, d->slot_places[slot]);
    return place->kind == IR_PLACE_LOCAL && !place->exposed;
}

static void
prepare_memory(deps_t *d, uint32_t accesses)
{
    uint32_t places = ir_places_count(d->places);
    d->slot_of     = core_alloc_array(uint32_t, places + 1);
    d->slot_places = core_alloc_array(uint32_t, places + 1);
    for (uint32_t p = 0; p < places; p++) d->slot_of[p] = UINT32_MAX;
    for (uint32_t a = 0; d->memssa && a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind != IR_ACCESS_LOAD
            || ir_places_access_private(d->places, access)
            || ir_memssa_load_def(d->memssa, a) == UINT32_MAX) continue;
        const uint32_t *ids = ir_places_access_places(d->places, access);
        for (uint32_t i = 0; i < access->count; i++)
            if (d->slot_of[ids[i]] == UINT32_MAX) {
                d->slot_places[d->slots] = ids[i];
                d->slot_of[ids[i]] = d->slots++;
            }
    }
    d->defs = d->slots ? ir_memssa_def_count(d->memssa) : 0;
    d->order = core_alloc_array(uint32_t, d->defs + 1);
    uint32_t  blocks = d->f->n_blocks;
    uint32_t *start  = core_alloc_array(uint32_t, blocks + 2);
    uint32_t *rank   = core_alloc_array(uint32_t, d->defs + 1);
    ir_doms_t *doms  = ir_func_doms(d->f);
    for (uint32_t def = 0; def < d->defs; def++) {
        const ir_memdef_t *md = ir_memssa_def(d->memssa, def);
        uint32_t position = md->block && doms
            ? ir_rpo_index(doms, md->block) : UINT32_MAX;
        rank[def] = position < blocks ? position : blocks;
        start[rank[def] + 1]++;
    }
    for (uint32_t b = 0; b < blocks + 1; b++) start[b + 1] += start[b];
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t def = 0; def < d->defs; def++) {
            bool phi = ir_memssa_def(d->memssa, def)->kind == IR_MEMDEF_PHI;
            if (phi == (pass == 0)) d->order[start[rank[def]]++] = def;
        }
    core_free(start);
    core_free(rank);
    d->call_slots = core_alloc_array(uint32_t,
                                     (size_t)d->defs * d->slots + 1);
    for (uint32_t def = 0; def < d->defs; def++) {
        const ir_memdef_t *md = ir_memssa_def(d->memssa, def);
        if (md->kind != IR_MEMDEF_CALL || !md->instr || !is_call(md->instr))
            continue;
        for (uint32_t slot = 0; slot < d->slots; slot++)
            if (!private_slot(d, slot))
                d->call_slots[(size_t)def * d->slots + slot] = 1
                    + call_place_part(d, md->instr, d->slot_places[slot]);
    }
}

static void
memory_states(deps_t *d, uint32_t accesses)
{
    for (uint32_t o = 0; o < d->defs; o++) {
        uint32_t           def  = d->order[o];
        const ir_memdef_t *md   = ir_memssa_def(d->memssa, def);
        uint32_t           prev = ir_memssa_def_prev(d->memssa, def);
        uint32_t           from = md->instr && md->kind == IR_MEMDEF_STORE
            ? instr_index(d, md->instr) : UINT32_MAX;
        for (uint32_t slot = 0; slot < d->slots; slot++) {
            uint64_t *out   = state_row(d, def, slot);
            uint32_t  place = d->slot_places[slot];
            switch (md->kind) {
            case IR_MEMDEF_ENTRY: {
                uint32_t bit = entry_input(d, place);
                if (bit != UINT32_MAX && bit < d->words * 64)
                    set_bit(d, out, bit);
                break;
            }
            case IR_MEMDEF_PHI:
                for (uint32_t i = 0; i < ir_memssa_phi_count(d->memssa, def);
                     i++) {
                    uint32_t in = ir_memssa_phi_incoming(d->memssa, def, i);
                    if (in != UINT32_MAX
                        && merge(d, out, state_row(d, in, slot)))
                        d->changed = true;
                }
                break;
            case IR_MEMDEF_STORE:
                if (!store_covers(d, md->access, place) && prev != UINT32_MAX
                    && merge(d, out, state_row(d, prev, slot)))
                    d->changed = true;
                if (from != UINT32_MAX && store_touches(d, md->access, place)
                    && merge(d, out, row(d, d->data, from)))
                    d->changed = true;
                break;
            case IR_MEMDEF_CALL: {
                if (prev != UINT32_MAX && merge(d, out, state_row(d, prev, slot)))
                    d->changed = true;
                uint32_t part = d->call_slots[(size_t)def * d->slots + slot];
                if (part) apply_recipe(d, d->parts.data[part - 1], out);
                break;
            }
            case IR_MEMDEF_ELEMENT_PHI:
                break;
            }
        }
    }
    for (uint32_t a = 0; a < accesses; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind != IR_ACCESS_LOAD
            || ir_places_access_private(d->places, access)) continue;
        uint64_t *into = row(d, d->loads, a);
        uint32_t  def  = d->memssa ? ir_memssa_load_def(d->memssa, a)
                                   : UINT32_MAX;
        if (def == UINT32_MAX) {
            set_bit(d, into, deps_input(d,
                                        (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN}));
            continue;
        }
        const uint32_t *ids = ir_places_access_places(d->places, access);
        for (uint32_t i = 0; i < access->count; i++)
            if (merge(d, into, state_row(d, def, d->slot_of[ids[i]])))
                d->changed = true;
    }
}

typedef struct {
    uint32_t  generation;
    uint32_t *bits;
    uint32_t *rows;
    uint32_t *stores;
    uint32_t *loads;
    uint32_t *parts;
} stamps_t;

static void
flatten_list(const core_list_t(uint32_t) *from, core_list_t(uint32_t) *into,
             uint32_t *stamp, uint32_t generation)
{
    for (size_t i = 0; i < from->len; i++) {
        uint32_t value = from->data[i];
        if (stamp[value] == generation) continue;
        stamp[value] = generation;
        if (!into->data) *into = core_list_new(uint32_t);
        core_list_push(*into, value);
    }
}

static void
flatten_into(deps_t *d, const recipe_t *from, recipe_t *into, stamps_t *s)
{
    flatten_list(&from->bits, &into->bits, s->bits, s->generation);
    flatten_list(&from->rows, &into->rows, s->rows, s->generation);
    flatten_list(&from->stores, &into->stores, s->stores, s->generation);
    flatten_list(&from->loads, &into->loads, s->loads, s->generation);
    for (size_t i = 0; i < from->parts.len; i++) {
        uint32_t part = from->parts.data[i];
        if (s->parts[part] == s->generation) continue;
        s->parts[part] = s->generation;
        flatten_into(d, d->parts.data[part], into, s);
    }
}

static void
apply_unique(deps_t *d, const recipe_t *r, uint64_t *into, stamps_t *s)
{
    for (size_t i = 0; i < r->bits.len; i++) set_bit(d, into, r->bits.data[i]);
    for (size_t i = 0; i < r->rows.len; i++) {
        uint32_t index = r->rows.data[i];
        if (s->rows[index] == s->generation) continue;
        s->rows[index] = s->generation;
        merge(d, into, row(d, d->data, index));
    }
    for (size_t i = 0; i < r->stores.len; i++) {
        uint32_t place = r->stores.data[i];
        if (s->stores[place] == s->generation) continue;
        s->stores[place] = s->generation;
        merge(d, into, row(d, d->place_stores, place));
    }
    for (size_t i = 0; i < r->loads.len; i++) {
        uint32_t access = r->loads.data[i];
        if (s->loads[access] == s->generation) continue;
        s->loads[access] = s->generation;
        merge(d, into, row(d, d->loads, access));
    }
    for (size_t i = 0; i < r->parts.len; i++) {
        uint32_t part = r->parts.data[i];
        if (s->parts[part] == s->generation) continue;
        s->parts[part] = s->generation;
        apply_unique(d, d->parts.data[part], into, s);
    }
}

static stamps_t
new_stamps(deps_t *d)
{
    return (stamps_t){
        .bits   = core_alloc_array(uint32_t, d->summary->inputs.len + 1),
        .rows   = core_alloc_array(uint32_t, d->instrs + 1),
        .stores = core_alloc_array(uint32_t, ir_places_count(d->places) + 1),
        .loads  = core_alloc_array(uint32_t,
                                   ir_places_access_count(d->places) + 1),
        .parts  = core_alloc_array(uint32_t, d->parts.len + 1),
    };
}

static void
free_stamps(stamps_t *s)
{
    core_free(s->bits);
    core_free(s->rows);
    core_free(s->stores);
    core_free(s->loads);
    core_free(s->parts);
}

static void
flatten_recipe(deps_t *d, recipe_t *recipe, stamps_t *s)
{
    if (!recipe->parts.len) return;
    s->generation++;
    recipe_t flat = {.built = true};
    flatten_into(d, recipe, &flat, s);
    recipe_free(recipe);
    *recipe = flat;
}

static void
call_dependences(deps_t *d, ir_instr_t *call, recipe_t *into)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    if (targets.unknown) {
        argument_dependences(d, call, into);
        set_input(d, into, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    }
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        if (opaque_summary(callee)) {
            opaque_dependences(d, call, callee, into);
            continue;
        }
        for (size_t o = 0; o < callee->outputs.len; o++) {
            const ir_soutput_t *output = &callee->outputs.data[o];
            if (output->kind == IR_SOUTPUT_RESULT
                || output->kind == IR_SOUTPUT_FAILURE)
                map_callee_output(d, call, callee, output, into);
        }
    }
    core_list_free(targets.targets);
}

static void
callee_inputs(deps_t *d, const ir_instr_t *call)
{
    call_targets_t targets;
    resolve_targets(d->summaries, call, &targets);
    for (size_t t = 0; t < targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(d->summaries,
                                              targets.targets.data[t]);
        for (size_t i = 0; i < callee->inputs.len; i++) {
            const ir_sinput_t *input = &callee->inputs.data[i];
            if (input->kind == IR_SINPUT_PLACE
                || input->kind == IR_SINPUT_CALL)
                deps_input(d, *input);
        }
        if (opaque_summary(callee) && !conservative_summary(callee)
            && callee->function)
            deps_input(d, (ir_sinput_t){.kind = IR_SINPUT_CALL,
                .callee = callee_name(d, callee->function)});
    }
    core_list_free(targets.targets);
}

static void
instruction_dependences(deps_t *d, ir_instr_t *in, uint32_t index)
{
    uint64_t *into = row(d, d->data, index);
    if (is_call(in)) {
        recipe_t *recipe = &d->call_recipes[index];
        if (!recipe->built) {
            recipe->built = true;
            call_dependences(d, in, recipe);
        }
        apply_recipe(d, recipe, into);
        return;
    }
    for (uint32_t e = 0; d->pdg && e < ir_pdg_in_count(d->pdg, in); e++) {
        const ir_dep_edge_t *edge = ir_pdg_in_at(d->pdg, in, e);
        uint32_t from = edge->kind == IR_DEP_DATA
            ? instr_index(d, edge->from) : UINT32_MAX;
        if (from != UINT32_MAX && merge(d, into, row(d, d->data, from)))
            d->changed = true;
    }
    for (uint32_t o = 0; o < in->n_operands; o++) {
        ir_value_t *value = ir_operand_value(&in->operands[o]);
        if (!value || value->kind != IR_VAL_PARAM) continue;
        for (uint32_t i = 0; i < d->f->n_params; i++)
            if (d->f->params[i] == value)
                set_bit(d, into, deps_input(d, (ir_sinput_t){
                    .kind = IR_SINPUT_VALUE, .param = i}));
    }
    uint32_t end = d->access_first[index] + d->access_count[index];
    for (uint32_t a = d->access_first[index]; a < end; a++) {
        const ir_access_t *access = ir_places_access_at(d->places, a);
        if (access->kind == IR_ACCESS_LOAD
            && merge(d, into, row(d, d->loads, a)))
            d->changed = true;
    }
}

static void
block_control(deps_t *d, ir_block_t *block, uint64_t *into)
{
    for (uint32_t c = 0; d->pdg && c < ir_controllers_of(d->pdg, block); c++) {
        ir_block_t *controller = ir_controller_at(d->pdg, block, c);
        ir_instr_t *branch = controller ? ir_block_terminator(controller)
                                        : nullptr;
        uint32_t index = branch ? instr_index(d, branch) : UINT32_MAX;
        if (index == UINT32_MAX) continue;
        if (merge(d, into, row(d, d->data, index))) d->changed = true;
        if (merge(d, into, row(d, d->control, index))) d->changed = true;
    }
}


static bool
same_range(const ir_summary_t *s, uint32_t a, uint32_t b, uint32_t count)
{
    return !count || !memcmp(s->dependences.data + a, s->dependences.data + b,
                             count * sizeof(uint32_t));
}

static uint32_t
intern_range(ir_summary_t *s, uint32_t first, uint32_t count)
{
    for (size_t o = 0; o < s->outputs.len; o++) {
        const ir_soutput_t *have = &s->outputs.data[o];
        uint32_t found = have->data_count == count
                && same_range(s, have->data_first, first, count)
            ? have->data_first
            : have->control_count == count
                && same_range(s, have->control_first, first, count)
            ? have->control_first : UINT32_MAX;
        if (found != UINT32_MAX) {
            s->dependences.len = first;
            return found;
        }
    }
    return first;
}

static void
add_output(deps_t *d, ir_soutput_kind_t kind, ir_splace_t place,
           const uint64_t *data, const uint64_t *control)
{
    ir_summary_t *s = d->summary;
    ir_soutput_t  out = {.kind = kind, .place = place};
    uint32_t      bits = s->inputs.len < d->words * 64
        ? (uint32_t)s->inputs.len : d->words * 64;
    out.data_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < bits; bit++)
        if (data[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.data_count = (uint32_t)s->dependences.len - out.data_first;
    out.data_first = intern_range(s, out.data_first, out.data_count);
    out.control_first = (uint32_t)s->dependences.len;
    for (uint32_t bit = 0; bit < bits; bit++)
        if (control[bit / 64] & (1ull << (bit % 64)))
            core_list_push(s->dependences, bit);
    out.control_count = (uint32_t)s->dependences.len - out.control_first;
    out.control_first = intern_range(s, out.control_first, out.control_count);
    core_list_push(s->outputs, out);
}

static void
compact_inputs(ir_summary_t *summary)
{
    size_t    count = summary->inputs.len;
    uint32_t *remap = core_alloc_array(uint32_t, count + 1);
    for (size_t i = 0; i < count; i++) remap[i] = UINT32_MAX;
    for (size_t i = 0; i < summary->dependences.len; i++)
        remap[summary->dependences.data[i]] = 0;
    uint32_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (remap[i] != UINT32_MAX) {
            summary->inputs.data[kept] = summary->inputs.data[i];
            remap[i] = kept++;
        }
    summary->inputs.len = kept;
    for (size_t i = 0; i < summary->dependences.len; i++)
        summary->dependences.data[i] = remap[summary->dependences.data[i]];
    core_free(remap);
}

static void
collect_outputs(deps_t *d)
{
    uint64_t *data    = core_alloc_array(uint64_t, d->words);
    uint64_t *control = core_alloc_array(uint64_t, d->words);
    place_list_t places = core_list_new(ir_splace_t);
    written_places(d->summary, &places);
    size_t     written  = places.len;
    recipe_t  *gathered = core_alloc_array(recipe_t, written + 1);
    core_list_t(uint32_t) *writers =
        core_alloc_array(core_list_t(uint32_t), written + 1);
    for (uint32_t b = 0; b < d->f->n_blocks; b++)
        for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = d->f->blocks[b]->instrs[i];
            if (!is_call(in)) continue;
            core_list_t(call_output_t) *entries = call_output_entries(d, in);
            bool writes = false;
            for (size_t e = 0; e < entries->len; e++)
                if (entries->data[e].place.kind == IR_SPLACE_UNKNOWN) {
                    recipe_add(&gathered[written].parts,
                               call_output_part(d, in, entries, e));
                    writes = true;
                }
            if (writes) recipe_add(&writers[written], instr_index(d, in));
        }
    for (size_t a = 0; a < written; a++) {
        const ir_splace_t *place = &places.data[a];
        for (uint32_t b = 0; b < d->f->n_blocks; b++)
            for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = d->f->blocks[b]->instrs[i];
                if (!is_call(in)) continue;
                core_list_t(call_output_t) *entries =
                    call_output_entries(d, in);
                bool writes = false;
                for (size_t e = 0; e < entries->len; e++)
                    if (entries->data[e].place.kind != IR_SPLACE_UNKNOWN
                        && output_writes(d, in, &entries->data[e].place,
                                         place, nullptr, 0)) {
                        recipe_add(&gathered[a].parts,
                                   call_output_part(d, in, entries, e));
                        writes = true;
                    }
                if (writes) recipe_add(&writers[a], instr_index(d, in));
            }
    }
    stamps_t stamps = new_stamps(d);
    uint64_t *shared_data    = core_alloc_array(uint64_t, d->words);
    uint64_t *shared_control = core_alloc_array(uint64_t, d->words);
    uint64_t *call_data      = core_alloc_array(uint64_t,
                                                (written + 1) * d->words);
    bool     *computed       = core_alloc_array(bool, written + 1);
    stamps.generation++;
    apply_unique(d, &gathered[written], shared_data, &stamps);
    for (size_t c = 0; c < writers[written].len; c++)
        merge(d, shared_control, row(d, d->control, writers[written].data[c]));
    for (int kind = IR_SOUTPUT_RESULT; kind <= IR_SOUTPUT_FAILURE; kind++) {
        memset(data, 0, d->words * sizeof(uint64_t));
        memset(control, 0, d->words * sizeof(uint64_t));
        bool any = false;
        ir_op_t op = kind == IR_SOUTPUT_RESULT ? IR_OP_RETURN : IR_OP_FAIL;
        for (uint32_t b = 0; b < d->f->n_blocks; b++)
            for (uint32_t i = 0; i < d->f->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = d->f->blocks[b]->instrs[i];
                uint32_t index = in->op == op ? instr_index(d, in)
                                              : UINT32_MAX;
                if (index == UINT32_MAX) continue;
                any = true;
                merge(d, data, row(d, d->data, index));
                merge(d, control, row(d, d->control, index));
            }
        if (any) add_output(d, (ir_soutput_kind_t)kind,
                            (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                            data, control);
    }
    for (size_t a = 0; a < written; a++) {
        const ir_splace_t *place = &places.data[a];
        memset(data, 0, d->words * sizeof(uint64_t));
        memset(control, 0, d->words * sizeof(uint64_t));
        for (uint32_t p = 0; p < ir_places_count(d->places); p++) {
            ir_splace_t mapped;
            if (portable_place(d->f, ir_place_at(d->places, p), &mapped)
                && ir_splace_eq(&mapped, place))
                merge(d, data, row(d, d->place_stores, p));
        }
        size_t same = a;
        for (size_t b = 0; same == a && b < a; b++)
            if (computed[b] && gathered[b].parts.len == gathered[a].parts.len
                && (!gathered[a].parts.len
                    || !memcmp(gathered[b].parts.data, gathered[a].parts.data,
                               gathered[a].parts.len * sizeof(uint32_t))))
                same = b;
        uint64_t *calls = call_data + (size_t)same * d->words;
        if (same == a) {
            stamps.generation++;
            apply_unique(d, &gathered[a], calls, &stamps);
            computed[a] = true;
        }
        merge(d, data, calls);
        merge(d, data, shared_data);
        merge(d, control, shared_control);
        for (size_t c = 0; c < writers[a].len; c++)
            merge(d, control, row(d, d->control, writers[a].data[c]));
        for (uint32_t a = 0; a < ir_places_access_count(d->places); a++) {
            const ir_access_t *store = ir_places_access_at(d->places, a);
            const uint32_t    *ids   = ir_places_access_places(d->places,
                                                               store);
            uint32_t           index = instr_index(d, store->instr);
            bool               hits  = false;
            for (uint32_t p = 0; !hits && p < store->count; p++) {
                ir_splace_t mapped;
                hits = portable_place(d->f, ir_place_at(d->places, ids[p]),
                                      &mapped)
                    && ir_splace_eq(&mapped, place);
            }
            if (store->kind == IR_ACCESS_STORE && hits && index != UINT32_MAX)
                merge(d, control, row(d, d->control, index));
        }
        add_output(d, IR_SOUTPUT_PLACE, *place, data, control);
    }
    core_list_free(places);
    for (size_t a = 0; a <= written; a++) {
        recipe_free(&gathered[a]);
        core_list_free(writers[a]);
    }
    core_free(shared_data);
    core_free(shared_control);
    core_free(call_data);
    core_free(computed);
    core_free(gathered);
    core_free(writers);
    core_free(data);
    core_free(control);
    free_stamps(&stamps);
}

static void
summarize_dependences(ir_summaries_t *summaries, ir_summary_t *summary,
                      ir_func_t *f)
{
    summary->inputs.len      = 0;
    summary->outputs.len     = 0;
    summary->dependences.len = 0;
    deps_t d = {.summaries = summaries, .summary = summary, .f = f,
                .places = ir_func_places(f), .memssa = ir_func_memssa(f),
                .pdg = ir_func_pdg(f)};
    if (!d.places) return;
    core_dict_init(&d.index, nullptr, nullptr);
    core_dict_init(&d.inputs, nullptr, nullptr);
    core_dict_init(&d.input_parts, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            core_dict_put(&d.index, in, (void *)(uintptr_t)(++d.instrs));
            if (is_call(in)) callee_inputs(&d, in);
            for (uint32_t o = 1; is_call(in) && o < in->n_operands; o++) {
                const uint32_t *ids = nullptr;
                uint32_t        n   = 0;
                ir_value_t     *arg = ir_operand_value(&in->operands[o]);
                argument_places(d.places, f, arg, &ids, &n);
            }
        }
    d.access_first = core_alloc_array(uint32_t, d.instrs + 1);
    d.access_count = core_alloc_array(uint32_t, d.instrs + 1);
    for (uint32_t a = 0, index = 0, at = 0; at < f->n_blocks; at++)
        for (uint32_t i = 0; i < f->blocks[at]->n_instrs; i++, index++) {
            d.access_first[index] = a;
            while (a < ir_places_access_count(d.places)
                   && ir_places_access_at(d.places, a)->instr
                          == f->blocks[at]->instrs[i])
                a++;
            d.access_count[index] = a - d.access_first[index];
        }
    for (uint32_t i = 0; i < f->n_params; i++) {
        input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_VALUE, .param = i});
        input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_REFERENT,
                                        .param = i});
    }
    uint32_t *place_bits = core_alloc_array(uint32_t,
                                            ir_places_count(d.places) + 1);
    for (uint32_t p = 0; p < ir_places_count(d.places); p++)
        place_bits[p] = entry_input(&d, p);
    d.place_bits = place_bits;
    input_of(summary, (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
    d.parts        = core_list_new(recipe_ptr_t);
    d.place_parts  = core_list_new(place_part_t);
    d.call_outputs = core_alloc_array(core_list_t(call_output_t), d.instrs + 1);
    core_dict_init(&d.call_parts, nullptr, nullptr);
    uint32_t accesses = ir_places_access_count(d.places);
    d.call_recipes = core_alloc_array(recipe_t, d.instrs + 1);
    d.words        = UINT32_MAX / 64;
    prepare_memory(&d, accesses);
    for (uint32_t b = 0, index = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++, index++)
            if (is_call(f->blocks[b]->instrs[i])) {
                d.call_recipes[index].built = true;
                call_dependences(&d, f->blocks[b]->instrs[i],
                                 &d.call_recipes[index]);
            }
    stamps_t stamps = new_stamps(&d);
    for (size_t i = 0; i < (size_t)d.defs * d.slots; i++)
        if (d.call_slots[i])
            flatten_recipe(&d, d.parts.data[d.call_slots[i] - 1], &stamps);
    for (uint32_t i = 0; i < d.instrs; i++)
        flatten_recipe(&d, &d.call_recipes[i], &stamps);
    free_stamps(&stamps);
    d.words   = (uint32_t)((summary->inputs.len + 63) / 64 + 1);
    d.data    = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.control = core_alloc_array(uint64_t, (size_t)(d.instrs + 1) * d.words);
    d.loads   = core_alloc_array(uint64_t, (size_t)(accesses + 1) * d.words);
    d.state   = core_alloc_array(uint64_t,
        ((size_t)d.defs * d.slots + 1) * d.words);
    d.place_stores = core_alloc_array(uint64_t,
        (size_t)(ir_places_count(d.places) + 1) * d.words);
    for (uint32_t round = 0; round < 256; round++) {
        d.changed = false;
        memory_states(&d, accesses);
        uint32_t index = 0;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++, index++) {
                instruction_dependences(&d, block->instrs[i], index);
                block_control(&d, block, row(&d, d.control, index));
            }
        }
        for (uint32_t a = 0; a < ir_places_access_count(d.places); a++) {
            const ir_access_t *access = ir_places_access_at(d.places, a);
            if (access->kind != IR_ACCESS_STORE) continue;
            uint32_t        from = instr_index(&d, access->instr);
            const uint32_t *ids  = ir_places_access_places(d.places, access);
            for (uint32_t p = 0; from != UINT32_MAX && p < access->count; p++)
                if (merge(&d, row(&d, d.place_stores, ids[p]),
                          row(&d, d.data, from)))
                    d.changed = true;
        }
        if (!d.changed) break;
    }
    if (d.changed) {
        uint32_t unknown = input_of(summary,
                                    (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN});
        for (uint32_t index = 0; index < d.instrs; index++)
            set_bit(&d, row(&d, d.data, index), unknown);
    }
    collect_outputs(&d);
    compact_inputs(summary);
    for (uint32_t i = 0; i < d.instrs; i++) recipe_free(&d.call_recipes[i]);
    core_free(d.slot_of);
    core_free(d.slot_places);
    core_free(d.order);
    core_free(d.call_slots);
    core_free(d.state);
    for (size_t i = 0; i < d.parts.len; i++) {
        recipe_free(d.parts.data[i]);
        core_free(d.parts.data[i]);
    }
    core_list_free(d.parts);
    core_list_free(d.place_parts);
    for (uint32_t i = 0; i < d.instrs; i++) core_list_free(d.call_outputs[i]);
    core_free(d.call_outputs);
    core_dict_free(&d.call_parts);
    core_dict_free(&d.inputs);
    core_dict_foreach(&d.input_parts, item) {
        core_dict_free(item->value);
        core_free(item->value);
    }
    core_dict_free(&d.input_parts);
    core_free(place_bits);
    core_free(d.call_recipes);
    core_free(d.loads);
    core_free(d.access_first);
    core_free(d.access_count);
    core_free(d.data);
    core_free(d.control);
    core_free(d.place_stores);
    core_dict_free(&d.index);
}


static bool
same_input(const ir_sinput_t *a, const ir_sinput_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case IR_SINPUT_VALUE:
    case IR_SINPUT_REFERENT:
        return a->param == b->param;
    case IR_SINPUT_CALL:
        return core_string_eq(a->callee, b->callee);
    case IR_SINPUT_PLACE:
        return ir_splace_eq(&a->place, &b->place);
    case IR_SINPUT_UNKNOWN:
        return true;
    }
    return false;
}

static int
compare_u32(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return a < b ? -1 : a > b;
}

static bool
same_dependence_list(const ir_summary_t *a, const uint32_t *remap,
                     uint32_t a_first, uint32_t a_count,
                     const ir_summary_t *b, uint32_t b_first,
                     uint32_t b_count)
{
    if (a_count != b_count) return false;
    if (!a_count) return true;
    uint32_t *mapped = core_alloc_array(uint32_t, a_count);
    uint32_t *wanted = core_alloc_array(uint32_t, b_count);
    bool same = true;
    for (uint32_t i = 0; same && i < a_count; i++) {
        mapped[i] = remap[a->dependences.data[a_first + i]];
        same = mapped[i] != UINT32_MAX;
    }
    if (same) {
        memcpy(wanted, b->dependences.data + b_first,
               b_count * sizeof(uint32_t));
        qsort(mapped, a_count, sizeof(uint32_t), compare_u32);
        qsort(wanted, b_count, sizeof(uint32_t), compare_u32);
        same = !memcmp(mapped, wanted, a_count * sizeof(uint32_t));
    }
    core_free(mapped);
    core_free(wanted);
    return same;
}

static bool
same_dependences(const ir_summary_t *a, const ir_summary_t *b)
{
    if (a->outputs.len != b->outputs.len
        || a->inputs.len != b->inputs.len) return false;
    core_dict_t keys;
    core_dict_init(&keys, nullptr, nullptr);
    for (size_t j = 0; j < b->inputs.len; j++)
        core_dict_add(&keys, input_key(&b->inputs.data[j]),
                      (void *)(uintptr_t)(j + 1));
    uint32_t *remap = core_alloc_array(uint32_t, a->inputs.len + 1);
    bool same = true;
    for (size_t i = 0; same && i < a->inputs.len; i++) {
        uintptr_t j = (uintptr_t)core_dict_get(&keys,
            input_key(&a->inputs.data[i]), nullptr);
        if (!j || !same_input(&a->inputs.data[i], &b->inputs.data[j - 1])) {
            j = 0;
            for (size_t k = 0; !j && k < b->inputs.len; k++)
                if (same_input(&a->inputs.data[i], &b->inputs.data[k]))
                    j = k + 1;
        }
        remap[i] = j ? (uint32_t)(j - 1) : UINT32_MAX;
        same = j != 0;
    }
    for (size_t i = 0; same && i < a->outputs.len; i++) {
        const ir_soutput_t *left  = &a->outputs.data[i];
        bool                found = false;
        for (size_t j = 0; !found && j < b->outputs.len; j++) {
            const ir_soutput_t *right = &b->outputs.data[j];
            found = left->kind == right->kind
                && ir_splace_eq(&left->place, &right->place)
                && same_dependence_list(a, remap, left->data_first,
                                        left->data_count, b,
                                        right->data_first, right->data_count)
                && same_dependence_list(a, remap, left->control_first,
                                        left->control_count, b,
                                        right->control_first,
                                        right->control_count);
        }
        same = found;
    }
    core_free(remap);
    core_dict_free(&keys);
    return same;
}


typedef struct {
    ir_summaries_t                *summaries;
    core_dict_t                    index;
    core_dict_t                    lowlink;
    core_dict_t                    on_stack;
    core_list_t(summary_func_ptr_t) stack;
    uint32_t                       counter;
} tarjan_t;

static void direct_callees(ir_summaries_t *summaries, ir_func_t *f,
                           core_list_t(summary_func_ptr_t) *out);

static uint8_t
divergence_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    return is_call(in) && ir_summary_call_diverges(context, in)
        ? 0 : IR_PATH_UNPASSED;
}

typedef struct {
    ir_summaries_t   *summaries;
    ir_func_t *const *members;
    size_t            count;
} recursion_t;

static uint8_t
recursion_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    recursion_t *recursion = context;
    if (!is_call(in)) return IR_PATH_UNPASSED;
    call_targets_t targets;
    resolve_targets(recursion->summaries, in, &targets);
    bool member = false;
    for (size_t t = 0; t < targets.targets.len; t++)
        for (size_t m = 0; m < recursion->count; m++)
            member |= targets.targets.data[t] == recursion->members[m];
    core_list_free(targets.targets);
    return member || !ir_summary_call_diverges(recursion->summaries, in)
        ? IR_PATH_UNPASSED : 0;
}

static void
summarize_divergence(ir_summaries_t *summaries, ir_summary_t *summary,
                     ir_func_t *f)
{
    summary->flags &= ~(uint32_t)(IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN);
    size_t          scope = ir_analysis_scope_begin();
    ir_path_exits_t exits;
    if (ir_paths_flow(f, IR_PATH_UNPASSED, divergence_step, summaries,
                      &exits)
        && !exits.returned && !exits.failed)
        summary->flags |= IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN;
    ir_analysis_scope_end(scope);
}

uint32_t
ir_summary_field_count(const ir_summary_t *summary)
{
    return summary ? (uint32_t)summary->fields.len : 0;
}

const ir_sfield_t *
ir_summary_field_at(const ir_summary_t *summary, uint32_t index)
{
    return summary && index < summary->fields.len
        ? &summary->fields.data[index] : nullptr;
}

static th_type *
record_key(const ir_func_t *f, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || th_kind_of(type) != TH_NAMED) return nullptr;
    th_type *definition = ir_type_definition(f->owner, type);
    definition = definition ? th_resolve(definition) : nullptr;
    return definition && th_kind_of(definition) == TH_RECORD ? type : nullptr;
}

static th_type *
record_or_referent(const ir_func_t *f, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    th_type *record = record_key(f, type);
    if (record || !type || th_kind_of(type) != TH_INDIRECTION) return record;
    return record_key(f, th_ind_referent(type));
}

static void
add_field(ir_summary_t *summary, th_type *record, core_string_t name,
          uint8_t flags)
{
    for (size_t i = 0; i < summary->fields.len; i++) {
        ir_sfield_t *field = &summary->fields.data[i];
        if (field->record == record && core_string_eq(field->name, name)) {
            field->flags |= flags;
            return;
        }
    }
    core_list_push(summary->fields,
                   ((ir_sfield_t){.record = record, .name = name,
                                  .flags = flags}));
}

static uint8_t
field_pointer_use(const ir_instr_t *use, uint32_t operand)
{
    if ((int32_t)operand == ir_op_source_operand(use->op))
        return IR_SFIELD_READ;
    if ((int32_t)operand != ir_op_address_operand(use->op))
        return IR_SFIELD_READ | IR_SFIELD_WRITE;
    uint8_t access = ir_op_memory_access(use->op);
    return (access & IR_MEM_READ ? IR_SFIELD_READ : 0)
         | (access & IR_MEM_WRITE ? IR_SFIELD_WRITE : 0);
}

static bool
opaque_use(ir_summaries_t *summaries, const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_BOX:
    case IR_OP_BOX_RAW:
    case IR_OP_BITCAST:
    case IR_OP_MEMCMP:
    case IR_OP_CALLBACK_CREATE:
        return true;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE: {
        call_targets_t targets;
        resolve_targets(summaries, in, &targets);
        bool opaque = targets.unknown;
        for (size_t t = 0; !opaque && t < targets.targets.len; t++)
            opaque = !targets.targets.data[t]->n_blocks;
        core_list_free(targets.targets);
        return opaque;
    }
    default:
        return false;
    }
}

static bool
generic_parameter(const ir_func_t *target, uint32_t index)
{
    th_type *type = index < target->n_params && target->params[index]->type
        ? th_resolve(target->params[index]->type) : nullptr;
    if (type && th_kind_of(type) == TH_INDIRECTION)
        type = th_resolve(th_ind_referent(type));
    return type && th_kind_of(type) == TH_VAR;
}

static bool
generic_argument(ir_summaries_t *summaries, const ir_instr_t *in,
                 uint32_t operand)
{
    if (in->op != IR_OP_CALL || operand == 0) return false;
    call_targets_t targets;
    resolve_targets(summaries, in, &targets);
    bool generic = false;
    for (size_t t = 0; !generic && t < targets.targets.len; t++)
        generic = generic_parameter(targets.targets.data[t], operand - 1);
    core_list_free(targets.targets);
    return generic;
}

enum {
    PT_EXTERNAL     = UINT32_MAX,
    PT_OBJECT_ITEMS = 8,
    PT_ROUNDS       = 64,
    PT_RESOLVE      = 8,
};

typedef struct {
    ir_spath_t path;
    uint32_t   site;
} pt_loc_t;

typedef struct {
    uint8_t           depth;
    bool              deep;
    bool              held;
    bool              slot;
    core_string_t     steps[IR_SPATH_STEPS];
    pt_loc_t          loc;
    const ir_instr_t *origin;
} pt_item_t;
core_list_decl(pt_item_t);
typedef core_list_t(pt_item_t) pt_set_t;
core_list_decl(pt_set_t);

typedef struct {
    const void       *key;
    th_type          *type;
    bool              elements;
    pt_set_t          contents;
    pt_set_t          aliases;
    const ir_local_t *local;
    const ir_value_t *value;
} pt_site_t;
core_list_decl(pt_site_t);

typedef struct {
    uint32_t   site;
    ir_spath_t path;
    bool       deep;
    pt_set_t   items;
} pt_write_t;
core_list_decl(pt_write_t);
typedef core_list_t(pt_write_t) pt_writes_t;

typedef enum : uint8_t {
    FLOW_KILL_NONE,
    FLOW_KILL_CELL,
    FLOW_KILL_SITE,
} flow_kill_t;

typedef struct {
    const ir_instr_t *instr;
    uint32_t          record;
    uint32_t          site;
    core_string_t     cell;
    flow_kill_t       kill;
} flow_def_t;
core_list_decl(flow_def_t);

typedef struct {
    uint32_t index;
    uint32_t first;
    uint32_t count;
} flow_step_t;
core_list_decl(flow_step_t);

typedef struct {
    core_dict_t              writes;
    core_dict_t              positions;
    core_list_t(flow_def_t)  defs;
    core_list_t(flow_step_t) steps;
    uint32_t                *block_steps;
    uint64_t                *kills;
    uint64_t                *block_in;
    uint64_t                *state;
    uint32_t                 words;
    bool                     stale;
    pt_set_t                 items;
} pt_flow_t;

typedef struct {
    call_targets_t             targets;
    core_list_t(summary_ptr_t) callees;
    bool                       known;
} pt_callees_t;
core_list_decl(pt_callees_t);

typedef struct {
    core_string_t name;
    bool          inside;
} pt_member_t;
core_list_decl(pt_member_t);
typedef core_list_t(pt_member_t) member_list_t;

typedef struct {
    core_dict_t pointer_types;
    core_dict_t members;
} type_memo_t;

static type_memo_t *
type_memo(ir_summaries_t *summaries, const ir_module_t *module)
{
    bool         found = false;
    type_memo_t *memo  = core_dict_get(&summaries->type_memos,
                                       (void *)module, &found);
    if (found) return memo;
    memo = core_alloc(type_memo_t);
    core_dict_init(&memo->pointer_types, nullptr, nullptr);
    core_dict_init(&memo->members, nullptr, nullptr);
    core_dict_put(&summaries->type_memos, (void *)module, memo);
    return memo;
}

static void
type_memos_free(core_dict_t *memos)
{
    core_dict_foreach(memos, item) {
        type_memo_t *memo = item->value;
        core_dict_foreach(&memo->members, entry) {
            member_list_t *members = entry->value;
            core_list_free(*members);
            core_free(members);
        }
        core_dict_free(&memo->members);
        core_dict_free(&memo->pointer_types);
        core_free(memo);
    }
    core_dict_free(memos);
}

typedef struct {
    ir_splace_t place;
    bool        load;
    bool        entry;
} pt_access_t;
core_list_decl(pt_access_t);

typedef struct {
    ir_summaries_t        *summaries;
    const ir_func_t       *f;
    core_dict_t            value_index;
    core_list_t(pt_set_t)  values;
    core_dict_t            site_index;
    core_list_t(pt_site_t) sites;
    type_memo_t           *types;
    core_dict_t            load_uses;
    ir_reach_t            *reach;
    core_list_t(uint32_t)  aliased;
    core_list_t(uint32_t)  described;
    core_dict_t            call_index;
    core_list_t(pt_callees_t) calls;
    pt_set_t               scratch[3];
    pt_set_t               seen;
    ir_places_t           *places;
    ir_memssa_t           *memssa;
    core_list_t(pt_access_t) context;
    bool                   contextual;
    const ir_instr_t      *at;
    pt_flow_t              flow;
    core_list_t(summary_instr_ptr_t) order;
    core_dict_t            readers;
    bool                  *dirty;
    uint32_t               reader;
    bool                   pending;
    const ir_points_to_hooks_t *hooks;
} pt_t;

enum { PT_NO_READER = UINT32_MAX };

static uintptr_t
contents_key(uint32_t site)
{
    return (uintptr_t)site << 2 | 1;
}

static uintptr_t
aliases_key(uint32_t site)
{
    return (uintptr_t)site << 2 | 3;
}

static void
pt_depend(pt_t *pt, uintptr_t key)
{
    if (pt->reader == PT_NO_READER) return;
    bool                   found   = false;
    core_list_t(uint32_t) *readers = core_dict_get(&pt->readers,
                                                   (void *)key, &found);
    if (!readers) {
        readers  = core_alloc(core_list_t(uint32_t));
        *readers = core_list_new(uint32_t);
        core_dict_put(&pt->readers, (void *)key, readers);
    }
    for (size_t r = 0; r < readers->len; r++)
        if (readers->data[r] == pt->reader) return;
    core_list_push(*readers, pt->reader);
}

static void
pt_notify(pt_t *pt, uintptr_t key)
{
    bool                   found   = false;
    core_list_t(uint32_t) *readers = core_dict_get(&pt->readers,
                                                   (void *)key, &found);
    for (size_t r = 0; readers && r < readers->len; r++)
        if (!pt->dirty[readers->data[r]]) {
            pt->dirty[readers->data[r]] = true;
            pt->pending                 = true;
        }
}

enum { LOAD_WHOLE = 1, LOAD_EXTRACTED = 2 };

static bool
loc_deep(const pt_loc_t *loc)
{
    return loc->path.flags & IR_SPATH_DEEP;
}

static pt_loc_t
external_loc(ir_splace_kind_t root, uint32_t param, const ir_global_t *global)
{
    return (pt_loc_t){.site = PT_EXTERNAL, .path = {
        .root = root, .param = param,
        .global = global ? ir_global_identity((ir_global_t *)global)
                         : nullptr}};
}

static pt_loc_t
unknown_loc(void)
{
    pt_loc_t loc = external_loc(IR_SPLACE_UNKNOWN, 0, nullptr);
    loc.path.flags = IR_SPATH_DEEP;
    return loc;
}

static pt_loc_t
global_loc(const ir_global_t *global)
{
    bool thread = ir_global_storage_duration((ir_global_t *)global)
        == IR_STORAGE_DURATION_THREAD;
    return external_loc(thread ? IR_SPLACE_THREAD : IR_SPLACE_GLOBAL, 0,
                        global);
}

static pt_loc_t
path_loc(const pt_t *pt, const ir_spath_t *path)
{
    bool opaque = pt->hooks && pt->hooks->callee_globals_unknown
        && (path->root == IR_SPLACE_GLOBAL || path->root == IR_SPLACE_THREAD);
    if (opaque || path->root == IR_SPLACE_HEAP
        || path->root == IR_SPLACE_UNKNOWN)
        return unknown_loc();
    pt_loc_t loc = {.site = PT_EXTERNAL, .path = *path};
    loc.path.flags &= IR_SPATH_DEEP;
    return loc;
}

static pt_loc_t
site_loc(uint32_t site)
{
    return (pt_loc_t){.site = site};
}

static bool
same_object(const pt_loc_t *a, const pt_loc_t *b)
{
    return a->site == b->site
        && (a->site != PT_EXTERNAL || same_path_root(&a->path, &b->path));
}

static bool
item_covers(const pt_item_t *a, const pt_item_t *b)
{
    return a->origin == b->origin
        && same_object(&a->loc, &b->loc)
        && steps_cover(a->depth, a->deep, a->steps, b->depth, b->deep,
                       b->steps)
        && steps_cover(a->loc.path.depth, loc_deep(&a->loc),
                       a->loc.path.steps, b->loc.path.depth,
                       loc_deep(&b->loc), b->loc.path.steps);
}

static bool
merge_reach(pt_item_t *into, const pt_item_t *from)
{
    bool held = into->held || from->held;
    bool slot = held && (!into->held || into->slot || !from->held
                         || from->slot);
    bool changed = held != into->held || slot != into->slot;
    into->held = held;
    into->slot = slot;
    return changed;
}

static bool
set_add(pt_set_t *set, pt_item_t item)
{
    uint32_t same = 0;
    for (size_t i = 0; i < set->len; i++) {
        if (item_covers(&set->data[i], &item))
            return merge_reach(&set->data[i], &item);
        same += same_object(&set->data[i].loc, &item.loc);
    }
    if (same >= PT_OBJECT_ITEMS) {
        if (item.loc.path.depth && !item.held) item.held = true;
        item.depth           = 0;
        item.deep            = true;
        item.loc.path.depth  = 0;
        item.loc.path.flags |= IR_SPATH_DEEP;
    }
    size_t kept = 0;
    for (size_t i = 0; i < set->len; i++)
        if (item_covers(&item, &set->data[i]))
            merge_reach(&item, &set->data[i]);
        else set->data[kept++] = set->data[i];
    set->len = kept;
    core_list_push(*set, item);
    return true;
}

static void
pt_add(pt_t *pt, pt_set_t *set, uintptr_t key, pt_item_t item)
{
    if (set_add(set, item)) pt_notify(pt, key);
}

static pt_item_t
item_at(pt_loc_t loc)
{
    return (pt_item_t){.loc = loc};
}

static pt_item_t
held_at(pt_loc_t loc)
{
    return (pt_item_t){.held = !loc.path.depth, .loc = loc};
}

static pt_item_t
deep_item(pt_loc_t loc)
{
    loc.path.flags |= IR_SPATH_DEEP;
    return (pt_item_t){.deep = true, .loc = loc};
}

static void
loc_push(pt_loc_t *loc, core_string_t step)
{
    if (loc_deep(loc)) return;
    bool repeated = loc->path.depth == IR_SPATH_STEPS;
    for (uint8_t s = 0; !repeated && s < loc->path.depth; s++)
        repeated = core_string_eq(loc->path.steps[s], step);
    if (repeated) loc->path.flags |= IR_SPATH_DEEP;
    else loc->path.steps[loc->path.depth++] = step;
}

static void
loc_extend(pt_loc_t *loc, const core_string_t *steps, uint8_t depth,
           bool deep)
{
    for (uint8_t s = 0; s < depth; s++) loc_push(loc, steps[s]);
    if (deep) loc->path.flags |= IR_SPATH_DEEP;
}

static pt_item_t
item_under(const core_string_t *steps, uint8_t depth, bool deep,
           pt_item_t item)
{
    pt_item_t placed = {.loc = item.loc, .deep = deep || item.deep,
                        .held = item.held, .slot = item.slot};
    for (uint8_t s = 0; s < depth; s++) placed.steps[placed.depth++] = steps[s];
    for (uint8_t s = 0; !deep && s < item.depth; s++) {
        if (placed.depth == IR_SPATH_STEPS) {
            placed.deep = true;
            break;
        }
        placed.steps[placed.depth++] = item.steps[s];
    }
    return placed;
}

static bool
inline_sequence(const ir_func_t *f, th_type *type)
{
    type = defined_type(f, type);
    uint64_t extent = 0;
    return type && th_kind_of(type) == TH_SEQUENCE
        && th_seq_extent(type, &extent);
}

static bool
pointer_type(pt_t *pt, th_type *type)
{
    if (!type) return true;
    bool  found  = false;
    void *cached = core_dict_get(&pt->types->pointer_types, type, &found);
    if (found) return cached != nullptr;
    bool pointers = holds_pointers(pt->f, type, 0);
    core_dict_put(&pt->types->pointer_types, type, pointers ? type : nullptr);
    return pointers;
}

static uint32_t
site_of(pt_t *pt, const void *key, th_type *type, bool elements)
{
    bool  found = false;
    void *index = core_dict_get(&pt->site_index, key, &found);
    if (found) return (uint32_t)(uintptr_t)index;
    uint32_t site = (uint32_t)pt->sites.len;
    core_list_push(pt->sites, ((pt_site_t){
        .key = key, .type = type, .elements = elements,
        .contents = core_list_new(pt_item_t),
        .aliases  = core_list_new(pt_item_t)}));
    core_dict_put(&pt->site_index, key, (void *)(uintptr_t)site);
    return site;
}

static uint32_t
local_site(pt_t *pt, const ir_local_t *local)
{
    uint32_t site = site_of(pt, local, local->type, false);
    pt->sites.data[site].local = local;
    return site;
}

static uint32_t
variable_site(pt_t *pt, const ir_value_t *value)
{
    if (value->kind == IR_VAL_LOCAL) return local_site(pt, value->local);
    size_t   before = pt->sites.len;
    uint32_t site   = site_of(pt, value, value->type, false);
    pt->sites.data[site].value = value;
    if (pt->sites.len == before || !pointer_type(pt, value->type))
        return site;
    uint32_t param = value->index;
    set_add(&pt->sites.data[site].contents, item_at(
        param < pt->f->n_params && pt->f->params[param] == value
            ? external_loc(IR_SPLACE_PARAM, param, nullptr)
            : unknown_loc()));
    return site;
}

static uint32_t
allocation_site(pt_t *pt, const ir_instr_t *in)
{
    th_type *type = in->dest ? th_resolve(in->dest->type) : nullptr;
    if (type && th_kind_of(type) == TH_INDIRECTION)
        return site_of(pt, in, th_ind_referent(type), false);
    th_type *sequence = defined_type(pt->f, type);
    bool elements = sequence && th_kind_of(sequence) == TH_SEQUENCE;
    return site_of(pt, in, elements ? type : nullptr, elements);
}

static core_string_t
item_cell(const pt_item_t *item)
{
    return item->depth ? item->steps[0] : (core_string_t){0};
}

static bool
write_has_cell(const pt_write_t *write, core_string_t cell)
{
    for (size_t i = 0; i < write->items.len; i++)
        if (core_string_eq(item_cell(&write->items.data[i]), cell))
            return true;
    return false;
}

static void
note_write(pt_t *pt, uint32_t site, const pt_loc_t *at, bool deep,
           pt_item_t item)
{
    if (!pt->at || !pt->sites.data[site].local) return;
    pt_flow_t   *flow   = &pt->flow;
    bool         found  = false;
    pt_writes_t *writes = core_dict_get(&flow->writes, pt->at, &found);
    if (!writes) {
        writes  = core_alloc(pt_writes_t);
        *writes = core_list_new(pt_write_t);
        core_dict_put(&flow->writes, pt->at, writes);
    }
    pt_write_t *write = nullptr;
    for (size_t w = 0; !write && w < writes->len; w++)
        if (writes->data[w].site == site) write = &writes->data[w];
    ir_spath_t path = at ? at->path : (ir_spath_t){0};
    deep |= at && loc_deep(at);
    if (!write) {
        core_list_push(*writes, ((pt_write_t){
            .site = site, .path = path, .deep = deep,
            .items = core_list_new(pt_item_t)}));
        write       = &writes->data[writes->len - 1];
        flow->stale = true;
    }
    else if (!write->deep
             && (deep || write->path.depth != path.depth
                 || !steps_prefix(path.depth, path.steps, write->path.depth,
                                  write->path.steps))) {
        write->deep = true;
        flow->stale = true;
    }
    if (!write_has_cell(write, item_cell(&item))) flow->stale = true;
    if (set_add(&write->items, item)) pt_notify(pt, contents_key(site));
}

static flow_kill_t
write_kill(const pt_write_t *write)
{
    if (write->deep) return FLOW_KILL_NONE;
    if (!write->path.depth) return FLOW_KILL_SITE;
    return write->path.depth == 1
            && !core_string_eq(write->path.steps[0], element_step)
        ? FLOW_KILL_CELL : FLOW_KILL_NONE;
}

static void
flow_apply(const pt_flow_t *flow, const flow_step_t *step, uint64_t *state)
{
    for (uint32_t d = step->first; d < step->first + step->count; d++) {
        const uint64_t *kill = &flow->kills[(size_t)d * flow->words];
        for (uint32_t w = 0; w < flow->words; w++) state[w] &= ~kill[w];
    }
    for (uint32_t d = step->first; d < step->first + step->count; d++)
        state[d / 64] |= UINT64_C(1) << (d % 64);
}

static void
flow_kills(pt_flow_t *flow)
{
    core_free(flow->kills);
    flow->kills = core_alloc_array(uint64_t,
                                   flow->defs.len * flow->words + 1);
    for (size_t d = 0; d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->kill == FLOW_KILL_NONE) continue;
        uint64_t *kill = &flow->kills[d * flow->words];
        for (size_t e = 0; e < flow->defs.len; e++)
            if (flow->defs.data[e].site == def->site
                && (def->kill == FLOW_KILL_SITE
                    || core_string_eq(flow->defs.data[e].cell, def->cell)))
                kill[e / 64] |= UINT64_C(1) << (e % 64);
    }
}

static void
flow_build(pt_t *pt)
{
    pt_flow_t       *flow = &pt->flow;
    const ir_func_t *f    = pt->f;
    flow->defs.len  = 0;
    flow->steps.len = 0;
    core_free(flow->block_steps);
    flow->block_steps = core_alloc_array(uint32_t, f->n_blocks + 1);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        flow->block_steps[b] = (uint32_t)flow->steps.len;
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in     = f->blocks[b]->instrs[i];
            bool              found  = false;
            pt_writes_t      *writes = core_dict_get(&flow->writes, in,
                                                     &found);
            uint32_t first = (uint32_t)flow->defs.len;
            for (size_t r = 0; writes && r < writes->len; r++) {
                const pt_write_t *write = &writes->data[r];
                flow_kill_t       kill  = write_kill(write);
                size_t            start = flow->defs.len;
                for (size_t t = 0; t < write->items.len; t++) {
                    core_string_t cell = item_cell(&write->items.data[t]);
                    bool          seen = false;
                    for (size_t d = start; !seen && d < flow->defs.len; d++)
                        seen = core_string_eq(flow->defs.data[d].cell, cell);
                    if (!seen)
                        core_list_push(flow->defs, ((flow_def_t){
                            .instr = in, .record = (uint32_t)r,
                            .site = write->site,
                            .cell = kill == FLOW_KILL_CELL
                                ? write->path.steps[0] : cell,
                            .kill = kill}));
                }
            }
            if (flow->defs.len != first)
                core_list_push(flow->steps, ((flow_step_t){
                    .index = i, .first = first,
                    .count = (uint32_t)flow->defs.len - first}));
        }
    }
    flow->block_steps[f->n_blocks] = (uint32_t)flow->steps.len;
    flow->words = (uint32_t)((flow->defs.len + 63) / 64) + 1;
    flow_kills(flow);
    core_free(flow->block_in);
    core_free(flow->state);
    flow->block_in = core_alloc_array(uint64_t,
                                      (size_t)flow->words * f->n_blocks + 1);
    flow->state    = core_alloc_array(uint64_t, flow->words);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            memcpy(flow->state, &flow->block_in[(size_t)b * flow->words],
                   flow->words * sizeof(uint64_t));
            for (uint32_t s = flow->block_steps[b];
                 s < flow->block_steps[b + 1]; s++)
                flow_apply(flow, &flow->steps.data[s], flow->state);
            ir_instr_t *last = block->n_instrs
                ? block->instrs[block->n_instrs - 1] : nullptr;
            for (uint32_t s = 0; last && s < ir_succ_count(last); s++) {
                ir_block_t *next = ir_succ_at(last, s);
                uint64_t   *in   = next
                    ? &flow->block_in[(size_t)next->index * flow->words]
                    : nullptr;
                for (uint32_t w = 0; in && w < flow->words; w++) {
                    uint64_t merged = in[w] | flow->state[w];
                    changed |= merged != in[w];
                    in[w] = merged;
                }
            }
        }
    }
    flow->stale = false;
}

static const pt_set_t *
site_contents(pt_t *pt, uint32_t site)
{
    pt_site_t *entry = &pt->sites.data[site];
    if (!entry->local || !pt->at) return &entry->contents;
    pt_flow_t *flow  = &pt->flow;
    bool       found = false;
    uintptr_t  at    = (uintptr_t)core_dict_get(&flow->positions, pt->at,
                                                &found);
    if (!found) return &entry->contents;
    if (flow->stale) flow_build(pt);
    uint32_t block = (uint32_t)(at >> 32), index = (uint32_t)at;
    memcpy(flow->state, &flow->block_in[(size_t)block * flow->words],
           flow->words * sizeof(uint64_t));
    for (uint32_t s = flow->block_steps[block];
         s < flow->block_steps[block + 1]
         && flow->steps.data[s].index < index; s++)
        flow_apply(flow, &flow->steps.data[s], flow->state);
    flow->items.len = 0;
    for (size_t d = 0; d < flow->defs.len; d++) {
        const flow_def_t *def = &flow->defs.data[d];
        if (def->site != site
            || !(flow->state[d / 64] >> (d % 64) & 1)) continue;
        bool               present = false;
        const pt_writes_t *writes  = core_dict_get(&flow->writes, def->instr,
                                                   &present);
        const pt_write_t  *write   = &writes->data[def->record];
        for (size_t t = 0; t < write->items.len; t++)
            if (def->kill == FLOW_KILL_CELL
                || core_string_eq(item_cell(&write->items.data[t]),
                                  def->cell))
                set_add(&flow->items, write->items.data[t]);
    }
    return &flow->items;
}

static void
pt_read(pt_t *pt, const ir_value_t *value, pt_set_t *into)
{
    if (!value || !pointer_type(pt, value->type)) return;
    const pt_set_t *from = nullptr;
    switch (value->kind) {
    case IR_VAL_GLOBAL:
        if (value->global) set_add(into, item_at(global_loc(value->global)));
        return;
    case IR_VAL_PARAM:
    case IR_VAL_LOCAL: {
        if (value->kind == IR_VAL_LOCAL && !value->local) {
            set_add(into, deep_item(unknown_loc()));
            return;
        }
        uint32_t site = variable_site(pt, value);
        pt_depend(pt, contents_key(site));
        from = site_contents(pt, site);
        break;
    }
    case IR_VAL_INSTR: {
        bool  found = false;
        void *index = core_dict_get(&pt->value_index, value, &found);
        pt_depend(pt, (uintptr_t)value);
        if (found) from = &pt->values.data[(uintptr_t)index];
        break;
    }
    default:
        return;
    }
    if (from && !into->len) {
        for (size_t i = 0; i < from->len; i++)
            core_list_push(*into, from->data[i]);
        return;
    }
    for (size_t i = 0; from && i < from->len; i++) set_add(into, from->data[i]);
}

static void
pt_address(pt_t *pt, const ir_value_t *value, pt_set_t *into)
{
    if (value && value->kind == IR_VAL_GLOBAL && value->global)
        set_add(into, item_at(global_loc(value->global)));
    else
        pt_read(pt, value, into);
}

static pt_set_t *
value_set(pt_t *pt, const ir_value_t *value)
{
    bool  found = false;
    void *index = core_dict_get(&pt->value_index, value, &found);
    if (!found) {
        index = (void *)(uintptr_t)pt->values.len;
        core_list_push(pt->values, core_list_new(pt_item_t));
        core_dict_put(&pt->value_index, value, index);
    }
    return &pt->values.data[(uintptr_t)index];
}

static void
define_items(pt_t *pt, const ir_instr_t *in, const pt_set_t *items)
{
    if (!items->len) return;
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        if (!pointer_type(pt, in->dest_local->type)) return;
        uint32_t site = local_site(pt, in->dest_local);
        for (size_t i = 0; i < items->len; i++) {
            pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
                   items->data[i]);
            note_write(pt, site, nullptr, false, items->data[i]);
        }
        return;
    }
    pt_set_t *target = in->dest && pointer_type(pt, in->dest->type)
        ? value_set(pt, in->dest) : nullptr;
    for (size_t i = 0; target && i < items->len; i++)
        pt_add(pt, target, (uintptr_t)in->dest, items->data[i]);
}

static void
pt_define(pt_t *pt, const ir_instr_t *in, const pt_set_t *items)
{
    ir_pt_fresh_t fresh = pt->hooks && pt->hooks->fresh
        ? pt->hooks->fresh(pt->hooks->context, in) : IR_PT_FRESH_NONE;
    if (fresh == IR_PT_FRESH_NONE) {
        define_items(pt, in, items);
        return;
    }
    uint32_t site = site_of(pt, in, nullptr, false);
    for (size_t i = 0; fresh == IR_PT_FRESH_HOLDING && i < items->len; i++)
        pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
               item_under(nullptr, 0, true, items->data[i]));
    pt_set_t object = core_list_new(pt_item_t);
    set_add(&object, item_at(site_loc(site)));
    define_items(pt, in, &object);
    core_list_free(object);
}

static void
project(const pt_set_t *from, core_string_t step, pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (item.depth) {
            if (!core_string_eq(item.steps[0], step)) continue;
            for (uint8_t s = 1; s < item.depth; s++)
                item.steps[s - 1] = item.steps[s];
            item.depth--;
            set_add(into, item);
            continue;
        }
        if (item.deep) set_add(into, item);
        item.deep = false;
        loc_push(&item.loc, step);
        set_add(into, item);
    }
}

static bool
is_address(const pt_item_t *item)
{
    return !item->depth || item->deep;
}

static void
address_step(const pt_set_t *from, core_string_t step, pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (!is_address(&item)) continue;
        item.depth = 0;
        item.deep  = false;
        loc_push(&item.loc, step);
        set_add(into, item);
    }
}

static void
pointer_offset(const pt_set_t *from, pt_set_t *into)
{
    for (size_t i = 0; i < from->len; i++) {
        pt_item_t item = from->data[i];
        if (!is_address(&item)) continue;
        uint8_t depth = item.loc.path.depth;
        item.depth = 0;
        item.deep  = false;
        if (!depth || !core_string_eq(item.loc.path.steps[depth - 1],
                                      element_step))
            loc_push(&item.loc, element_step);
        set_add(into, item);
    }
}

static bool pt_reaches(pt_t *pt, const ir_instr_t *from,
                       const ir_instr_t *to);

static void
content_at(pt_t *pt, const pt_loc_t *loc, pt_set_t *into)
{
    pt_depend(pt, contents_key(loc->site));
    pt_depend(pt, aliases_key(loc->site));
    const pt_set_t      *contents = site_contents(pt, loc->site);
    const core_string_t *at       = loc->path.steps;
    uint8_t              depth    = loc->path.depth;
    for (size_t i = 0; i < contents->len; i++) {
        const pt_item_t *held  = &contents->data[i];
        bool             under = steps_prefix(depth, at, held->depth,
                                              held->steps);
        bool             over  = steps_prefix(held->depth, held->steps,
                                              depth, at);
        if (!under && !over) continue;
        if (loc_deep(loc) || (held->deep && !under)) {
            set_add(into, deep_item(held->loc));
            continue;
        }
        if (under) {
            pt_item_t item = *held;
            for (uint8_t s = depth; s < held->depth; s++)
                item.steps[s - depth] = held->steps[s];
            item.depth = held->depth - depth;
            set_add(into, item);
            continue;
        }
        pt_item_t item = item_at(held->loc);
        loc_extend(&item.loc, at + held->depth, depth - held->depth, false);
        set_add(into, item);
    }
    const pt_set_t *aliases = &pt->sites.data[loc->site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        if (!pt_reaches(pt, aliases->data[a].origin, pt->at)) continue;
        pt_item_t item = item_at(aliases->data[a].loc);
        loc_extend(&item.loc, at, depth, loc_deep(loc));
        set_add(into, item);
    }
}

static void
pt_load(pt_t *pt, const pt_set_t *addresses, pt_set_t *into)
{
    for (size_t i = 0; i < addresses->len; i++) {
        const pt_item_t *address = &addresses->data[i];
        if (!is_address(address)) continue;
        if (address->loc.site == PT_EXTERNAL)
            set_add(into, held_at(address->loc));
        else
            content_at(pt, &address->loc, into);
    }
}

static bool
pt_reaches(pt_t *pt, const ir_instr_t *from, const ir_instr_t *to)
{
    return ir_instr_reaches(pt->reach, from, to);
}

static void
pt_alias(pt_t *pt, uint32_t site, pt_loc_t alias, const ir_instr_t *origin)
{
    pt_item_t item = item_at(alias);
    item.origin = origin;
    if (!pt->sites.data[site].aliases.len) core_list_push(pt->aliased, site);
    pt_add(pt, &pt->sites.data[site].aliases, aliases_key(site), item);
}

static void
pt_publish(pt_t *pt, const pt_set_t *values, const pt_loc_t *at)
{
    for (size_t i = 0; i < values->len; i++) {
        const pt_item_t *value = &values->data[i];
        if (value->loc.site == PT_EXTERNAL) continue;
        pt_loc_t alias = *at;
        loc_extend(&alias, value->steps, value->depth,
                   value->deep || value->loc.path.depth
                       || loc_deep(&value->loc));
        pt_alias(pt, value->loc.site, alias, pt->at);
    }
}

static void
pt_store(pt_t *pt, const pt_set_t *addresses, const pt_set_t *values)
{
    for (size_t i = 0; i < addresses->len; i++) {
        pt_loc_t at = addresses->data[i].loc;
        if (!is_address(&addresses->data[i])) continue;
        if (at.site == PT_EXTERNAL) {
            pt_publish(pt, values, &at);
            continue;
        }
        for (size_t v = 0; v < values->len; v++) {
            pt_item_t item = item_under(at.path.steps, at.path.depth,
                                        loc_deep(&at), values->data[v]);
            pt_add(pt, &pt->sites.data[at.site].contents,
                   contents_key(at.site), item);
            note_write(pt, at.site, &at, false, item);
        }
    }
}

static bool
pt_elements(pt_t *pt, const ir_value_t *base, pt_set_t *into)
{
    pt_set_t items = core_list_new(pt_item_t);
    pt_address(pt, base, &items);
    th_type *type   = base && base->type ? th_resolve(base->type) : nullptr;
    bool     memory = true;
    if (type && th_kind_of(type) == TH_INDIRECTION) {
        th_type *referent = defined_type(pt->f, th_ind_referent(type));
        uint64_t extent   = 0;
        if (referent && th_kind_of(referent) == TH_SEQUENCE
            && !th_seq_extent(referent, &extent)
            && !inline_field(pt->f, base, false)) {
            pt_set_t loaded = core_list_new(pt_item_t);
            pt_load(pt, &items, &loaded);
            core_list_free(items);
            items = loaded;
        }
    }
    else
        memory = !inline_sequence(pt->f, type);
    if (memory) address_step(&items, element_step, into);
    else project(&items, element_step, into);
    core_list_free(items);
    return memory;
}

static bool
member_inline(const ir_func_t *f, pt_t *pt, th_type *record,
              core_string_t name)
{
    if (!pt) return inline_member(f, record, name, false);
    bool           found   = false;
    member_list_t *members = core_dict_get(&pt->types->members, record,
                                           &found);
    for (size_t m = 0; members && m < members->len; m++)
        if (core_string_eq(members->data[m].name, name))
            return members->data[m].inside;
    if (!members) {
        members  = core_alloc(member_list_t);
        *members = core_list_new(pt_member_t);
        core_dict_put(&pt->types->members, record, members);
    }
    bool inside = inline_member(f, record, name, false);
    core_list_push(*members, ((pt_member_t){name, inside}));
    return inside;
}

static uint8_t
inline_depth(const ir_func_t *f, pt_t *pt, th_type *type, bool sequence_root,
             const ir_spath_t *at, bool *known)
{
    uint8_t start = 0;
    *known = type != nullptr;
    if (!type) return 0;
    if (sequence_root && at->depth) {
        th_type *sequence = defined_type(f, type);
        if (!core_string_eq(at->steps[0], element_step) || !sequence
            || th_kind_of(sequence) != TH_SEQUENCE) {
            *known = false;
            return 0;
        }
        type  = th_seq_elem(sequence);
        start = 1;
    }
    bool inline_elements = false;
    for (uint8_t i = start; i < at->depth; i++) {
        th_type      *current  = defined_type(f, type);
        core_string_t step     = at->steps[i];
        bool          elements = core_string_eq(step, element_step);
        uint64_t      extent   = 0;
        if (!current) {
            *known = false;
            return i;
        }
        switch (th_kind_of(current)) {
        case TH_INDIRECTION:
            return i;
        case TH_SEQUENCE:
            if (!elements) {
                *known = false;
                return i;
            }
            if (!inline_elements && !th_seq_extent(current, &extent))
                return i;
            type            = th_seq_elem(current);
            inline_elements = false;
            break;
        case TH_RECORD: {
            th_type *member = elements ? nullptr
                                       : th_record_lookup(current, step);
            if (!member) {
                *known = false;
                return i;
            }
            inline_elements = member_inline(f, pt, current, step);
            type            = member;
            break;
        }
        default:
            if (ir_machine_of(f->owner, current).class != IR_CLASS_PTR)
                *known = false;
            return i;
        }
    }
    return at->depth;
}

static uint8_t
inline_prefix(pt_t *pt, const pt_site_t *site, const ir_spath_t *at,
              bool *known)
{
    return inline_depth(pt->f, pt, site->type, site->elements, at, known);
}

static th_type *
end_type(ir_module_t *module, th_type *type, const ir_spath_t *path)
{
    for (uint8_t s = 0; type && s < path->depth; s++) {
        th_type *current = definition_in(module, type);
        if (!current) return nullptr;
        if (core_string_eq(path->steps[s], element_step))
            type = th_kind_of(current) == TH_SEQUENCE
                ? th_seq_elem(current) : nullptr;
        else
            type = th_kind_of(current) == TH_RECORD
                ? th_record_lookup(current, path->steps[s]) : nullptr;
    }
    return type;
}

static bool
refers_beyond(ir_module_t *module, th_type *type)
{
    th_type *current = definition_in(module, type);
    uint64_t extent  = 0;
    if (!current && type) current = th_resolve(type);
    if (!current || !module) return true;
    switch (th_kind_of(current)) {
    case TH_INDIRECTION:
    case TH_VAR:
    case TH_BOUND:
    case TH_BINDER:
    case TH_TRAIT:
        return true;
    case TH_SEQUENCE:
        return !th_seq_extent(current, &extent);
    case TH_RECORD:
        return false;
    default:
        return ir_machine_of(module, current).class == IR_CLASS_PTR;
    }
}

static th_type *
root_object_type(const ir_func_t *f, const ir_spath_t *path)
{
    if (named_root(path->root))
        return path->global ? path->global->type : nullptr;
    if (path->root != IR_SPLACE_PARAM || !f || path->param >= f->n_params
        || !f->params[path->param])
        return nullptr;
    th_type *type = defined_type(f, f->params[path->param]->type);
    return type && th_kind_of(type) == TH_INDIRECTION
        ? th_ind_referent(type) : type;
}

bool
ir_summary_path_inline(const ir_func_t *function, const ir_spath_t *path)
{
    if (path->flags & IR_SPATH_DEEP) return false;
    if (!path->depth) return true;
    th_type *root    = root_object_type(function, path);
    th_type *defined = function && root ? defined_type(function, root)
                                        : nullptr;
    bool     known   = true;
    uint8_t  inside  = function && root
        ? inline_depth(function, nullptr, root,
                       defined && th_kind_of(defined) == TH_SEQUENCE, path,
                       &known)
        : 0;
    return function && root && known && inside == path->depth;
}

th_type *
ir_summary_path_type(const ir_func_t *function, const ir_spath_t *path,
                     uint8_t depth)
{
    th_type *root = path->root == IR_SPLACE_UNKNOWN
        ? (path->flags & IR_SPATH_TYPED ? path->type : nullptr)
        : root_object_type(function, path);
    ir_module_t *module = function ? function->owner
        : path->global ? path->global->owner : nullptr;
    ir_spath_t prefix = *path;
    prefix.depth = depth < path->depth ? depth : path->depth;
    return end_type(module, root, &prefix);
}

ir_spath_t
ir_spath_of_place(const ir_splace_t *place, uint8_t flags)
{
    switch (place->kind) {
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return (ir_spath_t){.root = place->kind, .global = place->global,
                            .flags = flags | IR_SPATH_ROOT};
    case IR_SPLACE_PARAM:
        return (ir_spath_t){.root = IR_SPLACE_PARAM, .param = place->param,
                            .flags = flags | IR_SPATH_DEEP | IR_SPATH_ROOT};
    case IR_SPLACE_HEAP:
        return (ir_spath_t){.root = IR_SPLACE_UNKNOWN, .type = place->type,
                            .flags = flags | IR_SPATH_DEEP
                                | (place->type ? IR_SPATH_TYPED : 0)};
    case IR_SPLACE_UNKNOWN:
        break;
    }
    return (ir_spath_t){.root = IR_SPLACE_UNKNOWN,
                        .flags = flags | IR_SPATH_DEEP};
}

uint32_t
ir_summary_path_places(const ir_func_t *function, const ir_spath_t *path,
                       ir_splace_t places[2])
{
    th_type *heap = path->flags & IR_SPATH_TYPED ? path->type : nullptr;
    switch (path->root) {
    case IR_SPLACE_HEAP:
        return 0;
    case IR_SPLACE_UNKNOWN:
        places[0] = heap
            ? (ir_splace_t){.kind = IR_SPLACE_HEAP, .type = heap}
            : (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
        return 1;
    default:
        break;
    }
    th_type *root   = root_object_type(function, path);
    bool     known  = true;
    uint8_t  inside = path->depth;
    if (path->depth && function && root)
        inside = inline_depth(function, nullptr, root, false, path, &known);
    else if (path->depth)
        known = false;
    bool     deep   = (path->flags & IR_SPATH_DEEP) || !known;
    bool     within = !deep && inside == path->depth;
    bool     own    = path->flags & IR_SPATH_ROOT;
    uint32_t count  = 0;
    ir_module_t *module = function ? function->owner
        : path->global ? path->global->owner : nullptr;
    if (own && (deep || within))
        places[count++] = (ir_splace_t){
            .kind = path->root, .param = path->param,
            .global = path->global, .type = root};
    if (!own || !within
        || refers_beyond(module, end_type(module, root, path)))
        places[count++] = (ir_splace_t){.kind = IR_SPLACE_HEAP, .type = heap};
    return count;
}

static void
resolve_into(pt_t *pt, pt_loc_t loc, pt_set_t *into, pt_set_t *seen,
             uint32_t depth)
{
    if (loc.site == PT_EXTERNAL) {
        set_add(into, item_at(loc));
        return;
    }
    bool deep = loc_deep(&loc) || depth > PT_RESOLVE;
    if (deep) {
        loc.path.depth = 0;
        loc.path.flags |= IR_SPATH_DEEP;
    }
    if (!set_add(seen, item_at(loc))) return;
    bool    known  = false;
    uint8_t inside = deep ? 0 : inline_prefix(pt, &pt->sites.data[loc.site],
                                              &loc.path, &known);
    bool    local  = deep || !known || inside == loc.path.depth;
    if (local) set_add(into, item_at(loc));
    if (local && known && !deep) return;
    pt_depend(pt, contents_key(loc.site));
    for (size_t i = 0; i < pt->sites.data[loc.site].contents.len; i++) {
        pt_item_t held = pt->sites.data[loc.site].contents.data[i];
        pt_loc_t  next = held.loc;
        if (!deep && !steps_prefix(held.depth, held.steps, loc.path.depth,
                                   loc.path.steps))
            continue;
        if (deep || held.deep) next.path.flags |= IR_SPATH_DEEP;
        else loc_extend(&next, loc.path.steps + held.depth,
                        loc.path.depth - held.depth, false);
        resolve_into(pt, next, into, seen, depth + 1);
    }
}

static void
pt_resolve(pt_t *pt, pt_loc_t loc, pt_set_t *into)
{
    if (loc.site == PT_EXTERNAL) {
        set_add(into, item_at(loc));
        return;
    }
    pt->seen.len = 0;
    resolve_into(pt, loc, into, &pt->seen, 0);
}

static void
argument_items(pt_t *pt, const ir_instr_t *call, uint32_t param,
               pt_set_t *into)
{
    ir_call_arg_t argument = ir_call_argument(call, param);
    switch (argument.role) {
    case IR_ARG_ABSENT:
    case IR_ARG_MISSING:
    case IR_ARG_OUT:
        return;
    case IR_ARG_FRESH: {
        pt_set_t address = core_list_new(pt_item_t);
        pt_read(pt, argument.value, &address);
        pt_load(pt, &address, into);
        core_list_free(address);
        return;
    }
    default:
        if (argument.value) pt_read(pt, argument.value, into);
        else set_add(into, deep_item(unknown_loc()));
        return;
    }
}

static void
map_param_path(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
               pt_set_t *into)
{
    pt_set_t items = core_list_new(pt_item_t);
    argument_items(pt, call, path->param, &items);
    bool deep = path->flags & IR_SPATH_DEEP;
    for (size_t i = 0; i < items.len; i++) {
        const pt_item_t *item = &items.data[i];
        pt_loc_t         loc  = item->loc;
        if (steps_prefix(item->depth, item->steps, path->depth,
                         path->steps)) {
            if (item->deep) loc.path.flags |= IR_SPATH_DEEP;
            else loc_extend(&loc, path->steps + item->depth,
                            path->depth - item->depth, deep);
        }
        else if ((item->deep || deep)
                 && steps_prefix(path->depth, path->steps, item->depth,
                                 item->steps))
            loc.path.flags |= IR_SPATH_DEEP;
        else
            continue;
        pt_resolve(pt, loc, into);
    }
    core_list_free(items);
}


static const pt_callees_t *
callees_of(pt_t *pt, const ir_instr_t *call)
{
    bool  found = false;
    void *index = core_dict_get(&pt->call_index, call, &found);
    return found ? &pt->calls.data[(uintptr_t)index] : nullptr;
}

static void
note_callees(pt_t *pt, const ir_instr_t *call)
{
    pt_callees_t entry = {.callees = core_list_new(summary_ptr_t)};
    resolve_targets(pt->summaries, call, &entry.targets);
    entry.known = !entry.targets.unknown;
    for (size_t t = 0; t < entry.targets.targets.len; t++) {
        ir_summary_t *callee = target_summary(pt->summaries,
                                              entry.targets.targets.data[t]);
        entry.known = entry.known && callee->results_known;
        core_list_push(entry.callees, callee);
    }
    core_dict_put(&pt->call_index, call, (void *)(uintptr_t)pt->calls.len);
    core_list_push(pt->calls, entry);
}

static void
call_reach(pt_t *pt, const ir_instr_t *call, const pt_callees_t *callees,
           pt_set_t *into)
{
    pt_set_t items = core_list_new(pt_item_t);
    for (uint32_t o = call->op == IR_OP_CALL ? 1 : 0; o < call->n_operands;
         o++)
        pt_read(pt, referenced(call, o), &items);
    for (size_t i = 0; i < items.len; i++)
        set_add(into, deep_item(items.data[i].loc));
    core_list_free(items);
    if (callees->targets.unknown) set_add(into, deep_item(unknown_loc()));
    for (size_t t = 0; t < callees->callees.len; t++) {
        const ir_summary_t *callee = callees->callees.data[t];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if ((path->flags & IR_SPATH_READ) && path->root != IR_SPLACE_PARAM)
                set_add(into, deep_item(path_loc(pt, path)));
        }
    }
}

static void
write_reach(pt_t *pt, const pt_loc_t *at, const pt_set_t *reach)
{
    for (size_t r = 0; r < reach->len; r++) {
        pt_item_t item = item_under(at->path.steps, at->path.depth, true,
                                    reach->data[r]);
        pt_add(pt, &pt->sites.data[at->site].contents,
               contents_key(at->site), item);
        note_write(pt, at->site, at, true, item);
    }
}

static void
map_param_value(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
                pt_set_t *into)
{
    pt_set_t items = core_list_new(pt_item_t);
    argument_items(pt, call, path->param, &items);
    bool deep = path->flags & IR_SPATH_DEEP;
    for (size_t i = 0; i < items.len; i++) {
        pt_item_t item = items.data[i];
        if (steps_prefix(path->depth, path->steps, item.depth, item.steps)) {
            if (deep) item = deep_item(item.loc);
            else {
                for (uint8_t s = path->depth; s < item.depth; s++)
                    item.steps[s - path->depth] = item.steps[s];
                item.depth -= path->depth;
            }
            set_add(into, item);
            continue;
        }
        if (!steps_prefix(item.depth, item.steps, path->depth, path->steps))
            continue;
        pt_item_t at = item_at(item.loc);
        if (item.deep) at.loc.path.flags |= IR_SPATH_DEEP;
        else loc_extend(&at.loc, path->steps + item.depth,
                        path->depth - item.depth, deep);
        at.deep = deep;
        set_add(into, at);
        if (at.loc.site == PT_EXTERNAL) continue;
        pt_set_t address = core_list_new(pt_item_t);
        set_add(&address, at);
        pt_load(pt, &address, into);
        core_list_free(address);
    }
    core_list_free(items);
}

static void
map_location(pt_t *pt, const ir_instr_t *call, const ir_spath_t *path,
             bool value, pt_set_t *into)
{
    if (path->root == IR_SPLACE_HEAP) {
        pt_loc_t loc = site_loc(site_of(pt, call, nullptr, false));
        loc_extend(&loc, path->steps, path->depth,
                   path->flags & IR_SPATH_DEEP);
        set_add(into, item_at(loc));
    }
    else if (path->root == IR_SPLACE_PARAM && value)
        map_param_value(pt, call, path, into);
    else if (path->root == IR_SPLACE_PARAM)
        map_param_path(pt, call, path, into);
    else
        set_add(into, item_at(path_loc(pt, path)));
}

static void
mapped_output(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
              uint8_t output, pt_set_t *into)
{
    for (size_t r = 0; r < callee->results.len; r++) {
        const ir_sstore_t *result = &callee->results.data[r];
        if (result->to.output != output) continue;
        pt_set_t values = core_list_new(pt_item_t);
        map_location(pt, call, &result->from, true, &values);
        for (size_t v = 0; v < values.len; v++)
            set_add(into, item_under(result->to.steps, result->to.depth,
                                     result->to.flags & IR_SPATH_DEEP,
                                     values.data[v]));
        core_list_free(values);
    }
}

static void
mapped_store(pt_t *pt, const ir_instr_t *call, const ir_sstore_t *store,
             pt_set_t *to, pt_set_t *from)
{
    map_location(pt, call, &store->to, false, to);
    map_location(pt, call, &store->from, true, from);
}

static void
apply_stores(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee)
{
    for (size_t s = 0; s < callee->stores.len; s++) {
        pt_set_t to   = core_list_new(pt_item_t);
        pt_set_t from = core_list_new(pt_item_t);
        mapped_store(pt, call, &callee->stores.data[s], &to, &from);
        pt_store(pt, &to, &from);
        core_list_free(to);
        core_list_free(from);
    }
}

static void
call_result(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
            pt_set_t *into)
{
    if (!call->dest || !pointer_type(pt, call->dest->type)) return;
    if (callee->results_known) mapped_output(pt, call, callee, 0, into);
    else set_add(into, deep_item(unknown_loc()));
}

static void
call_escapes(pt_t *pt, const ir_instr_t *call, const ir_summary_t *callee,
             const pt_set_t *reach, pt_set_t *result)
{
    if (callee->results_known) {
        apply_stores(pt, call, callee);
        call_result(pt, call, callee, result);
        return;
    }
    pt_set_t written = core_list_new(pt_item_t);
    for (size_t p = 0; p < callee->paths.len; p++) {
        const ir_spath_t *path = &callee->paths.data[p];
        if (!(path->flags & IR_SPATH_WRITE)) continue;
        pt_set_t locs = core_list_new(pt_item_t);
        if (path->root == IR_SPLACE_PARAM) map_param_path(pt, call, path,
                                                          &locs);
        else set_add(&locs, item_at(path_loc(pt, path)));
        for (size_t l = 0; l < locs.len; l++) {
            if (locs.data[l].loc.site != PT_EXTERNAL) {
                write_reach(pt, &locs.data[l].loc, reach);
                continue;
            }
            pt_loc_t at = locs.data[l].loc;
            at.path.flags |= IR_SPATH_DEEP;
            set_add(&written, item_at(at));
        }
        core_list_free(locs);
    }
    for (uint32_t param = 0;
         written.len && param < ir_call_argument_count(call); param++) {
        if (!ir_summary_param_escape(callee, param)) continue;
        pt_set_t items = core_list_new(pt_item_t);
        argument_items(pt, call, param, &items);
        for (size_t w = 0; w < written.len; w++)
            pt_publish(pt, &items, &written.data[w].loc);
        core_list_free(items);
    }
    core_list_free(written);
    call_result(pt, call, callee, result);
}

static uint8_t
output_ordinal(const ir_instr_t *call, uint32_t param)
{
    th_type *signature = ir_call_function_type(call);
    size_t   ordinal   = signature ? ir_call_param_output(signature, param)
                                   : SIZE_MAX;
    return ordinal + 1 < UINT8_MAX ? (uint8_t)(ordinal + 1) : 0;
}

static void record_set(pt_t *pt, ir_summary_t *summary,
                       const pt_set_t *addresses, uint8_t flags,
                       th_type *carried);
static void record_stores(pt_t *pt, ir_summary_t *summary,
                          const pt_set_t *addresses, const pt_set_t *values);

static void
callee_outputs(pt_t *pt, const ir_instr_t *call,
               const pt_callees_t *callees, uint8_t output, pt_set_t *into)
{
    for (size_t t = 0; t < callees->callees.len; t++)
        mapped_output(pt, call, callees->callees.data[t], output, into);
}

static void
written_outputs(pt_t *pt, const ir_instr_t *call,
                const pt_callees_t *callees, const pt_set_t *reach,
                ir_summary_t *summary)
{
    for (uint32_t param = 0; param < ir_call_argument_count(call); param++) {
        ir_call_arg_t argument = ir_call_argument(call, param);
        if (!ir_call_argument_writes(&argument)) continue;
        uint8_t  output  = output_ordinal(call, param);
        pt_set_t address = core_list_new(pt_item_t);
        pt_set_t values  = core_list_new(pt_item_t);
        pt_address(pt, argument.value, &address);
        if (output && callees->known)
            callee_outputs(pt, call, callees, output, &values);
        else if (reach)
            for (size_t r = 0; r < reach->len; r++)
                set_add(&values, reach->data[r]);
        else
            call_reach(pt, call, callees, &values);
        if (summary) {
            record_set(pt, summary, &address, IR_SPATH_WRITE, nullptr);
            record_stores(pt, summary, &address, &values);
        }
        else
            pt_store(pt, &address, &values);
        core_list_free(address);
        core_list_free(values);
    }
}

static void
pt_call(pt_t *pt, const ir_instr_t *call)
{
    const pt_callees_t *callees = callees_of(pt, call);
    pt_set_t reach  = core_list_new(pt_item_t);
    pt_set_t result = core_list_new(pt_item_t);
    call_reach(pt, call, callees, &reach);
    if (callees->targets.unknown) {
        pt_loc_t unknown = unknown_loc();
        for (uint32_t o = 1; o < call->n_operands; o++) {
            pt_set_t items = core_list_new(pt_item_t);
            pt_read(pt, referenced(call, o), &items);
            pt_publish(pt, &items, &unknown);
            for (size_t i = 0; i < items.len; i++)
                if (items.data[i].loc.site != PT_EXTERNAL) {
                    pt_loc_t whole = items.data[i].loc;
                    whole.path.flags |= IR_SPATH_DEEP;
                    write_reach(pt, &whole, &reach);
                }
            core_list_free(items);
        }
        set_add(&result, deep_item(unknown));
    }
    for (size_t t = 0; t < callees->callees.len; t++)
        call_escapes(pt, call, callees->callees.data[t], &reach, &result);
    written_outputs(pt, call, callees, &reach, nullptr);
    pt_define(pt, call, &result);
    core_list_free(reach);
    core_list_free(result);
}

static void
call_output(pt_t *pt, const ir_instr_t *output, pt_set_t *into)
{
    size_t            ordinal   = 0;
    const ir_instr_t *call      = ir_call_output_source(output, &ordinal);
    th_type          *signature = call ? ir_call_function_type(call)
                                       : nullptr;
    size_t param = signature ? ir_call_output_param(signature, ordinal)
                             : SIZE_MAX;
    if (param == SIZE_MAX || ordinal >= UINT8_MAX) {
        set_add(into, deep_item(unknown_loc()));
        return;
    }
    const pt_callees_t *callees = callees_of(pt, call);
    if (!callees) {
        set_add(into, deep_item(unknown_loc()));
        return;
    }
    if (callees->known)
        callee_outputs(pt, call, callees, (uint8_t)(ordinal + 1), into);
    else {
        argument_items(pt, call, (uint32_t)param, into);
        call_reach(pt, call, callees, into);
    }
}

static void
allocate(pt_t *pt, const ir_instr_t *in, pt_set_t *into)
{
    uint32_t site  = allocation_site(pt, in);
    pt_set_t items = core_list_new(pt_item_t);
    bool sequence = in->op == IR_OP_AGGREGATE || in->op == IR_OP_ARRAY_NEW
        || in->op == IR_OP_ARRAY_NEW_USING;
    for (uint32_t o = 0; in->op != IR_OP_COPY && o < in->n_operands; o++) {
        pt_read(pt, referenced(in, o), &items);
        for (size_t i = 0; i < items.len; i++)
            pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
                   item_under(&element_step, sequence ? 1 : 0, !sequence,
                              items.data[i]));
        items.len = 0;
    }
    core_list_free(items);
    set_add(into, item_at(site_loc(site)));
}

static void
spread_operands(pt_t *pt, const ir_instr_t *in, pt_set_t *into)
{
    pt_set_t items = core_list_new(pt_item_t);
    for (uint32_t o = 0; o < in->n_operands; o++)
        pt_read(pt, referenced(in, o), &items);
    for (size_t i = 0; i < items.len; i++)
        set_add(into, item_under(nullptr, 0, true, items.data[i]));
    core_list_free(items);
}

static bool
holds_nothing(ir_op_t op)
{
    switch (op) {
    case IR_OP_CONST:
    case IR_OP_ENUM_CONST:
    case IR_OP_ENUM_VALUE:
    case IR_OP_ENUM_FROM_VALUE:
    case IR_OP_BITSET_CONST:
    case IR_OP_NIL:
    case IR_OP_OPTION_NONE:
    case IR_OP_UNDEF:
    case IR_OP_SIZEOF:
    case IR_OP_ALIGNOF:
    case IR_OP_OFFSETOF:
    case IR_OP_TYPEHASH:
    case IR_OP_TYPENAME:
    case IR_OP_TYPE_OF:
    case IR_OP_TYPE_INDEX:
    case IR_OP_TRAIT_METHOD:
    case IR_OP_PHASE_RESULT:
    case IR_OP_PARAM_PRESENT:
    case IR_OP_ARMTAG:
        return true;
    default:
        return false;
    }
}

static const ir_value_t *
role_value(const ir_instr_t *in, int32_t operand)
{
    return operand >= 0 ? referenced(in, (uint32_t)operand) : nullptr;
}

static bool
launders(pt_t *pt, const ir_instr_t *in)
{
    const ir_value_t *base = referenced(in, 0);
    return (in->op == IR_OP_CAST || in->op == IR_OP_BITCAST) && base
        && pointer_type(pt, base->type) && in->dest
        && !pointer_type(pt, in->dest->type);
}

static bool
stored_sets(pt_t *pt, const ir_instr_t *in, pt_set_t *to, pt_set_t *from)
{
    if (!ir_op_transfers(in->op)) return false;
    const ir_value_t *target = role_value(in, ir_op_address_operand(in->op));
    if (ir_op_addresses_element(in->op)) {
        if (!pt_elements(pt, target, to)) return false;
    }
    else
        pt_address(pt, target, to);
    int32_t stored = ir_op_stored_operand(in->op);
    if (stored >= 0) {
        pt_read(pt, role_value(in, stored), from);
        return true;
    }
    pt_set_t address = core_list_new(pt_item_t);
    pt_set_t copied  = core_list_new(pt_item_t);
    pt_address(pt, role_value(in, ir_op_source_operand(in->op)), &address);
    pt_load(pt, &address, &copied);
    for (size_t i = 0; i < copied.len; i++)
        set_add(from, item_under(nullptr, 0, true, copied.data[i]));
    core_list_free(address);
    core_list_free(copied);
    return true;
}

static bool
step_matters(pt_t *pt, const ir_instr_t *in)
{
    if (ir_op_transfers(in->op) || in->op == IR_OP_CALL
        || in->op == IR_OP_CALLBACK_INVOKE || launders(pt, in))
        return true;
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local)
        return pointer_type(pt, in->dest_local->type);
    return in->dest && pointer_type(pt, in->dest->type);
}

static void
pt_step(pt_t *pt, const ir_instr_t *in)
{
    if (!step_matters(pt, in)) return;
    pt_set_t out   = pt->scratch[0];
    pt_set_t first = pt->scratch[1];
    pt_set_t other = pt->scratch[2];
    out.len = first.len = other.len = 0;
    const ir_value_t *base  = referenced(in, 0);
    const ir_value_t *third = referenced(in, 2);
    bool named = in->n_operands > 1 && in->operands[1].kind == IR_OPD_NAME;
    if (ir_instr_allocates_storage(in)) allocate(pt, in, &out);
    else switch (in->op) {
    case IR_OP_ADDRESS_OF:
        if (base && (base->kind == IR_VAL_PARAM
                     || (base->kind == IR_VAL_LOCAL && base->local)))
            set_add(&out, item_at(site_loc(variable_site(pt, base))));
        else if (base && base->kind == IR_VAL_GLOBAL && base->global)
            set_add(&out, item_at(global_loc(base->global)));
        else if (base) {
            uint32_t site = site_of(pt, in, base->type, false);
            pt_read(pt, base, &first);
            for (size_t i = 0; i < first.len; i++)
                pt_add(pt, &pt->sites.data[site].contents,
                       contents_key(site), first.data[i]);
            set_add(&out, item_at(site_loc(site)));
        }
        break;
    case IR_OP_COPY:
    case IR_OP_CONSUME:
    case IR_OP_RETAIN_REF:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_TYPE_NARROW:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_VARIANT_PAYLOAD:
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_ATOMIC_WRAP:
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
        pt_read(pt, base, &out);
        break;
    case IR_OP_CAST:
    case IR_OP_BITCAST:
        if (launders(pt, in)) {
            pt_loc_t unknown = unknown_loc();
            pt_read(pt, base, &first);
            pt_publish(pt, &first, &unknown);
        }
        else if (base && pointer_type(pt, base->type))
            pt_read(pt, base, &out);
        else set_add(&out, deep_item(unknown_loc()));
        break;
    case IR_OP_PHI:
    case IR_OP_SELECT:
    case IR_OP_OPTION_GET_OR_ELSE:
        for (uint32_t o = in->op == IR_OP_SELECT ? 1 : 0; o < in->n_operands;
             o++)
            pt_read(pt, referenced(in, o), &out);
        break;
    case IR_OP_FIELD:
        pt_address(pt, base, &first);
        if (named) address_step(&first, in->operands[1].name, &out);
        else set_add(&out, deep_item(unknown_loc()));
        break;
    case IR_OP_INDEX:
        pt_elements(pt, base, &out);
        break;
    case IR_OP_OFFSET:
    case IR_OP_ADD:
    case IR_OP_SUB:
        for (uint32_t o = 0; o < in->n_operands; o++)
            pt_read(pt, referenced(in, o), &first);
        pointer_offset(&first, &out);
        break;
    case IR_OP_EXTRACT:
        pt_read(pt, base, &first);
        if (named) project(&first, in->operands[1].name, &out);
        else spread_operands(pt, in, &out);
        break;
    case IR_OP_EXTRACT_INDEX:
        pt_read(pt, base, &first);
        project(&first, element_step, &out);
        break;
    case IR_OP_INSERT:
        pt_read(pt, base, &out);
        pt_read(pt, third, &first);
        for (size_t i = 0; i < first.len; i++)
            set_add(&out, named ? item_under(&in->operands[1].name, 1, false,
                                             first.data[i])
                                : item_under(nullptr, 0, true,
                                             first.data[i]));
        break;
    case IR_OP_AGGREGATE: {
        th_type *record = in->dest ? record_of(pt->f, in->dest->type)
                                   : nullptr;
        for (uint32_t o = 0; o < in->n_operands; o++) {
            const th_member *member = record && o < th_record_len(record)
                ? th_record_member(record, o) : nullptr;
            first.len = 0;
            pt_read(pt, referenced(in, o), &first);
            for (size_t i = 0; i < first.len; i++)
                set_add(&out, member ? item_under(&member->name, 1, false,
                                                  first.data[i])
                                     : item_under(nullptr, 0, true,
                                                  first.data[i]));
        }
        break;
    }
    case IR_OP_VARIANT_MAKE:
    case IR_OP_CALLBACK_CREATE:
    case IR_OP_REST_COLLECT:
        spread_operands(pt, in, &out);
        break;
    case IR_OP_LOAD:
        pt_address(pt, base, &first);
        pt_load(pt, &first, &out);
        break;
    case IR_OP_SEQUENCE_GET:
        if (pt_elements(pt, base, &first)) pt_load(pt, &first, &out);
        else for (size_t i = 0; i < first.len; i++) set_add(&out,
                                                            first.data[i]);
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        pt_call(pt, in);
        break;
    case IR_OP_CALL_OUTPUT:
        call_output(pt, in, &out);
        break;
    default: {
        if (ir_op_transfers(in->op)) {
            if (!stored_sets(pt, in, &first, &other)) break;
            if (ir_op_memory_access(in->op) & IR_MEM_READ)
                pt_load(pt, &first, &out);
            pt_store(pt, &first, &other);
            break;
        }
        if (holds_nothing(in->op)) break;
        bool pointers = false;
        for (uint32_t o = 0; !pointers && o < in->n_operands; o++) {
            const ir_value_t *operand = referenced(in, o);
            pointers = operand && pointer_type(pt, operand->type);
        }
        if (pointers) spread_operands(pt, in, &out);
        else set_add(&out, deep_item(unknown_loc()));
        break;
    }
    }
    pt_define(pt, in, &out);
    pt->scratch[0] = out;
    pt->scratch[1] = first;
    pt->scratch[2] = other;
}

static void
spread_escapes(pt_t *pt)
{
    for (size_t e = 0; e < pt->aliased.len; e++) {
        size_t s = pt->aliased.data[e];
        for (size_t c = 0; c < pt->sites.data[s].contents.len; c++) {
            pt_item_t held = pt->sites.data[s].contents.data[c];
            if (held.loc.site == PT_EXTERNAL || held.loc.site == s) continue;
            for (size_t a = 0; a < pt->sites.data[s].aliases.len; a++) {
                pt_item_t parent = pt->sites.data[s].aliases.data[a];
                loc_extend(&parent.loc, held.steps, held.depth,
                           held.deep || held.loc.path.depth
                               || loc_deep(&held.loc));
                pt_alias(pt, held.loc.site, parent.loc, parent.origin);
            }
        }
    }
}

static void
note_load_uses(pt_t *pt)
{
    const ir_func_t *f = pt->f;
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *used = referenced(in, o);
                if (!used || used->kind != IR_VAL_INSTR || !used->def
                    || used->def->op != IR_OP_LOAD) continue;
                bool extracted = in->op == IR_OP_EXTRACT && o == 0
                    && in->n_operands > 1
                    && in->operands[1].kind == IR_OPD_NAME;
                bool  found = false;
                void *bits  = core_dict_get(&pt->load_uses, used->def,
                                            &found);
                uintptr_t mark = (uintptr_t)bits
                    | (extracted ? LOAD_EXTRACTED : LOAD_WHOLE);
                core_dict_put(&pt->load_uses, used->def, (void *)mark);
            }
        }
}

static bool
whole_load(pt_t *pt, const ir_instr_t *load)
{
    bool      found = false;
    uintptr_t bits  = (uintptr_t)core_dict_get(&pt->load_uses, load, &found);
    return (bits & LOAD_WHOLE) || !(bits & LOAD_EXTRACTED);
}

static void name_loc(pt_t *pt, const pt_loc_t *loc, path_list_t *into,
                     core_list_t(uint32_t) *pending);

static bool
constant_read(const ir_spath_t *path)
{
    const ir_global_t *global = path->global
        ? ir_global_identity((ir_global_t *)path->global) : nullptr;
    return global && named_root(path->root) && !path->depth
        && !(path->flags & (IR_SPATH_WRITE | IR_SPATH_DEEP))
        && global->mutability != IR_MUT_MUTABLE
        && global->has_static_initializer;
}

static void
note_context(pt_t *pt, const ir_instr_t *in)
{
    pt->context.len = 0;
    uint32_t first = 0;
    uint32_t count = pt->places
        ? ir_places_accesses_of(pt->places, in, &first) : 0;
    for (uint32_t a = first; a < first + count; a++) {
        const ir_access_t *access = ir_places_access_at(pt->places, a);
        const uint32_t    *ids    = ir_places_access_places(pt->places,
                                                            access);
        bool load  = access->kind == IR_ACCESS_LOAD;
        bool entry = load && pt->memssa
            && ir_memssa_reads_entry(pt->memssa, a);
        for (uint32_t p = 0; p < access->count; p++) {
            ir_splace_t mapped;
            if (portable_place(pt->f, ir_place_at(pt->places, ids[p]),
                               &mapped))
                core_list_push(pt->context,
                               ((pt_access_t){mapped, load, entry}));
        }
    }
}

static bool
context_matches(const ir_splace_t *have, const ir_splace_t *want)
{
    return named_place(want) ? ir_splace_eq(have, want) : !named_place(have);
}

static void
apply_context(pt_t *pt, ir_spath_t *path)
{
    ir_splace_t places[2];
    ir_spath_t  probe = *path;
    probe.type   = nullptr;
    probe.flags |= IR_SPATH_ROOT;
    uint32_t count   = ir_summary_path_places(pt->f, &probe, places);
    bool     read    = path->flags & IR_SPATH_READ;
    bool     write   = path->flags & IR_SPATH_WRITE;
    bool     heap    = false;
    bool     root    = !pt->context.len;
    bool     unknown = !pt->context.len;
    th_type *type    = nullptr;
    for (size_t c = 0; c < pt->context.len; c++) {
        const pt_access_t *access = &pt->context.data[c];
        if (!(access->load ? read : write)) continue;
        for (uint32_t p = 0; p < count; p++) {
            if (!context_matches(&access->place, &places[p])) continue;
            if (access->entry) path->flags |= IR_SPATH_READS_ENTRY;
            if (named_place(&places[p])) {
                root = true;
                continue;
            }
            if (access->place.kind != IR_SPLACE_HEAP || !access->place.type)
                root = unknown = true;
            else if (!heap) type = access->place.type;
            else if (!same_type(type, access->place.type)) unknown = true;
            heap = true;
        }
    }
    if (root || !heap) path->flags |= IR_SPATH_ROOT;
    if (unknown) return;
    path->type   = heap ? type : nullptr;
    path->flags |= IR_SPATH_TYPED;
}

static void
record_loc(pt_t *pt, ir_summary_t *summary, const pt_loc_t *loc,
           uint8_t flags, th_type *carried)
{
    if (pt->summaries->escapes_only && !(flags & IR_SPATH_WRITE)) return;
    path_list_t names = core_list_new(ir_spath_t);
    name_loc(pt, loc, &names, nullptr);
    for (size_t n = 0; n < names.len; n++) {
        ir_spath_t path = names.data[n];
        path.flags |= flags;
        path.type   = carried;
        if (pt->contextual) apply_context(pt, &path);
        if (loc->site != PT_EXTERNAL)
            path.flags &= (uint8_t)~IR_SPATH_ROOT;
        if (!constant_read(&path)) add_function_path(summary, pt->f, &path);
    }
    release_paths(&names);
    core_list_free(names);
}

static void
record_set(pt_t *pt, ir_summary_t *summary, const pt_set_t *addresses,
           uint8_t flags, th_type *carried)
{
    for (size_t i = 0; i < addresses->len; i++)
        if (is_address(&addresses->data[i]))
            record_loc(pt, summary, &addresses->data[i].loc, flags, carried);
}

static void
record_global_values(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in)
{
    if (in->op == IR_OP_ADDRESS_OF) return;
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = referenced(in, o);
        if (!value || value->kind != IR_VAL_GLOBAL || !value->global
            || ir_op_is_address_operand(in->op, o)) continue;
        pt_loc_t at = global_loc(value->global);
        record_loc(pt, summary, &at, IR_SPATH_READ, nullptr);
    }
}

static void
record_operand(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in,
               uint32_t operand, uint8_t flags)
{
    if (operand >= in->n_operands) return;
    pt_set_t addresses = core_list_new(pt_item_t);
    pt_address(pt, referenced(in, operand), &addresses);
    record_set(pt, summary, &addresses, flags, nullptr);
    core_list_free(addresses);
}

static void
record_callee_stores(pt_t *pt, ir_summary_t *summary, const ir_instr_t *call,
                     const ir_summary_t *callee)
{
    pt_set_t to   = core_list_new(pt_item_t);
    pt_set_t from = core_list_new(pt_item_t);
    if (callee->results_known)
        for (size_t s = 0; s < callee->stores.len; s++) {
            to.len   = 0;
            from.len = 0;
            mapped_store(pt, call, &callee->stores.data[s], &to, &from);
            record_stores(pt, summary, &to, &from);
        }
    else {
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & IR_SPATH_WRITE)) continue;
            if (path->root == IR_SPLACE_PARAM)
                map_param_path(pt, call, path, &to);
            else
                set_add(&to, item_at(path_loc(pt, path)));
        }
        for (uint32_t param = 0; param < ir_call_argument_count(call);
             param++)
            if (ir_summary_param_escape(callee, param)) {
                pt_set_t items = core_list_new(pt_item_t);
                argument_items(pt, call, param, &items);
                for (size_t i = 0; i < items.len; i++)
                    set_add(&from, deep_item(items.data[i].loc));
                core_list_free(items);
            }
        for (size_t i = 0; i < to.len; i++)
            to.data[i].loc.path.flags |= IR_SPATH_DEEP;
        record_stores(pt, summary, &to, &from);
    }
    core_list_free(to);
    core_list_free(from);
}

static void
rebase_access(pt_t *pt, const ir_instr_t *call, uint32_t param,
              ir_spath_t *carried)
{
    ir_call_arg_t   argument = ir_call_argument(call, param);
    const uint32_t *ids      = nullptr;
    uint32_t        count    = 0;
    bool known = argument.value && pt->places
        && ir_places_of(pt->places, argument.value, &ids, &count);
    bool typed = known && (carried->flags & IR_SPATH_TYPED);
    bool owned = !known;
    for (uint32_t i = 0; i < count; i++) {
        ir_splace_t mapped;
        if (!portable_place(pt->f, ir_place_at(pt->places, ids[i]), &mapped))
            continue;
        if (named_place(&mapped)) {
            owned = true;
            continue;
        }
        if (mapped.kind != IR_SPLACE_HEAP) owned = true;
        ir_spath_t part = {.flags = IR_SPATH_TYPED, .type = mapped.type};
        typed = typed && mapped.kind == IR_SPLACE_HEAP && mapped.type
            && merge_type(carried, &part);
    }
    if (!owned) carried->flags &= (uint8_t)~IR_SPATH_ROOT;
    if (!typed) {
        carried->flags &= (uint8_t)~IR_SPATH_TYPED;
        carried->type   = nullptr;
    }
}

static void
record_call(pt_t *pt, ir_summary_t *summary, const ir_instr_t *call)
{
    const pt_callees_t *callees = callees_of(pt, call);
    if (callees->targets.unknown) {
        add_unknown_path(summary, IR_SPATH_ACCESS);
        pt_set_t to   = core_list_new(pt_item_t);
        pt_set_t from = core_list_new(pt_item_t);
        set_add(&to, item_at(unknown_loc()));
        call_reach(pt, call, callees, &from);
        record_stores(pt, summary, &to, &from);
        core_list_free(to);
        core_list_free(from);
    }
    pt->contextual = false;
    for (size_t t = 0; t < callees->callees.len; t++) {
        const ir_summary_t *callee = callees->callees.data[t];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & (IR_SPATH_READ | IR_SPATH_WRITE))) continue;
            ir_spath_t carried = {
                .flags = path->flags
                    & (IR_SPATH_ACCESS | IR_SPATH_TYPED | IR_SPATH_ROOT),
                .type  = path->type};
            if (path->root != IR_SPLACE_PARAM) {
                pt_loc_t loc = path_loc(pt, path);
                record_loc(pt, summary, &loc, carried.flags, carried.type);
                continue;
            }
            rebase_access(pt, call, path->param, &carried);
            pt_set_t locs = core_list_new(pt_item_t);
            map_param_path(pt, call, path, &locs);
            record_set(pt, summary, &locs, carried.flags, carried.type);
            core_list_free(locs);
        }
        record_callee_stores(pt, summary, call, callee);
    }
    pt->contextual = true;
    written_outputs(pt, call, callees, nullptr, summary);
}

static void
record_instr(pt_t *pt, ir_summary_t *summary, const ir_instr_t *in)
{
    pt_set_t first  = core_list_new(pt_item_t);
    pt_set_t second = core_list_new(pt_item_t);
    const ir_value_t *base = referenced(in, 0);
    pt->contextual = true;
    note_context(pt, in);
    record_global_values(pt, summary, in);
    switch (in->op) {
    case IR_OP_LOAD:
        if (whole_load(pt, in))
            record_operand(pt, summary, in, 0, IR_SPATH_READ);
        break;
    case IR_OP_EXTRACT:
        if (base && base->kind == IR_VAL_INSTR && base->def
            && base->def->op == IR_OP_LOAD && base->def->n_operands
            && !whole_load(pt, base->def) && in->n_operands > 1
            && in->operands[1].kind == IR_OPD_NAME) {
            note_context(pt, base->def);
            pt_address(pt, referenced(base->def, 0), &first);
            address_step(&first, in->operands[1].name, &second);
            record_set(pt, summary, &second, IR_SPATH_READ, nullptr);
        }
        break;
    case IR_OP_SEQUENCE_GET:
        if (pt_elements(pt, base, &first))
            record_set(pt, summary, &first, IR_SPATH_READ, nullptr);
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        record_call(pt, summary, in);
        break;
    case IR_OP_CAST:
    case IR_OP_BITCAST:
        if (launders(pt, in)) {
            set_add(&first, item_at(unknown_loc()));
            pt_read(pt, base, &second);
            record_stores(pt, summary, &first, &second);
        }
        break;
    default: {
        uint8_t access = ir_op_memory_access(in->op);
        uint8_t flags  = (access & IR_MEM_READ ? IR_SPATH_READ : 0)
                      | (access & IR_MEM_WRITE ? IR_SPATH_WRITE : 0);
        bool transfers = ir_op_transfers(in->op);
        if (!access || (transfers && !stored_sets(pt, in, &first, &second)))
            break;
        if (transfers) record_set(pt, summary, &first, flags, nullptr);
        else record_operand(pt, summary, in,
                            (uint32_t)ir_op_address_operand(in->op), flags);
        int32_t source = ir_op_source_operand(in->op);
        if (source >= 0)
            record_operand(pt, summary, in, (uint32_t)source, IR_SPATH_READ);
        if (transfers) record_stores(pt, summary, &first, &second);
        break;
    }
    }
    core_list_free(first);
    core_list_free(second);
}

typedef core_list_t(uint32_t) site_list_t;

static void
path_push(ir_spath_t *path, core_string_t step)
{
    if (path->flags & IR_SPATH_DEEP) return;
    if (path->depth == IR_SPATH_STEPS) path->flags |= IR_SPATH_DEEP;
    else path->steps[path->depth++] = step;
}

static void
name_loc(pt_t *pt, const pt_loc_t *loc, path_list_t *into,
         site_list_t *pending)
{
    if (loc->site == PT_EXTERNAL) {
        ir_spath_t path = loc->path;
        path.flags &= IR_SPATH_DEEP;
        add_path_to(into, &path);
        return;
    }
    if (pending) {
        ir_spath_t heap = {.root  = IR_SPLACE_HEAP,
                           .flags = loc->path.flags & IR_SPATH_DEEP};
        for (uint8_t s = 0; s < loc->path.depth; s++)
            path_push(&heap, loc->path.steps[s]);
        add_path_to(into, &heap);
        core_list_push(*pending, loc->site);
    }
    const pt_set_t *aliases = &pt->sites.data[loc->site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        if (!pt_reaches(pt, aliases->data[a].origin, pt->at)) continue;
        pt_loc_t alias = aliases->data[a].loc;
        loc_extend(&alias, loc->path.steps, loc->path.depth, loc_deep(loc));
        name_loc(pt, &alias, into, nullptr);
    }
}

static void
describe_values(pt_t *pt, const pt_set_t *values, const ir_spath_t *base,
                pair_list_t *into, site_list_t *pending)
{
    for (size_t i = 0; i < values->len; i++) {
        const pt_item_t *item = &values->data[i];
        ir_spath_t       to   = *base;
        for (uint8_t s = 0; s < item->depth; s++) path_push(&to, item->steps[s]);
        if (item->deep) to.flags |= IR_SPATH_DEEP;
        path_list_t from = core_list_new(ir_spath_t);
        name_loc(pt, &item->loc, &from, pending);
        for (size_t f = 0; f < from.len; f++) {
            if (item->loc.site == PT_EXTERNAL)
                from.data[f].flags |= (item->held ? IR_SPATH_DEEP : 0)
                    | (!item->held || item->slot ? IR_SPATH_ROOT : 0);
            add_pair(into, &to, &from.data[f]);
        }
        release_paths(&from);
        core_list_free(from);
    }
}

static void
describe_heap(pt_t *pt, ir_summary_t *summary, site_list_t *pending)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    while (pending->len) {
        uint32_t site = pending->data[--pending->len];
        void    *key  = (void *)(uintptr_t)(site + 1);
        if (core_dict_contains(&seen, key)) continue;
        core_dict_put(&seen, key, key);
        ir_spath_t heap = {.root = IR_SPLACE_HEAP};
        describe_values(pt, &pt->sites.data[site].contents, &heap,
                        &summary->stores, pending);
    }
    core_dict_free(&seen);
}

static void
summarize_output(pt_t *pt, ir_summary_t *summary, const pt_set_t *returned,
                 uint8_t output)
{
    ir_spath_t base = {.output = output};
    describe_values(pt, returned, &base, &summary->results, &pt->described);
}

static void
record_stores(pt_t *pt, ir_summary_t *summary, const pt_set_t *addresses,
              const pt_set_t *values)
{
    if (!values->len) return;
    path_list_t to = core_list_new(ir_spath_t);
    for (size_t i = 0; i < addresses->len; i++)
        if (is_address(&addresses->data[i]))
            name_loc(pt, &addresses->data[i].loc, &to, nullptr);
    for (size_t t = 0; t < to.len; t++)
        describe_values(pt, values, &to.data[t], &summary->stores,
                        &pt->described);
    release_paths(&to);
    core_list_free(to);
}

static void
summarize_results(pt_t *pt, ir_summary_t *summary)
{
    const ir_func_t *f = pt->f;
    summary->results_known = true;
    size_t   count   = ir_call_output_count(f->type);
    uint32_t outputs = count < UINT8_MAX ? (uint32_t)count + 1 : UINT8_MAX;
    for (uint32_t output = returns_value(f) ? 0 : 1; output < outputs;
         output++) {
        size_t operand = output
            ? ir_return_output_operand(f->owner, f->type, output - 1) : 0;
        pt_set_t returned = core_list_new(pt_item_t);
        for (uint32_t b = 0; b < f->n_blocks; b++)
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = f->blocks[b]->instrs[i];
                if (in->op == IR_OP_RETURN && operand < in->n_operands)
                    pt_read(pt, referenced(in, operand),
                            &returned);
            }
        summarize_output(pt, summary, &returned, (uint8_t)output);
        core_list_free(returned);
    }
}

static void
pt_init(pt_t *pt, ir_summaries_t *summaries, const ir_func_t *f)
{
    *pt = (pt_t){
        .summaries = summaries, .f = f,
        .values    = core_list_new(pt_set_t),
        .sites     = core_list_new(pt_site_t),
    };
    core_dict_init(&pt->value_index, nullptr, nullptr);
    core_dict_init(&pt->site_index, nullptr, nullptr);
    pt->types = type_memo(summaries, f->owner);
    core_dict_init(&pt->load_uses, nullptr, nullptr);
    core_dict_init(&pt->call_index, nullptr, nullptr);
    pt->reach     = ir_reach_new(f);
    pt->aliased   = core_list_new(uint32_t);
    pt->described = core_list_new(uint32_t);
    pt->seen      = core_list_new(pt_item_t);
    pt->context   = core_list_new(pt_access_t);
    for (size_t s = 0; s < 3; s++) pt->scratch[s] = core_list_new(pt_item_t);
    pt->calls     = core_list_new(pt_callees_t);
    pt->flow      = (pt_flow_t){
        .defs  = core_list_new(flow_def_t),
        .steps = core_list_new(flow_step_t),
        .items = core_list_new(pt_item_t),
    };
    core_dict_init(&pt->flow.writes, nullptr, nullptr);
    core_dict_init(&pt->flow.positions, nullptr, nullptr);
    pt->order  = core_list_new(summary_instr_ptr_t);
    pt->reader = PT_NO_READER;
    core_dict_init(&pt->readers, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (is_call(in)) note_callees(pt, in);
            if (step_matters(pt, in)) core_list_push(pt->order, in);
            core_dict_put(&pt->flow.positions, in,
                          (void *)((uintptr_t)b << 32 | i));
        }
    pt->dirty = core_alloc_array(bool, pt->order.len ? pt->order.len : 1);
    note_load_uses(pt);
    flow_build(pt);
}

static void
flow_free(pt_flow_t *flow)
{
    core_dict_foreach(&flow->writes, item) {
        pt_writes_t *writes = item->value;
        for (size_t w = 0; w < writes->len; w++)
            core_list_free(writes->data[w].items);
        core_list_free(*writes);
        core_free(writes);
    }
    core_dict_free(&flow->writes);
    core_dict_free(&flow->positions);
    core_list_free(flow->defs);
    core_list_free(flow->steps);
    core_list_free(flow->items);
    core_free(flow->block_steps);
    core_free(flow->kills);
    core_free(flow->block_in);
    core_free(flow->state);
}

static void
pt_free(pt_t *pt)
{
    for (size_t i = 0; i < pt->values.len; i++)
        core_list_free(pt->values.data[i]);
    for (size_t i = 0; i < pt->sites.len; i++) {
        core_list_free(pt->sites.data[i].contents);
        core_list_free(pt->sites.data[i].aliases);
    }
    core_list_free(pt->values);
    core_list_free(pt->sites);
    core_dict_free(&pt->value_index);
    core_dict_free(&pt->site_index);
    core_dict_free(&pt->load_uses);
    ir_reach_free(pt->reach);
    for (size_t i = 0; i < pt->calls.len; i++) {
        core_list_free(pt->calls.data[i].targets.targets);
        core_list_free(pt->calls.data[i].callees);
    }
    core_list_free(pt->calls);
    core_dict_free(&pt->call_index);
    core_list_free(pt->aliased);
    core_list_free(pt->described);
    core_list_free(pt->seen);
    core_list_free(pt->context);
    for (size_t s = 0; s < 3; s++) core_list_free(pt->scratch[s]);
    flow_free(&pt->flow);
    core_list_free(pt->order);
    core_dict_foreach(&pt->readers, item) {
        core_list_t(uint32_t) *readers = item->value;
        core_list_free(*readers);
        core_free(readers);
    }
    core_dict_free(&pt->readers);
    core_free(pt->dirty);
}

static bool
pt_solve(pt_t *pt)
{
    size_t count  = pt->order.len;
    size_t budget = (size_t)PT_ROUNDS * (count ? count : 1);
    size_t work   = 0;
    for (size_t i = 0; i < count; i++) pt->dirty[i] = true;
    pt->pending = count != 0;
    for (;;) {
        while (pt->pending) {
            pt->pending = false;
            for (size_t i = 0; i < count; i++) {
                if (!pt->dirty[i]) continue;
                if (work++ == budget) return false;
                pt->dirty[i] = false;
                pt->reader   = (uint32_t)i;
                pt->at       = pt->order.data[i];
                pt_step(pt, pt->at);
            }
        }
        pt->reader = PT_NO_READER;
        pt->at     = nullptr;
        spread_escapes(pt);
        if (!pt->pending) return true;
    }
}

static void
pt_record(pt_t *pt, ir_summary_t *summary)
{
    const ir_func_t *f = pt->f;
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            pt->at = f->blocks[b]->instrs[i];
            record_instr(pt, summary, pt->at);
        }
    pt->at = nullptr;
}

static void
summarize_paths(ir_summaries_t *summaries, ir_summary_t *summary,
                ir_func_t *f)
{
    pt_t pt;
    pt_init(&pt, summaries, f);
    if (pt_solve(&pt)) {
        pt.places = ir_func_places(f);
        pt.memssa = ir_func_memssa(f);
        pt_record(&pt, summary);
        summarize_results(&pt, summary);
        describe_heap(&pt, summary, &pt.described);
    }
    else
        add_unknown_path(summary, IR_SPATH_ACCESS);
    pt_free(&pt);
}

struct ir_points_to_t {
    pt_t pt;
};

bool
ir_pto_eq(const ir_pto_t *a, const ir_pto_t *b)
{
    return a->kind == b->kind && a->param == b->param && a->instr == b->instr
        && a->value == b->value && a->local == b->local
        && a->global == b->global;
}

bool
ir_pto_has(const ir_pto_list_t *objects, const ir_pto_t *object)
{
    for (size_t i = 0; i < objects->len; i++)
        if (ir_pto_eq(&objects->data[i], object)) return true;
    return false;
}

bool
ir_pto_add(ir_pto_list_t *objects, ir_pto_t object)
{
    if (ir_pto_has(objects, &object)) return false;
    core_list_push(*objects, object);
    return true;
}

static ir_pto_t
site_object(const pt_t *pt, uint32_t site)
{
    const pt_site_t *entry = &pt->sites.data[site];
    return (ir_pto_t){
        .kind  = IR_PTO_STORAGE,
        .local = entry->local,
        .value = entry->local ? nullptr : entry->value,
        .instr = entry->local || entry->value ? nullptr : entry->key,
    };
}

static ir_pto_t
external_object(const ir_spath_t *path, bool contents)
{
    switch (path->root) {
    case IR_SPLACE_PARAM:
        return (ir_pto_t){.kind = contents ? IR_PTO_CONTENTS : IR_PTO_PARAM,
                          .param = path->param};
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        return (ir_pto_t){.kind = IR_PTO_GLOBAL, .global = path->global};
    default:
        return (ir_pto_t){.kind = IR_PTO_UNKNOWN};
    }
}

static void
item_objects(const pt_t *pt, const pt_item_t *item, ir_pto_list_t *into)
{
    const pt_loc_t *loc = &item->loc;
    if (loc->site != PT_EXTERNAL) {
        ir_pto_add(into, site_object(pt, loc->site));
        return;
    }
    bool beyond = loc->path.depth || item->held;
    bool self   = !loc->path.depth && (!item->held || item->slot);
    if (self) ir_pto_add(into, external_object(&loc->path, false));
    if (beyond || (self && loc_deep(loc)))
        ir_pto_add(into, external_object(&loc->path, true));
}

static void
items_objects(const pt_t *pt, const pt_set_t *items, ir_pto_list_t *into)
{
    for (size_t i = 0; i < items->len; i++)
        item_objects(pt, &items->data[i], into);
}

static bool
object_site(const pt_t *pt, const ir_pto_t *object, uint32_t *site)
{
    const void *key = object->local ? (const void *)object->local
        : object->value ? (const void *)object->value
                        : (const void *)object->instr;
    bool  found = false;
    void *index = object->kind == IR_PTO_STORAGE
        ? core_dict_get((core_dict_t *)&pt->site_index, key, &found)
        : nullptr;
    *site = (uint32_t)(uintptr_t)index;
    return found;
}

ir_points_to_t *
ir_points_to_new(ir_summaries_t *summaries, const ir_func_t *function,
                 const ir_points_to_hooks_t *hooks)
{
    ir_points_to_t *points_to = core_alloc(ir_points_to_t);
    pt_init(&points_to->pt, summaries, function);
    points_to->pt.hooks = hooks;
    if (pt_solve(&points_to->pt)) return points_to;
    ir_points_to_free(points_to);
    return nullptr;
}

void
ir_points_to_free(ir_points_to_t *points_to)
{
    if (!points_to) return;
    pt_free(&points_to->pt);
    core_free(points_to);
}

void
ir_points_to_value(ir_points_to_t *points_to, const ir_instr_t *at,
                   const ir_value_t *value, ir_pto_list_t *into)
{
    pt_t    *pt    = &points_to->pt;
    pt_set_t items = core_list_new(pt_item_t);
    pt->at = at;
    pt_read(pt, value, &items);
    pt->at = nullptr;
    items_objects(pt, &items, into);
    core_list_free(items);
}

void
ir_points_to_loaded(ir_points_to_t *points_to, const ir_instr_t *at,
                    const ir_value_t *value, ir_pto_list_t *into)
{
    pt_t    *pt        = &points_to->pt;
    pt_set_t addresses = core_list_new(pt_item_t);
    pt_set_t loaded    = core_list_new(pt_item_t);
    pt->at = at;
    pt_read(pt, value, &addresses);
    pt_load(pt, &addresses, &loaded);
    pt->at = nullptr;
    items_objects(pt, &loaded, into);
    core_list_free(addresses);
    core_list_free(loaded);
}

static bool
alias_under(const pt_item_t *alias, const ir_pto_t *object)
{
    const ir_spath_t *path = &alias->loc.path;
    switch (object->kind) {
    case IR_PTO_PARAM:
    case IR_PTO_CONTENTS:
        return path->root == IR_SPLACE_PARAM && path->param == object->param;
    case IR_PTO_GLOBAL:
        return (path->root == IR_SPLACE_GLOBAL
                || path->root == IR_SPLACE_THREAD)
            && path->global == object->global;
    case IR_PTO_UNKNOWN:
        return path->root == IR_SPLACE_UNKNOWN;
    default:
        return false;
    }
}

void
ir_points_to_reach(ir_points_to_t *points_to, ir_pto_list_t *objects,
                   bool contents)
{
    const pt_t *pt = &points_to->pt;
    for (size_t i = 0; i < objects->len; i++) {
        ir_pto_t object = objects->data[i];
        uint32_t site   = 0;
        if (object_site(pt, &object, &site)) {
            items_objects(pt, &pt->sites.data[site].contents, objects);
            continue;
        }
        if (contents && object.kind == IR_PTO_PARAM)
            ir_pto_add(objects, (ir_pto_t){.kind  = IR_PTO_CONTENTS,
                                           .param = object.param});
        for (size_t a = 0; a < pt->aliased.len; a++) {
            const pt_site_t *entry = &pt->sites.data[pt->aliased.data[a]];
            for (size_t k = 0; k < entry->aliases.len; k++)
                if (alias_under(&entry->aliases.data[k], &object)) {
                    ir_pto_add(objects,
                               site_object(pt, pt->aliased.data[a]));
                    break;
                }
        }
    }
}

static void
argument_reach(ir_points_to_t *points_to, const ir_instr_t *call,
               uint64_t trusted, ir_pto_list_t *into)
{
    pt_t    *pt    = &points_to->pt;
    pt_set_t items = core_list_new(pt_item_t);
    for (uint32_t p = 0; p < ir_call_argument_count(call); p++)
        if (p >= 64 || !(trusted >> p & 1)) argument_items(pt, call, p,
                                                           &items);
    ir_pto_list_t reached = core_list_new(ir_pto_t);
    items_objects(pt, &items, &reached);
    ir_points_to_reach(points_to, &reached, true);
    for (size_t i = 0; i < reached.len; i++)
        ir_pto_add(into, reached.data[i]);
    core_list_free(reached);
    core_list_free(items);
}

bool
ir_points_to_call_writes(ir_points_to_t *points_to, const ir_instr_t *call,
                         uint64_t trusted, ir_pto_list_t *into)
{
    pt_t               *pt      = &points_to->pt;
    const pt_callees_t *callees = callees_of(pt, call);
    if (!callees || callees->targets.unknown) return false;
    pt->at = call;
    pt_set_t written = core_list_new(pt_item_t);
    for (size_t c = 0; c < callees->callees.len; c++) {
        const ir_summary_t *callee = callees->callees.data[c];
        for (size_t p = 0; p < callee->paths.len; p++) {
            const ir_spath_t *path = &callee->paths.data[p];
            if (!(path->flags & IR_SPATH_WRITE)) continue;
            if (path->root == IR_SPLACE_PARAM) {
                if (path->param >= 64 || !(trusted >> path->param & 1))
                    map_param_path(pt, call, path, &written);
            }
            else if (path->root == IR_SPLACE_UNKNOWN)
                argument_reach(points_to, call, trusted, into);
            else set_add(&written, item_at(path_loc(pt, path)));
        }
    }
    pt->at = nullptr;
    items_objects(pt, &written, into);
    core_list_free(written);
    return true;
}

static uint64_t
instr_position(const pt_t *pt, const ir_instr_t *in)
{
    bool      found = false;
    uintptr_t at    = (uintptr_t)core_dict_get(
        (core_dict_t *)&pt->flow.positions, in, &found);
    return found ? at : UINT64_MAX;
}

bool
ir_points_to_escapes(ir_points_to_t *points_to, const ir_pto_t *object,
                     const ir_instr_t **where)
{
    const pt_t *pt   = &points_to->pt;
    uint32_t    site = 0;
    *where = nullptr;
    if (!object_site(pt, object, &site)
        || !pt->sites.data[site].aliases.len)
        return false;
    uint64_t first = UINT64_MAX;
    const pt_set_t *aliases = &pt->sites.data[site].aliases;
    for (size_t a = 0; a < aliases->len; a++) {
        const ir_instr_t *origin = aliases->data[a].origin;
        uint64_t at = origin ? instr_position(pt, origin) : UINT64_MAX;
        if (origin && (!*where || at < first)) {
            *where = origin;
            first  = at;
        }
    }
    return true;
}

static bool
holds_object(const pt_t *pt, const pt_set_t *items, const ir_pto_t *object,
             ir_pto_list_t *scratch, ir_points_to_t *points_to)
{
    scratch->len = 0;
    items_objects(pt, items, scratch);
    ir_points_to_reach(points_to, scratch, false);
    return ir_pto_has(scratch, object);
}

void
ir_points_to_holders(ir_points_to_t *points_to, const ir_pto_t *object,
                     ir_points_to_holder_fn holder, void *context)
{
    const pt_t   *pt      = &points_to->pt;
    ir_pto_list_t scratch = core_list_new(ir_pto_t);
    core_dict_foreach((core_dict_t *)&pt->value_index, entry) {
        const pt_set_t *items =
            &pt->values.data[(uintptr_t)entry->value];
        if (holds_object(pt, items, object, &scratch, points_to))
            holder(context, entry->key, nullptr);
    }
    for (size_t s = 0; s < pt->sites.len; s++) {
        const pt_site_t *entry = &pt->sites.data[s];
        if (entry->local
            && holds_object(pt, &entry->contents, object, &scratch,
                            points_to))
            holder(context, nullptr, entry->local);
    }
    core_list_free(scratch);
}

static void
summarize_fields(ir_summaries_t *summaries, ir_summary_t *summary,
                 ir_func_t *f)
{
    core_dict_t pointers;
    core_dict_init(&pointers, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->n_operands < 2 || in->operands[1].kind != IR_OPD_NAME)
                continue;
            const ir_value_t *base = ir_operand_value(&in->operands[0]);
            if (in->op == IR_OP_FIELD && in->dest) {
                th_type *record = base && base->type
                    ? record_key(f, th_ind_referent(th_resolve(base->type)))
                    : nullptr;
                if (record) core_dict_put(&pointers, in->dest, in);
            }
            else if (in->op == IR_OP_EXTRACT || in->op == IR_OP_INSERT) {
                th_type *record = base ? record_key(f, base->type) : nullptr;
                if (record)
                    add_field(summary, record, in->operands[1].name,
                              in->op == IR_OP_EXTRACT ? IR_SFIELD_READ
                                                      : IR_SFIELD_WRITE);
            }
        }
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in     = f->blocks[b]->instrs[i];
            bool        opaque = opaque_use(summaries, in);
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_value(&in->operands[o]);
                if (!value) continue;
                bool        found = false;
                ir_instr_t *field = core_dict_get(&pointers, value, &found);
                if (found) {
                    const ir_value_t *base =
                        ir_operand_value(&field->operands[0]);
                    add_field(summary,
                              record_key(f, th_ind_referent(
                                  th_resolve(base->type))),
                              field->operands[1].name,
                              field_pointer_use(in, o));
                }
                th_type *record = opaque || generic_argument(summaries, in, o)
                    ? record_or_referent(f, value) : nullptr;
                if (record)
                    add_field(summary, record, (core_string_t){0},
                              IR_SFIELD_WHOLE);
            }
        }
    core_dict_free(&pointers);
}

typedef core_list_t(uint32_t) member_callers_t;

static member_callers_t *
member_callers(ir_summaries_t *summaries, ir_func_t *const *members,
               size_t count)
{
    member_callers_t *callers = core_alloc_array(member_callers_t, count);
    core_dict_t       index;
    core_dict_init(&index, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) {
        callers[i] = core_list_new(uint32_t);
        core_dict_put(&index, members[i], (void *)(uintptr_t)(i + 1));
    }
    for (size_t i = 0; i < count; i++) {
        core_list_t(summary_func_ptr_t) callees =
            core_list_new(summary_func_ptr_t);
        direct_callees(summaries, members[i], &callees);
        for (size_t c = 0; c < callees.len; c++) {
            bool      found = false;
            uintptr_t at    = (uintptr_t)core_dict_get(&index,
                                                       callees.data[c],
                                                       &found);
            member_callers_t *list = found ? &callers[at - 1] : nullptr;
            bool seen = !list;
            for (size_t k = 0; !seen && k < list->len; k++)
                seen = list->data[k] == i;
            if (!seen) core_list_push(*list, (uint32_t)i);
        }
        core_list_free(callees);
    }
    core_dict_free(&index);
    return callers;
}

static void
solve_component(ir_summaries_t *summaries, ir_func_t *const *members,
                size_t count)
{
    for (size_t i = 0; i < count; i++) {
        ir_summary_t *summary = ir_summary_new(summaries, members[i]);
        summary->flags = IR_SUMMARY_NO_RETURN | IR_SUMMARY_DIVERGES;
        summary->results_known = true;
        core_dict_put(&summaries->by_function, members[i], summary);
    }
    member_callers_t *callers = member_callers(summaries, members, count);
    bool cyclic = count > 1 || callers[0].len;
    bool *dirty = core_alloc_array(bool, count);
    bool *next_dirty = core_alloc_array(bool, count);
    for (size_t i = 0; i < count; i++) dirty[i] = true;
    bool changed = false;
    for (uint32_t round = 0; round < 64; round++) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            if (!dirty[i]) continue;
            ir_summary_t *summary = ir_summary_of(summaries, members[i]);
            ir_summary_t *next    = ir_summary_new(nullptr, members[i]);
            size_t scope = ir_analysis_scope_begin();
            if (summaries->escapes_only)
                summarize_divergence(summaries, next, members[i]);
            else summarize_function(summaries, next, members[i]);
            summarize_paths(summaries, next, members[i]);
            if (!summaries->escapes_only) {
                summarize_writes(summaries, next, members[i]);
                summarize_dependences(summaries, next, members[i]);
            }
            ir_analysis_scope_end(scope);
            bool differs = cyclic
                && (next->flags != summary->flags
                    || next->traps != summary->traps
                    || !same_effects(next, summary)
                    || !same_paths(next, summary)
                    || !same_dependences(next, summary));
            for (size_t c = 0; differs && c < callers[i].len; c++) {
                uint32_t caller = callers[i].data[c];
                if (caller <= i || !dirty[caller]) next_dirty[caller] = true;
            }
            changed |= differs;
            ir_summary_t previous = *summary;
            *summary = *next;
            *next    = previous;
            summary_free(next);
        }
        if (!cyclic || !changed) {
            changed = false;
            break;
        }
        bool *swap = dirty;
        dirty      = next_dirty;
        next_dirty = swap;
        for (size_t i = 0; i < count; i++) next_dirty[i] = false;
    }
    for (size_t i = 0; i < count; i++) core_list_free(callers[i]);
    core_free(callers);
    core_free(dirty);
    core_free(next_dirty);
    for (size_t i = 0; changed && i < count; i++)
        core_dict_put(&summaries->by_function, members[i],
                      ir_summary_conservative(summaries, members[i]));
    for (size_t i = 0; !summaries->escapes_only && i < count; i++)
        summarize_fields(summaries, ir_summary_of(summaries, members[i]),
                         members[i]);
    for (size_t i = 0; cyclic && !summaries->escapes_only && i < count;
         i++) {
        ir_summary_t *summary = ir_summary_of(summaries, members[i]);
        if (!(summary->flags & IR_SUMMARY_DIVERGES)) continue;
        recursion_t     recursion = {.summaries = summaries,
                                     .members = members, .count = count};
        size_t          scope = ir_analysis_scope_begin();
        ir_path_exits_t exits;
        if (ir_paths_flow(members[i], IR_PATH_UNPASSED, recursion_step,
                          &recursion, &exits)
            && (exits.returned || exits.failed))
            summary->flags |= IR_SUMMARY_UNBOUNDED_RECURSION;
        ir_analysis_scope_end(scope);
    }
}

static void
direct_callees(ir_summaries_t *summaries, ir_func_t *f,
               core_list_t(summary_func_ptr_t) *out)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (!is_call(in)) continue;
            call_targets_t targets;
            resolve_targets(summaries, in, &targets);
            for (size_t t = 0; t < targets.targets.len; t++)
                if (targets.targets.data[t]->n_blocks)
                    core_list_push(*out, targets.targets.data[t]);
            core_list_free(targets.targets);
        }
}

static uint32_t
dict_u32(core_dict_t *dict, const void *key, bool *found)
{
    return (uint32_t)(uintptr_t)core_dict_get(dict, (void *)key, found);
}

typedef struct {
    ir_func_t                      *f;
    core_list_t(summary_func_ptr_t) callees;
    size_t                          next;
} frame_t;
core_list_decl(frame_t);

static void
strongconnect(tarjan_t *t, ir_func_t *root)
{
    core_list_t(frame_t) frames = core_list_new(frame_t);
    frame_t first = {.f = root, .callees = core_list_new(summary_func_ptr_t)};
    direct_callees(t->summaries, root, &first.callees);
    core_dict_put(&t->index, root, (void *)(uintptr_t)(t->counter + 1));
    core_dict_put(&t->lowlink, root, (void *)(uintptr_t)(t->counter + 1));
    t->counter++;
    core_list_push(t->stack, root);
    core_dict_put(&t->on_stack, root, root);
    core_list_push(frames, first);
    while (frames.len) {
        frame_t *frame = &frames.data[frames.len - 1];
        if (frame->next < frame->callees.len) {
            ir_func_t *callee = frame->callees.data[frame->next++];
            bool found = false;
            dict_u32(&t->index, callee, &found);
            if (!found) {
                frame_t next = {.f = callee,
                                .callees = core_list_new(summary_func_ptr_t)};
                direct_callees(t->summaries, callee, &next.callees);
                core_dict_put(&t->index, callee,
                              (void *)(uintptr_t)(t->counter + 1));
                core_dict_put(&t->lowlink, callee,
                              (void *)(uintptr_t)(t->counter + 1));
                t->counter++;
                core_list_push(t->stack, callee);
                core_dict_put(&t->on_stack, callee, callee);
                core_list_push(frames, next);
            }
            else if (core_dict_contains(&t->on_stack, callee)) {
                uint32_t low = dict_u32(&t->lowlink, frame->f, &found);
                uint32_t idx = dict_u32(&t->index, callee, &found);
                if (idx < low)
                    core_dict_put(&t->lowlink, frame->f,
                                  (void *)(uintptr_t)idx);
            }
            continue;
        }
        ir_func_t *f = frame->f;
        bool found = false;
        uint32_t low = dict_u32(&t->lowlink, f, &found);
        uint32_t idx = dict_u32(&t->index, f, &found);
        if (low == idx) {
            core_list_t(summary_func_ptr_t) members =
                core_list_new(summary_func_ptr_t);
            ir_func_t *member;
            do {
                member = t->stack.data[--t->stack.len];
                core_dict_remove(&t->on_stack, member);
                core_list_push(members, member);
            } while (member != f);
            solve_component(t->summaries, members.data, members.len);
            core_list_free(members);
        }
        core_list_free(frame->callees);
        frames.len--;
        if (frames.len) {
            ir_func_t *parent = frames.data[frames.len - 1].f;
            uint32_t parent_low = dict_u32(&t->lowlink, parent, &found);
            if (low < parent_low)
                core_dict_put(&t->lowlink, parent, (void *)(uintptr_t)low);
        }
    }
    core_list_free(frames);
}

static void
collect_reinterpretations(ir_summaries_t *summaries,
                          ir_func_t *const *functions, size_t count)
{
    ir_ctx_t *ctx = count ? functions[0]->owner->ctx : nullptr;
    if (!ctx) return;
    ir_reinterpretations_open(ctx);
    core_list_t(summary_func_ptr_t) pending = core_list_new(summary_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) core_list_push(pending, functions[i]);
    while (pending.len) {
        ir_func_t *f = pending.data[--pending.len];
        if (!f || !core_dict_add(&seen, f, f)) continue;
        if (!f->n_blocks) {
            if (target_summary(summaries, f)->flags & IR_SUMMARY_REINTERPRETS)
                ir_reinterpretations_note_unseen(ctx);
            continue;
        }
        ir_reinterpretations_note_function(ctx, f);
        for (uint32_t b = 0; b < f->n_blocks; b++)
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *call = f->blocks[b]->instrs[i];
                if (call->op != IR_OP_CALL
                    && call->op != IR_OP_CALLBACK_INVOKE) continue;
                call_targets_t targets;
                resolve_targets(summaries, call, &targets);
                for (size_t t = 0; t < targets.targets.len; t++)
                    core_list_push(pending, targets.targets.data[t]);
                core_list_free(targets.targets);
            }
    }
    core_dict_free(&seen);
    core_list_free(pending);
    ir_reinterpretations_seal(ctx);
}

static ir_summaries_t *
compute_summaries(ir_func_t *const *functions, size_t count,
                  const ir_summary_hooks_t *hooks, bool escapes_only)
{
    ir_summaries_t *summaries = core_alloc(ir_summaries_t);
    summaries->hooks = hooks;
    summaries->escapes_only = escapes_only;
    summaries->owned = core_list_new(summary_ptr_t);
    core_dict_init(&summaries->by_function, nullptr, nullptr);
    core_dict_init(&summaries->names, nullptr, nullptr);
    core_dict_init(&summaries->type_memos, nullptr, nullptr);
    collect_reinterpretations(summaries, functions, count);
    tarjan_t t = {.summaries = summaries,
                  .stack = core_list_new(summary_func_ptr_t)};
    core_dict_init(&t.index, nullptr, nullptr);
    core_dict_init(&t.lowlink, nullptr, nullptr);
    core_dict_init(&t.on_stack, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) {
        bool found = false;
        dict_u32(&t.index, functions[i], &found);
        if (!found && functions[i]->n_blocks) strongconnect(&t, functions[i]);
    }
    core_dict_free(&t.index);
    core_dict_free(&t.lowlink);
    core_dict_free(&t.on_stack);
    core_list_free(t.stack);
    return summaries;
}

ir_summaries_t *
ir_summaries_compute(ir_func_t *const *functions, size_t count,
                     const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, false);
}

ir_summaries_t *
ir_escapes_compute(ir_func_t *const *functions, size_t count,
                   const ir_summary_hooks_t *hooks)
{
    return compute_summaries(functions, count, hooks, true);
}

void
ir_summaries_free(ir_summaries_t *summaries)
{
    if (!summaries) return;
    if (ir_analysis_summaries() == summaries) ir_analysis_bind_summaries(nullptr);
    for (size_t i = 0; i < summaries->owned.len; i++)
        summary_free(summaries->owned.data[i]);
    core_list_free(summaries->owned);
    core_dict_free(&summaries->by_function);
    core_dict_free(&summaries->names);
    type_memos_free(&summaries->type_memos);
    core_free(summaries);
}

enum { SUMMARY_CODEC_VERSION = 1 };

static void
put_u8(core_buffer_t *out, uint8_t value)
{
    core_buffer_append(out, (const char *)&value, 1);
}

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static void
put_text(core_buffer_t *out, const ir_summary_texts_t *texts,
         core_string_t text)
{
    if (texts) {
        put_u32(out, text.data && text.u8_bytes
            ? texts->intern(texts->context, text) : 0);
        return;
    }
    put_u32(out, text.data ? (uint32_t)text.u8_bytes : 0);
    if (text.data && text.u8_bytes)
        core_buffer_append(out, text.data, text.u8_bytes);
}

static void
put_global(core_buffer_t *out, const ir_summary_texts_t *texts,
           const ir_global_t *global)
{
    const ir_global_t *identity = global ? ir_global_identity(global)
                                         : nullptr;
    put_text(out, texts, identity && identity->owner
        ? ir_module_name(identity->owner) : (core_string_t){0});
    put_text(out, texts, identity ? identity->name : (core_string_t){0});
}

static void
put_place_type(core_buffer_t *out, const ir_summary_texts_t *texts,
               th_ctx *types, const ir_splace_t *place)
{
    bool named = place->kind == IR_SPLACE_GLOBAL
        || place->kind == IR_SPLACE_THREAD;
    put_text(out, texts, !named && types && place->type
        ? ir_types_text(types, place->type) : (core_string_t){0});
}

static void
put_splace(core_buffer_t *out, const ir_summary_texts_t *texts,
           th_ctx *types, const ir_splace_t *place)
{
    put_u8(out, place->kind);
    put_u32(out, place->param);
    put_global(out, texts, place->kind == IR_SPLACE_GLOBAL
            || place->kind == IR_SPLACE_THREAD ? place->global : nullptr);
    put_place_type(out, texts, types, place);
}

static void
put_path(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
         const ir_spath_t *path)
{
    put_u8(out, path->root);
    put_u32(out, path->param);
    put_global(out, texts, named_root(path->root) ? path->global : nullptr);
    put_text(out, texts, types && path->type
        ? ir_types_text(types, path->type) : (core_string_t){0});
    put_u8(out, path->flags);
    put_u8(out, path->output);
    put_u8(out, path->depth);
    for (uint8_t s = 0; s < path->depth; s++)
        put_text(out, texts, path->steps[s]);
}

static void
put_paths(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
          const path_list_t *paths)
{
    put_u32(out, (uint32_t)paths->len);
    for (size_t i = 0; i < paths->len; i++)
        put_path(out, texts, types, &paths->data[i]);
}

static void
put_pairs(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
          const pair_list_t *pairs)
{
    put_u32(out, (uint32_t)pairs->len);
    for (size_t i = 0; i < pairs->len; i++) {
        put_path(out, texts, types, &pairs->data[i].to);
        put_path(out, texts, types, &pairs->data[i].from);
    }
}

static bool
encode_summary(const ir_summary_t *summary, core_buffer_t *out,
               const ir_summary_texts_t *texts, bool positions)
{
    if (!summary || !out || (texts && !texts->intern)) return false;
    th_ctx *types = summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr;
    core_buffer_append(out, !positions ? "IRSI" : texts ? "IRSN" : "IRSM",
                       4);
    put_u8(out, SUMMARY_CODEC_VERSION);
    put_u32(out, summary->flags);
    put_u8(out, summary->traps);
    put_u32(out, positions ? (uint32_t)summary->trap_sites.len : 0);
    for (size_t i = 0; positions && i < summary->trap_sites.len; i++) {
        const ir_strap_site_t *site = &summary->trap_sites.data[i];
        put_u8(out, site->kind);
        put_u32(out, site->line);
        put_u32(out, site->column);
    }
    put_u32(out, (uint32_t)summary->inputs.len);
    for (size_t i = 0; i < summary->inputs.len; i++) {
        const ir_sinput_t *input = &summary->inputs.data[i];
        put_u8(out, input->kind);
        put_u32(out, input->param);
        put_splace(out, texts, types, &input->place);
        put_text(out, texts, input->kind == IR_SINPUT_CALL ? input->callee
                                                   : (core_string_t){0});
    }
    put_u32(out, (uint32_t)summary->outputs.len);
    for (size_t i = 0; i < summary->outputs.len; i++) {
        const ir_soutput_t *output = &summary->outputs.data[i];
        put_u8(out, output->kind);
        put_splace(out, texts, types, &output->place);
        put_u32(out, output->data_first);
        put_u32(out, output->data_count);
        put_u32(out, output->control_first);
        put_u32(out, output->control_count);
    }
    put_u32(out, (uint32_t)summary->dependences.len);
    for (size_t i = 0; i < summary->dependences.len; i++)
        put_u32(out, summary->dependences.data[i]);
    put_u32(out, (uint32_t)summary->effects.len);
    for (size_t i = 0; i < summary->effects.len; i++)
        put_text(out, texts, summary->effects.data[i]);
    put_paths(out, texts, types, &summary->paths);
    put_u8(out, (uint8_t)(summary->results_known
                          | summary->returns_external << 1));
    put_pairs(out, texts, types, &summary->results);
    put_pairs(out, texts, types, &summary->stores);
    put_u32(out, positions ? (uint32_t)summary->witnesses.len : 0);
    for (size_t i = 0; positions && i < summary->witnesses.len; i++) {
        const ir_switness_t *witness = &summary->witnesses.data[i];
        put_u32(out, witness->flag);
        put_text(out, texts, witness->module);
        put_text(out, texts, witness->name);
        put_u32(out, witness->line);
        put_u32(out, witness->column);
    }
    return true;
}

bool
ir_summary_encode(const ir_summary_t *summary, core_buffer_t *out)
{
    return encode_summary(summary, out, nullptr, true);
}

bool
ir_summary_encode_identity(const ir_summary_t *summary, core_buffer_t *out)
{
    return encode_summary(summary, out, nullptr, false);
}

bool
ir_summary_encode_with(const ir_summary_t *summary, core_buffer_t *out,
                       const ir_summary_texts_t *texts)
{
    return texts && encode_summary(summary, out, texts, true);
}

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         at;
    bool           failed;
    const ir_summary_texts_t    *texts;
    const ir_summary_resolver_t *resolver;
    th_ctx        *types;
} summary_reader_t;

static uint8_t
get_u8(summary_reader_t *r)
{
    if (r->failed || r->at >= r->size) {
        r->failed = true;
        return 0;
    }
    return r->data[r->at++];
}

static uint32_t
get_u32(summary_reader_t *r)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= (uint32_t)get_u8(r) << (8 * i);
    return value;
}

static core_string_t
get_text(summary_reader_t *r)
{
    if (r->texts) {
        uint32_t reference = get_u32(r);
        return reference && !r->failed
            ? r->texts->lookup(r->texts->context, reference)
            : (core_string_t){0};
    }
    uint32_t length = get_u32(r);
    if (r->failed || length > r->size - r->at) {
        r->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = length
        ? core_string_borrow((const char *)r->data + r->at, length)
        : (core_string_t){0};
    r->at += length;
    return text;
}

static const ir_global_t *
get_global(summary_reader_t *r)
{
    core_string_t module = get_text(r);
    core_string_t name   = get_text(r);
    return !r->failed && module.data && name.data && r->resolver
        && r->resolver->global
        ? r->resolver->global(r->resolver->context, module, name) : nullptr;
}

static void
get_place_type(summary_reader_t *r, th_ctx *types, ir_splace_t *place)
{
    core_string_t text = get_text(r);
    if (!place->global && types && text.data && text.u8_bytes)
        place->type = ir_type_from_text(types, text);
}

static ir_splace_t
get_splace(summary_reader_t *r, th_ctx *types)
{
    ir_splace_t place = {.kind = get_u8(r)};
    place.param  = get_u32(r);
    place.global = get_global(r);
    if (place.kind > IR_SPLACE_UNKNOWN) r->failed = true;
    if (place.global) place.type = place.global->type;
    get_place_type(r, types, &place);
    if ((place.kind == IR_SPLACE_GLOBAL || place.kind == IR_SPLACE_THREAD)
        && !place.global)
        place = (ir_splace_t){.kind = IR_SPLACE_UNKNOWN};
    return place;
}

static ir_spath_t
get_path(summary_reader_t *r)
{
    ir_spath_t path = {.root = (ir_splace_kind_t)get_u8(r)};
    path.param  = get_u32(r);
    path.global = get_global(r);
    core_string_t type = get_text(r);
    if (r->types && type.u8_bytes)
        path.type = ir_type_from_text(r->types, type);
    path.flags  = get_u8(r);
    path.output = get_u8(r);
    path.depth  = get_u8(r);
    if (path.depth > IR_SPATH_STEPS || path.root > IR_SPLACE_UNKNOWN)
        r->failed = true;
    for (uint8_t s = 0; !r->failed && s < path.depth; s++) {
        path.steps[s] = get_text(r);
        if (!path.steps[s].u8_bytes) r->failed = true;
    }
    return path;
}

static void
get_paths(summary_reader_t *r, path_list_t *paths)
{
    uint32_t count = get_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        ir_spath_t path = get_path(r);
        if (!r->failed) add_path_to(paths, &path);
    }
}

static void
get_pairs(summary_reader_t *r, pair_list_t *pairs)
{
    uint32_t count = get_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        ir_spath_t to   = get_path(r);
        ir_spath_t from = get_path(r);
        if (!r->failed) add_pair(pairs, &to, &from);
    }
}

static core_string_t
get_owned_text(summary_reader_t *r, const ir_func_t *function)
{
    core_string_t text = get_text(r);
    return text.u8_bytes && function && !r->failed
        ? ir_strdup(function->owner->ctx, text) : (core_string_t){0};
}

static ir_switness_t
get_witness(summary_reader_t *r, const ir_func_t *function)
{
    ir_switness_t witness = {.flag = get_u32(r)};
    witness.module = get_owned_text(r, function);
    witness.name   = get_owned_text(r, function);
    witness.line   = get_u32(r);
    witness.column = get_u32(r);
    const ir_summary_resolver_t *resolver = r->resolver;
    if (witness.module.data && witness.name.data && resolver
        && resolver->function)
        witness.callee = resolver->function(resolver->context,
                                            witness.module, witness.name);
    return witness;
}

static ir_summary_t *
decode_summary(ir_summaries_t *summaries, const ir_func_t *function,
               const uint8_t *data, size_t size,
               const ir_summary_resolver_t *resolver,
               const ir_summary_texts_t *texts)
{
    summary_reader_t r = {.data = data, .size = size, .texts = texts,
                          .resolver = resolver};
    if (!data || size < 5 || (texts && !texts->lookup)
        || memcmp(data, texts ? "IRSN" : "IRSM", 4)
        || data[4] != SUMMARY_CODEC_VERSION) return nullptr;
    r.at = 5;
    th_ctx *types = function && function->owner
        ? ir_module_types(function->owner) : nullptr;
    r.types = types;
    ir_summary_t *summary = ir_summary_new(nullptr, function);
    summary->flags      = get_u32(&r);
    summary->traps      = get_u8(&r) & IR_STRAP_ALL;
    uint32_t trap_sites = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < trap_sites; i++) {
        ir_strap_site_t site = {.kind = get_u8(&r) & IR_STRAP_ALL};
        site.line   = get_u32(&r);
        site.column = get_u32(&r);
        core_list_push(summary->trap_sites, site);
    }
    uint32_t inputs = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < inputs; i++) {
        ir_sinput_t input = {.kind = get_u8(&r)};
        input.param = get_u32(&r);
        input.place = get_splace(&r, types);
        core_string_t callee = get_owned_text(&r, function);
        if (input.kind > IR_SINPUT_CALL) r.failed = true;
        if (input.kind == IR_SINPUT_CALL) input.callee = callee;
        if ((input.kind == IR_SINPUT_PLACE
             && input.place.kind == IR_SPLACE_UNKNOWN)
            || (input.kind == IR_SINPUT_CALL && !input.callee.data))
            input = (ir_sinput_t){.kind = IR_SINPUT_UNKNOWN};
        core_list_push(summary->inputs, input);
    }
    uint32_t outputs = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < outputs; i++) {
        ir_soutput_t output = {.kind = get_u8(&r)};
        output.place         = get_splace(&r, types);
        output.data_first    = get_u32(&r);
        output.data_count    = get_u32(&r);
        output.control_first = get_u32(&r);
        output.control_count = get_u32(&r);
        if (output.kind > IR_SOUTPUT_PLACE) r.failed = true;
        core_list_push(summary->outputs, output);
    }
    uint32_t dependences = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < dependences; i++) {
        uint32_t input = get_u32(&r);
        if (input >= summary->inputs.len) r.failed = true;
        core_list_push(summary->dependences, input);
    }
    for (size_t i = 0; !r.failed && i < summary->outputs.len; i++) {
        const ir_soutput_t *output = &summary->outputs.data[i];
        if ((uint64_t)output->data_first + output->data_count > dependences
            || (uint64_t)output->control_first + output->control_count
                   > dependences)
            r.failed = true;
    }
    uint32_t effects = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < effects; i++) {
        core_string_t name = get_text(&r);
        if (!name.u8_bytes || !ir_summary_add_effect(summary, name))
            r.failed = true;
    }
    get_paths(&r, &summary->paths);
    uint8_t results = get_u8(&r);
    summary->results_known    = results & 1;
    summary->returns_external = results >> 1 & 1;
    get_pairs(&r, &summary->results);
    get_pairs(&r, &summary->stores);
    uint32_t witnesses = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < witnesses; i++) {
        ir_switness_t witness = get_witness(&r, function);
        if (!r.failed) core_list_push(summary->witnesses, witness);
    }
    if (r.failed || r.at != r.size) {
        summary_free(summary);
        return nullptr;
    }
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

ir_summary_t *
ir_summary_decode(ir_summaries_t *summaries, const ir_func_t *function,
                  const uint8_t *data, size_t size,
                  const ir_summary_resolver_t *resolver)
{
    return decode_summary(summaries, function, data, size, resolver,
                          nullptr);
}

ir_summary_t *
ir_summary_decode_with(ir_summaries_t *summaries, const ir_func_t *function,
                       const uint8_t *data, size_t size,
                       const ir_summary_resolver_t *resolver,
                       const ir_summary_texts_t *texts)
{
    return texts ? decode_summary(summaries, function, data, size, resolver,
                                  texts)
                 : nullptr;
}
