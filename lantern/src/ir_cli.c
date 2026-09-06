#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
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
    char              *text;
    const char        *path;
    ncc_cmdr_t        *cmdr;
    ncc_cmdr_result_t *args;
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

typedef int (*cmd_fn)(job *j, FILE *out, FILE *err, const char *list);

static int cmd_parse(job *j, FILE *out, FILE *err, const char *list);
static int cmd_print(job *j, FILE *out, FILE *err, const char *list);
static int cmd_verify(job *j, FILE *out, FILE *err, const char *list);
static int cmd_analyze(job *j, FILE *out, FILE *err, const char *list);
static int cmd_explain(job *j, FILE *out, FILE *err, const char *list);
static int cmd_pass(job *j, FILE *out, FILE *err, const char *list);
static int cmd_cfg(job *j, FILE *out, FILE *err, const char *list);
static int cmd_dom(job *j, FILE *out, FILE *err, const char *list);
static int cmd_cdg(job *j, FILE *out, FILE *err, const char *list);
static int cmd_dfg(job *j, FILE *out, FILE *err, const char *list);
static int cmd_pdg(job *j, FILE *out, FILE *err, const char *list);
static int cmd_regions(job *j, FILE *out, FILE *err, const char *list);

static const struct {
    const char *name;
    const char *doc;
    cmd_fn      run;
} COMMANDS[] = {
    {"parse",   "read the module and report what the parser found", cmd_parse},
    {"print",   "write the module back out in canonical form", cmd_print},
    {"verify",  "report which properties hold", cmd_verify},
    {"analyze", "report unreachable blocks, unset and unused locals",
     cmd_analyze},
    {"explain", "say which typing rule fires for each instruction",
     cmd_explain},
    {"pass",    "run passes in order, then print", cmd_pass},
    {"cfg",     "blocks and their successors, with edge kinds", cmd_cfg},
    {"dom",     "dominators, post-dominators, dominance frontiers", cmd_dom},
    {"cdg",     "control dependence, over blocks", cmd_cdg},
    {"dfg",     "data dependence, over instructions", cmd_dfg},
    {"pdg",     "both kinds of dependence, in one graph", cmd_pdg},
    {"regions", "the region overlay, as a nested rendering", cmd_regions},
};

#define N_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static ncc_cmdr_t *build_cli(void)
{
    ncc_cmdr_t *c = ncc_cmdr_new();

    ncc_cmdr_set_name(c, "ir");
    ncc_cmdr_add_flag(c, nullptr, "--help", NCC_CMDR_TYPE_BOOL, false,
                      "show this help");
    ncc_cmdr_add_flag_alias(c, nullptr, "--help", "-h");

    for (size_t i = 0; i < N_COMMANDS; i++) {
        const char *name = COMMANDS[i].name;

        ncc_cmdr_add_command(c, name, COMMANDS[i].doc);
        ncc_cmdr_add_flag(c, name, "--profile", NCC_CMDR_TYPE_WORD, true,
                          "bce (default) or synth");
        ncc_cmdr_add_flag(c, name, "--json", NCC_CMDR_TYPE_BOOL, false,
                          "render diagnostics as JSON");
        ncc_cmdr_add_flag(c, name, "--help", NCC_CMDR_TYPE_BOOL, false,
                          "show this help");
        ncc_cmdr_add_flag_alias(c, name, "--help", "-h");
        ncc_cmdr_add_positional(c, name, "file", NCC_CMDR_TYPE_WORD, 1, 1);
    }

    ncc_cmdr_add_flag(c, "pass", "--passes", NCC_CMDR_TYPE_WORD, true,
                      "comma separated, in order");
    ncc_cmdr_finalize(c);

    return c;
}

static void usage_flags(ncc_cmdr_command_t *cmd, FILE *err)
{
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        ncc_cmdr_flag_spec_t *fl = &cmd->flags[i];

        if (!strcmp(fl->name.data, "--help")) {
            continue;
        }

        char spelled[64];

        snprintf(spelled, sizeof(spelled), "%s%s", fl->name.data,
                 fl->takes_value ? " <value>" : "");
        fprintf(err, "  %-18s %s\n", spelled, fl->doc.data);
    }
}

static void usage(ncc_cmdr_t *c, FILE *err)
{
    fputs("usage: ir <command> [options] <file.ir>\n\n", err);

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        ncc_cmdr_command_t *sub = &c->root.subcommands[i];

        fprintf(err, "  %-9s %s\n", sub->name.data, sub->doc.data);
    }

    fputc('\n', err);

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        ncc_cmdr_command_t *sub = &c->root.subcommands[i];

        if (!strcmp(sub->name.data, "pass")) {
            usage_flags(sub, err);
            break;
        }
    }
}

static const char *source_of(void *state, int32_t file, size_t *len)
{
    job *j = state;

    if (file != 0 || !j->text) {
        return nullptr;
    }
    *len = strlen(j->text);

    return j->text;
}

static char *slurp(const char *path, FILE *err)
{
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;

    if (!f) {
        fprintf(err, "ir: cannot open %s\n", path);

        return nullptr;
    }

    size_t cap = 4096;
    size_t n   = 0;
    char  *p   = malloc(cap);

    for (;;) {
        if (n + 1 >= cap) {
            cap *= 2;

            char *q = realloc(p, cap);

            if (!q) {
                free(p);
                fclose(f);

                return nullptr;
            }
            p = q;
        }

        size_t got = fread(p + n, 1, cap - n - 1, f);

        n += got;

        if (got == 0) {
            break;
        }
    }
    p[n] = 0;

    if (f != stdin) {
        fclose(f);
    }

    return p;
}

static void report(job *j, FILE *out, bool json)
{
    if (!ir_diags_count(j->d)) {
        return;
    }

    ir_render_t r = {.source = source_of, .state = j, .module = j->m};
    char *text = json ? ir_diags_render_json(j->d, &r)
                      : ir_diags_render_text(j->d, &r);

    if (text) {
        fputs(text, out);
        free(text);
    }
}

static bool load(job *j, const char *path, const ir_profile_t *const *ps,
                 size_t n, FILE *err)
{
    j->path = path;
    j->text = slurp(path, err);

    if (!j->text) {
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
    free(j->text);

    if (j->args) {
        ncc_cmdr_result_free(j->args);
    }

    if (j->cmdr) {
        ncc_cmdr_free(j->cmdr);
    }
}

static const char *tag(ir_instr_t *in, char *buf, size_t n)
{
    const char *name = in->dest_kind == IR_DEST_VALUE && in->dest
                         ? in->dest->name
                     : in->dest_kind == IR_DEST_LOCAL && in->dest_local
                         ? in->dest_local->name
                         : ir_op_spelling(in->op);

    snprintf(buf, n, "%s#%u:%s", in->block->name, in->index, name);

    return buf;
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

static bool cfg_func(ir_visit_t *vi)
{
    dump *u = vi->user;

    u->pdg = ir_pdg_of(vi->func);
    fprintf(u->out, "func @%s\n", vi->func->name);

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
            fprintf(u->out, "  %s -> %s %s\n", bb->name, to->name,
                    ir_edge_kind_name(ir_edge_kind(u->pdg, bb, s)));
        }
    }

    if (!ir_succ_count(t)) {
        fprintf(u->out, "  %s -> exit\n", bb->name);
    }

    return false;
}

static void install_cfg(ir_walk_t *w)
{
    ir_walk_on_func(w, cfg_func, nullptr, nullptr);
    ir_walk_on_block(w, cfg_block, nullptr, nullptr);
}

static int cmd_cfg(job *j, FILE *out, FILE *err, const char *list)
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
    fprintf(u->out, "func @%s\n", vi->func->name);

    return true;
}

static bool dom_block(ir_visit_t *vi)
{
    dump       *u  = vi->user;
    ir_block_t *bb = vi->block;
    ir_block_t *id = ir_idom_of(u->dom, bb);
    ir_block_t *ip = ir_ipdom_of(u->dom, bb);

    if (!ir_block_reachable(u->dom, bb)) {
        fprintf(u->out, "  %s unreachable\n", bb->name);

        return false;
    }
    fprintf(u->out, "  %s idom %s ipdom %s\n", bb->name,
            id ? id->name : "-", ip ? ip->name : "-");

    for (uint32_t k = 0; k < ir_df_count(u->dom, bb); k++) {
        fprintf(u->out, "    frontier %s\n", ir_df_at(u->dom, bb, k)->name);
    }

    return false;
}

static void install_dom(ir_walk_t *w)
{
    ir_walk_on_func(w, dom_func, nullptr, nullptr);
    ir_walk_on_block(w, dom_block, nullptr, nullptr);
}

static int cmd_dom(job *j, FILE *out, FILE *err, const char *list)
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
        fprintf(u->out, "  %s on %s\n", bb->name,
                ir_controller_at(u->pdg, bb, k)->name);
    }

    return false;
}

static void install_cdg(ir_walk_t *w)
{
    ir_walk_on_func(w, cfg_func, nullptr, nullptr);
    ir_walk_on_block(w, cdg_block, nullptr, nullptr);
}

static int cmd_cdg(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    dump u = {.out = out};

    return dump_run(j, &u, install_cdg);
}

static void put_edges(FILE *out, ir_pdg_t *g, bool control, bool data)
{
    char from[128];
    char to[128];

    for (uint32_t i = 0; i < ir_pdg_edge_count(g); i++) {
        const ir_dep_edge_t *e  = ir_pdg_edge_at(g, i);
        bool                 is = e->kind == IR_DEP_CONTROL;

        if ((is && !control) || (!is && !data)) {
            continue;
        }
        fprintf(out, "  %-8s %s %s %s", is ? "control" : "data",
                tag(e->from, from, sizeof from),
                e->certainty == IR_DEP_EXACT ? "=>" : "~>",
                tag(e->to, to, sizeof to));

        if (e->via) {
            fprintf(out, " via $%s", e->via->name);
        }
        fputc('\n', out);
    }
}

static bool explain_func(ir_visit_t *vi)
{
    dump *u = vi->user;

    fprintf(u->out, "func @%s\n", vi->func->name);

    return true;
}

static bool explain_instr(ir_visit_t *vi)
{
    dump *u   = vi->user;
    char  nm[128];
    char *why = ir_rule_explain(u->m, u->rules, vi->instr);

    fprintf(u->out, "  %s\n", tag(vi->instr, nm, sizeof nm));

    if (why) {
        fputs(why, u->out);
        free(why);
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

    fprintf(u->out, "func @%s %s\n", vi->func->name,
            ir_pdg_is_exact(g) ? "exact" : "may");
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

static int cmd_regions(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    if (!ir_structure(j->m, j->d)) {
        fprintf(err, "ir: the region overlay could not be built\n");

        return IR_CLI_OK;
    }
    ir_module_establish(j->m, IR_PROP_STRUCTURED);

    char *text = ir_module_render_nested(j->m);

    if (text) {
        fputs(text, out);
        free(text);
    }

    return IR_CLI_OK;
}

static int cmd_explain(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    dump u = {.out = out, .rules = ir_module_rules(j->m)};

    return dump_run(j, &u, install_explain);
}

static int cmd_verify(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    ir_props_t held = ir_verify(j->m, j->d);

    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
        fprintf(out, "%-18s %s\n", ir_prop_name(PROPS[i]),
                held & PROPS[i] ? "holds" : "does not hold");
    }

    return IR_CLI_OK;
}

static const ir_pass_t *pass_named(const char *name, size_t n)
{
    for (size_t i = 0; i < sizeof(PASSES) / sizeof(PASSES[0]); i++) {
        if (strlen(PASSES[i]->name) == n && !memcmp(PASSES[i]->name, name, n)) {
            return PASSES[i];
        }
    }

    return nullptr;
}

static int cmd_dfg(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    return cmd_graph(j, out, false, true);
}

static int cmd_pdg(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    return cmd_graph(j, out, true, true);
}

static int cmd_parse(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    fprintf(out, "parsed %s: %zu function%s\n", j->path, ir_func_count(j->m),
            ir_func_count(j->m) == 1 ? "" : "s");

    return IR_CLI_OK;
}

static int cmd_print(job *j, FILE *out, FILE *err, const char *list)
{
    (void)err;
    (void)list;

    char *text = ir_module_print(j->m);

    if (text) {
        fputs(text, out);
        free(text);
    }

    return IR_CLI_OK;
}

static int cmd_analyze(job *j, FILE *out, FILE *err, const char *list)
{
    (void)out;
    (void)err;
    (void)list;

    ir_analyze(j->m, j->d);

    return IR_CLI_OK;
}

static int cmd_pass(job *j, FILE *out, FILE *err, const char *list)
{
    for (const char *at = list; at && *at;) {
        const char *comma = strchr(at, ',');
        size_t      n     = comma ? (size_t)(comma - at) : strlen(at);
        const ir_pass_t *p = pass_named(at, n);

        if (!p) {
            fprintf(err, "ir: no pass named %.*s\n", (int)n, at);

            return IR_CLI_USAGE;
        }

        ir_props_t       unmet = 0;
        ir_pass_result_t r = ir_pass_run_checked(p, j->m, j->d, &unmet, j->d);

        if (r == IR_PASS_UNMET) {
            fprintf(err, "ir: %s needs", p->name);

            for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
                if (unmet & PROPS[i]) {
                    fprintf(err, " %s", ir_prop_name(PROPS[i]));
                }
            }
            fputs(", which does not hold\n", err);

            return IR_CLI_DIAG;
        }

        if (r != IR_PASS_OK) {
            fprintf(err, "ir: %s did not run\n", p->name);

            return IR_CLI_DIAG;
        }
        fprintf(out, "ran %s\n", p->name);
        at = comma ? comma + 1 : nullptr;
    }

    char *text = ir_module_print(j->m);

    if (text) {
        fputs(text, out);
        free(text);
    }

    return IR_CLI_OK;
}

int ir_cli(int argc, char **argv, FILE *out, FILE *err)
{
    const ir_profile_t *const BOTH[] = {&ir_profile_bce, &ir_profile_synth};
    const ir_profile_t *only    = nullptr;
    const char         *cmd     = nullptr;
    const char         *path    = nullptr;
    const char         *list    = nullptr;
    bool                json    = false;
    bool                help    = false;

    job j  = {0};

    j.cmdr = build_cli();

    for (int i = 1; i < argc; i++) {
        help = help || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help");
    }

    if (help || argc < 2) {
        usage(j.cmdr, err);
        unload(&j);

        return help ? IR_CLI_OK : IR_CLI_USAGE;
    }

    j.args = ncc_cmdr_parse(j.cmdr, argc - 1, (const char **)argv + 1);

    if (!j.args->ok || !ncc_cmdr_result_command(j.args)) {
        const char *bad = argv[1];

        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "-")) {
                bad = argv[i];
                break;
            }
        }

        fprintf(err,
                bad[0] == '-' ? "ir: unknown option %s\n"
                              : "ir: no command named %s\n",
                bad);
        usage(j.cmdr, err);
        unload(&j);

        return IR_CLI_USAGE;
    }

    cmd = ncc_cmdr_result_command(j.args);

    if (ncc_cmdr_flag_present(j.args, "--profile")) {
        const char *want = ncc_cmdr_flag_str(j.args, "--profile");

        if (want && !strcmp(want, "bce")) {
            only = &ir_profile_bce;
        }
        else if (want && !strcmp(want, "synth")) {
            only = &ir_profile_synth;
        }
        else {
            fprintf(err, "ir: no profile named %s\n", want ? want : "");
            unload(&j);

            return IR_CLI_USAGE;
        }
    }

    json = ncc_cmdr_flag_present(j.args, "--json");
    list = ncc_cmdr_flag_present(j.args, "--passes")
             ? ncc_cmdr_flag_str(j.args, "--passes")
             : nullptr;
    path = ncc_cmdr_arg_count(j.args) > 0 ? ncc_cmdr_arg_str(j.args, 0)
                                          : nullptr;

    if (!strcmp(cmd, "pass")) {
        if (!list) {
            fprintf(err, "ir: pass needs --passes\n");
            unload(&j);

            return IR_CLI_USAGE;
        }

        for (const char *at = list; at && *at;) {
            const char *comma = strchr(at, ',');
            size_t      n     = comma ? (size_t)(comma - at) : strlen(at);

            if (!pass_named(at, n)) {
                fprintf(err, "ir: no pass named %.*s\n", (int)n, at);
                unload(&j);

                return IR_CLI_USAGE;
            }
            at = comma ? comma + 1 : nullptr;
        }
    }

    if (!path) {
        fprintf(err, "ir: %s needs a file\n", cmd);
        unload(&j);

        return IR_CLI_USAGE;
    }

    bool ok = load(&j, path, only ? &only : BOTH, only ? 1 : 2, err);

    if (!ok) {
        int rc = j.text ? IR_CLI_DIAG : IR_CLI_USAGE;

        report(&j, out, json);
        unload(&j);

        return rc;
    }

    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (strcmp(cmd, COMMANDS[i].name)) {
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
