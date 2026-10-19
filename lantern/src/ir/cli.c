#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "parse/commander.h"
#include "ir/analyze.h"
#include "ir/cli.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/doms.h"
#include "ir/walk.h"
#include "ir/verify.h"

#define IR_CLI_OK    0
#define IR_CLI_DIAG  1
#define IR_CLI_USAGE 2

typedef struct {
    ir_ctx_t          *ctx;
    ir_diags_t        *d;
    ir_module_t       *m;
    core_string_t      text;
    core_string_t      path;
    core_string_t     *argv;
    int                argc;
    parse_cmdr_t        *cmdr;
    parse_cmdr_result_t *args;
} job;

static const ir_pass_t *const PASSES[] = {
    &ir_pass_promote,
    &ir_pass_structure,
    &ir_pass_layout,
    &ir_pass_split_edges,
};

static const ir_props_t PROPS[] = {
    IR_PROP_PARSED, IR_PROP_TYPED,        IR_PROP_SSA,
    IR_PROP_STRUCTURED, IR_PROP_LAID_OUT, IR_PROP_NO_CRIT_EDGE,
    IR_PROP_VERIFIED,
};

typedef int (*cmd_fn)(job *j, FILE *out, FILE *err, core_string_t list);

static int cmd_parse(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_print(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_verify(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_analyze(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_explain(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_pass(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_cfg(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_dom(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_cdg(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_dfg(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_pdg(job *j, FILE *out, FILE *err, core_string_t list);
static int cmd_regions(job *j, FILE *out, FILE *err, core_string_t list);

static const struct {
    core_string_t name;
    core_string_t doc;
    cmd_fn       run;
} COMMANDS[] = {
    {CORE_STRING_STATIC("parse"),   CORE_STRING_STATIC("read the module and report what the parser found"), cmd_parse},
    {CORE_STRING_STATIC("print"),   CORE_STRING_STATIC("write the module back out in canonical form"), cmd_print},
    {CORE_STRING_STATIC("verify"),  CORE_STRING_STATIC("report which properties hold"), cmd_verify},
    {CORE_STRING_STATIC("analyze"), CORE_STRING_STATIC("report unreachable blocks, unset and unused locals"),
     cmd_analyze},
    {CORE_STRING_STATIC("explain"), CORE_STRING_STATIC("say which typing rule fires for each instruction"),
     cmd_explain},
    {CORE_STRING_STATIC("pass"),    CORE_STRING_STATIC("run passes in order, then print"), cmd_pass},
    {CORE_STRING_STATIC("cfg"),     CORE_STRING_STATIC("blocks and their successors, with edge kinds"), cmd_cfg},
    {CORE_STRING_STATIC("dom"),     CORE_STRING_STATIC("dominators, post-dominators, dominance frontiers"), cmd_dom},
    {CORE_STRING_STATIC("cdg"),     CORE_STRING_STATIC("control dependence, over blocks"), cmd_cdg},
    {CORE_STRING_STATIC("dfg"),     CORE_STRING_STATIC("data dependence, over instructions"), cmd_dfg},
    {CORE_STRING_STATIC("pdg"),     CORE_STRING_STATIC("both kinds of dependence, in one graph"), cmd_pdg},
    {CORE_STRING_STATIC("regions"), CORE_STRING_STATIC("the region overlay, as a nested rendering"), cmd_regions},
};

#define N_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static const struct {
    core_string_t       name;
    const ir_profile_t *p;
} PROFILES[] = {
    {CORE_STRING_STATIC("bce"),   &ir_profile_bce  },
    {CORE_STRING_STATIC("synth"), &ir_profile_synth},
};

#define N_PROFILES (sizeof(PROFILES) / sizeof(PROFILES[0]))

static const parse_cmdr_flag_t GLOBAL_FLAGS[] = {
    {CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
     PARSE_CMDR_TYPE_BOOL, false, CORE_STRING_STATIC("show this help")},
    {{0}, {0}, 0, false, {0}},
};

static const parse_cmdr_flag_t CMD_FLAGS[] = {
    {CORE_STRING_STATIC("--profile"), {0}, PARSE_CMDR_TYPE_WORD, true,
     CORE_STRING_STATIC("bce (default) or synth")},
    {CORE_STRING_STATIC("--json"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("render diagnostics as JSON")},
    {CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
     PARSE_CMDR_TYPE_BOOL, false, CORE_STRING_STATIC("show this help")},
    {{0}, {0}, 0, false, {0}},
};

static const parse_cmdr_flag_t PASS_FLAGS[] = {
    {CORE_STRING_STATIC("--profile"), {0}, PARSE_CMDR_TYPE_WORD, true,
     CORE_STRING_STATIC("bce (default) or synth")},
    {CORE_STRING_STATIC("--json"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("render diagnostics as JSON")},
    {CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
     PARSE_CMDR_TYPE_BOOL, false, CORE_STRING_STATIC("show this help")},
    {CORE_STRING_STATIC("--passes"), {0}, PARSE_CMDR_TYPE_WORD, true,
     CORE_STRING_STATIC("comma separated, in order")},
    {{0}, {0}, 0, false, {0}},
};

static parse_cmdr_t *build_cli(void)
{
    static parse_cmdr_cmd_t cmds[N_COMMANDS + 1];

    for (size_t i = 0; i < N_COMMANDS; i++) {
        bool is_pass = core_string_eq(COMMANDS[i].name,
                                     CORE_STRING_STATIC("pass"));

        cmds[i] = (parse_cmdr_cmd_t){
            .name     = COMMANDS[i].name,
            .doc      = COMMANDS[i].doc,
            .flags    = is_pass ? PASS_FLAGS : CMD_FLAGS,
            .arg      = CORE_STRING_STATIC("file"),
            .min_args = 1,
            .max_args = 1,
        };
    }

    const parse_cmdr_spec_t spec = {
        .tool     = CORE_STRING_STATIC("ir"),
        .usage    = CORE_STRING_STATIC(
            "usage: ir <command> [options] <file.ir>"),
        .flags    = GLOBAL_FLAGS,
        .commands = cmds,
    };

    return parse_cmdr_new(&spec);
}

static void put(FILE *out, core_string_t s)
{
    if (s.data && s.u8_bytes) {
        fwrite(s.data, 1, s.u8_bytes, out);
    }
}

static core_string_t source_of(void *state, int32_t file)
{
    job *j = state;

    if (file != 0 || !j->text.data) {
        return (core_string_t){};
    }

    return j->text;
}

static core_string_t slurp(core_string_t path, FILE *err)
{
    FILE *f = stdin;

    if (!core_string_eq(path, CORE_STRING_STATIC("-"))) {
        char *native = core_string_to_cstr(path);

        f = fopen(native, "rb");
        core_free(native);
    }

    if (!f) {
        fprintf(err, "ir: cannot open %.*s\n", (int)path.u8_bytes, path.data);

        return (core_string_t){};
    }

    core_buffer_t *b = core_buffer_empty();
    char          chunk[4096];

    for (;;) {
        size_t got = fread(chunk, 1, sizeof chunk, f);

        if (!got) {
            break;
        }
        core_buffer_append(b, chunk, got);
    }

    if (f != stdin) {
        fclose(f);
    }

    return core_buffer_take_string(b);
}

static void report(job *j, FILE *out, bool json)
{
    if (!ir_diags_count(j->d)) {
        return;
    }

    ir_render_t   r    = {.source = source_of, .state = j, .module = j->m};
    core_string_t text = json ? ir_diags_render_json(j->d, &r)
                              : ir_diags_render_text(j->d, &r);

    if (text.data) {
        put(out, text);
        core_free(text.data);
    }
}

static bool load(job *j, core_string_t path, const ir_profile_t *const *ps,
                 size_t n, FILE *err)
{
    j->path = path;
    j->text = slurp(path, err);

    if (!j->text.data) {
        return false;
    }
    j->ctx = ir_ctx_new();
    j->d   = ir_diags_new(j->ctx, nullptr);
    j->m   = ir_module_parse(j->ctx, j->text, 0, ps, n, j->d);

    if (j->m) {
        ir_module_establish(j->m, IR_PROP_PARSED | IR_PROP_TYPED);
    }

    return j->m != nullptr;
}

static void unload(job *j)
{
    if (j->ctx) {
        ir_ctx_free(j->ctx);
    }
    core_free(j->text.data);

    if (j->args) {
        parse_cmdr_result_free(j->args);
    }

    if (j->cmdr) {
        parse_cmdr_free(j->cmdr);
    }

    for (int i = 0; j->argv && i < j->argc; i++) {
        core_free(j->argv[i].data);
    }
    core_free(j->argv);
}

static core_string_t tag(ir_instr_t *in)
{
    core_string_t name = in->dest_kind == IR_DEST_VALUE && in->dest
                           ? in->dest->name
                       : in->dest_kind == IR_DEST_LOCAL && in->dest_local
                           ? in->dest_local->name
                           : ir_op_spelling(in->op);

    core_buffer_t *buf = core_buffer_empty();
    core_buffer_append_string(buf, in->block->name);
    core_buffer_printf(buf, "#%u:", in->index);
    core_buffer_append_string(buf, name);
    return core_buffer_take_string(buf);
}

typedef struct {
    FILE        *out;
    ir_module_t *m;
    ir_pdg_t    *pdg;
    ir_doms_t   *dom;
    ir_rules_t  *rules;
    bool         control;
    bool         data;
} dump;

static int dump_run(job *j, dump *u, void (*install)(ir_walk_t *w))
{
    ir_walk_t *w = ir_walk_new(u);

    u->m = j->m;
    install(w);
    ir_walk_run(w, j->m);
    ir_walk_free(w);

    return IR_CLI_OK;
}

static void put_func(FILE *out, ir_func_t *f)
{
    fprintf(out, "func @%.*s", (int)f->name.u8_bytes, f->name.data);
}

static bool cfg_func(ir_visit_t *vi)
{
    dump *u = vi->user;

    u->pdg = ir_pdg_of(vi->func);
    put_func(u->out, vi->func);
    fputc('\n', u->out);

    return true;
}

static bool cfg_block(ir_visit_t *vi)
{
    dump       *u  = vi->user;
    ir_block_t *bb = vi->block;
    ir_instr_t *t  = ir_block_terminator(bb);

    for (uint32_t s = 0; t && s < t->n_operands; s++) {
        ir_block_t *to = ir_succ_of(&t->operands[s]);

        if (to) {
            core_string_t kind = ir_edge_kind_name(ir_edge_kind(u->pdg, bb, s));

            fprintf(u->out, "  %.*s -> %.*s %.*s\n",
                    (int)bb->name.u8_bytes, bb->name.data,
                    (int)to->name.u8_bytes, to->name.data,
                    (int)kind.u8_bytes, kind.data);
        }
    }

    if (!ir_succ_count(t)) {
        fprintf(u->out, "  %.*s -> exit\n", (int)bb->name.u8_bytes,
                bb->name.data);
    }

    return false;
}

static void install_cfg(ir_walk_t *w)
{
    ir_walk_on_func(w, cfg_func, nullptr, nullptr);
    ir_walk_on_block(w, cfg_block, nullptr, nullptr);
}

static int cmd_cfg(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    dump u = {.out = out};

    return dump_run(j, &u, install_cfg);
}

static bool dom_func(ir_visit_t *vi)
{
    dump *u = vi->user;

    u->dom = ir_doms_of(vi->func);
    put_func(u->out, vi->func);
    fputc('\n', u->out);

    return true;
}

static bool dom_block(ir_visit_t *vi)
{
    dump       *u  = vi->user;
    ir_block_t *bb = vi->block;
    ir_block_t *id = ir_idom_of(u->dom, bb);
    ir_block_t *ip = ir_ipdom_of(u->dom, bb);

    if (!ir_block_reachable(u->dom, bb)) {
        fprintf(u->out, "  %.*s unreachable\n", (int)bb->name.u8_bytes,
                bb->name.data);

        return false;
    }

    core_string_t idom  = id ? id->name : CORE_STRING_STATIC("-");
    core_string_t ipdom = ip ? ip->name : CORE_STRING_STATIC("-");

    fprintf(u->out, "  %.*s idom %.*s ipdom %.*s\n",
            (int)bb->name.u8_bytes, bb->name.data,
            (int)idom.u8_bytes, idom.data,
            (int)ipdom.u8_bytes, ipdom.data);

    for (uint32_t k = 0; k < ir_df_count(u->dom, bb); k++) {
        core_string_t frontier = ir_df_at(u->dom, bb, k)->name;

        fprintf(u->out, "    frontier %.*s\n", (int)frontier.u8_bytes,
                frontier.data);
    }

    return false;
}

static void install_dom(ir_walk_t *w)
{
    ir_walk_on_func(w, dom_func, nullptr, nullptr);
    ir_walk_on_block(w, dom_block, nullptr, nullptr);
}

static int cmd_dom(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    dump u = {.out = out};

    return dump_run(j, &u, install_dom);
}

static bool cdg_block(ir_visit_t *vi)
{
    dump       *u  = vi->user;
    ir_block_t *bb = vi->block;

    for (uint32_t k = 0; k < ir_controllers_of(u->pdg, bb); k++) {
        core_string_t on = ir_controller_at(u->pdg, bb, k)->name;

        fprintf(u->out, "  %.*s on %.*s\n", (int)bb->name.u8_bytes,
                bb->name.data, (int)on.u8_bytes, on.data);
    }

    return false;
}

static void install_cdg(ir_walk_t *w)
{
    ir_walk_on_func(w, cfg_func, nullptr, nullptr);
    ir_walk_on_block(w, cdg_block, nullptr, nullptr);
}

static int cmd_cdg(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    dump u = {.out = out};

    return dump_run(j, &u, install_cdg);
}

static void put_edges(FILE *out, ir_pdg_t *g, bool control, bool data)
{
    for (uint32_t i = 0; i < ir_pdg_edge_count(g); i++) {
        const ir_dep_edge_t *e  = ir_pdg_edge_at(g, i);
        bool                 is = e->kind == IR_DEP_CONTROL;

        if ((is && !control) || (!is && !data)) {
            continue;
        }
        core_string_t from = tag(e->from);
        core_string_t to   = tag(e->to);
        fprintf(out, "  %-8s %.*s %s %.*s", is ? "control" : "data",
                (int)from.u8_bytes, from.data,
                e->certainty == IR_DEP_EXACT ? "=>" : "~>",
                (int)to.u8_bytes, to.data);
        core_free(from.data);
        core_free(to.data);

        if (e->via) {
            fprintf(out, " via $%.*s", (int)e->via->name.u8_bytes,
                    e->via->name.data);
        }
        fputc('\n', out);
    }
}

static bool explain_func(ir_visit_t *vi)
{
    dump *u = vi->user;

    put_func(u->out, vi->func);
    fputc('\n', u->out);

    return true;
}

static bool explain_instr(ir_visit_t *vi)
{
    dump         *u   = vi->user;
    core_string_t why = ir_rule_explain(u->m, u->rules, vi->instr);
    core_string_t nm  = tag(vi->instr);

    fprintf(u->out, "  %.*s\n", (int)nm.u8_bytes, nm.data);
    core_free(nm.data);

    if (why.data) {
        put(u->out, why);
        core_free(why.data);
    }

    return false;
}

static void install_explain(ir_walk_t *w)
{
    ir_walk_on_func(w, explain_func, nullptr, nullptr);
    ir_walk_on_instr(w, explain_instr, nullptr, nullptr);
}

static bool graph_func(ir_visit_t *vi)
{
    dump     *u = vi->user;
    ir_pdg_t *g = ir_pdg_of(vi->func);

    put_func(u->out, vi->func);
    fprintf(u->out, " %s\n", ir_pdg_is_exact(g) ? "exact" : "may");
    put_edges(u->out, g, u->control, u->data);

    return false;
}

static void install_graph(ir_walk_t *w)
{
    ir_walk_on_func(w, graph_func, nullptr, nullptr);
}

static int cmd_graph(job *j, FILE *out, bool control, bool data)
{
    dump u = {.out = out, .control = control, .data = data};

    return dump_run(j, &u, install_graph);
}

static int cmd_regions(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    if (!ir_structure(j->m, j->d)) {
        fprintf(err, "ir: the region overlay could not be built\n");

        return IR_CLI_OK;
    }
    ir_module_establish(j->m, IR_PROP_STRUCTURED);

    core_string_t text = ir_module_render_nested(j->m);

    if (text.data) {
        put(out, text);
        core_free(text.data);
    }

    return IR_CLI_OK;
}

static int cmd_explain(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    dump u = {.out = out, .rules = ir_module_rules(j->m)};

    return dump_run(j, &u, install_explain);
}

static int cmd_verify(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    ir_props_t held = ir_verify(j->m, j->d);

    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
        core_string_t name = ir_prop_name(PROPS[i]);

        fprintf(out, "%-18.*s %s\n", (int)name.u8_bytes, name.data,
                held & PROPS[i] ? "holds" : "does not hold");
    }

    return IR_CLI_OK;
}

static const ir_pass_t *pass_named(core_string_t name)
{
    for (size_t i = 0; i < sizeof(PASSES) / sizeof(PASSES[0]); i++) {
        if (core_string_eq(PASSES[i]->name, name)) {
            return PASSES[i];
        }
    }

    return nullptr;
}

static core_string_t next_pass(core_string_t list, size_t *at)
{
    core_string_t rest  = core_string_slice(list, *at, list.u8_bytes - *at);
    int64_t       comma = core_string_find_byte(rest, ',');
    size_t        n     = comma < 0 ? rest.u8_bytes : (size_t)comma;

    *at = comma < 0 ? list.u8_bytes : *at + n + 1;

    return core_string_slice(rest, 0, n);
}

static int cmd_dfg(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    return cmd_graph(j, out, false, true);
}

static int cmd_pdg(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    return cmd_graph(j, out, true, true);
}

static int cmd_parse(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    fprintf(out, "parsed %.*s: %zu function%s\n", (int)j->path.u8_bytes,
            j->path.data, ir_func_count(j->m),
            ir_func_count(j->m) == 1 ? "" : "s");

    return IR_CLI_OK;
}

static void print_module(job *j, FILE *out)
{
    core_string_t text = ir_module_print(j->m);

    if (text.data) {
        put(out, text);
        core_free(text.data);
    }
}

static int cmd_print(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)err;
    (void)list;

    print_module(j, out);

    return IR_CLI_OK;
}

static int cmd_analyze(job *j, FILE *out, FILE *err, core_string_t list)
{
    (void)out;
    (void)err;
    (void)list;

    ir_analyze(j->m, j->d);

    return IR_CLI_OK;
}

static int cmd_pass(job *j, FILE *out, FILE *err, core_string_t list)
{
    for (size_t at = 0; list.data && at < list.u8_bytes;) {
        core_string_t    name = next_pass(list, &at);
        const ir_pass_t *p    = pass_named(name);

        if (!p) {
            fprintf(err, "ir: no pass named %.*s\n", (int)name.u8_bytes,
                    name.data);

            return IR_CLI_USAGE;
        }

        ir_props_t       unmet = 0;
        ir_pass_result_t r = ir_pass_run(p, j->m, nullptr, &unmet, j->d);

        if (r == IR_PASS_UNMET) {
            fprintf(err, "ir: %.*s needs", (int)p->name.u8_bytes,
                    p->name.data);

            for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
                if (unmet & PROPS[i]) {
                    core_string_t prop = ir_prop_name(PROPS[i]);

                    fprintf(err, " %.*s", (int)prop.u8_bytes, prop.data);
                }
            }
            fputs(", which does not hold\n", err);

            return IR_CLI_DIAG;
        }

        if (r != IR_PASS_OK) {
            fprintf(err, "ir: %.*s did not run\n", (int)p->name.u8_bytes,
                    p->name.data);

            return IR_CLI_DIAG;
        }
        fprintf(out, "ran %.*s\n", (int)p->name.u8_bytes, p->name.data);
    }

    print_module(j, out);

    return IR_CLI_OK;
}

int ir_cli(int argc, char **argv, FILE *out, FILE *err)
{
    const ir_profile_t *const BOTH[] = {&ir_profile_bce, &ir_profile_synth};
    const ir_profile_t *only    = nullptr;
    core_string_t        cmd     = {0};
    core_string_t        path    = {0};
    core_string_t        list    = {0};
    bool                json    = false;
    bool                help    = false;

    job j  = {0};

    j.argc = argc > 0 ? argc : 0;
    j.argv = core_alloc_array(core_string_t, j.argc ? (size_t)j.argc : 1);

    for (int i = 0; i < j.argc; i++) {
        j.argv[i] = core_string_from_cstr(argv[i]);
    }

    j.cmdr = build_cli();

    for (int i = 1; i < argc; i++) {
        help = help
            || core_string_eq(j.argv[i], CORE_STRING_STATIC("-h"))
            || core_string_eq(j.argv[i], CORE_STRING_STATIC("--help"));
    }

    if (help || argc < 2) {
        parse_cmdr_print_usage(j.cmdr, err);
        unload(&j);

        return help ? IR_CLI_OK : IR_CLI_USAGE;
    }

    j.args = parse_cmdr_parse(j.cmdr, argc - 1, (const char **)argv + 1);

    if (!parse_cmdr_result_ok(j.args)
        || !parse_cmdr_result_command(j.args).data) {
        core_string_t bad = j.argv[1];

        for (int i = 1; i < argc; i++) {
            if (j.argv[i].u8_bytes > 1 && j.argv[i].data[0] == '-') {
                bad = j.argv[i];
                break;
            }
        }

        if (bad.u8_bytes && bad.data[0] == '-') {
            fprintf(err, "ir: unknown option %.*s\n", (int)bad.u8_bytes,
                    bad.data);
        }
        else {
            fprintf(err, "ir: no command named %.*s\n", (int)bad.u8_bytes,
                    bad.data);
        }
        parse_cmdr_print_usage(j.cmdr, err);
        unload(&j);

        return IR_CLI_USAGE;
    }

    cmd = parse_cmdr_result_command(j.args);

    if (parse_cmdr_flag_present(j.args, CORE_STRING_STATIC("--profile"))) {
        core_string_t want
            = parse_cmdr_flag_str(j.args, CORE_STRING_STATIC("--profile"));

        for (size_t i = 0; want.data && i < N_PROFILES; i++) {
            if (core_string_eq(want, PROFILES[i].name)) {
                only = PROFILES[i].p;
                break;
            }
        }

        if (!only) {
            fprintf(err, "ir: no profile named %.*s\n", (int)want.u8_bytes,
                    want.data ? want.data : "");
            unload(&j);

            return IR_CLI_USAGE;
        }
    }

    json = parse_cmdr_flag_present(j.args, CORE_STRING_STATIC("--json"));
    list = parse_cmdr_flag_present(j.args, CORE_STRING_STATIC("--passes"))
             ? parse_cmdr_flag_str(j.args, CORE_STRING_STATIC("--passes"))
             : (core_string_t){};
    path = parse_cmdr_arg_count(j.args) > 0
             ? parse_cmdr_arg_str(j.args, 0)
             : (core_string_t){};

    if (core_string_eq(cmd, CORE_STRING_STATIC("pass"))) {
        if (!list.data) {
            fprintf(err, "ir: pass needs --passes\n");
            unload(&j);

            return IR_CLI_USAGE;
        }

        for (size_t at = 0; at < list.u8_bytes;) {
            core_string_t name = next_pass(list, &at);

            if (!pass_named(name)) {
                fprintf(err, "ir: no pass named %.*s\n", (int)name.u8_bytes,
                        name.data);
                unload(&j);

                return IR_CLI_USAGE;
            }
        }
    }

    if (!path.data) {
        fprintf(err, "ir: %.*s needs a file\n", (int)cmd.u8_bytes, cmd.data);
        unload(&j);

        return IR_CLI_USAGE;
    }

    bool ok = load(&j, path, only ? &only : BOTH, only ? 1 : 2, err);

    if (!ok) {
        int rc = j.text.data ? IR_CLI_DIAG : IR_CLI_USAGE;

        report(&j, out, json);
        unload(&j);

        return rc;
    }

    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (!core_string_eq(cmd, COMMANDS[i].name)) {
            continue;
        }

        int rc = COMMANDS[i].run(&j, out, err, list);

        if (rc != IR_CLI_OK) {
            report(&j, out, json);
            unload(&j);

            return rc;
        }

        break;
    }

    report(&j, out, json);

    int rc = ok && !ir_diags_failed(j.d) ? IR_CLI_OK : IR_CLI_DIAG;

    unload(&j);

    return rc;
}
