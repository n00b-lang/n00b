#include "internal.h"

typedef struct {
    core_string_t name;
    bool          inside;
    bool          known;
} pt_member_t;
core_list_decl(pt_member_t);
typedef core_list_t(pt_member_t) member_list_t;

typedef struct {
    th_type             *root;
    const core_string_t *steps;
    uint32_t             depth;
} slot_key_t;

typedef struct {
    slot_key_t key;
    th_type   *raw;
    th_type   *slot;
    bool       maybe_pointer;
    bool       pointer;
} slot_info_t;

static bool is_call(const ir_instr_t *in);
static th_type *record_of(const ir_func_t *f, th_type *type);


static void typed_placement(pt_t *pt, uint32_t site, pt_item_t *item);

static bool slot_pointer(const pt_t *pt, const pt_item_t *item);

static bool
is_call(const ir_instr_t *in)
{
    return in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE;
}

static th_type *
definition_in(ir_module_t *module, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    th_ctx *types = module ? ir_ctx_types(module->ctx) : nullptr;
    while (type) {
        th_type *item = th_option_item(type);
        if (!item && types)
            item = th_builtin_item(types, type, TH_BUILTIN_ATOMIC, 0);
        if (!item) break;
        type = th_resolve(item);
    }
    if (type && th_kind_of(type) == TH_NAMED) {
        th_type *definition = module ? ir_type_definition(module, type)
                                     : nullptr;
        type = definition && definition != type
            ? definition_in(module, definition) : nullptr;
    }
    return type;
}

static th_type *
pointee_type(ir_module_t *module, th_type *type)
{
    th_type *current = definition_in(module, type);
    uint64_t extent  = 0;
    if (!current) return nullptr;
    if (th_kind_of(current) == TH_INDIRECTION)
        return th_ind_referent(current);
    if (th_kind_of(current) == TH_SEQUENCE && !th_seq_extent(current, &extent))
        return current;
    return nullptr;
}

static bool
pointer_class(ir_module_t *module, th_type *type)
{
    return pointee_type(module, type)
        || ir_machine_of(module, type).class == IR_CLASS_PTR;
}

static bool
generic_type(th_type *type)
{
    switch (th_kind_of(th_resolve(type))) {
    case TH_VAR:
    case TH_BOUND:
    case TH_BINDER:
    case TH_TRAIT:
        return true;
    default:
        return false;
    }
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

static void
collect_leaves(const ir_func_t *f, th_type *type, ir_spath_t prefix,
               path_list_t *out)
{
    th_type *record = record_of(f, type);
    if (!record) {
        core_list_push(*out, prefix);
        return;
    }
    for (size_t i = 0; i < th_record_len(record); i++) {
        const th_member *member = th_record_member(record, i);
        ir_spath_t       next   = prefix;
        path_push(&next, member->name);
        collect_leaves(f, member->type, next, out);
    }
}

static path_list_t
place_leaves(const ir_func_t *f, const ir_splace_t *place)
{
    path_list_t leaves = core_list_new(ir_spath_t);
    collect_leaves(f, place->type, (ir_spath_t){0}, &leaves);
    return leaves;
}

static const ir_value_t *
field_chain(const ir_value_t *address, ir_op_t step, ir_spath_t *path)
{
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    while (address && address->kind == IR_VAL_INSTR && address->def
           && address->def->op == step && address->def->n_operands == 2
           && address->def->operands[1].kind == IR_OPD_NAME) {
        core_list_push(names, address->def->operands[1].name);
        address = ir_operand_value(&address->def->operands[0]);
    }
    for (size_t i = 0; i < names.len / 2; i++) {
        core_string_t swap              = names.data[i];
        names.data[i]                   = names.data[names.len - 1 - i];
        names.data[names.len - 1 - i]   = swap;
    }
    path->steps = steps_join(names.data, (uint32_t)names.len, path->steps,
                             path->depth);
    path->depth += (uint32_t)names.len;
    core_list_free(names);
    return address;
}

static core_hash_value_t
slot_key_hash(void *key)
{
    const slot_key_t *k = key;
    return core_hash_word((void *)k->root)
         ^ (core_hash_word((void *)k->steps) * 31u)
         ^ (core_hash_value_t)k->depth;
}

static bool
slot_key_eq(const void *a, const void *b)
{
    const slot_key_t *x = a, *y = b;
    return x->root == y->root && x->steps == y->steps && x->depth == y->depth;
}

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
    core_dict_init(&memo->definitions, nullptr, nullptr);
    core_dict_init(&memo->fields, nullptr, nullptr);
    core_dict_init(&memo->slots, slot_key_hash, slot_key_eq);
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
        core_dict_foreach(&memo->fields, entry) {
            core_dict_t *fields = entry->value;
            core_dict_foreach(fields, field) {
                core_dict_string_key_free(field->key);
            }
            core_dict_free(fields);
            core_free(fields);
        }
        core_dict_free(&memo->members);
        core_dict_free(&memo->pointer_types);
        core_dict_free(&memo->definitions);
        core_dict_free(&memo->fields);
        core_dict_foreach(&memo->slots, entry) {
            core_free(entry->value);
        }
        core_dict_free(&memo->slots);
        core_free(memo);
    }
    core_dict_free(memos);
}

static th_type *
memo_defined(const pt_t *pt, th_type *type)
{
    if (!type) return nullptr;
    bool     found   = false;
    th_type *defined = core_dict_get(&pt->types->definitions, type, &found);
    if (found) return defined;
    defined = defined_type(pt->f, type);
    core_dict_put(&pt->types->definitions, type, defined);
    return defined;
}

static th_type *
field_type(const pt_t *pt, th_type *record, core_string_t step)
{
    bool         found  = false;
    core_dict_t *fields = core_dict_get(&pt->types->fields, record, &found);
    if (!found) {
        fields = core_alloc(core_dict_t);
        core_dict_init_string(fields);
        core_dict_put(&pt->types->fields, record, fields);
    }
    th_type *type = core_dict_get(fields, &step, &found);
    if (found) return type;
    type = th_record_lookup(record, step);
    core_dict_put(fields, core_dict_string_key_copy(step), type);
    return type;
}

static th_type *
step_through(const pt_t *pt, ir_module_t *module, th_type *type,
             core_string_t step)
{
    th_type *current = pt ? memo_defined(pt, type)
                          : definition_in(module, type);
    if (!current) return nullptr;
    if (step_eq(step, value_step)) return pointee_type(module, current);
    if (step_eq(step, element_step))
        return th_kind_of(current) == TH_SEQUENCE
            ? th_seq_elem(current) : nullptr;
    if (th_kind_of(current) != TH_RECORD) return nullptr;
    return pt ? field_type(pt, current, step)
              : th_record_lookup(current, step);
}

static const slot_info_t *
slot_info(const pt_t *pt, const pt_loc_t *loc, uint32_t depth)
{
    th_type *root = loc->site == PT_EXTERNAL
        ? root_object_type(pt->f, &loc->path, pt)
        : pt->sites.data[loc->site].type;
    slot_key_t   probe = {root, loc->path.steps, depth};
    bool         found = false;
    slot_info_t *info  = core_dict_get(&pt->types->slots, &probe, &found);
    if (found) return info;
    ir_module_t *module = pt->f->owner;
    th_type     *raw    = root;
    for (uint32_t s = 0; raw && s < depth; s++)
        raw = step_through(pt, module, raw, loc->path.steps[s]);
    th_type *slot = memo_defined(pt, raw);
    info  = core_alloc(slot_info_t);
    *info = (slot_info_t){
        .key           = probe,
        .raw           = raw,
        .slot          = slot,
        .maybe_pointer = !raw || pointer_class(module, raw)
                      || generic_type(raw),
        .pointer       = slot && pointer_class(module, slot),
    };
    core_dict_put(&pt->types->slots, &info->key, info);
    return info;
}

static th_type *
loc_type(const pt_t *pt, const pt_loc_t *loc, uint32_t depth)
{
    return slot_info(pt, loc, depth)->slot;
}

static bool
same_object_type(const pt_t *pt, th_type *a, th_type *b)
{
    return same_type(a, b)
        || (a && b && ir_type_eq(pt->f->owner, a, b));
}

static th_type *
owner_type(const pt_t *pt, th_type *type)
{
    if (type && th_kind_of(type) == TH_INDIRECTION)
        type = memo_defined(pt, th_ind_referent(type));
    return type;
}

static void
typed_placement(pt_t *pt, uint32_t site, pt_item_t *item)
{
    const pt_site_t *entry = &pt->sites.data[site];
    th_type         *type  = entry->type;
    uint32_t         s     = 0;
    if (!type || !item->depth) return;
    if (entry->elements) {
        th_type *sequence = memo_defined(pt, type);
        if (!sequence || th_kind_of(sequence) != TH_SEQUENCE) return;
        if (step_eq(item->steps[0], length_step) && item->depth == 1) return;
        if (!step_eq(item->steps[0], element_step)) s = 0;
        else {
            type = th_seq_elem(sequence);
            s    = 1;
        }
        if (!s) {
            item->depth = 0;
            item->deep  = true;
            return;
        }
    }
    for (; s < item->depth; s++) {
        th_type *current = memo_defined(pt, type);
        if (!current) return;
        core_string_t step = item->steps[s];
        th_kind       kind = th_kind_of(current);
        if (kind == TH_SUM || kind == TH_NAMED || kind == TH_VAR
            || kind == TH_BOUND || kind == TH_BINDER) return;
        th_type *next = nullptr;
        if (kind == TH_SEQUENCE && step_eq(step, element_step))
            next = th_seq_elem(current);
        else if (kind == TH_SEQUENCE && step_eq(step, length_step)
                 && s + 1 == item->depth)
            return;
        else if (kind == TH_RECORD)
            next = field_type(pt, current, step);
        if (!next) break;
        type = next;
    }
    if (s < item->depth) {
        item->depth = s;
        item->deep  = true;
    }
}

static bool
recursive_step(const pt_t *pt, const pt_loc_t *loc, uint32_t at)
{
    th_type *there = owner_type(pt, loc_type(pt, loc, at));
    th_type *here  = there
        ? owner_type(pt, loc_type(pt, loc, loc->path.depth))
        : nullptr;
    return !here || same_object_type(pt, here, there);
}

static bool
recursive_object(const pt_t *pt, pt_loc_t *loc, th_type *slot)
{
    th_type *object = slot ? memo_defined(pt,
                                          pointee_type(pt->f->owner,
                                                       slot))
                           : nullptr;
    if (!object) return false;
    for (uint32_t k = 1; k <= loc->path.depth; k++) {
        if (!step_eq(loc->path.steps[k - 1], value_step)) continue;
        if (!same_object_type(pt, loc_type(pt, loc, k), object)) continue;
        loc->path.flags |= IR_SPATH_DEEP;
        return true;
    }
    path_push(&loc->path, value_step);
    return true;
}

static bool
step_fits(const pt_t *pt, th_type *type, core_string_t step)
{
    uint32_t kind = th_kind_of(type);
    if (kind != TH_RECORD && kind != TH_SEQUENCE && kind != TH_INDIRECTION
        && kind != TH_BITINT)
        return true;
    if (step_eq(step, value_step))
        return pointer_class(pt->f->owner, type);
    if (step_eq(step, element_step) || step_eq(step, length_step))
        return kind == TH_SEQUENCE;
    return kind == TH_RECORD && field_type(pt, type, step);
}

static bool
layout_member_inline(const ir_func_t *f, th_type *named, th_type *record,
                     core_string_t name, bool *known)
{
    bool         inside = ir_layout_member_inline(f->owner, record, name,
                                                  known);
    ir_module_t *owner  = *known || !named
        ? nullptr : ir_type_definition_owner(f->owner, named);
    if (owner && owner != f->owner)
        inside = ir_layout_member_inline(owner, record, name, known);
    return *known && inside;
}

static bool
member_inline(const ir_func_t *f, pt_t *pt, th_type *named, th_type *record,
              core_string_t name, bool *known)
{
    bool           found   = false;
    member_list_t *members = core_dict_get(&pt->types->members, record,
                                           &found);
    for (size_t m = 0; members && m < members->len; m++)
        if (core_string_eq(members->data[m].name, name)) {
            *known = members->data[m].known;
            return members->data[m].inside;
        }
    if (!members) {
        members  = core_alloc(member_list_t);
        *members = core_list_new(pt_member_t);
        core_dict_put(&pt->types->members, record, members);
    }
    bool inside = layout_member_inline(f, named, record, name, known);
    core_list_push(*members, ((pt_member_t){name, inside, *known}));
    return inside;
}

static uint32_t
inline_depth(const ir_func_t *f, th_type *type, bool sequence_root,
             const ir_spath_t *at, bool *known)
{
    uint32_t start = 0;
    *known = type != nullptr;
    if (!type) return 0;
    if (sequence_root && at->depth == 1
        && core_string_eq(at->steps[0], length_step))
        return 1;
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
    bool layout_known    = true;
    for (uint32_t i = start; i < at->depth; i++) {
        th_type      *current  = defined_type(f, type);
        core_string_t step     = at->steps[i];
        bool          elements = core_string_eq(step, element_step);
        uint64_t      extent   = 0;
        if (!current) {
            *known = false;
            return i;
        }
        if (core_string_eq(step, value_step)) {
            if (!pointee_type(f->owner, type)) *known = false;
            return i;
        }
        switch (th_kind_of(current)) {
        case TH_INDIRECTION:
            return i;
        case TH_SEQUENCE:
            if (core_string_eq(step, length_step))
                return th_seq_extent(current, &extent) || inline_elements
                    ? at->depth : i;
            if (!elements) {
                *known = false;
                return i;
            }
            if (!inline_elements && !th_seq_extent(current, &extent)) {
                if (!layout_known) *known = false;
                return i;
            }
            type            = th_seq_elem(current);
            inline_elements = false;
            layout_known    = true;
            break;
        case TH_RECORD: {
            th_type *member = elements ? nullptr
                                       : th_record_lookup(current, step);
            if (!member) {
                *known = false;
                return i;
            }
            inline_elements = layout_member_inline(f, type, current, step,
                                                   &layout_known);
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

static th_type *
end_type(ir_module_t *module, th_type *type, const ir_spath_t *path)
{
    for (uint32_t s = 0; type && s < path->depth; s++)
        type = step_through(nullptr, module, type, path->steps[s]);
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
root_object_type(const ir_func_t *f, const ir_spath_t *path, const pt_t *pt)
{
    if (named_root(path->root))
        return path->global ? path->global->type : nullptr;
    if (path->root != IR_SPLACE_PARAM || !f || path->param >= f->n_params
        || !f->params[path->param])
        return nullptr;
    th_type *type    = f->params[path->param]->type;
    th_type *defined = pt ? memo_defined(pt, type) : defined_type(f, type);
    return defined ? defined : type ? th_resolve(type) : nullptr;
}

static bool
slot_pointer(const pt_t *pt, const pt_item_t *item)
{
    uint32_t          site = item->loc.site;
    const ir_spath_t *path = &item->loc.path;
    if (item->kind != PT_CONTENTS
        || (site != PT_EXTERNAL && site >= pt->sites.len))
        return true;
    if (path->depth && step_eq(path->steps[path->depth - 1], length_step))
        return false;
    return slot_info(pt, &item->loc, path->depth)->maybe_pointer;
}

static bool
known_pointer(const pt_t *pt, const pt_loc_t *loc)
{
    return slot_info(pt, loc, loc->path.depth)->pointer;
}

static bool
flat_slot(const pt_t *pt, const pt_loc_t *loc)
{
    const slot_info_t *info = slot_info(pt, loc, loc->path.depth);
    if (!info->slot) return false;
    uint32_t kind = th_kind_of(info->slot);
    return kind == TH_INDIRECTION || kind == TH_BITINT || info->pointer;
}
