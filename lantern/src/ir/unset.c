#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyze.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/places.h"
#include "ir/summary.h"
#include "ir/layout.h"
#include "lib/dict.h"

enum { LEAF_LIMIT = 512, DEPTH_LIMIT = 8 };

typedef struct node_t node_t;
struct node_t {
    core_string_t name;
    bool          inline_array;
    node_t       *parent;
    node_t      **children;
    uint32_t      n_children;
    uint32_t      first;
    uint32_t      count;
};

typedef node_t *node_ptr_t;
core_list_decl(node_ptr_t);

typedef struct {
    ir_local_t *local;
    node_t     *root;
    uint32_t    offset;
} tracked_t;

core_list_decl(tracked_t);

typedef enum : uint8_t { EVENT_READ, EVENT_SET, EVENT_ZERO } event_kind_t;

typedef struct {
    ir_instr_t  *instr;
    event_kind_t kind;
    uint32_t     tracked;
    node_t      *node;
} event_t;

core_list_decl(event_t);

typedef struct {
    ir_instr_t *instr;
    uint32_t    operand;
} use_t;

core_list_decl(use_t);
typedef core_list_t(use_t) use_list_t;

typedef struct {
    ir_summaries_t           *summaries;
    ir_func_t                *f;
    th_ctx                   *types;
    ir_places_t              *places;
    core_list_t(tracked_t)    tracked;
    core_list_t(event_t)      events;
    core_dict_t               uses;
    size_t                   *block_events;
    uint32_t                  bits;
} U;

static node_t *
leaves(U *u, th_type *type, core_string_t name, node_t *parent,
       uint32_t depth, uint32_t *next)
{
    node_t *node = core_alloc(node_t);
    *node = (node_t){.name = name, .parent = parent, .first = *next};
    th_type *record = type && depth < DEPTH_LIMIT
        ? th_fields_record(u->types, type) : nullptr;
    size_t n = record ? th_record_len(record) : 0;
    if (n && *next + n <= LEAF_LIMIT) {
        node->children = core_alloc_array(node_t *, n);
        for (size_t i = 0; i < n; i++) {
            const th_member *member = th_record_member(record, i);
            node_t *child = leaves(u, member->type, member->name, node,
                                   depth + 1, next);
            bool known = false;
            child->inline_array = !child->n_children
                && ir_layout_member_inline(u->f->owner, type, member->name,
                                           &known);
            node->children[node->n_children++] = child;
        }
    }
    if (!node->n_children) (*next)++;
    node->count = *next - node->first;
    return node;
}

static void
free_node(node_t *node)
{
    for (uint32_t i = 0; node && i < node->n_children; i++)
        free_node(node->children[i]);
    if (node) core_free(node->children);
    core_free(node);
}

static node_t *
child_named(node_t *node, core_string_t name)
{
    for (uint32_t i = 0; node && i < node->n_children; i++)
        if (core_string_eq(node->children[i]->name, name))
            return node->children[i];
    return nullptr;
}

static bool
place_node(U *u, uint32_t id, uint32_t *tracked, node_t **node,
           bool *element)
{
    const ir_place_t *chain[DEPTH_LIMIT * 4];
    uint32_t depth = 0;
    *element = false;
    for (const ir_place_t *place = ir_place_at(u->places, id); place;) {
        if (depth == sizeof(chain) / sizeof(*chain)) return false;
        chain[depth++] = place;
        if (!place->nested) break;
        place = ir_place_at(u->places, place->parent);
    }
    const ir_place_t *root = chain[depth - 1];
    if (root->kind != IR_PLACE_LOCAL && root->kind != IR_PLACE_HEAP)
        return false;
    for (uint32_t t = 0; t < u->tracked.len; t++) {
        if (u->tracked.data[t].local->value != root->root) continue;
        node_t *at = u->tracked.data[t].root;
        for (uint32_t i = depth - 1; i-- > 0;) {
            if (chain[i]->element) {
                *element = true;
                break;
            }
            node_t *child = child_named(at, chain[i]->step);
            if (!child) break;
            at = child;
        }
        *tracked = t;
        *node = at;
        return true;
    }
    return false;
}

static void
event(U *u, ir_instr_t *in, event_kind_t kind, uint32_t tracked,
      node_t *node)
{
    core_list_push(u->events, ((event_t){.instr = in, .kind = kind,
                                         .tracked = tracked, .node = node}));
}

static use_list_t *
uses_of(U *u, const ir_value_t *value)
{
    bool found = false;
    use_list_t *list = core_dict_get(&u->uses, (void *)value, &found);
    return found ? list : nullptr;
}

static void
read_value(U *u, ir_instr_t *in, const ir_value_t *value, uint32_t tracked,
           node_t *node, uint32_t depth)
{
    use_list_t *list = value ? uses_of(u, value) : nullptr;
    bool narrow = depth < DEPTH_LIMIT && node->n_children;
    for (size_t i = 0; narrow && list && i < list->len; i++) {
        ir_instr_t *use = list->data[i].instr;
        narrow = use->op == IR_OP_EXTRACT && list->data[i].operand == 0
            && use->n_operands > 1 && use->operands[1].kind == IR_OPD_NAME
            && use->dest && child_named(node, use->operands[1].name);
    }
    if (list && !list->len) return;
    if (!list || !narrow) {
        event(u, in, EVENT_READ, tracked, node);
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        ir_instr_t *use = list->data[i].instr;
        read_value(u, in, use->dest, tracked,
                   child_named(node, use->operands[1].name), depth + 1);
    }
}

static bool
reference_type(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *item = type ? th_option_item(type) : nullptr;
    if (item) type = th_resolve(item);
    return type && th_kind_of(type) == TH_INDIRECTION;
}

typedef struct {
    uint32_t                param;
    node_t                 *node;
    core_list_t(node_ptr_t) reads;
    core_list_t(node_ptr_t) sets;
    uint32_t                callees;
} callee_t;

static node_t *
path_node(node_t *node, const ir_spath_t *path)
{
    uint32_t i = path->steps && path->depth
        && core_string_eq(path->steps[0], CORE_STRING_STATIC("__value"));
    for (; node && path->steps && i < path->depth; i++) {
        node_t *child = child_named(node, path->steps[i]);
        if (!child) break;
        node = child;
    }
    return node;
}

static void
visit_callee(void *context, const ir_func_t *target,
             const ir_summary_t *summary)
{
    (void)target;
    callee_t *c = context;
    c->callees++;
    if (!summary) {
        core_list_push(c->reads, c->node);
        return;
    }
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if (path->root != IR_SPLACE_PARAM || path->param != c->param)
            continue;
        if (path->flags & IR_SPATH_READS_ENTRY)
            core_list_push(c->reads, path_node(c->node, path));
        if ((path->flags & IR_SPATH_WRITE)
            && (path->flags & IR_SPATH_MUST_SUCCESS))
            core_list_push(c->sets, path_node(c->node, path));
    }
}

static bool
consumed(const ir_value_t *argument)
{
    return argument->kind == IR_VAL_INSTR && argument->def
        && argument->def->op == IR_OP_CONSUME;
}

static void
call_events(U *u, ir_instr_t *in)
{
    for (uint32_t o = 1; o < in->n_operands; o++) {
        ir_value_t *argument = ir_operand_value(&in->operands[o]);
        const uint32_t *ids = nullptr;
        uint32_t count = 0;
        if (!argument || ir_operand_local(&in->operands[o])
            || !reference_type(argument->type) || consumed(argument)
            || !ir_places_of(u->places, argument, &ids, &count))
            continue;
        for (uint32_t p = 0; p < count; p++) {
            uint32_t tracked;
            node_t  *node;
            bool     element;
            if (!place_node(u, ids[p], &tracked, &node, &element)) continue;
            callee_t c = {.param = o - 1, .node = node,
                          .reads = core_list_new(node_ptr_t),
                          .sets = core_list_new(node_ptr_t)};
            if (!ir_summary_call_callees(u->summaries, in, visit_callee, &c))
                core_list_push(c.reads, node);
            for (size_t i = 0; i < c.reads.len; i++)
                event(u, in, EVENT_READ, tracked, c.reads.data[i]);
            for (size_t i = 0; count == 1 && !element && c.callees == 1
                 && i < c.sets.len; i++)
                event(u, in, EVENT_SET, tracked, c.sets.data[i]);
            core_list_free(c.reads);
            core_list_free(c.sets);
        }
    }
}

static bool
fills(ir_op_t op)
{
    return op == IR_OP_MEMSET || op == IR_OP_MEMCPY || op == IR_OP_MEMMOVE;
}

static bool
declaration_zero(ir_instr_t *in)
{
    ir_annot_t *zero = in->op == IR_OP_MEMSET ? ir_annot_get(&in->annots,
        CORE_STRING_STATIC("ir.declaration_zero")) : nullptr;
    return zero && zero->kind == IR_ANNOT_BOOL && zero->b;
}

static void
access_events(U *u, ir_instr_t *in)
{
    uint32_t first = 0;
    uint32_t n = ir_places_accesses_of(u->places, in, &first);
    for (uint32_t a = first; a < first + n; a++) {
        const ir_access_t *access = ir_places_access_at(u->places, a);
        const uint32_t    *ids    = ir_places_access_places(u->places,
                                                            access);
        for (uint32_t p = 0; p < access->count; p++) {
            uint32_t tracked;
            node_t  *node;
            bool     element;
            if (!place_node(u, ids[p], &tracked, &node, &element)) continue;
            if (access->kind == IR_ACCESS_LOAD) {
                bool whole_copy = access->direct && in->op == IR_OP_COPY
                    && in->dest_kind == IR_DEST_VALUE;
                if (whole_copy)
                    read_value(u, in, in->dest, tracked, node, 0);
                else event(u, in, EVENT_READ, tracked, node);
            }
            else if (access->count == 1 && !element && declaration_zero(in))
                event(u, in, EVENT_ZERO, tracked, node);
            else if (access->count == 1 && !element
                     && (access->whole || fills(in->op)))
                event(u, in, EVENT_SET, tracked, node);
        }
    }
}

static void
index_uses(U *u)
{
    core_dict_init(&u->uses, nullptr, nullptr);
    for (uint32_t b = 0; b < u->f->n_blocks; b++)
        for (uint32_t i = 0; i < u->f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = u->f->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_value(&in->operands[o]);
                if (!value || ir_operand_local(&in->operands[o])) continue;
                bool found = false;
                use_list_t *list = core_dict_get(&u->uses, (void *)value,
                                                 &found);
                if (!found) {
                    list = core_alloc(use_list_t);
                    *list = core_list_new(use_t);
                    core_dict_put(&u->uses, (void *)value, list);
                }
                core_list_push(*list, ((use_t){.instr = in, .operand = o}));
            }
        }
    for (uint32_t b = 0; b < u->f->n_blocks; b++)
        for (uint32_t i = 0; i < u->f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = u->f->blocks[b]->instrs[i];
            if (!in->dest || core_dict_contains(&u->uses, in->dest)) continue;
            use_list_t *list = core_alloc(use_list_t);
            *list = core_list_new(use_t);
            core_dict_put(&u->uses, in->dest, list);
        }
}

static void
free_uses(U *u)
{
    core_dict_foreach(&u->uses, item) {
        use_list_t *list = item->value;
        core_list_free(*list);
        core_free(list);
    }
    core_dict_free(&u->uses);
}

static bool
all_set(const uint64_t *state, uint32_t first, uint32_t count)
{
    for (uint32_t b = first; b < first + count; b++)
        if (!(state[b / 64] & (UINT64_C(1) << (b % 64)))) return false;
    return true;
}

static void
set_bits(uint64_t *state, uint32_t first, uint32_t count)
{
    for (uint32_t b = first; b < first + count; b++)
        state[b / 64] |= UINT64_C(1) << (b % 64);
}

static void
set_inline_arrays(uint64_t *state, uint32_t offset, const node_t *node)
{
    if (node->inline_array) set_bits(state, offset + node->first, 1);
    for (uint32_t i = 0; i < node->n_children; i++)
        set_inline_arrays(state, offset, node->children[i]);
}

static node_t *
unset_leaf(node_t *node, const uint64_t *state, uint32_t offset)
{
    for (uint32_t i = 0; i < node->n_children; i++)
        if (!all_set(state, offset + node->children[i]->first,
                     node->children[i]->count))
            return unset_leaf(node->children[i], state, offset);
    return node;
}

static core_string_t
node_path(const ir_local_t *local, const node_t *node)
{
    core_buffer_t text = {0};
    core_string_t name = ir_local_label(local);
    core_buffer_append_string(&text, name);
    core_free(name.data);
    const node_t *chain[DEPTH_LIMIT + 1];
    uint32_t depth = 0;
    for (const node_t *at = node; at && at->parent && depth <= DEPTH_LIMIT;
         at = at->parent)
        chain[depth++] = at;
    while (depth--) {
        core_buffer_append_string(&text, CORE_STRING_STATIC("."));
        core_buffer_append_string(&text, chain[depth]->name);
    }
    core_string_t path = core_string_dup(core_buffer_string(&text));
    core_buffer_release(&text);
    return path;
}

static void
transfer(U *u, uint32_t b, uint64_t *state, bool *reported,
         ir_unset_read_fn report, void *context)
{
    for (size_t e = u->block_events[b]; e < u->block_events[b + 1]; e++) {
        event_t *ev = &u->events.data[e];
        uint32_t offset = u->tracked.data[ev->tracked].offset;
        if (ev->kind == EVENT_SET) {
            set_bits(state, offset + ev->node->first, ev->node->count);
            continue;
        }
        if (ev->kind == EVENT_ZERO) {
            set_inline_arrays(state, offset, ev->node);
            continue;
        }
        if (!report || reported[ev->tracked]
            || all_set(state, offset + ev->node->first, ev->node->count))
            continue;
        reported[ev->tracked] = true;
        ir_local_t *local = u->tracked.data[ev->tracked].local;
        core_string_t path = node_path(local,
            unset_leaf(ev->node, state, offset));
        report(context, u->f, local, ev->instr, path);
        core_free(path.data);
    }
}

static bool
candidate(U *u, ir_local_t *local)
{
    return local->address_taken && !local->synthetic && !local->has_init
        && local->value && local->type
        && !th_zero_size(u->types, local->type);
}

void
ir_unset_locals(ir_summaries_t *summaries, ir_func_t *f,
                ir_unset_read_fn report, void *context)
{
    if (!f || !f->n_blocks || !report) return;
    U u = {.summaries = summaries, .f = f,
           .types = ir_module_types(f->owner),
           .tracked = core_list_new(tracked_t),
           .events = core_list_new(event_t)};
    for (uint32_t l = 0; l < f->n_locals; l++) {
        if (!candidate(&u, f->locals[l])) continue;
        uint32_t next = 0;
        node_t *root = leaves(&u, f->locals[l]->type, (core_string_t){0},
                              nullptr, 0, &next);
        core_list_push(u.tracked, ((tracked_t){.local = f->locals[l],
                                               .root = root,
                                               .offset = u.bits}));
        u.bits += root->count;
    }
    if (!u.tracked.len) {
        core_list_free(u.tracked);
        core_list_free(u.events);
        return;
    }
    size_t scope = ir_analysis_scope_begin();
    u.places = ir_func_places(f);
    ir_doms_t *doms = ir_func_doms(f);
    index_uses(&u);
    u.block_events = core_alloc_array(size_t, f->n_blocks + 1);
    for (uint32_t b = 0; u.places && b < f->n_blocks; b++) {
        u.block_events[b] = u.events.len;
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            access_events(&u, in);
            if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                call_events(&u, in);
        }
    }
    u.block_events[f->n_blocks] = u.events.len;

    uint32_t words = (u.bits + 63) / 64;
    size_t   bytes = words * sizeof(uint64_t);
    uint64_t *in_state = core_alloc_array(uint64_t, (size_t)words * f->n_blocks);
    uint64_t *gen = core_alloc_array(uint64_t, (size_t)words * f->n_blocks);
    uint64_t *scratch = core_alloc_array(uint64_t, words);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        transfer(&u, b, gen + (size_t)b * words, nullptr, nullptr, nullptr);
        if (b) memset(in_state + (size_t)b * words, 0xff, bytes);
    }
    for (bool changed = true; changed && doms;) {
        changed = false;
        for (uint32_t b = 1; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            if (!ir_block_reachable(doms, block)) continue;
            memset(scratch, 0xff, bytes);
            for (uint32_t p = 0; p < ir_pred_count(doms, block); p++) {
                ir_block_t *pred = ir_pred_at(doms, block, p);
                if (!ir_block_reachable(doms, pred)) continue;
                const uint64_t *in = in_state + (size_t)pred->index * words;
                const uint64_t *add = gen + (size_t)pred->index * words;
                for (uint32_t w = 0; w < words; w++)
                    scratch[w] &= in[w] | add[w];
            }
            uint64_t *have = in_state + (size_t)b * words;
            if (memcmp(have, scratch, bytes)) {
                memcpy(have, scratch, bytes);
                changed = true;
            }
        }
    }
    bool *reported = core_alloc_array(bool, u.tracked.len);
    for (uint32_t b = 0; doms && b < f->n_blocks; b++) {
        if (!ir_block_reachable(doms, f->blocks[b])) continue;
        memcpy(scratch, in_state + (size_t)b * words, bytes);
        transfer(&u, b, scratch, reported, report, context);
    }
    core_free(reported);
    core_free(scratch);
    core_free(in_state);
    core_free(gen);
    core_free(u.block_events);
    free_uses(&u);
    ir_analysis_scope_end(scope);
    for (size_t t = 0; t < u.tracked.len; t++)
        free_node(u.tracked.data[t].root);
    core_list_free(u.tracked);
    core_list_free(u.events);
}
