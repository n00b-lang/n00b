#include <stdio.h>

#include "private/ir/internal.h"
#include "ir/places.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "th/typehash.h"

core_list_decl(ir_place_t);
core_list_decl(ir_access_t);
typedef const ir_value_t *place_value_ptr_t;
core_list_decl(place_value_ptr_t);

typedef struct {
    uint32_t first;
    uint32_t count;
} place_span_t;
core_list_decl(place_span_t);
core_list_decl(uint32_t);

struct ir_places_t {
    ir_module_t                      *module;
    core_list_t(ir_place_t)           places;
    core_list_t(uint32_t)             ids;
    core_list_t(ir_access_t)          accesses;
    core_list_t(place_span_t)         spans;
    core_dict_t                       resolved;
    core_dict_t                       by_instr;
    core_dict_t                       overlaps;
    core_dict_t                       exposed;
    bool                              escape_facts;
};

typedef th_type *place_type_ptr_t;
core_list_decl(place_type_ptr_t);

struct ir_reinterpretations_t {
    core_list_t(place_type_ptr_t) members;
    core_list_t(uint32_t)         components;
    core_list_t(place_type_ptr_t) universal;
    core_list_t(place_type_ptr_t) previous_members;
    core_list_t(uint32_t)         previous_components;
    core_list_t(place_type_ptr_t) previous_universal;
    core_dict_t                   answers;
    core_dict_t                   reach;
    core_dict_t                   hashes;
    bool                          sealed;
    bool                          unseen;
    bool                          previous_unseen;
};

typedef struct {
    core_list_t(uint32_t) components;
    bool                  universal;
} type_reach_t;

static bool
annotated_flag(ir_annots_t *annots, core_string_t key, bool *flag)
{
    ir_annot_t *annot = annots ? ir_annot_get(annots, key) : nullptr;
    if (!annot || annot->kind != IR_ANNOT_BOOL) return false;
    *flag = annot->b;
    return true;
}

static bool
proven_local(const ir_value_t *value)
{
    bool flag = false;
    if (value->kind == IR_VAL_LOCAL)
        return annotated_flag(ir_value_annots((ir_value_t *)value),
                              CORE_STRING_STATIC("ir.lifetime_escapes"), &flag)
            && !flag;
    return value->kind == IR_VAL_INSTR && value->def
        && value->def->op == IR_OP_ALLOC
        && annotated_flag(&value->def->annots,
                          CORE_STRING_STATIC("ir.allocation_local"), &flag)
        && flag;
}

static th_type *
storage_type(const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;
    if (type && th_kind_of(type) == TH_INDIRECTION) {
        th_type *referent = th_ind_referent(type);
        return referent ? th_resolve(referent) : type;
    }
    return type;
}

static bool
open_type(th_type *type)
{
    return !type || th_kind_of(type) == TH_VAR || th_kind_of(type) == TH_BOUND
        || th_kind_of(type) == TH_BINDER || th_kind_of(type) == TH_TRAIT;
}

static bool
same_type(ir_module_t *module, th_type *a, th_type *b)
{
    return a == b || (a && b && ir_type_eq(module, a, b));
}

static bool
contains(ir_module_t *module, th_type *outer, th_type *inner, uint32_t depth)
{
    outer = outer ? th_resolve(outer) : nullptr;
    if (open_type(outer) || depth > 16) return true;
    if (same_type(module, outer, inner)) return true;
    switch (th_kind_of(outer)) {
    case TH_NAMED: {
        th_type *definition = ir_type_definition(module, outer);
        return definition && definition != outer
            && contains(module, definition, inner, depth + 1);
    }
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(outer); i++) {
            const th_member *member = th_record_member(outer, i);
            if (!member || contains(module, member->type, inner, depth + 1))
                return true;
        }
        return false;
    case TH_SUM:
        for (size_t i = 0; i < th_sum_len(outer); i++)
            if (contains(module, th_sum_arm(outer, i), inner, depth + 1))
                return true;
        return false;
    case TH_SEQUENCE:
        return contains(module, th_seq_elem(outer), inner, depth + 1);
    default:
        return false;
    }
}

static bool
types_overlap(ir_module_t *module, th_type *a, th_type *b)
{
    return open_type(a) || open_type(b) || same_type(module, a, b)
        || contains(module, a, b, 0) || contains(module, b, a, 0);
}

static void
clear_answers(ir_reinterpretations_t *registry)
{
    core_dict_foreach(&registry->answers, item) {
        core_dict_free(item->value);
        core_free(item->value);
    }
    core_dict_free(&registry->answers);
    core_dict_init(&registry->answers, nullptr, nullptr);
    core_dict_foreach(&registry->reach, item) {
        type_reach_t *reach = item->value;
        core_list_free(reach->components);
        core_free(reach);
    }
    core_dict_free(&registry->reach);
    core_dict_init(&registry->reach, nullptr, nullptr);
}

static void
free_reinterpretations(void *pointer)
{
    ir_reinterpretations_t *registry = pointer;
    clear_answers(registry);
    core_dict_free(&registry->answers);
    core_dict_free(&registry->reach);
    core_dict_free(&registry->hashes);
    core_list_free(registry->members);
    core_list_free(registry->components);
    core_list_free(registry->universal);
    core_list_free(registry->previous_members);
    core_list_free(registry->previous_components);
    core_list_free(registry->previous_universal);
    core_free(registry);
}

static ir_reinterpretations_t *
reinterpretations(ir_ctx_t *ctx)
{
    if (!ctx->reinterpretations) {
        ir_reinterpretations_t *registry = core_alloc(ir_reinterpretations_t);
        registry->members             = core_list_new(place_type_ptr_t);
        registry->components          = core_list_new(uint32_t);
        registry->universal           = core_list_new(place_type_ptr_t);
        registry->previous_members    = core_list_new(place_type_ptr_t);
        registry->previous_components = core_list_new(uint32_t);
        registry->previous_universal  = core_list_new(place_type_ptr_t);
        core_dict_init(&registry->answers, nullptr, nullptr);
        core_dict_init(&registry->reach, nullptr, nullptr);
        core_dict_init(&registry->hashes, nullptr, nullptr);
        ctx->reinterpretations = registry;
        ir_ctx_on_free(ctx, free_reinterpretations, registry);
    }
    return ctx->reinterpretations;
}

void
ir_reinterpretations_open(ir_ctx_t *ctx)
{
    ir_reinterpretations_t *registry = reinterpretations(ctx);
    core_list_t(place_type_ptr_t) members    = registry->previous_members;
    core_list_t(uint32_t)         components = registry->previous_components;
    core_list_t(place_type_ptr_t) universal  = registry->previous_universal;
    registry->previous_members    = registry->members;
    registry->previous_components = registry->components;
    registry->previous_universal  = registry->universal;
    registry->previous_unseen     = registry->unseen;
    registry->members             = members;
    registry->components          = components;
    registry->universal           = universal;
    registry->members.len         = 0;
    registry->components.len      = 0;
    registry->universal.len       = 0;
    registry->sealed              = false;
    registry->unseen              = false;
}

static bool
same_types(const core_list_t(place_type_ptr_t) *a,
           const core_list_t(place_type_ptr_t) *b)
{
    if (a->len != b->len) return false;
    for (size_t i = 0; i < a->len; i++)
        if (a->data[i] != b->data[i]) return false;
    return true;
}

void
ir_reinterpretations_note_unseen(ir_ctx_t *ctx)
{
    reinterpretations(ctx)->unseen = true;
}

void
ir_reinterpretations_seal(ir_ctx_t *ctx)
{
    ir_reinterpretations_t *registry = reinterpretations(ctx);
    registry->sealed = true;
    bool same = registry->unseen == registry->previous_unseen
        && same_types(&registry->members, &registry->previous_members)
        && same_types(&registry->universal, &registry->previous_universal)
        && registry->components.len == registry->previous_components.len;
    for (size_t i = 0; same && i < registry->components.len; i++)
        same = registry->components.data[i]
            == registry->previous_components.data[i];
    if (!same) clear_answers(registry);
}

static uint32_t
member_component(ir_reinterpretations_t *registry, ir_module_t *module,
                 th_type *type)
{
    for (size_t i = 0; i < registry->members.len; i++)
        if (same_type(module, registry->members.data[i], type))
            return registry->components.data[i];
    uint32_t component = (uint32_t)registry->members.len;
    core_list_push(registry->members, type);
    core_list_push(registry->components, component);
    return component;
}

static void
note_pair(ir_reinterpretations_t *registry, ir_module_t *module, th_type *a,
          th_type *b)
{
    if (open_type(a) && open_type(b)) {
        registry->unseen = true;
        return;
    }
    if (open_type(a) || open_type(b)) {
        core_list_push(registry->universal, open_type(a) ? b : a);
        return;
    }
    uint32_t left  = member_component(registry, module, a);
    uint32_t right = member_component(registry, module, b);
    for (size_t i = 0; left != right && i < registry->components.len; i++)
        if (registry->components.data[i] == right)
            registry->components.data[i] = left;
}

static th_type *
pointee(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return nullptr;
    if (th_kind_of(type) == TH_INDIRECTION) {
        th_type *referent = th_ind_referent(type);
        return referent ? th_resolve(referent) : nullptr;
    }
    if (th_kind_of(type) == TH_SEQUENCE
        || th_builtin_applied(type, TH_BUILTIN_ANY, 0))
        return type;
    return nullptr;
}

static bool
reinterprets(ir_reinterpretations_t *registry, const ir_func_t *f)
{
    bool found = false;
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if ((in->op != IR_OP_BITCAST && in->op != IR_OP_CAST
                 && in->op != IR_OP_NOMINAL_CAST)
                || !in->dest || !in->n_operands) continue;
            ir_value_t *source = ir_operand_value(&in->operands[0]);
            th_type *from = source ? pointee(source->type) : nullptr;
            th_type *to   = pointee(in->dest->type);
            if (!from || !to
                || (th_kind_of(th_resolve(source->type)) != TH_INDIRECTION
                    && th_kind_of(th_resolve(in->dest->type))
                           != TH_INDIRECTION)
                || same_type(f->owner, from, to)) continue;
            found = true;
            if (!registry) return true;
            note_pair(registry, f->owner, from, to);
        }
    }
    return found;
}

bool
ir_reinterpretations_note_function(ir_ctx_t *ctx, const ir_func_t *f)
{
    return f && reinterprets(ctx ? reinterpretations(ctx) : nullptr, f);
}

static uint64_t
type_identity(ir_reinterpretations_t *registry, ir_module_t *module,
              th_type *type)
{
    uintptr_t known = (uintptr_t)core_dict_get(&registry->hashes, type,
                                               nullptr);
    if (known) return known;
    uint64_t hash = th_hash(module->ctx->types, type) | 1;
    core_dict_put(&registry->hashes, type, (void *)(uintptr_t)hash);
    return hash;
}

static const type_reach_t *
type_reach(ir_reinterpretations_t *registry, ir_module_t *module,
           th_type *type)
{
    void *key = (void *)(uintptr_t)type_identity(registry, module, type);
    type_reach_t *reach = core_dict_get(&registry->reach, key, nullptr);
    if (reach) return reach;
    reach = core_alloc(type_reach_t);
    reach->components = core_list_new(uint32_t);
    for (size_t i = 0; !reach->universal && i < registry->universal.len; i++)
        reach->universal = types_overlap(module, type,
                                         registry->universal.data[i]);
    for (size_t i = 0; !reach->universal && i < registry->members.len; i++) {
        uint32_t component = registry->components.data[i];
        bool     known     = false;
        for (size_t j = 0; !known && j < reach->components.len; j++)
            known = reach->components.data[j] == component;
        if (!known && types_overlap(module, type, registry->members.data[i]))
            core_list_push(reach->components, component);
    }
    core_dict_put(&registry->reach, key, reach);
    return reach;
}

static bool
computed_overlap(ir_reinterpretations_t *registry, ir_module_t *module,
                 th_type *a, th_type *b)
{
    if (types_overlap(module, a, b)) return true;
    const type_reach_t *left  = type_reach(registry, module, a);
    const type_reach_t *right = type_reach(registry, module, b);
    if (left->universal || right->universal) return true;
    for (size_t i = 0; i < left->components.len; i++)
        for (size_t j = 0; j < right->components.len; j++)
            if (left->components.data[i] == right->components.data[j])
                return true;
    return false;
}

static bool
registered_overlap(ir_module_t *module, th_type *a, th_type *b)
{
    ir_reinterpretations_t *registry = module->ctx->reinterpretations;
    if (!registry || !registry->sealed || registry->unseen) return true;
    a = a ? th_resolve(a) : nullptr;
    b = b ? th_resolve(b) : nullptr;
    if (open_type(a) || open_type(b)) return true;
    if (a == b) return true;
    uint64_t left  = type_identity(registry, module, a);
    uint64_t right = type_identity(registry, module, b);
    if (left == right) return true;
    if (left > right) {
        uint64_t swap = left;
        left  = right;
        right = swap;
        th_type *other = a;
        a = b;
        b = other;
    }
    core_dict_t *inner = core_dict_get(&registry->answers,
                                       (void *)(uintptr_t)left, nullptr);
    if (!inner) {
        inner = core_alloc(core_dict_t);
        core_dict_init(inner, nullptr, nullptr);
        core_dict_put(&registry->answers, (void *)(uintptr_t)left, inner);
    }
    void     *key   = (void *)(uintptr_t)right;
    uintptr_t known = (uintptr_t)core_dict_get(inner, key, nullptr);
    if (known) return known == 2;
    bool overlap = computed_overlap(registry, module, a, b);
    core_dict_put(inner, key, (void *)(uintptr_t)(overlap ? 2 : 1));
    return overlap;
}

bool
ir_places_types_overlap(ir_module_t *module, th_type *a, th_type *b)
{
    return !module || registered_overlap(module, a, b);
}

static uint32_t
intern_place(ir_places_t *places, ir_place_t place)
{
    bool named = place.kind == IR_PLACE_GLOBAL || place.kind == IR_PLACE_THREAD;
    const ir_global_t *global = named && place.root && place.root->global
        ? ir_global_identity(place.root->global) : nullptr;
    for (size_t i = 0; i < places->places.len; i++) {
        const ir_place_t *have = &places->places.data[i];
        if (have->kind != place.kind) continue;
        bool same_global = global && have->root && have->root->global
            && ir_global_identity(have->root->global) == global;
        if (place.kind == IR_PLACE_HEAP
                ? have->root == place.root
                    && same_type(places->module, have->type, place.type)
                : place.kind == IR_PLACE_UNKNOWN || have->root == place.root
                    || same_global)
            return (uint32_t)i;
    }
    core_list_push(places->places, place);
    return (uint32_t)(places->places.len - 1);
}

static uint32_t
heap_place(ir_places_t *places, const ir_value_t *value)
{
    return intern_place(places, (ir_place_t){
        .kind = IR_PLACE_HEAP, .type = storage_type(value)});
}

static uint32_t
object_place(ir_places_t *places, const ir_value_t *value)
{
    bool exposed = core_dict_contains(&places->exposed, (void *)value);
    if (!exposed || (places->escape_facts && proven_local(value)))
        return intern_place(places, (ir_place_t){
            .kind = IR_PLACE_LOCAL, .root = value,
            .type = storage_type(value), .exposed = exposed});
    return intern_place(places, (ir_place_t){
        .kind = IR_PLACE_HEAP, .root = value, .type = storage_type(value)});
}

static uint32_t
unknown_place(ir_places_t *places)
{
    return intern_place(places, (ir_place_t){.kind = IR_PLACE_UNKNOWN});
}

static uint32_t
storage_place(ir_places_t *places, const ir_value_t *value)
{
    switch (value->kind) {
    case IR_VAL_GLOBAL: {
        ir_global_t *global = value->global;
        bool thread = global && ir_global_storage_duration(global)
            == IR_STORAGE_DURATION_THREAD;
        return intern_place(places, (ir_place_t){
            .kind = thread ? IR_PLACE_THREAD : IR_PLACE_GLOBAL,
            .root = value, .type = storage_type(value)});
    }
    case IR_VAL_PARAM:
        return intern_place(places, (ir_place_t){
            .kind = IR_PLACE_PARAM, .root = value,
            .type = storage_type(value)});
    case IR_VAL_LOCAL:
        return object_place(places, value);
    default:
        return unknown_place(places);
    }
}

static bool
alias_edges(const ir_value_t *value, core_list_t(place_value_ptr_t) *pending)
{
    ir_annots_t *annots = ir_value_annots((ir_value_t *)value);
    ir_annot_t  *count  = annots
        ? ir_annot_get(annots, CORE_STRING_STATIC("ir.alias_count"))
        : nullptr;
    bool complete = false;
    if (!count || count->kind != IR_ANNOT_INT || count->i.neg
        || count->i.mag == 0 || count->i.mag > UINT32_MAX
        || !annotated_flag(annots,
                           CORE_STRING_STATIC("ir.alias_analysis_complete"),
                           &complete)
        || !complete) return false;
    for (uint64_t i = 0; i < count->i.mag; i++) {
        char key[48];
        int  length = snprintf(key, sizeof(key), "ir.alias%llu",
                               (unsigned long long)i);
        ir_annot_t *edge = length > 0
            ? ir_annot_get(annots, core_string_borrow(key, (size_t)length))
            : nullptr;
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v) return false;
    }
    for (uint64_t i = 0; i < count->i.mag; i++) {
        char key[48];
        int  length = snprintf(key, sizeof(key), "ir.alias%llu",
                               (unsigned long long)i);
        core_list_push(*pending, ir_annot_get(annots,
            core_string_borrow(key, (size_t)length))->v);
    }
    return true;
}

static void
add_id(core_list_t(uint32_t) *ids, uint32_t id)
{
    for (size_t i = 0; i < ids->len; i++)
        if (ids->data[i] == id) return;
    core_list_push(*ids, id);
}

static void
follow(core_list_t(place_value_ptr_t) *pending, const ir_instr_t *in,
       uint32_t operand)
{
    if (operand < in->n_operands) {
        ir_value_t *value = ir_operand_value(&in->operands[operand]);
        if (value) core_list_push(*pending, value);
    }
}

static void
resolve_into(ir_places_t *places, const ir_value_t *address,
             core_list_t(uint32_t) *ids)
{
    core_list_t(place_value_ptr_t) pending = core_list_new(place_value_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(pending, address);
    while (pending.len) {
        const ir_value_t *value = pending.data[--pending.len];
        if (!value || !core_dict_add(&seen, (void *)value, (void *)value))
            continue;
        if (value->kind != IR_VAL_INSTR || !value->def) {
            add_id(ids, storage_place(places, value));
            continue;
        }
        const ir_instr_t *in = value->def;
        switch (in->op) {
        case IR_OP_ALLOCA:
        case IR_OP_ALLOC:
        case IR_OP_ALLOC_USING:
        case IR_OP_ARRAY_NEW:
        case IR_OP_ARRAY_NEW_USING:
        case IR_OP_VARIANT_NEW:
        case IR_OP_VARIANT_NEW_USING:
            add_id(ids, object_place(places, value));
            break;
        case IR_OP_ADDRESS_OF:
            if (in->n_operands == 1 && ir_operand_value(&in->operands[0]))
                add_id(ids, storage_place(places,
                    ir_operand_value(&in->operands[0])));
            else
                add_id(ids, unknown_place(places));
            break;
        case IR_OP_COPY:
        case IR_OP_CONSUME:
        case IR_OP_RETAIN_REF:
        case IR_OP_CAST:
        case IR_OP_BITCAST:
        case IR_OP_BOX_RAW:
        case IR_OP_BOX_FROM_RAW:
        case IR_OP_NOMINAL_CAST:
        case IR_OP_TYPE_NARROW:
        case IR_OP_FIELD:
        case IR_OP_INDEX:
        case IR_OP_OFFSET:
        case IR_OP_OPTION_SOME:
        case IR_OP_OPTION_GET:
            follow(&pending, in, 0);
            break;
        case IR_OP_OPTION_GET_OR_ELSE:
            follow(&pending, in, 0);
            follow(&pending, in, 1);
            break;
        case IR_OP_SELECT:
            follow(&pending, in, 1);
            follow(&pending, in, 2);
            break;
        case IR_OP_PHI:
            for (uint32_t i = 0; i < in->n_operands; i++)
                follow(&pending, in, i);
            break;
        default:
            if (!alias_edges(value, &pending))
                add_id(ids, heap_place(places, value));
            break;
        }
    }
    core_dict_free(&seen);
    core_list_free(pending);
}

bool
ir_places_of(ir_places_t *places, const ir_value_t *address,
             const uint32_t **ids, uint32_t *count)
{
    if (!places || !address) return false;
    bool   found = false;
    size_t slot  = (size_t)(uintptr_t)core_dict_get(&places->resolved,
                                                    (void *)address, &found);
    if (!found) {
        core_list_t(uint32_t) set = core_list_new(uint32_t);
        resolve_into(places, address, &set);
        place_span_t span = {(uint32_t)places->ids.len, (uint32_t)set.len};
        for (size_t i = 0; i < set.len; i++)
            core_list_push(places->ids, set.data[i]);
        core_list_free(set);
        core_list_push(places->spans, span);
        slot = places->spans.len;
        core_dict_put(&places->resolved, (void *)address, (void *)slot);
    }
    place_span_t span = places->spans.data[slot - 1];
    *ids   = places->ids.data + span.first;
    *count = span.count;
    return true;
}

static bool
whole_object(const ir_value_t *address)
{
    for (uint32_t depth = 0; address && depth < 64; depth++) {
        if (address->kind != IR_VAL_INSTR || !address->def) return true;
        const ir_instr_t *in = address->def;
        switch (in->op) {
        case IR_OP_ADDRESS_OF:
        case IR_OP_ALLOCA:
        case IR_OP_ALLOC:
        case IR_OP_ALLOC_USING:
            return true;
        case IR_OP_COPY:
        case IR_OP_CONSUME:
        case IR_OP_RETAIN_REF:
        case IR_OP_CAST:
        case IR_OP_NOMINAL_CAST:
        case IR_OP_TYPE_NARROW:
            address = in->n_operands ? ir_operand_value(&in->operands[0])
                                     : nullptr;
            break;
        default:
            return false;
        }
    }
    return false;
}

static void
record_access(ir_places_t *places, ir_instr_t *in, ir_access_kind_t kind,
              uint32_t operand)
{
    ir_value_t *address = operand < in->n_operands
        ? ir_operand_value(&in->operands[operand]) : nullptr;
    const uint32_t *ids   = nullptr;
    uint32_t        count = 0;
    size_t          from  = SIZE_MAX;
    if (address && ir_places_of(places, address, &ids, &count) && count)
        from = (size_t)(ids - places->ids.data);
    else
        count = 1;
    ir_access_t access = {.instr = in, .kind = kind,
                          .whole = in->op == IR_OP_GLOBAL_STORE
                              || (in->op == IR_OP_STORE && whole_object(address)),
                          .first = (uint32_t)places->ids.len, .count = count};
    if (from == SIZE_MAX) {
        uint32_t unknown = unknown_place(places);
        access.first     = (uint32_t)places->ids.len;
        core_list_push(places->ids, unknown);
    }
    else {
        for (uint32_t i = 0; i < count; i++)
            core_list_push(places->ids, places->ids.data[from + i]);
    }
    core_list_push(places->accesses, access);
}

static void
record_storage(ir_places_t *places, ir_instr_t *in, ir_access_kind_t kind,
               const ir_value_t *storage)
{
    uint32_t id = storage_place(places, storage);
    ir_access_t access = {.instr = in, .kind = kind, .whole = true,
                          .direct = true,
                          .first = (uint32_t)places->ids.len, .count = 1};
    core_list_push(places->ids, id);
    core_list_push(places->accesses, access);
}

static bool
address_operand(const ir_instr_t *in, uint32_t operand)
{
    switch (in->op) {
    case IR_OP_ADDRESS_OF:
    case IR_OP_LOAD:
    case IR_OP_STORE:
    case IR_OP_GLOBAL_STORE:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_SET:
    case IR_OP_MEMSET:
    case IR_OP_DEALLOC:
    case IR_OP_FINALIZE:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
        return operand == 0;
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
    case IR_OP_MEMCMP:
        return operand <= 1;
    default:
        return false;
    }
}

static void
collect_storage_uses(ir_places_t *places, ir_instr_t *in)
{
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = ir_operand_value(&in->operands[o]);
        if (!value || address_operand(in, o)) continue;
        if (value->kind == IR_VAL_GLOBAL || value->kind == IR_VAL_LOCAL)
            record_storage(places, in, IR_ACCESS_LOAD, value);
    }
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local
        && in->dest_local->value)
        record_storage(places, in, IR_ACCESS_STORE, in->dest_local->value);
}

static bool
local_storage_root(const ir_value_t *value)
{
    if (value->kind == IR_VAL_LOCAL) return true;
    if (value->kind != IR_VAL_INSTR || !value->def) return false;
    switch (value->def->op) {
    case IR_OP_ALLOCA:
    case IR_OP_ALLOC:
    case IR_OP_ALLOC_USING:
    case IR_OP_ARRAY_NEW:
    case IR_OP_ARRAY_NEW_USING:
    case IR_OP_VARIANT_NEW:
    case IR_OP_VARIANT_NEW_USING:
        return true;
    default:
        return false;
    }
}

static bool
derives_address(ir_op_t op)
{
    switch (op) {
    case IR_OP_COPY:
    case IR_OP_CONSUME:
    case IR_OP_RETAIN_REF:
    case IR_OP_CAST:
    case IR_OP_BITCAST:
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_TYPE_NARROW:
    case IR_OP_FIELD:
    case IR_OP_INDEX:
    case IR_OP_OFFSET:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_OPTION_GET_OR_ELSE:
    case IR_OP_SELECT:
    case IR_OP_PHI:
        return true;
    default:
        return false;
    }
}

static void
expose_roots(core_dict_t *locals, const ir_value_t *address)
{
    core_list_t(place_value_ptr_t) pending = core_list_new(place_value_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(pending, address);
    while (pending.len) {
        const ir_value_t *value = pending.data[--pending.len];
        if (!value || value->kind != IR_VAL_INSTR || !value->def
            || !core_dict_add(&seen, (void *)value, (void *)value))
            continue;
        const ir_instr_t *in = value->def;
        if (local_storage_root(value)) {
            core_dict_put(locals, (void *)value, (void *)value);
            continue;
        }
        if (in->op == IR_OP_ADDRESS_OF) {
            const ir_value_t *storage = in->n_operands
                ? ir_operand_value(&in->operands[0]) : nullptr;
            if (storage && local_storage_root(storage))
                core_dict_put(locals, (void *)storage, (void *)storage);
            continue;
        }
        if (!derives_address(in->op)) continue;
        for (uint32_t o = 0; o < in->n_operands; o++)
            follow(&pending, in, o);
    }
    core_dict_free(&seen);
    core_list_free(pending);
}

static void
collect_exposure(const ir_func_t *f, core_dict_t *locals)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in->op == IR_OP_ADDRESS_OF
                || (derives_address(in->op)
                    && in->dest_kind == IR_DEST_VALUE)) continue;
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_value(&in->operands[o]);
                if (value && !address_operand(in, o))
                    expose_roots(locals, value);
            }
        }
    }
}

static void
collect_accesses(ir_places_t *places, ir_func_t *f)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in    = block->instrs[i];
            size_t      start = places->accesses.len;
            collect_storage_uses(places, in);
            switch (in->op) {
            case IR_OP_LOAD:
            case IR_OP_SEQUENCE_GET:
                record_access(places, in, IR_ACCESS_LOAD, 0);
                break;
            case IR_OP_STORE:
            case IR_OP_GLOBAL_STORE:
            case IR_OP_SEQUENCE_SET:
            case IR_OP_MEMSET:
            case IR_OP_DEALLOC:
            case IR_OP_FINALIZE:
                record_access(places, in, IR_ACCESS_STORE, 0);
                break;
            case IR_OP_MEMCPY:
            case IR_OP_MEMMOVE:
                record_access(places, in, IR_ACCESS_STORE, 0);
                record_access(places, in, IR_ACCESS_LOAD, 1);
                break;
            case IR_OP_MEMCMP:
                record_access(places, in, IR_ACCESS_LOAD, 0);
                record_access(places, in, IR_ACCESS_LOAD, 1);
                break;
            case IR_OP_ATOMIC_RMW:
            case IR_OP_ATOMIC_CAS:
                record_access(places, in, IR_ACCESS_LOAD, 0);
                record_access(places, in, IR_ACCESS_STORE, 0);
                break;
            default:
                break;
            }
            if (places->accesses.len > start)
                core_dict_put(&places->by_instr, in,
                              (void *)(uintptr_t)(start + 1));
        }
    }
}

ir_places_t *
ir_places_build(ir_func_t *f)
{
    if (!f) return nullptr;
    ir_places_t *places = core_alloc(ir_places_t);
    places->module   = f->owner;
    places->places   = core_list_new(ir_place_t);
    places->ids      = core_list_new(uint32_t);
    places->accesses = core_list_new(ir_access_t);
    places->spans    = core_list_new(place_span_t);
    core_dict_init(&places->resolved, nullptr, nullptr);
    core_dict_init(&places->by_instr, nullptr, nullptr);
    core_dict_init(&places->overlaps, nullptr, nullptr);
    core_dict_init(&places->exposed, nullptr, nullptr);
    annotated_flag(&f->annots, CORE_STRING_STATIC("ir.summary_escapes"),
                   &places->escape_facts);
    collect_exposure(f, &places->exposed);
    collect_accesses(places, f);
    return places;
}

void
ir_places_free(ir_places_t *places)
{
    if (!places) return;
    core_list_free(places->places);
    core_list_free(places->ids);
    core_list_free(places->accesses);
    core_list_free(places->spans);
    core_dict_free(&places->resolved);
    core_dict_free(&places->by_instr);
    core_dict_free(&places->overlaps);
    core_dict_free(&places->exposed);
    core_free(places);
}

uint32_t
ir_places_count(const ir_places_t *places)
{
    return places ? (uint32_t)places->places.len : 0;
}

const ir_place_t *
ir_place_at(const ir_places_t *places, uint32_t index)
{
    return places && index < places->places.len
        ? &places->places.data[index] : nullptr;
}

uint32_t
ir_places_access_count(const ir_places_t *places)
{
    return places ? (uint32_t)places->accesses.len : 0;
}

const ir_access_t *
ir_places_access_at(const ir_places_t *places, uint32_t index)
{
    return places && index < places->accesses.len
        ? &places->accesses.data[index] : nullptr;
}

uint32_t
ir_places_accesses_of(const ir_places_t *places, const ir_instr_t *in,
                      uint32_t *first)
{
    uintptr_t start = places ? (uintptr_t)core_dict_get(
        (core_dict_t *)&places->by_instr, (void *)in, nullptr) : 0;
    *first = start ? (uint32_t)(start - 1) : 0;
    uint32_t count = 0;
    while (start && *first + count < places->accesses.len
           && places->accesses.data[*first + count].instr == in)
        count++;
    return count;
}

const uint32_t *
ir_places_access_places(const ir_places_t *places, const ir_access_t *access)
{
    return places && access ? places->ids.data + access->first : nullptr;
}

bool
ir_places_access_private(const ir_places_t *places, const ir_access_t *access)
{
    if (!places || !access || !access->direct || access->count != 1)
        return false;
    const ir_place_t *place = ir_place_at(places,
                                          places->ids.data[access->first]);
    return place && place->kind == IR_PLACE_LOCAL && !place->exposed;
}

bool
ir_places_may_alias(const ir_places_t *places, uint32_t a, uint32_t b)
{
    const ir_place_t *x = ir_place_at(places, a);
    const ir_place_t *y = ir_place_at(places, b);
    if (!x || !y) return true;
    if (a == b) return true;
    if (x->kind == IR_PLACE_LOCAL || y->kind == IR_PLACE_LOCAL) {
        const ir_place_t *local = x->kind == IR_PLACE_LOCAL ? x : y;
        const ir_place_t *other = local == x ? y : x;
        return local->exposed && other->kind == IR_PLACE_UNKNOWN;
    }
    if (x->kind == IR_PLACE_UNKNOWN || y->kind == IR_PLACE_UNKNOWN)
        return true;
    bool x_named = x->kind == IR_PLACE_GLOBAL || x->kind == IR_PLACE_THREAD;
    bool y_named = y->kind == IR_PLACE_GLOBAL || y->kind == IR_PLACE_THREAD;
    if (x_named && y_named) {
        const ir_global_t *gx = x->root && x->root->global
            ? ir_global_identity(x->root->global) : nullptr;
        const ir_global_t *gy = y->root && y->root->global
            ? ir_global_identity(y->root->global) : nullptr;
        return !gx || !gy || gx == gy;
    }
    bool x_site = x->kind == IR_PLACE_HEAP && x->root;
    bool y_site = y->kind == IR_PLACE_HEAP && y->root;
    if (x_site && y_site) return false;
    if ((x_site || y_site) && (x_site ? y : x)->kind != IR_PLACE_HEAP)
        return false;
    uint64_t     lo       = a < b ? a : b;
    uint64_t     hi       = a < b ? b : a;
    void        *key      = (void *)(uintptr_t)(((hi << 32) | lo) + 1);
    core_dict_t *overlaps = (core_dict_t *)&places->overlaps;
    uintptr_t    known    = (uintptr_t)core_dict_get(overlaps, key, nullptr);
    if (known) return known == 2;
    bool overlap = registered_overlap(places->module, x->type, y->type);
    core_dict_put(overlaps, key, (void *)(uintptr_t)(overlap ? 2 : 1));
    return overlap;
}
