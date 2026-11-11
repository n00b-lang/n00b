#include "lower/lower_internal.h"

static void
dump_ir_instr(FILE *out, const ir_instr_t *instr);
static bool
dump_memory_def(void *context, const ir_memdef_t *def);
static void
dump_ir_places(ir_module_t *module, ir_func_t *function);

static void
dump_ir_instr(FILE *out, const ir_instr_t *instr)
{
    core_string_t function = instr->block->owner->name;
    core_string_t block = instr->block->name;
    core_string_t spelling = ir_op_spelling(instr->op);
    fprintf(out, "@%.*s:%.*s#%u (%.*s)",
            (int)function.u8_bytes, function.data,
            (int)block.u8_bytes, block.data, instr->index,
            (int)spelling.u8_bytes, spelling.data);
}

static bool
dump_memory_def(void *context, const ir_memdef_t *def)
{
    (void)context;
    fputc(' ', stdout);
    if (def->kind == IR_MEMDEF_ENTRY) fputs("entry", stdout);
    else dump_ir_instr(stdout, def->instr);
    return true;
}

static void
dump_ir_places(ir_module_t *module, ir_func_t *function)
{
    static const char *const kinds[] = {
        "local", "param", "global", "thread", "heap", "unknown"};
    ir_places_t *places = ir_func_places(function);
    th_ctx *types = ir_module_types(module);
    for (uint32_t i = 0; i < ir_places_count(places); i++) {
        const ir_place_t *place = ir_place_at(places, i);
        fprintf(stdout, "  place %u %s", i, kinds[place->kind]);
        if (place->root)
            fprintf(stdout, " %.*s", (int)place->root->name.u8_bytes,
                    place->root->name.data);
        if (place->type) {
            core_string_t text = th_print(types, place->type);
            fprintf(stdout, " : %.*s", (int)text.u8_bytes, text.data);
        }
        fputc('\n', stdout);
    }
    for (uint32_t i = 0; i < ir_places_access_count(places); i++) {
        const ir_access_t *access = ir_places_access_at(places, i);
        const uint32_t *ids = ir_places_access_places(places, access);
        fputs(access->kind == IR_ACCESS_LOAD ? "  load " : "  store ", stdout);
        dump_ir_instr(stdout, access->instr);
        fputs(" @", stdout);
        for (uint32_t p = 0; p < access->count; p++)
            fprintf(stdout, "%s%u", p ? "," : "", ids[p]);
        if (access->kind == IR_ACCESS_LOAD) {
            fputs(" <=", stdout);
            ir_pdg_memory_visit_clobbers(ir_pdg_memory(ir_func_pdg(function)), i,
                                     dump_memory_def, nullptr);
        }
        fputc('\n', stdout);
    }
    static const char *const site_kinds[] = {
        "branch", "loop_bound", "address", "divisor", "shift_amount",
        "boxed", "call_argument", "call_result", "copy_out", "returned",
        "trap", "failure"};
    ir_sites_t *sites = ir_func_sites(function);
    for (uint32_t i = 0; i < ir_sites_count(sites); i++) {
        const ir_site_t *site = ir_site_at(sites, i);
        fprintf(stdout, "  site %s ", site_kinds[site->kind]);
        dump_ir_instr(stdout, site->instr);
        if (site->operand != UINT32_MAX)
            fprintf(stdout, " operand %u", site->operand);
        fputc('\n', stdout);
    }
    ir_path_exits_t exits = {0};
    ir_paths_flow(function, IR_PATH_UNPASSED, nullptr, nullptr, &exits);
    fprintf(stdout, "  paths %s\n",
            exits.returned || exits.failed ? "may-return" : "never-returns");
}

void
dump_ir_analysis(ir_module_t *module, bool control_flow, bool full_pdg)
{
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (control_flow) {
            ir_pdg_t *pdg = ir_func_pdg(function);
            fprintf(stdout, "control-flow @%.*s\n",
                    (int)function->name.u8_bytes, function->name.data);
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                ir_instr_t *term = ir_block_terminator(block);
                bool has_successor = false;
                for (uint32_t s = 0; term && s < term->n_operands; s++) {
                    ir_block_t *target = ir_succ_of(&term->operands[s]);
                    if (!target) continue;
                    core_string_t kind = ir_edge_kind_name(
                        ir_edge_kind(pdg, block, s));
                    fprintf(stdout, "  %.*s -> %.*s [%.*s]\n",
                            (int)block->name.u8_bytes, block->name.data,
                            (int)target->name.u8_bytes, target->name.data,
                            (int)kind.u8_bytes, kind.data);
                    has_successor = true;
                }
                if (!has_successor)
                    fprintf(stdout, "  %.*s -> exit\n",
                            (int)block->name.u8_bytes, block->name.data);
            }
        }
        if (full_pdg) {
            ir_pdg_t *pdg = ir_func_pdg(function);
            core_string_t certainty = ir_pdg_is_exact(pdg)
                ? CORE_STRING_STATIC("exact") : CORE_STRING_STATIC("may");
            fprintf(stdout, "pdg @%.*s (%.*s)\n",
                    (int)function->name.u8_bytes, function->name.data,
                    (int)certainty.u8_bytes, certainty.data);
            for (uint32_t e = 0; e < ir_pdg_edge_count(pdg); e++) {
                const ir_dep_edge_t *edge = ir_pdg_edge_at(pdg, e);
                fputs(edge->kind == IR_DEP_CONTROL ? "  control "
                      : edge->kind == IR_DEP_MEMORY ? "  memory "
                                                    : "  data ",
                      stdout);
                if (edge->from)
                    dump_ir_instr(stdout, edge->from);
                else
                    fprintf(stdout, "param %%%.*s",
                            (int)edge->param->name.u8_bytes,
                            edge->param->name.data);
                fputs(edge->certainty == IR_DEP_EXACT ? " => " : " ~> ",
                      stdout);
                dump_ir_instr(stdout, edge->to);
                if (edge->via)
                    fprintf(stdout, " via $%.*s",
                            (int)edge->via->name.u8_bytes,
                            edge->via->name.data);
                fputc('\n', stdout);
            }
            dump_ir_places(module, function);
        }
    }
}
