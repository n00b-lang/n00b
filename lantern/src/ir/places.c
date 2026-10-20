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
};

static bool
annotated_flag(const ir_value_t *value, core_string_t key, bool *flag)
{
    ir_annots_t *annots = ir_value_annots((ir_value_t *)value);
    ir_annot_t  *annot  = annots ? ir_annot_get(annots, key) : nullptr;
    if (!annot || annot->kind != IR_ANNOT_BOOL) return false;
    *flag = annot->b;
    return true;
}

static bool
private_storage(const ir_value_t *value)
{
    bool escapes = true;
    return annotated_flag(value, CORE_STRING_STATIC("ir.lifetime_escapes"),
                          &escapes)
        && !escapes;
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

static uint32_t
intern_place(ir_places_t *places, ir_place_t place)
{
    for (size_t i = 0; i < places->places.len; i++) {
        const ir_place_t *have = &places->places.data[i];
        if (have->kind != place.kind) continue;
        if (place.kind == IR_PLACE_HEAP
                ? same_type(places->module, have->type, place.type)
                : place.kind == IR_PLACE_UNKNOWN || have->root == place.root)
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
        return private_storage(value)
            ? intern_place(places, (ir_place_t){
                  .kind = IR_PLACE_LOCAL, .root = value,
                  .type = storage_type(value)})
            : heap_place(places, value);
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
        || !annotated_flag(value,
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
            add_id(ids, private_storage(value)
                ? intern_place(places, (ir_place_t){
                      .kind = IR_PLACE_LOCAL, .root = value,
                      .type = storage_type(value)})
                : heap_place(places, value));
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
    if (places->places.data[id].kind == IR_PLACE_LOCAL) return;
    ir_access_t access = {.instr = in, .kind = kind, .whole = true,
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
ir_places_may_alias(const ir_places_t *places, uint32_t a, uint32_t b)
{
    const ir_place_t *x = ir_place_at(places, a);
    const ir_place_t *y = ir_place_at(places, b);
    if (!x || !y) return true;
    if (a == b) return true;
    if (x->kind == IR_PLACE_LOCAL || y->kind == IR_PLACE_LOCAL) return false;
    if (x->kind == IR_PLACE_UNKNOWN || y->kind == IR_PLACE_UNKNOWN)
        return true;
    bool x_named = x->kind == IR_PLACE_GLOBAL || x->kind == IR_PLACE_THREAD;
    bool y_named = y->kind == IR_PLACE_GLOBAL || y->kind == IR_PLACE_THREAD;
    if (x_named && y_named) return false;
    uint64_t     lo       = a < b ? a : b;
    uint64_t     hi       = a < b ? b : a;
    void        *key      = (void *)(uintptr_t)(((hi << 32) | lo) + 1);
    core_dict_t *overlaps = (core_dict_t *)&places->overlaps;
    uintptr_t    known    = (uintptr_t)core_dict_get(overlaps, key, nullptr);
    if (known) return known == 2;
    bool overlap = types_overlap(places->module, x->type, y->type);
    core_dict_put(overlaps, key, (void *)(uintptr_t)(overlap ? 2 : 1));
    return overlap;
}
