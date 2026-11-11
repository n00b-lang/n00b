#include "types/refine.h"
#include "ir/pass.h"
#include "ir/doms.h"
#include "ir/pdg.h"
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef ir_instr_t *ty_instr_ptr_t;

core_list_decl(ty_instr_ptr_t);

typedef struct {
    ir_value_t    *subject;
    ir_instr_t    *read;
    ir_block_t    *branch;
    uint32_t       present;
    uint32_t       next;
} test_t;

typedef struct {
    ir_instr_t *at;
    ir_value_t *subject;
    uint32_t    shadow;
    size_t      first_user;
    size_t      users;
} edit_t;

typedef struct {
    ir_value_t                  *place;
    ir_instr_t                  *last;
    core_list_t(ty_instr_ptr_t)  fields;
} place_t;

core_list_decl(test_t);
core_list_decl(edit_t);
core_list_decl(place_t);

typedef struct {
    const ir_pdg_t              *g;
    core_list_t(test_t)          tests;
    core_dict_t                  by_root;
    core_dict_t                  by_subject;
    core_list_t(edit_t)          edits;
    core_list_t(ty_instr_ptr_t)  users;
    core_list_t(place_t)         places;
    core_dict_t                  written;
    core_dict_t                  shadows;
    ir_reach_t                  *reach;
} refine_t;

static const void *global_of(const ir_value_t *value)
{
    return value && value->kind == IR_VAL_GLOBAL && value->global
             ? (const void *)ir_global_identity(value->global)
             : nullptr;
}

static const void *location_of(const ir_instr_t *read)
{
    if (!read || read->n_operands != 1) {
        return nullptr;
    }
    if (read->op == IR_OP_COPY) {
        const void *local = ir_operand_local(&read->operands[0]);

        return local ? local : global_of(ir_operand_value(&read->operands[0]));
    }
    if (read->op == IR_OP_LOAD) {
        ir_value_t *value  = ir_operand_value(&read->operands[0]);
        const void *global = global_of(value);

        return global ? global : value;
    }

    return nullptr;
}

static bool reaching(const ir_pdg_t *g, ir_instr_t *read,
                     core_list_t(ty_instr_ptr_t) *defs)
{
    for (uint32_t i = 0; i < ir_pdg_in_count(g, read); i++) {
        const ir_dep_edge_t *e = ir_pdg_in_at(g, read, i);

        if ((e->kind == IR_DEP_DATA && e->via) || e->kind == IR_DEP_MEMORY) {
            core_list_push(*defs, e->from);
        }
    }

    return defs->len > 0;
}

static bool is_read(const ir_instr_t *in)
{
    return location_of(in)
        || (in && in->op == IR_OP_EXTRACT && in->n_operands == 2
            && in->operands[1].kind == IR_OPD_NAME);
}

static bool same_defs(const ir_pdg_t *g, ir_instr_t *a, ir_instr_t *b)
{
    core_list_t(ty_instr_ptr_t) left  = core_list_new(ty_instr_ptr_t);
    core_list_t(ty_instr_ptr_t) right = core_list_new(ty_instr_ptr_t);
    reaching(g, a, &left);
    reaching(g, b, &right);
    bool same = left.len == right.len
             && (left.len || global_of(ir_operand_value(&a->operands[0])));

    for (size_t i = 0; same && i < left.len; i++) {
        bool found = false;

        for (size_t j = 0; !found && j < right.len; j++) {
            found = left.data[i] == right.data[j];
        }
        same = found;
    }
    core_list_free(left);
    core_list_free(right);

    return same;
}

static bool same_read(const ir_pdg_t *g, ir_instr_t *a, ir_instr_t *b);

static bool same_value(const ir_pdg_t *g, ir_value_t *a, ir_value_t *b)
{
    return a == b
        || (a && b && a->def && b->def && same_read(g, a->def, b->def));
}

static bool same_read(const ir_pdg_t *g, ir_instr_t *a, ir_instr_t *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b || a->op != b->op || !is_read(a) || !is_read(b)) {
        return false;
    }
    if (a->op == IR_OP_EXTRACT) {
        return core_string_eq(a->operands[1].name, b->operands[1].name)
            && same_value(g, ir_operand_value(&a->operands[0]),
                          ir_operand_value(&b->operands[0]));
    }

    return location_of(a) == location_of(b) && same_defs(g, a, b);
}

static ir_value_t *nil_test(ir_value_t *condition, bool *nil_when_true,
                            ir_instr_t **tester)
{
    *nil_when_true = true;
    for (uint32_t depth = 0; condition && condition->def && depth < 4;
         depth++) {
        ir_instr_t *def = condition->def;

        if (def->op == IR_OP_OPTION_IS_NIL && def->n_operands == 1) {
            *tester = def;
            return ir_operand_value(&def->operands[0]);
        }
        if ((def->op != IR_OP_EQ && def->op != IR_OP_NE)
            || def->n_operands != 2) {
            return nullptr;
        }

        ir_value_t *left  = ir_operand_value(&def->operands[0]);
        ir_value_t *right = ir_operand_value(&def->operands[1]);
        ir_instr_t *k     = right && right->def
                              && right->def->op == IR_OP_CONST
                              ? right->def
                              : nullptr;

        if (!k || !k->n_operands || k->operands[0].kind != IR_OPD_CONST) {
            return nullptr;
        }

        bool truth = k->operands[0].lit.i.mag != 0;

        if ((def->op == IR_OP_EQ) != truth) {
            *nil_when_true = !*nil_when_true;
        }
        condition = left;
    }

    return nullptr;
}

static const void *root_of(const ir_instr_t *read)
{
    for (uint32_t depth = 0; read && depth < 16; depth++) {
        if (read->op != IR_OP_EXTRACT) {
            return location_of(read);
        }

        ir_value_t *base = ir_operand_value(&read->operands[0]);

        if (base && !base->def) {
            return base;
        }
        read = base ? base->def : nullptr;
    }

    return nullptr;
}

static uint32_t first_test(core_dict_t *index, const void *key)
{
    bool  found = false;
    void *value = key ? core_dict_get(index, (void *)key, &found) : nullptr;

    return found ? (uint32_t)((uintptr_t)value - 1) : UINT32_MAX;
}

static void index_test(refine_t *r, core_dict_t *index, const void *key)
{
    r->tests.data[r->tests.len - 1].next = first_test(index, key);
    core_dict_put(index, (void *)key, (void *)(uintptr_t)r->tests.len);
}

static bool controlled(const ir_pdg_t *g, ir_block_t *b, const test_t *t,
                       core_dict_t *seen)
{
    for (uint32_t i = 0; i < ir_controllers_of(g, b); i++) {
        ir_block_t *c    = ir_controller_at(g, b, i);
        uint32_t    slot = ir_controller_slot_at(g, b, i);
        bool        visited = false;

        if (c == t->branch && slot == t->present) {
            return true;
        }
        if (c == t->branch || c == b) {
            continue;
        }
        core_dict_get(seen, c, &visited);
        if (visited) {
            continue;
        }
        core_dict_put(seen, c, c);
        if (controlled(g, c, t, seen)) {
            return true;
        }
    }

    return false;
}

static bool unwraps(const ir_instr_t *in)
{
    return in->op == IR_OP_OPTION_IS_NIL || in->op == IR_OP_OPTION_GET
        || in->op == IR_OP_OPTION_GET_OR_ELSE || in->op == IR_OP_CALL_OUTPUT;
}

static bool under_test(const ir_pdg_t *g, ir_block_t *b, const test_t *t)
{
    if (b == t->branch || !ir_dominates(ir_pdg_doms(g), t->branch, b)) {
        return false;
    }

    core_dict_t seen;

    core_dict_init(&seen, nullptr, nullptr);

    bool hit = controlled(g, b, t, &seen);

    core_dict_free(&seen);

    return hit;
}

static bool present_store(const ir_instr_t *in, const void *location)
{
    ir_value_t *address = in->op == IR_OP_STORE && in->n_operands == 2
                            ? ir_operand_value(&in->operands[0])
                            : nullptr;
    ir_value_t *value   = address ? ir_operand_value(&in->operands[1])
                                  : nullptr;
    size_t            ordinal = 0;
    const ir_instr_t *call    = value && value->def
                                  ? ir_call_output_source(value->def, &ordinal)
                                  : nullptr;
    th_type          *type    = call ? ir_call_output_type(
                                           ir_call_function_type(call),
                                           ordinal)
                              : value ? value->type
                                      : nullptr;

    type = type ? th_resolve(type) : nullptr;

    return type
        && (global_of(address) ? global_of(address) : address) == location
        && ((value->def && value->def->op == IR_OP_OPTION_SOME)
            || (th_kind_of(type) != TH_VAR && !th_option_item(type)
                && !th_is_nil(type)));
}

static const void *address_location(ir_value_t *address)
{
    ir_instr_t *take = address ? address->def : nullptr;
    const void *held = take && take->op == IR_OP_ADDRESS_OF
                           && take->n_operands
                         ? global_of(ir_operand_value(&take->operands[0]))
                         : nullptr;

    return global_of(address) ? global_of(address) : held ? held : address;
}

static bool writes_location(const ir_instr_t *in, const void *location)
{
    return (in->op == IR_OP_STORE || in->op == IR_OP_GLOBAL_STORE)
        && in->n_operands == 2
        && address_location(ir_operand_value(&in->operands[0])) == location;
}

static bool shared_read(const ir_instr_t *read)
{
    ir_value_t *address = read && read->op == IR_OP_LOAD
                              && read->n_operands == 1
                            ? ir_operand_value(&read->operands[0])
                            : nullptr;

    return (address && !(address->def && address->def->op == IR_OP_ALLOC))
        || (read && read->op == IR_OP_COPY && read->n_operands == 1
            && global_of(ir_operand_value(&read->operands[0])));
}

static ir_value_t *present_value(const ir_instr_t *write)
{
    ir_value_t *value = ir_operand_value(&write->operands[1]);
    ir_instr_t *def   = value ? value->def : nullptr;
    th_type    *type  = value && value->type ? th_resolve(value->type)
                                             : nullptr;

    if (def && def->op == IR_OP_OPTION_SOME && def->n_operands) {
        return ir_operand_value(&def->operands[0]);
    }
    if (def && def->op == IR_OP_CONST) {
        return value;
    }

    return type && th_kind_of(type) != TH_VAR && !th_option_item(type)
               && !th_is_nil(type)
             ? value
             : nullptr;
}

static ir_instr_t *test_branch(const test_t *t)
{
    return t->branch->instrs[t->branch->n_instrs - 1];
}

static bool shadow_reaches(refine_t *r, const test_t *t, ir_instr_t *use)
{
    const void *location = location_of(t->read);
    ir_func_t  *f        = use->block->owner;
    ir_instr_t *branch   = test_branch(t);

    for (uint32_t b = 0; b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[b]->instrs[k];

            if (writes_location(in, location) && !present_value(in)
                && ir_instr_reaches(r->reach, branch, in)
                && ir_instr_reaches(r->reach, in, use)) {
                return false;
            }
        }
    }

    return true;
}

static bool kept_present(refine_t *r, ir_instr_t *a, ir_instr_t *b)
{
    if (a->op != b->op || a->op == IR_OP_EXTRACT
        || location_of(a) != location_of(b)) {
        return false;
    }

    core_list_t(ty_instr_ptr_t) left  = core_list_new(ty_instr_ptr_t);
    core_list_t(ty_instr_ptr_t) right = core_list_new(ty_instr_ptr_t);
    bool kept = reaching(r->g, a, &left);

    reaching(r->g, b, &right);
    for (size_t i = 0; kept && i < left.len; i++) {
        bool found = false;

        for (size_t j = 0; !found && j < right.len; j++) {
            found = left.data[i] == right.data[j];
        }
        if (!found) {
            core_dict_get(&r->written, left.data[i], &found);
        }
        kept = found || present_store(left.data[i], location_of(a));
    }
    core_list_free(left);
    core_list_free(right);

    return kept;
}

static bool refined(refine_t *r, ir_instr_t *read)
{
    for (uint32_t i = first_test(&r->by_root, root_of(read)); i != UINT32_MAX;
         i = r->tests.data[i].next) {
        const test_t *t = &r->tests.data[i];

        if (read != t->read
            && (same_read(r->g, read, t->read) || kept_present(r, read, t->read))
            && under_test(r->g, read->block, t)) {
            return true;
        }
    }

    return false;
}

static bool place_tested(refine_t *r, const ir_instr_t *in)
{
    ir_value_t *base = in->op == IR_OP_FIELD && in->n_operands == 2
                         ? ir_operand_value(&in->operands[0])
                         : nullptr;
    const void *key  = global_of(base) ? global_of(base) : base;

    for (uint32_t i = key ? first_test(&r->by_root, key) : UINT32_MAX;
         i != UINT32_MAX; i = r->tests.data[i].next) {
        const test_t *t = &r->tests.data[i];

        if (t->read->op != IR_OP_EXTRACT && location_of(t->read) == key
            && under_test(r->g, in->block, t)) {
            return true;
        }
    }

    return false;
}

static size_t derived_place(const ir_instr_t *in, core_dict_t *derived)
{
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_operand_t *op = &in->operands[o];
        ir_value_t         *v  = op->kind == IR_OPD_NAMED_ARG ? op->named.value
                               : ir_operand_local(op) ? nullptr
                                                      : ir_operand_value(op);
        void               *at = v ? core_dict_get(derived, v, nullptr)
                                   : nullptr;

        if (at) {
            return (size_t)(uintptr_t)at;
        }
    }

    return 0;
}

static void plan_places(refine_t *r, ir_block_t *block)
{
    core_dict_t derived;
    core_dict_t open;

    core_dict_init(&derived, nullptr, nullptr);
    core_dict_init(&open, nullptr, nullptr);
    for (uint32_t k = 0; k < block->n_instrs; k++) {
        ir_instr_t *in   = block->instrs[k];
        const void *read = location_of(in);

        if (read) {
            core_dict_remove(&open, (void *)read);
        }
        if (place_tested(r, in)) {
            ir_value_t *base = ir_operand_value(&in->operands[0]);
            size_t      at   = (size_t)(uintptr_t)core_dict_get(&open, base,
                                                                nullptr);

            if (!at) {
                core_list_push(r->places,
                               ((place_t){.place  = base,
                                          .fields = core_list_new(
                                              ty_instr_ptr_t)}));
                at = r->places.len;
                core_dict_put(&open, base, (void *)(uintptr_t)at);
            }
            core_list_push(r->places.data[at - 1].fields, in);
            r->places.data[at - 1].last = in;
            core_dict_put(&derived, in->dest, (void *)(uintptr_t)at);
            continue;
        }

        size_t at = derived_place(in, &derived);

        if (at) {
            r->places.data[at - 1].last = in;
            core_dict_put(&r->written, in, in);
            if (in->dest_kind == IR_DEST_VALUE && in->dest) {
                core_dict_put(&derived, in->dest, (void *)(uintptr_t)at);
            }
        }
    }
    core_dict_free(&derived);
    core_dict_free(&open);
}

static ir_instr_t *insert(ir_instr_t *before, ir_op_t op, uint32_t offset)
{
    ir_instr_t *in = ir_instr_insert(before->block, op,
                                     before->index + offset);

    if (in) {
        in->span = before->span;
    }

    return in;
}

static bool apply_place(ir_func_t *f, place_t *p, ty_unwrap_fn on_unwrap,
                        void *state)
{
    th_ctx     *types = ir_module_types(f->owner);
    ir_instr_t *first = p->fields.data[0];
    ir_local_t *local = nullptr;

    for (uint32_t n = f->n_locals; !local; n++) {
        core_buffer_t name = {0};

        core_buffer_printf(&name, "__refined_place_%u", n);
        local = ir_local_find(f, core_buffer_string(&name))
                  ? nullptr
                  : ir_local_add(f, core_buffer_string(&name),
                                 th_var(types));
        core_buffer_release(&name);
    }

    ir_instr_t *load = insert(first, IR_OP_LOAD, 0);

    if (!load || !ir_instr_set_fresh_dest(load, "__refined_", th_var(types))
        || !ir_instr_operand_value(load, p->place)) {
        return false;
    }

    ir_instr_t *get = insert(first, IR_OP_OPTION_GET, 0);

    if (!get || !ir_instr_set_fresh_dest(get, "__refined_", th_var(types))
        || !ir_instr_operand_value(get, load->dest)) {
        return false;
    }
    if (on_unwrap) {
        on_unwrap(state, get);
    }

    ir_instr_t *copy = insert(first, IR_OP_COPY, 0);
    ir_instr_t *take = copy && ir_instr_set_dest_local(copy, local)
                          && ir_instr_operand_value(copy, get->dest)
                         ? insert(first, IR_OP_ADDRESS_OF, 0)
                         : nullptr;

    if (!take
        || !ir_instr_set_fresh_dest(take, "__refined_",
                                    th_ind(types, 0, local->type))
        || !ir_instr_operand_local(take, local)) {
        return false;
    }
    for (size_t i = 0; i < p->fields.len; i++) {
        p->fields.data[i]->operands[0].value = take->dest;
        ir_instr_uses_changed(p->fields.data[i]);
    }

    uint32_t    after = ir_op_is_terminator(p->last->op) ? 0 : 1;
    ir_instr_t *read  = insert(p->last, IR_OP_COPY, after);
    ir_instr_t *some  = read && ir_instr_set_fresh_dest(read, "__refined_",
                                                       local->type)
                          && ir_instr_operand_local(read, local)
                          ? insert(read, IR_OP_OPTION_SOME, 1)
                          : nullptr;
    ir_instr_t *store = some
                          && ir_instr_set_fresh_dest(
                              some, "__refined_",
                              th_option(types, th_var(types)))
                          && ir_instr_operand_value(some, read->dest)
                          ? insert(some, IR_OP_STORE, 1)
                          : nullptr;

    return store && ir_instr_operand_value(store, p->place)
        && ir_instr_operand_value(store, some->dest);
}

static void plan_read(refine_t *r, ir_instr_t *read, uint32_t shadow)
{
    size_t first = r->users.len;

    for (uint32_t i = 0; i < ir_pdg_out_count(r->g, read); i++) {
        const ir_dep_edge_t *edge = ir_pdg_out_at(r->g, read, i);

        if (edge->kind == IR_DEP_DATA && !edge->via && !unwraps(edge->to)) {
            core_list_push(r->users, edge->to);
        }
    }
    core_list_push(r->edits, ((edit_t){.at         = read,
                                       .shadow     = shadow,
                                       .first_user = first,
                                       .users      = r->users.len - first}));
}

static uint32_t shared_test(refine_t *r, const void *location,
                            ir_instr_t *use)
{
    for (uint32_t i = location ? first_test(&r->by_root, location)
                               : UINT32_MAX;
         i != UINT32_MAX; i = r->tests.data[i].next) {
        const test_t *t = &r->tests.data[i];

        if (t->read != use && shared_read(t->read)
            && location_of(t->read) == location
            && under_test(r->g, use->block, t)
            && shadow_reaches(r, t, use)) {
            core_dict_put(&r->shadows, (void *)(uintptr_t)(i + 1),
                          (void *)(uintptr_t)0);
            return i + 1;
        }
    }

    return 0;
}

static void plan_shared_read(refine_t *r, ir_instr_t *read)
{
    uint32_t shadow = shared_test(r, location_of(read), read);

    if (shadow) {
        plan_read(r, read, shadow);
    }
}

static void plan_shared_operand(refine_t *r, ir_instr_t *in, ir_value_t *v)
{
    uint32_t shadow = shared_test(r, global_of(v), in);

    if (shadow) {
        core_list_push(r->edits, ((edit_t){.at      = in,
                                           .subject = v,
                                           .shadow  = shadow}));
    }
}

static ir_local_t *fresh_local(ir_func_t *f, const char *prefix)
{
    th_ctx     *types = ir_module_types(f->owner);
    ir_local_t *local = nullptr;

    for (uint32_t n = f->n_locals; !local; n++) {
        core_buffer_t name = {0};

        core_buffer_printf(&name, "%s%u", prefix, n);
        local = ir_local_find(f, core_buffer_string(&name))
                  ? nullptr
                  : ir_local_add(f, core_buffer_string(&name),
                                 th_var(types));
        core_buffer_release(&name);
    }

    return local;
}

static ir_instr_t *set_local(ir_block_t *block, uint32_t at, ir_local_t *local,
                             ir_value_t *value, ir_span_t span)
{
    ir_instr_t *copy = ir_instr_insert(block, IR_OP_COPY, at);

    if (!copy || !ir_instr_set_dest_local(copy, local)
        || !ir_instr_operand_value(copy, value)) {
        return nullptr;
    }
    copy->span = span;

    return copy;
}

static ir_local_t *place_shadow(refine_t *r, ir_func_t *f, const test_t *t,
                                ty_unwrap_fn on_unwrap, void *state)
{
    ir_instr_t *branch   = test_branch(t);
    ir_block_t *target   = ir_succ_at(branch, t->present - 1);
    const void *location = location_of(t->read);
    ir_local_t *local    = target ? fresh_local(f, "__refined_shadow_")
                                  : nullptr;
    uint32_t    at       = 0;

    while (target && at < target->n_instrs
           && target->instrs[at]->op == IR_OP_PHI) {
        at++;
    }

    ir_instr_t *get = local ? ir_instr_insert(target, IR_OP_OPTION_GET, at)
                            : nullptr;

    if (!get
        || !ir_instr_set_fresh_dest(get, "__refined_",
                                    th_var(ir_module_types(f->owner)))
        || !ir_instr_operand_value(get, t->read->dest)) {
        return nullptr;
    }
    get->span = branch->span;
    if (on_unwrap) {
        on_unwrap(state, get);
    }
    if (!set_local(target, at + 1, local, get->dest, branch->span)) {
        return nullptr;
    }

    core_list_t(ty_instr_ptr_t) writes = core_list_new(ty_instr_ptr_t);

    for (uint32_t b = 0; b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[b]->instrs[k];

            if (writes_location(in, location) && present_value(in)
                && ir_instr_reaches(r->reach, branch, in)) {
                core_list_push(writes, in);
            }
        }
    }

    bool ok = true;

    for (size_t i = 0; ok && i < writes.len; i++) {
        ir_instr_t *in = writes.data[i];

        ok = set_local(in->block, in->index + 1, local, present_value(in),
                       in->span) != nullptr;
    }
    core_list_free(writes);

    return ok ? local : nullptr;
}

static bool names_place(const ir_instr_t *in, uint32_t operand)
{
    return operand == 0
        && (in->op == IR_OP_GLOBAL_STORE || in->op == IR_OP_ADDRESS_OF);
}

static void plan_uses(refine_t *r, ir_instr_t *in)
{
    if (unwraps(in) || in->op == IR_OP_PHI) {
        return;
    }

    size_t start = r->edits.len;

    for (uint32_t o = 0; o < in->n_operands; o++) {
        if (names_place(in, o)) {
            continue;
        }
        const ir_operand_t *op = &in->operands[o];
        ir_value_t         *v  = op->kind == IR_OPD_NAMED_ARG ? op->named.value
                               : op->kind == IR_OPD_PHI_ARM
                                     || ir_operand_local(op)
                                   ? nullptr
                                   : ir_operand_value(op);
        bool                seen = false;

        for (size_t e = start; !seen && e < r->edits.len; e++) {
            seen = r->edits.data[e].subject == v;
        }
        if (!seen && global_of(v)) {
            plan_shared_operand(r, in, v);
            continue;
        }
        for (uint32_t i = first_test(&r->by_subject, v); i != UINT32_MAX;
             i = r->tests.data[i].next) {
            const test_t *t        = &r->tests.data[i];
            bool          repeated = false;

            for (size_t e = start; !repeated && e < r->edits.len; e++) {
                repeated = r->edits.data[e].subject == t->subject;
            }
            if (!repeated && under_test(r->g, in->block, t)) {
                core_list_push(r->edits,
                               ((edit_t){.at = in, .subject = t->subject}));
            }
        }
    }
}

static void replace_in(ir_instr_t *in, ir_value_t *from, ir_value_t *to,
                       bool phis)
{
    bool changed = false;

    for (uint32_t o = 0; o < in->n_operands; o++) {
        ir_operand_t *op = &in->operands[o];

        if (names_place(in, o)) {
            continue;
        }
        if (op->kind == IR_OPD_NAMED_ARG) {
            if (op->named.value == from) {
                op->named.value = to;
                changed         = true;
            }
        }
        else if (op->kind == IR_OPD_PHI_ARM) {
            if (phis && op->phi.value == from) {
                op->phi.value = to;
                changed       = true;
            }
        }
        else if (!ir_operand_local(op) && ir_operand_value(op) == from) {
            op->value = to;
            changed   = true;
        }
    }
    if (changed) {
        ir_instr_uses_changed(in);
    }
    if (changed && in->dest_kind == IR_DEST_VALUE && in->dest
        && in->dest->type == from->type
        && (in->op == IR_OP_COPY || in->op == IR_OP_CONSUME
            || in->op == IR_OP_RETAIN_REF)) {
        in->dest->type = th_var(ir_module_types(in->block->owner->owner));
    }
}

size_t ty_refine_options(ir_func_t *f, const ir_pdg_t *g,
                         ty_unwrap_fn on_unwrap, void *state)
{
    if (!f || !f->n_blocks || !g) {
        return 0;
    }

    refine_t r = {.g     = g,
                  .tests = core_list_new(test_t),
                  .edits = core_list_new(edit_t),
                  .users  = core_list_new(ty_instr_ptr_t),
                  .places = core_list_new(place_t)};

    core_dict_init(&r.by_root, nullptr, nullptr);
    core_dict_init(&r.by_subject, nullptr, nullptr);
    core_dict_init(&r.written, nullptr, nullptr);
    core_dict_init(&r.shadows, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        ir_instr_t *last  = block->n_instrs
                              ? block->instrs[block->n_instrs - 1]
                              : nullptr;

        if (!last || last->op != IR_OP_BRANCH || !last->n_operands) {
            continue;
        }

        bool        nil_when_true = true;
        ir_instr_t *tester        = nullptr;
        ir_value_t *subject = nil_test(ir_operand_value(&last->operands[0]),
                                       &nil_when_true, &tester);

        if (subject && tester && global_of(subject)) {
            ir_instr_t *copy = insert(tester, IR_OP_COPY, 0);

            if (!copy
                || !ir_instr_set_fresh_dest(copy, "__refined_snapshot_",
                                            subject->type)
                || !ir_instr_operand_value(copy, subject)) {
                continue;
            }
            tester->operands[0].value = copy->dest;
            ir_instr_uses_changed(tester);
            subject                   = copy->dest;
        }

        ir_instr_t *read    = subject && is_read(subject->def)
                                ? subject->def
                                : nullptr;
        const void *root    = read ? root_of(read) : nullptr;

        if (!subject || (read && !root)) {
            continue;
        }
        core_list_push(r.tests, ((test_t){.subject = subject,
                                          .read    = read,
                                          .branch  = block,
                                          .present = nil_when_true ? 2 : 1}));
        if (read) {
            index_test(&r, &r.by_root, root);
        }
        else {
            index_test(&r, &r.by_subject, subject);
        }
    }
    r.reach = r.tests.len ? ir_reach_new(f) : nullptr;
    for (uint32_t b = 0; r.tests.len && b < f->n_blocks; b++) {
        plan_places(&r, f->blocks[b]);
    }
    for (uint32_t b = 0; r.tests.len && b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[b]->instrs[k];
            bool        read = in->dest_kind == IR_DEST_VALUE && in->dest
                            && is_read(in);

            if (read && shared_read(in)) {
                plan_shared_read(&r, in);
            }
            else if (read && refined(&r, in)) {
                plan_read(&r, in, 0);
            }
            else {
                plan_uses(&r, in);
            }
        }
    }
    for (uint32_t i = 0; i < r.tests.len; i++) {
        void *key   = (void *)(uintptr_t)(i + 1);
        bool  found = false;

        core_dict_get(&r.shadows, key, &found);
        if (found) {
            core_dict_put(&r.shadows, key,
                          place_shadow(&r, f, &r.tests.data[i], on_unwrap,
                                       state));
        }
    }

    size_t count = 0;

    for (size_t i = r.edits.len; i > 0; i--) {
        edit_t     *edit  = &r.edits.data[i - 1];
        ir_value_t *from  = edit->subject ? edit->subject : edit->at->dest;
        ir_local_t *local = edit->shadow
                              ? core_dict_get(&r.shadows,
                                              (void *)(uintptr_t)edit->shadow,
                                              nullptr)
                              : nullptr;
        uint32_t    at    = edit->at->index + (edit->subject ? 0 : 1);
        ir_instr_t *made  = edit->shadow && !local
                              ? nullptr
                              : ir_instr_insert(edit->at->block,
                                                local ? IR_OP_COPY
                                                      : IR_OP_OPTION_GET,
                                                at);

        if (!made
            || !ir_instr_set_fresh_dest(made, "__refined_",
                                        th_var(ir_module_types(f->owner)))
            || !(local ? ir_instr_operand_local(made, local)
                       : ir_instr_operand_value(made, from))) {
            continue;
        }
        made->span = edit->at->span;
        if (!local && on_unwrap) {
            on_unwrap(state, made);
        }

        ir_value_t *to = made->dest;

        if (edit->subject) {
            replace_in(edit->at, from, to, false);
        }
        for (size_t u = 0; u < edit->users; u++) {
            replace_in(r.users.data[edit->first_user + u], from, to, true);
        }
        count++;
    }
    for (size_t i = 0; i < r.places.len; i++) {
        count += apply_place(f, &r.places.data[i], on_unwrap, state);
        core_list_free(r.places.data[i].fields);
    }
    core_dict_free(&r.by_root);
    core_dict_free(&r.by_subject);
    core_dict_free(&r.written);
    core_dict_free(&r.shadows);
    if (r.reach) {
        ir_reach_free(r.reach);
    }
    core_list_free(r.tests);
    core_list_free(r.edits);
    core_list_free(r.users);
    core_list_free(r.places);

    return count;
}
