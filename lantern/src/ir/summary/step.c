#include "internal.h"

enum { LOAD_WHOLE = 1, LOAD_EXTRACTED = 2 };

static bool writes_target(flow_query_t *q, const ir_instr_t *in);
static bool argument_is(flow_query_t *q, const ir_instr_t *call,
                        const ir_spath_t *path, const ir_splace_t *want);

static void
allocate(pt_t *pt, const ir_instr_t *in, pt_set_t *into)
{
    uint32_t site  = allocation_site(pt, in);
    pt_set_t items = (pt_set_t){0};
    bool sequence = in->op == IR_OP_AGGREGATE || in->op == IR_OP_ARRAY_NEW
        || in->op == IR_OP_ARRAY_NEW_USING;
    const core_string_t *step = in->op == IR_OP_ARRAY_NEW ? &length_step
                                                          : &element_step;
    for (uint32_t o = 0; in->op != IR_OP_COPY && o < in->n_operands; o++) {
        pt_read(pt, referenced(in, o), &items);
        for (size_t i = 0; i < items.len; i++)
            pt_add(pt, &pt->sites.data[site].contents, contents_key(site),
                   item_under(step, sequence ? 1 : 0, !sequence,
                              items.data[i]));
        items.len = 0;
    }
    core_list_free(items);
    set_add(pt, into, item_at(site_loc(site)));
}

static void
spread_operands(pt_t *pt, const ir_instr_t *in, pt_set_t *into)
{
    pt_set_t items = (pt_set_t){0};
    for (uint32_t o = 0; o < in->n_operands; o++)
        pt_read(pt, referenced(in, o), &items);
    for (size_t i = 0; i < items.len; i++)
        set_add(pt, into, item_under(nullptr, 0, true, items.data[i]));
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
must_store(const ir_instr_t *in)
{
    return in->op == IR_OP_STORE || in->op == IR_OP_GLOBAL_STORE;
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
    pt_set_t address = (pt_set_t){0};
    pt_set_t copied  = (pt_set_t){0};
    pt_address(pt, role_value(in, ir_op_source_operand(in->op)), &address);
    pt_load(pt, &address, &copied);
    for (size_t i = 0; i < copied.len; i++)
        set_add(pt, from, item_under(nullptr, 0, true, copied.data[i]));
    core_list_free(address);
    core_list_free(copied);
    return true;
}

static bool
named_path(pt_t *pt, const pt_loc_t *loc, ir_spath_t *path)
{
    if (loc->site == PT_EXTERNAL) {
        *path = loc->path;
        return true;
    }
    pt_loc_t own = {.site = PT_EXTERNAL};
    if (!copy_in_param(pt, loc->site, &own.path)) return false;
    loc_extend(pt, &own, loc->path.steps, loc->path.depth, loc_deep(loc));
    *path = own.path;
    return true;
}

static bool
definite_path(pt_t *pt, const pt_set_t *addresses, ir_spath_t *path)
{
    return addresses->len == 1 && is_address(&addresses->data[0])
        && !loc_deep(&addresses->data[0].loc)
        && named_path(pt, &addresses->data[0].loc, path)
        && path->root != IR_SPLACE_UNKNOWN;
}

static bool
covers_place(const ir_spath_t *path, const ir_splace_t *place,
             const ir_spath_t *leaf)
{
    if (path->root != place->kind) return false;
    bool same = false;
    switch (place->kind) {
    case IR_SPLACE_PARAM:
        same = path->param == place->param;
        break;
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        same = path->global && place->global
            && ir_global_identity((ir_global_t *)path->global)
                   == ir_global_identity((ir_global_t *)place->global);
        break;
    default:
        break;
    }
    return same && (leaf ? steps_prefix(path->depth, path->steps,
                                        leaf->depth, leaf->steps)
                         : !path->depth);
}

static bool
definite_store(pt_t *pt, const ir_instr_t *in, ir_spath_t *path)
{
    if (!ir_op_transfers(in->op) || ir_op_addresses_element(in->op)
        || in->op == IR_OP_ATOMIC_CAS) return false;
    const ir_instr_t *at = pt->at;
    pt->at = in;
    pt_set_t addresses = (pt_set_t){0};
    pt_address(pt, role_value(in, ir_op_address_operand(in->op)), &addresses);
    bool definite = definite_path(pt, &addresses, path);
    core_list_free(addresses);
    pt->at = at;
    return definite;
}

static void
entry_build(pt_t *pt)
{
    pt_entry_t      *entry = &pt->entry;
    const ir_func_t *f     = pt->f;
    entry->built = true;
    entry->cells = core_list_new(ir_spath_t);
    core_dict_init(&entry->gens, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            ir_spath_t        path;
            if (!definite_store(pt, in, &path)) continue;
            path.flags = 0;
            size_t cell = 0;
            while (cell < entry->cells.len
                   && compare_paths(&entry->cells.data[cell], &path))
                cell++;
            if (cell == entry->cells.len) core_list_push(entry->cells, path);
            core_dict_put(&entry->gens, (void *)in,
                          (void *)(uintptr_t)(cell + 1));
        }
    entry->words = (uint32_t)((entry->cells.len + 63) / 64);
    if (!entry->words) return;
    size_t size = (size_t)entry->words * f->n_blocks;
    entry->in    = core_alloc_array(uint64_t, size);
    entry->state = core_alloc_array(uint64_t, entry->words);
    memset(entry->in, 0xff, size * sizeof(uint64_t));
    memset(entry->in, 0, entry->words * sizeof(uint64_t));
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            const ir_block_t *block = f->blocks[b];
            memcpy(entry->state, &entry->in[(size_t)b * entry->words],
                   entry->words * sizeof(uint64_t));
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                uintptr_t cell = (uintptr_t)core_dict_get(
                    &entry->gens, (void *)block->instrs[i], nullptr);
                if (cell--) entry->state[cell / 64] |= 1ull << (cell % 64);
            }
            ir_instr_t *last = block->n_instrs
                ? block->instrs[block->n_instrs - 1] : nullptr;
            for (uint32_t s = 0; last && s < ir_succ_count(last); s++) {
                ir_block_t *next = ir_succ_at(last, s);
                uint64_t   *in   = next && next->index
                    ? &entry->in[(size_t)next->index * entry->words]
                    : nullptr;
                for (uint32_t w = 0; in && w < entry->words; w++) {
                    uint64_t met = in[w] & entry->state[w];
                    changed |= met != in[w];
                    in[w] = met;
                }
            }
        }
    }
}

static bool
written_before(pt_t *pt, const ir_instr_t *at, const ir_spath_t *read)
{
    if (!at || !at->block) return false;
    if (!pt->entry.built) entry_build(pt);
    pt_entry_t *entry = &pt->entry;
    if (!entry->words) return false;
    memcpy(entry->state, &entry->in[(size_t)at->block->index * entry->words],
           entry->words * sizeof(uint64_t));
    for (uint32_t i = 0; i < at->index; i++) {
        uintptr_t cell = (uintptr_t)core_dict_get(
            &entry->gens, (void *)at->block->instrs[i], nullptr);
        if (cell--) entry->state[cell / 64] |= 1ull << (cell % 64);
    }
    for (size_t c = 0; c < entry->cells.len; c++) {
        const ir_spath_t *cell = &entry->cells.data[c];
        if ((entry->state[c / 64] >> (c % 64) & 1)
            && !compare_path_roots(cell, read) && path_within(cell, read) >= 0)
            return true;
    }
    return false;
}

static bool
writes_target(flow_query_t *q, const ir_instr_t *in)
{
    if (!q->target) return false;
    ir_spath_t path;
    if (!q->stores)
        return definite_store(q->pt, in, &path)
            && covers_place(&path, q->target, q->leaf);
    bool        found  = false;
    ir_spath_t *stored = core_dict_get(q->stores, (void *)in, &found);
    if (!found) {
        stored = definite_store(q->pt, in, &path) ? core_alloc(ir_spath_t)
                                                  : nullptr;
        if (stored) *stored = path;
        core_dict_put(q->stores, (void *)in, stored);
    }
    return stored && covers_place(stored, q->target, q->leaf);
}

static bool
argument_is(flow_query_t *q, const ir_instr_t *call, const ir_spath_t *path,
            const ir_splace_t *want)
{
    pt_t             *pt    = q->pt;
    const ir_instr_t *at    = pt->at;
    pt_set_t          items = (pt_set_t){0};
    pt->at = call;
    if (path->output) {
        ir_spath_t own = {.root   = IR_SPLACE_PARAM, .param = path->param,
                          .output = path->output};
        map_own_storage(pt, call, &own, false, &items);
    }
    else {
        argument_items(pt, call, path->param, &items);
        for (size_t i = 0; i < items.len; i++)
            if (items.data[i].flow != PT_SAME || items.data[i].depth)
                items.len = 0;
    }
    ir_spath_t mapped;
    bool matches = definite_path(pt, &items, &mapped)
        && covers_place(&mapped, want, nullptr);
    core_list_free(items);
    pt->at = at;
    return matches;
}

static const ir_value_t *
branch_condition(ir_block_t *block)
{
    ir_instr_t *last = block ? ir_block_terminator(block) : nullptr;
    if (!last || (last->op != IR_OP_BRANCH && last->op != IR_OP_SWITCH)
        || !last->n_operands) return nullptr;
    return ir_operand_value(&last->operands[0]);
}

static void
controllers_into(ir_pdg_t *pdg, ir_block_t *block, core_dict_t *into)
{
    for (uint32_t c = 0; pdg && c < ir_controllers_of(pdg, block); c++) {
        ir_block_t *controller = ir_controller_at(pdg, block, c);
        if (!controller || core_dict_contains(into, controller)) continue;
        core_dict_put(into, controller, controller);
        controllers_into(pdg, controller, into);
    }
}

static void
read_conditions(pt_t *pt, core_dict_t *blocks, const core_dict_t *except,
                pt_set_t *into)
{
    pt_set_t conditions = (pt_set_t){0};
    core_dict_foreach(blocks, entry) {
        if (except && core_dict_contains((core_dict_t *)except, entry->key))
            continue;
        pt_read(pt, branch_condition(entry->key), &conditions);
    }
    derive_items(&conditions, PT_CONTROL);
    for (size_t i = 0; i < conditions.len; i++)
        set_add(pt, into, conditions.data[i]);
    core_list_free(conditions);
}

static ir_pdg_t *
pt_pdg(pt_t *pt)
{
    if (!pt->value_flow) return nullptr;
    if (!pt->pdg) pt->pdg = ir_func_control((ir_func_t *)pt->f);
    return pt->pdg;
}

static void
reach_control(pt_t *pt, ir_block_t *block, pt_set_t *into)
{
    ir_pdg_t *pdg = pt_pdg(pt);
    if (!pdg) return;
    core_dict_t controllers;
    core_dict_init(&controllers, nullptr, nullptr);
    controllers_into(pdg, block, &controllers);
    read_conditions(pt, &controllers, nullptr, into);
    core_dict_free(&controllers);
}

static void
phi_control(pt_t *pt, const ir_instr_t *phi, pt_set_t *into)
{
    ir_pdg_t *pdg = pt_pdg(pt);
    if (!pdg) return;
    core_dict_t reach, choose;
    core_dict_init(&reach, nullptr, nullptr);
    core_dict_init(&choose, nullptr, nullptr);
    controllers_into(pdg, phi->block, &reach);
    for (uint32_t o = 0; o < phi->n_operands; o++) {
        ir_block_t *arm = phi->operands[o].kind == IR_OPD_PHI_ARM
            ? phi->operands[o].phi.block : nullptr;
        if (!arm) continue;
        if (branch_condition(arm)) core_dict_put(&choose, arm, arm);
        controllers_into(pdg, arm, &choose);
    }
    read_conditions(pt, &choose, &reach, into);
    core_dict_free(&reach);
    core_dict_free(&choose);
}

static bool
defines_data(pt_t *pt, const ir_instr_t *in)
{
    if (!pt->value_flow) return false;
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local)
        return !pointer_type(pt, in->dest_local->type);
    return in->dest && !pointer_type(pt, in->dest->type);
}

static bool
step_matters(pt_t *pt, const ir_instr_t *in)
{
    if (ir_op_transfers(in->op) || in->op == IR_OP_CALL
        || in->op == IR_OP_CALLBACK_INVOKE || launders(pt, in))
        return true;
    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local)
        return tracks(pt, in->dest_local->type);
    return in->dest && tracks(pt, in->dest->type);
}

static pt_item_t
sequence_length(const pt_t *pt, const pt_item_t *item)
{
    if (item->flow != PT_SAME || item->depth || item->deep) return *item;
    pt_loc_t at = location_of(pt, item);
    if (loc_deep(&at)) return *item;
    loc_push(pt, &at, length_step);
    pt_item_t length = contents_item(at);
    length.flow   = PT_DERIVED;
    length.origin = item->origin;
    return length;
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
            set_add(pt, &out, item_at(site_loc(variable_site(pt, base))));
        else if (base && base->kind == IR_VAL_GLOBAL && base->global)
            set_add(pt, &out, item_at(global_loc(base->global)));
        else if (base) {
            uint32_t site = site_of(pt, in, base->type, false,
                                    instr_origin(pt, in));
            pt_read(pt, base, &first);
            for (size_t i = 0; i < first.len; i++)
                pt_add(pt, &pt->sites.data[site].contents,
                       contents_key(site), first.data[i]);
            set_add(pt, &out, item_at(site_loc(site)));
        }
        break;
    case IR_OP_COPY:
    case IR_OP_CONSUME:
    case IR_OP_RETAIN_REF:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_TYPE_NARROW:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_ATOMIC_WRAP:
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
        pt_read(pt, base, &out);
        break;
    case IR_OP_CAST:
    case IR_OP_BITCAST:
        if (launders(pt, in)) {
            note_unknown(pt, CORE_STRING_STATIC("pointer made from an integer"),
                         nullptr);
            pt_loc_t unknown = unknown_loc();
            pt_read(pt, base, &first);
            pt_publish(pt, &first, &unknown);
            if (pt->value_flow) pt_read(pt, base, &out);
        }
        else if (base && pointer_type(pt, base->type))
            pt_read(pt, base, &out);
        else if (base && defines_data(pt, in)) {
            pt_read(pt, base, &out);
            derive_items(&out, PT_DERIVED);
        }
        else {
            note_unknown(pt, CORE_STRING_STATIC("a cast to a pointer"),
                         nullptr);
            set_add(pt, &out, deep_item(unknown_loc()));
        }
        break;
    case IR_OP_PHI:
    case IR_OP_SELECT:
    case IR_OP_OPTION_GET_OR_ELSE:
        for (uint32_t o = in->op == IR_OP_SELECT ? 1 : 0; o < in->n_operands;
             o++)
            pt_read(pt, referenced(in, o), &out);
        if (!pt->value_flow) break;
        if (in->op == IR_OP_PHI) phi_control(pt, in, &out);
        else if (in->op == IR_OP_SELECT) {
            pt_read(pt, referenced(in, 0), &first);
            derive_items(&first, PT_CONTROL);
            for (size_t i = 0; i < first.len; i++)
                set_add(pt, &out, first.data[i]);
        }
        break;
    case IR_OP_FIELD:
        pt_address(pt, base, &first);
        if (named) address_step(pt, &first, in->operands[1].name, &out);
        else {
            note_unknown(pt, CORE_STRING_STATIC("a field chosen at run time"),
                         nullptr);
            set_add(pt, &out, deep_item(unknown_loc()));
        }
        break;
    case IR_OP_INDEX:
        pt_elements(pt, base, &out);
        break;
    case IR_OP_OFFSET:
    case IR_OP_ADD:
    case IR_OP_SUB:
        if (defines_data(pt, in)) {
            for (uint32_t o = 0; o < in->n_operands; o++)
                pt_read(pt, referenced(in, o), &out);
            derive_items(&out, PT_DERIVED);
            break;
        }
        for (uint32_t o = 0; o < in->n_operands; o++)
            pt_read(pt, referenced(in, o), &first);
        pointer_offset(pt, &first, &out);
        break;
    case IR_OP_EXTRACT:
        pt_read(pt, base, &first);
        if (named) project(pt, &first, in->operands[1].name, &out);
        else spread_operands(pt, in, &out);
        break;
    case IR_OP_EXTRACT_INDEX:
        pt_read(pt, base, &first);
        project(pt, &first, element_step, &out);
        break;
    case IR_OP_VARIANT_PAYLOAD:
        pt_read(pt, base, &first);
        if (named) variant_arm(pt, &first, in->operands[1].name, &out);
        else for (size_t i = 0; i < first.len; i++)
            set_add(pt, &out, first.data[i]);
        break;
    case IR_OP_VARIANT_TAG:
        if (pt->value_flow) {
            pt_read(pt, base, &first);
            variant_arm(pt, &first, outcome_arms[2], &out);
        }
        else if (base && pointer_type(pt, base->type))
            spread_operands(pt, in, &out);
        else set_add(pt, &out, deep_item(unknown_loc()));
        break;
    case IR_OP_INSERT:
        pt_read(pt, base, &out);
        pt_read(pt, third, &first);
        for (size_t i = 0; i < first.len; i++)
            set_add(pt, &out,
                    named ? item_under(&in->operands[1].name, 1, false,
                                       first.data[i])
                          : item_under(nullptr, 0, true, first.data[i]));
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
                set_add(pt, &out, member ? item_under(&member->name, 1, false,
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
        else for (size_t i = 0; i < first.len; i++) set_add(pt, &out,
                                                            first.data[i]);
        break;
    case IR_OP_SEQUENCE_LENGTH:
        if (!defines_data(pt, in)) break;
        pt_read(pt, base, &first);
        for (size_t i = 0; i < first.len; i++)
            set_add(pt, &out, sequence_length(pt, &first.data[i]));
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
            pt_store(pt, &first, &other,
                     must_store(in) && strong_writer(pt, in));
            if (first.len) settle_writes(pt, in);
            break;
        }
        if (holds_nothing(in->op)) break;
        if (defines_data(pt, in)) {
            for (uint32_t o = 0; o < in->n_operands; o++)
                pt_read(pt, referenced(in, o), &out);
            derive_items(&out, PT_DERIVED);
            break;
        }
        bool pointers = false;
        for (uint32_t o = 0; !pointers && o < in->n_operands; o++) {
            const ir_value_t *operand = referenced(in, o);
            pointers = operand && pointer_type(pt, operand->type);
        }
        if (pointers) spread_operands(pt, in, &out);
        else set_add(pt, &out, deep_item(unknown_loc()));
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
            if (held.loc.site == PT_EXTERNAL || held.flow != PT_SAME)
                continue;
            for (size_t a = 0; a < pt->sites.data[s].aliases.len; a++) {
                pt_item_t parent = pt->sites.data[s].aliases.data[a].item;
                pt_loc_t  through;
                if (!alias_through(pt, &parent, held.steps, held.depth,
                                   held.deep, &through)) continue;
                if (held.loc.site == s) through.path.flags |= IR_SPATH_DEEP;
                for (size_t o = 0;
                     o < pt->sites.data[s].aliases.data[a].origins.len; o++)
                    pt_alias(pt, &held, through,
                             pt->sites.data[s].aliases.data[a].origins.data[o]);
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
                    || (used->def->op != IR_OP_LOAD
                        && used->def->op != IR_OP_SEQUENCE_GET)) continue;
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
