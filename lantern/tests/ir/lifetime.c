#include "ir/ir.h"
#include "harness.h"

static bool set_complete_pointer_facts(ir_ctx_t *ctx, ir_value_t *value);

typedef struct {
    size_t point_count;
    size_t edge_count;
    ir_func_t *function;
    ir_value_t *binding;
    ir_instr_t *point;
    ir_block_t *from;
    ir_block_t *to;
} endpoint_collector_t;

static bool
collect_lifetime_endpoint(void *opaque, ir_lifetime_endpoint_t endpoint)
{
    endpoint_collector_t *collector = opaque;
    if (endpoint.point && !endpoint.from && !endpoint.to) {
        collector->point_count++;
        collector->function = endpoint.function;
        collector->binding = endpoint.binding;
        collector->point = endpoint.point;
        return true;
    }
    if (!endpoint.point && endpoint.from && endpoint.to) {
        collector->edge_count++;
        collector->function = endpoint.function;
        collector->binding = endpoint.binding;
        collector->from = endpoint.from;
        collector->to = endpoint.to;
        return true;
    }
    return false;
}

static void test_branch_paths(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "branch",
        ir_type_parse(module, "(bool, bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, "value",
                                      ir_type_parse(module, "bool"));
    ir_value_t *condition = ir_param_add(function, "condition",
                                          ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *left = ir_block_add(function, "left");
    ir_block_t *right = ir_block_add(function, "right");
    ir_instr_t *point = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(point, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(point, 0);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_t *left_return = ir_instr_add(left, IR_OP_RETURN);
    ir_instr_t *right_return = ir_instr_add(right, IR_OP_RETURN);
    ir_instr_operand_value(right_return, value);
    endpoint_collector_t endpoints = {0};
    bool pointer_facts = set_complete_pointer_facts(ctx, value);
    bool endpoint_scan = pointer_facts
        && ir_value_visit_lifetime_endpoints(
            value, collect_lifetime_endpoint, &endpoints);
    ir_liveness_query_t *query = pointer_facts
        ? ir_liveness_query_new(value) : NULL;
    endpoint_collector_t cached_endpoints = {0};
    bool cached_scan = query && ir_liveness_query_visit_endpoints(query,
        collect_lifetime_endpoint, &cached_endpoints);

    check("a use on any reachable successor path keeps a value live",
          ir_value_liveness_after(value, point) == IR_LIVENESS_LIVE);
    check("a sibling branch is not considered after a returning path",
          ir_value_liveness_after(value, left_return) == IR_LIVENESS_DEAD);
    check("the return operand is consumed at its own instruction",
          ir_value_liveness_after(value, right_return) == IR_LIVENESS_DEAD);
    check("edge liveness distinguishes a dead branch from its live sibling",
          ir_value_liveness_on_edge(value, entry, left) == IR_LIVENESS_DEAD
              && ir_value_liveness_on_edge(value, entry, right)
                     == IR_LIVENESS_LIVE);
    check("edge liveness rejects a pair that is not a CFG edge",
          ir_value_liveness_on_edge(value, left, right)
              == IR_LIVENESS_UNKNOWN);
    check("lifetime endpoint scan reports final-use point and dead branch edge",
          endpoint_scan && endpoints.point_count == 1
              && endpoints.function == function && endpoints.binding == value
              && endpoints.point == right_return && endpoints.edge_count == 1
              && endpoints.from == entry && endpoints.to == left);
    check("cached alias queries match one-shot liveness and endpoints",
          query && cached_scan
              && ir_liveness_query_alias_after(query, left_return)
                  == ir_value_alias_liveness_after(value, left_return)
              && ir_liveness_query_alias_on_edge(query, entry, left)
                  == ir_value_alias_liveness_on_edge(value, entry, left)
              && ir_liveness_query_alias_on_edge(query, entry, right)
                  == ir_value_alias_liveness_on_edge(value, entry, right)
              && cached_endpoints.point_count == endpoints.point_count
              && cached_endpoints.edge_count == endpoints.edge_count
              && cached_endpoints.point == endpoints.point
              && cached_endpoints.from == endpoints.from
              && cached_endpoints.to == endpoints.to);
    ir_liveness_query_free(query);
    ir_ctx_free(ctx);
}

static void test_unreachable_use(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "unreachable",
        ir_type_parse(module, "(bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, "value",
                                      ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *dead = ir_block_add(function, "dead");
    ir_instr_t *point = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_t *dead_return = ir_instr_add(dead, IR_OP_RETURN);
    ir_instr_operand_value(dead_return, value);

    check("uses in unreachable blocks do not extend liveness",
          ir_value_liveness_after(value, point) == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static void test_uninitialized_local_lifetime(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "uninitialized_local",
        ir_type_parse(module, "() -> void"), IR_LINK_EXTERNAL);
    ir_local_t *local = ir_local_add(function, "unused",
                                     ir_type_parse(module, "i32"));
    ir_value_t *value = local ? ir_local_value(function, local) : NULL;
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    endpoint_collector_t endpoints = {0};
    bool facts = value && set_complete_pointer_facts(ctx, value);
    bool endpoint_scan = facts && ir_value_visit_lifetime_endpoints(
        value, collect_lifetime_endpoint, &endpoints);

    check("an uninitialized local has an entry-based lifetime endpoint",
          endpoint_scan && endpoints.point_count == 1
              && endpoints.function == function
              && endpoints.binding == value && endpoints.point == ret);
    ir_ctx_free(ctx);
}

static void test_incomplete_facts(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "non_dominating",
        ir_type_parse(module, "(bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *condition = ir_param_add(function, "condition",
                                          ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *definition = ir_block_add(function, "definition");
    ir_block_t *use = ir_block_add(function, "use");
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, definition);
    ir_instr_operand_block(branch, use);
    ir_instr_t *defined = ir_instr_add(definition, IR_OP_CONST);
    ir_instr_set_dest(defined, "defined", ir_type_parse(module, "bool"));
    ir_instr_operand_int(defined, 1);
    ir_instr_t *definition_return = ir_instr_add(definition, IR_OP_RETURN);
    ir_instr_operand_value(definition_return, defined->dest);
    ir_instr_t *use_return = ir_instr_add(use, IR_OP_RETURN);
    ir_instr_operand_value(use_return, defined->dest);

    check("a definition that does not dominate the query is unknown",
          ir_value_liveness_after(defined->dest, use_return)
              == IR_LIVENESS_UNKNOWN
              && ir_value_liveness_on_edge(defined->dest, entry, use)
                     == IR_LIVENESS_UNKNOWN);

    ir_func_t *incomplete = ir_func_add(module, "incomplete",
        ir_type_parse(module, "() -> void"), IR_LINK_EXTERNAL);
    ir_block_t *open_block = ir_block_add(incomplete, "open");
    ir_instr_t *open_point = ir_instr_add(open_block, IR_OP_CONST);
    ir_instr_set_dest(open_point, "open_value",
                      ir_type_parse(module, "i:32"));
    ir_instr_operand_int(open_point, 1);
    endpoint_collector_t endpoints = {0};
    check("a reachable unterminated block has unknown liveness",
          ir_value_liveness_after(open_point->dest, open_point)
              == IR_LIVENESS_UNKNOWN
              && !ir_value_visit_lifetime_endpoints(
                    open_point->dest, collect_lifetime_endpoint, &endpoints));
    ir_liveness_query_t *query = ir_liveness_query_new(open_point->dest);
    check("cached queries preserve incomplete-fact results",
          query
              && ir_liveness_query_alias_after(query, open_point)
                  == IR_LIVENESS_UNKNOWN
              && !ir_liveness_query_visit_endpoints(query,
                  collect_lifetime_endpoint, &endpoints));
    ir_liveness_query_free(query);

    ir_func_t *malformed = ir_func_add(module, "multiple_terminators",
        ir_type_parse(module, "(bool) -> void"), IR_LINK_EXTERNAL);
    ir_value_t *input = ir_param_add(malformed, "input",
                                     ir_type_parse(module, "bool"));
    ir_block_t *bad_entry = ir_block_add(malformed, "entry");
    ir_block_t *exit = ir_block_add(malformed, "exit");
    ir_instr_t *jump = ir_instr_add(bad_entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, exit);
    ir_instr_add(bad_entry, IR_OP_RETURN);
    ir_instr_add(exit, IR_OP_RETURN);
    endpoints = (endpoint_collector_t){0};
    check("endpoint traversal rejects blocks with multiple terminators",
          !ir_value_visit_lifetime_endpoints(
              input, collect_lifetime_endpoint, &endpoints));
    ir_ctx_free(ctx);
}

static void test_loop_backedge(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "loop",
        ir_type_parse(module, "(bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, "value",
                                      ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *loop = ir_block_add(function, "loop");
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, loop);
    ir_instr_t *select = ir_instr_add(loop, IR_OP_SELECT);
    ir_instr_set_dest(select, "used", ir_type_parse(module, "bool"));
    ir_instr_operand_value(select, value);
    ir_instr_operand_value(select, value);
    ir_instr_operand_value(select, value);
    ir_instr_t *point = ir_instr_add(loop, IR_OP_CONST);
    ir_instr_set_dest(point, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(point, 0);
    ir_instr_t *backedge = ir_instr_add(loop, IR_OP_JUMP);
    ir_instr_operand_block(backedge, loop);
    ir_func_t *other_function = ir_func_add(module, "other",
        ir_type_parse(module, "(bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *other_value = ir_param_add(other_function, "other_value",
                                           ir_type_parse(module, "bool"));

    check("a loop backedge revisits uses before the queried point",
          ir_value_liveness_after(value, point) == IR_LIVENESS_LIVE
              && ir_value_liveness_on_edge(value, loop, loop)
                     == IR_LIVENESS_LIVE);
    check("the query rejects values from another function",
          ir_value_liveness_after(other_value, point) == IR_LIVENESS_UNKNOWN);
    ir_ctx_free(ctx);
}

static void test_phi_edge_liveness(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "phi_backedge",
        ir_type_parse(module, "(bool, bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *seed = ir_param_add(function, "seed",
                                     ir_type_parse(module, "bool"));
    ir_value_t *again = ir_param_add(function, "again",
                                      ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *loop = ir_block_add(function, "loop");
    ir_block_t *exit = ir_block_add(function, "exit");
    ir_instr_t *enter = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(enter, loop);
    ir_instr_t *phi = ir_instr_add(loop, IR_OP_PHI);
    ir_instr_set_dest(phi, "carried", ir_type_parse(module, "bool"));
    ir_instr_operand_phi_arm(phi, entry, seed);
    ir_instr_operand_phi_arm(phi, loop, phi->dest);
    ir_instr_t *branch = ir_instr_add(loop, IR_OP_BRANCH);
    ir_instr_operand_value(branch, again);
    ir_instr_operand_block(branch, loop);
    ir_instr_operand_block(branch, exit);
    ir_instr_t *ret = ir_instr_add(exit, IR_OP_RETURN);
    ir_instr_operand_value(ret, seed);

    check("PHI self-use keeps its previous-iteration value live on the backedge",
          ir_value_liveness_on_edge(phi->dest, loop, loop)
              == IR_LIVENESS_LIVE);
    check("a value defined in the edge destination is not assumed available",
          ir_value_liveness_on_edge(phi->dest, entry, loop)
              == IR_LIVENESS_UNKNOWN);
    check("a sibling exit edge does not inherit the PHI backedge use",
          ir_value_liveness_on_edge(phi->dest, loop, exit)
              == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static bool set_alias_facts(ir_ctx_t *ctx, ir_value_t *value,
                           ir_value_t *target)
{
    ir_annots_t *facts = ir_value_annots(value);
    return facts
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
            .b = true,
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
            .b = false,
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias_unresolved", .kind = IR_ANNOT_BOOL,
            .b = false,
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias_count", .kind = IR_ANNOT_INT,
            .i = {.mag = 1},
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
            .i = {.mag = 0},
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias0", .kind = IR_ANNOT_VALUE, .v = target,
        })
        && ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias0_escapes", .kind = IR_ANNOT_BOOL,
            .b = false,
        });
}

static void test_alias_component_liveness(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "aliases",
        ir_type_parse(module, "(bool, bool) -> bool"), IR_LINK_EXTERNAL);
    ir_value_t *source = ir_param_add(function, "source",
                                       ir_type_parse(module, "bool"));
    ir_value_t *alias = ir_param_add(function, "alias",
                                      ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_instr_t *point = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(point, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(point, 0);
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, alias);
    bool facts = set_alias_facts(ctx, source, alias)
        && set_alias_facts(ctx, alias, source);

    check("uses through a complete alias cycle keep the component live",
          facts && ir_value_alias_liveness_after(source, point)
                       == IR_LIVENESS_LIVE);
    check("the alias component is dead after its final direct use",
          facts && ir_value_alias_liveness_after(source, ret)
                       == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static void test_alias_edge_liveness(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "alias_branches",
        ir_type_parse(module, "(ptr[i32], bool) -> void"), IR_LINK_EXTERNAL);
    ir_value_t *owner = ir_param_add(function, "owner",
                                      ir_type_parse(module, "ptr[i32]"));
    ir_value_t *condition = ir_param_add(function, "condition",
                                          ir_type_parse(module, "bool"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_block_t *left = ir_block_add(function, "left");
    ir_block_t *right = ir_block_add(function, "right");
    bool facts = set_complete_pointer_facts(ctx, owner);
    ir_instr_t *address = ir_instr_add(entry, IR_OP_ADDRESS_OF);
    bool address_built = ir_instr_set_dest(address, "owner_address",
            ir_type_parse(module, "ptr[ptr[i32]]"))
        && ir_instr_operand_value(address, owner);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_add(left, IR_OP_RETURN);
    ir_instr_t *load = ir_instr_add(right, IR_OP_LOAD);
    ir_instr_set_dest(load, "loaded", ir_type_parse(module, "i32"));
    ir_instr_operand_value(load, owner);
    ir_instr_add(right, IR_OP_RETURN);
    ir_annot_t *address_complete = address_built
        ? ir_annot_get(ir_value_annots(address->dest),
                       "ir.alias_analysis_complete") : nullptr;

    check("address.of inherits complete alias facts from its target",
          facts && address_built && address_complete
              && address_complete->kind == IR_ANNOT_BOOL
              && address_complete->b);
    check("alias liveness can place cleanup on only the dead CFG edge",
          ir_value_alias_liveness_on_edge(owner, entry, left)
                  == IR_LIVENESS_DEAD);
    check("live alias use on the sibling edge prevents cleanup",
          ir_value_alias_liveness_on_edge(owner, entry, right)
                  == IR_LIVENESS_LIVE);
    check("edge finalization query requires dead direct and alias liveness",
          ir_value_can_finalize_on_edge(owner, entry, left)
              && !ir_value_can_finalize_on_edge(owner, entry, right)
              && !ir_value_can_finalize_on_edge(owner, left, right));
    check("address.of keeps its known target edge through alias liveness",
          address_built
              && ir_value_alias_liveness_on_edge(address->dest, entry, left)
                     == IR_LIVENESS_DEAD);
    bool marked_escaping = ir_annot_set(ctx, ir_value_annots(owner),
        (ir_annot_t){
            .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
            .b = true,
        });
    check("edge finalization fails closed for escaping aliases",
          marked_escaping
              && ir_value_alias_liveness_on_edge(owner, entry, left)
                     == IR_LIVENESS_UNKNOWN
              && !ir_value_can_finalize_on_edge(owner, entry, left));
    ir_ctx_free(ctx);
}

static bool set_complete_pointer_facts(ir_ctx_t *ctx, ir_value_t *value)
{
    ir_annots_t *annots = ir_value_annots(value);
    return ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_analysis_complete",
               .kind = IR_ANNOT_BOOL,
               .b = true,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.lifetime_escapes",
               .kind = IR_ANNOT_BOOL,
               .b = false,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_unresolved",
               .kind = IR_ANNOT_BOOL,
               .b = false,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.alias_count",
               .kind = IR_ANNOT_INT,
               .i = {0},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = "ir.aliased_by_count",
               .kind = IR_ANNOT_INT,
               .i = {0},
           });
}

static void test_pointer_escape_stays_unknown(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "return_pointer",
        ir_type_parse(module, "(ptr[i32]) -> ptr[i32]"), IR_LINK_EXTERNAL);
    ir_value_t *pointer = ir_param_add(function, "pointer",
                                        ir_type_parse(module, "ptr[i32]"));
    ir_block_t *entry = ir_block_add(function, "entry");
    ir_instr_t *marker = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(marker, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(marker, 0);
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, pointer);
    bool facts = set_complete_pointer_facts(ctx, pointer);
    endpoint_collector_t endpoints = {0};
    bool endpoint_scan = facts && ir_value_visit_lifetime_endpoints(
        pointer, collect_lifetime_endpoint, &endpoints);

    check("returning a pointer makes alias lifetime unknown",
          facts && ir_value_alias_liveness_after(pointer, marker)
                       == IR_LIVENESS_UNKNOWN
              && endpoint_scan && !endpoints.point_count
              && !endpoints.edge_count);
    ir_ctx_free(ctx);
}

static void test_pointer_copy_aliases(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "copy_alias",
        ir_type_parse(module, "(ptr[i32]) -> void"), IR_LINK_EXTERNAL);
    ir_value_t *owner = ir_param_add(function, "owner",
                                      ir_type_parse(module, "ptr[i32]"));
    ir_block_t *entry = ir_block_add(function, "entry");
    bool facts = set_complete_pointer_facts(ctx, owner);
    ir_instr_t *copy = ir_instr_add(entry, IR_OP_COPY);
    ir_instr_set_dest(copy, "alias", owner->type);
    facts = facts && set_complete_pointer_facts(ctx, copy->dest);
    bool copied = ir_instr_operand_value(copy, owner);
    ir_instr_t *marker = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(marker, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(marker, 0);
    ir_instr_t *load = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(load, "loaded", ir_type_parse(module, "i32"));
    ir_instr_operand_value(load, copy->dest);
    ir_instr_add(entry, IR_OP_RETURN);

    check("pointer-copy aliases extend lifetime liveness",
          copied && facts && ir_value_alias_liveness_after(owner, marker)
                       == IR_LIVENESS_LIVE
              && ir_value_alias_liveness_after(owner, load)
                       == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static void test_pointer_join_and_derived_aliases(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "liveness", &ir_profile_bce,
                                         "test");
    ir_func_t *function = ir_func_add(module, "joined_aliases",
        ir_type_parse(module, "(ptr[i32], ptr[i32], bool, i32) -> void"),
        IR_LINK_EXTERNAL);
    ir_value_t *left = ir_param_add(function, "left",
                                     ir_type_parse(module, "ptr[i32]"));
    ir_value_t *right = ir_param_add(function, "right",
                                      ir_type_parse(module, "ptr[i32]"));
    ir_value_t *condition = ir_param_add(function, "condition",
                                          ir_type_parse(module, "bool"));
    ir_value_t *index = ir_param_add(function, "index",
                                      ir_type_parse(module, "i32"));
    ir_block_t *entry = ir_block_add(function, "entry");
    th_type *pointer = ir_type_parse(module, "ptr[i32]");
    bool facts = set_complete_pointer_facts(ctx, left)
        && set_complete_pointer_facts(ctx, right);

    ir_instr_t *select = ir_instr_add(entry, IR_OP_SELECT);
    ir_instr_set_dest(select, "selected", pointer);
    facts = facts && set_complete_pointer_facts(ctx, select->dest);
    bool built = ir_instr_operand_value(select, condition)
        && ir_instr_operand_value(select, left)
        && ir_instr_operand_value(select, right);

    ir_instr_t *phi = ir_instr_add(entry, IR_OP_PHI);
    ir_instr_set_dest(phi, "joined", pointer);
    facts = facts && set_complete_pointer_facts(ctx, phi->dest);
    built = built && ir_instr_operand_phi_arm(phi, entry, left)
        && ir_instr_operand_phi_arm(phi, entry, right);

    ir_instr_t *field = ir_instr_add(entry, IR_OP_FIELD);
    ir_instr_set_dest(field, "field", pointer);
    facts = facts && set_complete_pointer_facts(ctx, field->dest);
    built = built && ir_instr_operand_value(field, left)
        && ir_instr_operand_name(field, "member");

    ir_instr_t *indexed = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(indexed, "indexed", pointer);
    facts = facts && set_complete_pointer_facts(ctx, indexed->dest);
    built = built && ir_instr_operand_value(indexed, left)
        && ir_instr_operand_value(indexed, index);

    ir_instr_t *offset = ir_instr_add(entry, IR_OP_OFFSET);
    ir_instr_set_dest(offset, "offset", pointer);
    facts = facts && set_complete_pointer_facts(ctx, offset->dest);
    built = built && ir_instr_operand_value(offset, left)
        && ir_instr_operand_value(offset, index);

    ir_instr_t *marker = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(marker, "marker", ir_type_parse(module, "bool"));
    ir_instr_operand_int(marker, 0);
    ir_value_t *derived[] = {
        select->dest, phi->dest, field->dest, indexed->dest, offset->dest,
    };
    for (size_t i = 0; i < sizeof(derived) / sizeof(derived[0]); i++) {
        ir_instr_t *load = ir_instr_add(entry, IR_OP_LOAD);
        char name[32];
        snprintf(name, sizeof(name), "load_%zu", i);
        ir_instr_set_dest(load, name, ir_type_parse(module, "i32"));
        built = built && ir_instr_operand_value(load, derived[i]);
    }
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);

    check("pointer selects and phi joins retain every candidate alias",
          built && facts
              && ir_value_alias_liveness_after(left, marker)
                     == IR_LIVENESS_LIVE
              && ir_value_alias_liveness_after(left, ret)
                     == IR_LIVENESS_DEAD);
    check("field, index, and offset pointers retain their base alias",
          built && facts
              && ir_value_alias_liveness_after(left, marker)
                     == IR_LIVENESS_LIVE);
    ir_ctx_free(ctx);
}

static void test_fresh_construction_storage(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "fresh_lifetime",
        &ir_profile_bce, "test");
    ir_func_t *caller = ir_func_add(module, "caller",
        ir_type_parse(module, "() -> void"), IR_LINK_INTERNAL);
    ir_block_t *entry = ir_block_add(caller, "entry");
    ir_instr_t *storage = ir_instr_add(entry, IR_OP_ALLOC);
    ir_instr_operand_type(storage, ir_type_parse(module, "i32"));
    ir_instr_set_dest(storage, "storage",
        ir_type_parse(module, "ptr[i32]"));
    ir_value_t *initializer = ir_alloc(ctx, sizeof(*initializer));
    initializer->kind = IR_VAL_FUNC;
    initializer->type = ir_type_parse(module, "(inout i32) -> void");
    ir_instr_t *call = ir_instr_add(entry, IR_OP_CALL);
    bool built = ir_instr_operand_value(call, initializer)
        && ir_instr_operand_fresh(call, storage->dest);
    ir_instr_t *load = ir_instr_add(entry, IR_OP_LOAD);
    built = built && ir_instr_operand_value(load, storage->dest);
    ir_instr_set_dest(load, "result", ir_type_parse(module, "i32"));
    ir_instr_add(entry, IR_OP_RETURN);

    check("fresh modify construction does not make local storage escape",
          built && ir_allocation_has_only_local_uses(caller, storage));
    ir_ctx_free(ctx);
}

int main(void)
{
    test_branch_paths();
    test_unreachable_use();
    test_uninitialized_local_lifetime();
    test_incomplete_facts();
    test_loop_backedge();
    test_phi_edge_liveness();
    test_alias_component_liveness();
    test_alias_edge_liveness();
    test_pointer_escape_stays_unknown();
    test_pointer_copy_aliases();
    test_pointer_join_and_derived_aliases();
    test_fresh_construction_storage();
    return fails != 0;
}
