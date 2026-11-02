#include "internal.h"

typedef struct {
    const core_string_t *steps;
    uint32_t             depth;
} step_run_t;

static int  compare_paths(const ir_spath_t *a, const ir_spath_t *b);

static const core_string_t element_step = CORE_STRING_STATIC("[]");
static const core_string_t value_step   = CORE_STRING_STATIC("__value");
static const core_string_t length_step  = CORE_STRING_STATIC("__len");

static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static core_dict_t     step_table;
static core_dict_t     run_table;
static core_dict_t     run_starts;
static bool            step_table_ready;

static core_hash_value_t
run_hash(void *key)
{
    const step_run_t *run = key;
    return core_hash_raw((void *)run->steps,
                         run->depth * sizeof(core_string_t));
}

static bool
run_eq(const void *a, const void *b)
{
    const step_run_t *x = a, *y = b;
    return x->depth == y->depth
        && !memcmp(x->steps, y->steps, x->depth * sizeof(core_string_t));
}

static void
step_tables_ready(void)
{
    if (step_table_ready) return;
    core_dict_init_string(&step_table);
    core_dict_init(&run_table, run_hash, run_eq);
    core_dict_init(&run_starts, nullptr, nullptr);
    core_dict_put(&step_table, (void *)&element_step, (void *)&element_step);
    core_dict_put(&step_table, (void *)&value_step, (void *)&value_step);
    core_dict_put(&step_table, (void *)&length_step, (void *)&length_step);
    step_table_ready = true;
}

static core_string_t
step_intern_locked(core_string_t step)
{
    if (!step.u8_bytes || step.data == value_step.data
        || step.data == element_step.data || step.data == length_step.data)
        return step;
    bool           found = false;
    core_string_t *canon = core_dict_get(&step_table, &step, &found);
    if (!found) {
        canon = core_dict_string_key_copy(step);
        core_dict_put(&step_table, canon, canon);
    }
    return *canon;
}

static const core_string_t *
steps_join(const core_string_t *head, uint32_t head_depth,
           const core_string_t *tail, uint32_t tail_depth)
{
    uint32_t depth = head_depth + tail_depth;
    if (!depth) return nullptr;
    core_string_t  local[8];
    core_string_t *built = depth <= 8 ? local
                                      : core_alloc_array(core_string_t, depth);
    pthread_mutex_lock(&step_lock);
    step_tables_ready();
    bool      started = false;
    uintptr_t run_len = head_depth ? (uintptr_t)core_dict_get(
        &run_starts, (void *)head, &started) : 0;
    if (started && run_len >= head_depth)
        memcpy(built, head, head_depth * sizeof(core_string_t));
    else
        for (uint32_t s = 0; s < head_depth; s++)
            built[s] = step_intern_locked(head[s]);
    for (uint32_t s = 0; s < tail_depth; s++)
        built[head_depth + s] = step_intern_locked(tail[s]);
    step_run_t  probe = {built, depth};
    bool        found = false;
    step_run_t *run   = core_dict_get(&run_table, &probe, &found);
    if (!found) {
        core_string_t *owned = built;
        if (built == local) {
            owned = core_alloc_array(core_string_t, depth);
            memcpy(owned, local, depth * sizeof(core_string_t));
        }
        built  = local;
        run    = core_alloc(step_run_t);
        *run   = (step_run_t){owned, depth};
        core_dict_put(&run_table, run, run);
        core_dict_put(&run_starts, owned, (void *)(uintptr_t)depth);
    }
    pthread_mutex_unlock(&step_lock);
    if (built != local) core_free(built);
    return run->steps;
}

static const core_string_t *
steps_make(const core_string_t *steps, uint32_t depth)
{
    return steps_join(steps, depth, nullptr, 0);
}

static const core_string_t *
steps_append(const core_string_t *steps, uint32_t depth, core_string_t step)
{
    return steps_join(steps, depth, &step, 1);
}

static void
path_push(ir_spath_t *path, core_string_t step)
{
    if (path->flags & IR_SPATH_DEEP) return;
    path->steps = steps_append(path->steps, path->depth, step);
    path->depth++;
}

static inline bool
step_eq(core_string_t a, core_string_t b)
{
    if (a.data == b.data) return a.u8_bytes == b.u8_bytes;
    return a.u8_bytes == b.u8_bytes && a.u8_bytes
        && a.data[0] == b.data[0] && !memcmp(a.data, b.data, a.u8_bytes);
}

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
    if ((a->root == IR_SPLACE_PARAM || a->root == IR_SPLACE_SOURCE
         || a->root == IR_SPLACE_HEAP)
        && a->param != b->param)
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
    for (uint32_t s = 0; s < a->depth; s++)
        if (!core_string_eq(a->steps[s], b->steps[s])) return false;
    return true;
}

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

static bool
steps_prefix(uint32_t depth, const core_string_t *steps, uint32_t other_depth,
             const core_string_t *other)
{
    if (depth > other_depth) return false;
    if (steps == other) return true;
    for (uint32_t s = 0; s < depth; s++)
        if (!step_eq(steps[s], other[s])) return false;
    return true;
}

static uint32_t
first_deref(const core_string_t *steps, uint32_t depth)
{
    for (uint32_t s = 0; s < depth; s++)
        if (step_eq(steps[s], value_step)) return s;
    return depth;
}

static uint32_t
last_deref(const ir_spath_t *path)
{
    for (uint32_t s = path->depth; s > 0; s--)
        if (step_eq(path->steps[s - 1], value_step)) return s - 1;
    return UINT32_MAX;
}

static bool
steps_cover(uint32_t depth, bool deep, const core_string_t *steps,
            uint32_t other_depth, bool other_deep, const core_string_t *other)
{
    if (!deep && (depth != other_depth || other_deep)) return false;
    return steps_prefix(depth, steps, other_depth, other);
}

static int32_t
path_within(const ir_spath_t *outer, const ir_spath_t *inner)
{
    return steps_prefix(outer->depth, outer->steps, inner->depth,
                        inner->steps)
        ? (int32_t)outer->depth : -1;
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
        || ((path->flags & IR_SPATH_DEEP) && !(prefix->flags & IR_SPATH_DEEP))
        || ((prefix->flags ^ path->flags) & IR_SPATH_ROOT)
        || (outer && !same_type(outer, heap_type_of(path))))
        return false;
    return steps_prefix(prefix->depth, prefix->steps, path->depth,
                        path->steps);
}

static ir_spath_t
canonical_path(const ir_spath_t *path)
{
    ir_spath_t copy = *path;
    if (named_root(copy.root) && copy.global)
        copy.global = ir_global_identity((ir_global_t *)copy.global);
    return copy;
}

static bool
same_place(const ir_spath_t *a, const ir_spath_t *b)
{
    uint8_t shape = IR_SPATH_DEEP | IR_SPATH_ROOT;
    return !compare_path_roots(a, b) && a->depth == b->depth
        && ((a->flags ^ b->flags) & shape) == 0
        && steps_prefix(a->depth, a->steps, b->depth, b->steps);
}

static bool
access_covers(const ir_spath_t *a, const ir_spath_t *b)
{
    uint8_t may  = IR_SPATH_READ | IR_SPATH_WRITE | IR_SPATH_READS_ENTRY;
    uint8_t must = IR_SPATH_MUST_SUCCESS | IR_SPATH_MUST_FAILURE;
    return !(b->flags & must) && !(b->flags & may & ~a->flags)
        && path_covers(a, b);
}

static void
add_path_to(path_list_t *paths, const ir_spath_t *path)
{
    ir_spath_t copy = canonical_path(path);
    size_t first, last;
    root_range(paths, &copy, &first, &last);
    for (size_t i = first; i < last; i++) {
        if (!same_place(&paths->data[i], &copy)) continue;
        ir_spath_t merged = paths->data[i];
        merge_path_into(&merged, &copy);
        if (merged.flags == paths->data[i].flags
            && merged.type == paths->data[i].type)
            return;
        copy = merged;
        core_list_delete(*paths, i);
        root_range(paths, &copy, &first, &last);
        break;
    }
    for (size_t i = first; i < last; i++)
        if (access_covers(&paths->data[i], &copy)) return;
    size_t kept = first;
    for (size_t i = first; i < last; i++) {
        ir_spath_t *old = &paths->data[i];
        if (!access_covers(&copy, old)) paths->data[kept++] = *old;
    }
    for (size_t i = last; kept < last && i < paths->len; i++)
        paths->data[kept + i - last] = paths->data[i];
    paths->len -= last - kept;
    size_t at = first;
    while (at < kept && compare_paths(&paths->data[at], &copy) <= 0) at++;
    copy.steps = steps_make(copy.steps, copy.depth);
    core_list_insert(*paths, at, copy);
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

static int
compare_paths(const ir_spath_t *a, const ir_spath_t *b)
{
    int order = compare_path_roots(a, b);
    if (order) return order;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    for (uint32_t s = 0; s < a->depth; s++) {
        order = core_string_cmp(a->steps[s], b->steps[s]);
        if (order) return order;
    }
    return a->flags == b->flags ? 0 : a->flags < b->flags ? -1 : 1;
}

static bool
spath_covers(const ir_spath_t *a, const ir_spath_t *b)
{
    return same_path_root(a, b)
        && !((a->flags ^ b->flags) & IR_SPATH_ROOT)
        && steps_cover(a->depth, a->flags & IR_SPATH_DEEP, a->steps,
                       b->depth, b->flags & IR_SPATH_DEEP, b->steps);
}

static bool
same_paths(const ir_summary_t *a, const ir_summary_t *b)
{
    return same_path_list(&a->paths, &b->paths)
        && same_flows(&a->flows[IR_SFLOW_RESULTS], &b->flows[IR_SFLOW_RESULTS])
        && same_flows(&a->flows[IR_SFLOW_STORES], &b->flows[IR_SFLOW_STORES])
        && a->results_known == b->results_known
        && a->returns_external == b->returns_external;
}

static core_hash_value_t
spath_hash(void *key)
{
    const ir_spath_t *path    = key;
    uint64_t          head[3] = {
        (uint64_t)(uintptr_t)path->global, path->param,
        (uint64_t)path->root | (uint64_t)path->output << 8
            | (uint64_t)path->depth << 16 | (uint64_t)path->flags << 24};
    core_hash_value_t hash = core_hash_raw(head, sizeof(head));
    for (uint32_t s = 0; s < path->depth; s++)
        hash = hash * 31 + core_string_hash(path->steps[s]);
    return hash;
}

static bool
spath_eq(const void *a, const void *b)
{
    return !compare_paths(a, b);
}

int
ir_spath_compare(const ir_spath_t *a, const ir_spath_t *b)
{
    ir_spath_t left  = canonical_path(a);
    ir_spath_t right = canonical_path(b);
    left.flags  &= IR_SPATH_DEEP | IR_SPATH_ROOT;
    right.flags &= IR_SPATH_DEEP | IR_SPATH_ROOT;
    return compare_paths(&left, &right);
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

static void
parse_query_path(core_string_t text, core_string_t *root, ir_spath_t *path)
{
    size_t start = 0;
    bool   named = false;
    for (size_t at = 0; at <= text.u8_bytes; at++) {
        char byte = at < text.u8_bytes ? text.data[at] : '.';
        if (byte != '.' && byte != '[') continue;
        core_string_t token = core_string_slice(text, start, at - start);
        if (token.u8_bytes) {
            if (!named) *root = token;
            else path_push(path, token);
            named = true;
        }
        if (byte == '[') {
            path_push(path, element_step);
            if (at + 1 < text.u8_bytes && text.data[at + 1] == ']') at++;
        }
        start = at + 1;
    }
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
    parse_query_path(query, &root, &wanted);
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
        if (!absorbed[i]) into->data[kept++] = into->data[i];
    into->len = kept;
    core_free(absorbed);
}

void
ir_spath_list_release(ir_spath_list_t *paths)
{
    core_list_free(*paths);
}

static bool
param_object(const ir_func_t *function, const ir_spath_t *path,
             ir_spath_t *object, th_type **root)
{
    *object = *path;
    *root   = root_object_type(function, path, nullptr);
    th_type *pointee = path->root == IR_SPLACE_PARAM && *root && function
        ? pointee_type(function->owner, *root) : nullptr;
    if (!pointee) return true;
    *root = defined_type(function, pointee);
    if (path->depth && step_eq(path->steps[0], value_step)) {
        object->steps++;
        object->depth--;
        return true;
    }
    return !path->depth && (path->flags & IR_SPATH_DEEP);
}

bool
ir_spath_derefs(const ir_spath_t *path)
{
    return first_deref(path->steps, path->depth) < path->depth;
}

bool
ir_summary_path_inline(const ir_func_t *function, const ir_spath_t *path)
{
    if (path->flags & IR_SPATH_DEEP) return false;
    if (!path->depth) return true;
    ir_spath_t object;
    th_type   *root = nullptr;
    if (!param_object(function, path, &object, &root)) return false;
    if (!object.depth) return true;
    path = &object;
    th_type *defined = function && root ? defined_type(function, root)
                                        : nullptr;
    bool     known   = true;
    uint32_t inside  = function && root
        ? inline_depth(function, root,
                       defined && th_kind_of(defined) == TH_SEQUENCE, path,
                       &known)
        : 0;
    return function && root && known && inside == path->depth;
}

th_type *
ir_summary_path_type(const ir_func_t *function, const ir_spath_t *path,
                     uint32_t depth)
{
    th_type *root = path->root == IR_SPLACE_UNKNOWN
        ? (path->flags & IR_SPATH_TYPED ? path->type : nullptr)
        : root_object_type(function, path, nullptr);
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
                            .steps = steps_make(&value_step, 1), .depth = 1,
                            .flags = flags | IR_SPATH_DEEP | IR_SPATH_ROOT};
    case IR_SPLACE_HEAP:
        return (ir_spath_t){.root = IR_SPLACE_UNKNOWN, .type = place->type,
                            .flags = flags | IR_SPATH_DEEP
                                | (place->type ? IR_SPATH_TYPED : 0)};
    case IR_SPLACE_UNKNOWN:
    case IR_SPLACE_SOURCE:
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
    ir_spath_t referent;
    th_type   *root = nullptr;
    if (!param_object(function, path, &referent, &root)) return 0;
    path = &referent;
    bool     known  = true;
    uint32_t inside = path->depth;
    if (path->depth && function && root)
        inside = inline_depth(function, root, false, path, &known);
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
