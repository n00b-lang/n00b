#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/analyze.h"
#include "ir/cli.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"

#define IR_CLI_OK    0
#define IR_CLI_DIAG  1
#define IR_CLI_USAGE 2

typedef struct {
    ir_ctx_t    *ctx;
    ir_diags_t  *d;
    ir_module_t *m;
    char        *text;
    const char  *path;
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

static void usage(FILE *err)
{
    fputs("usage: ir <command> [options] <file.ir>\n"
          "\n"
          "  parse     read the module and report what the parser found\n"
          "  print     write the module back out in canonical form\n"
          "  verify    report which properties hold\n"
          "  analyze   report unreachable blocks, unset and unused locals\n"
          "  explain   say which typing rule fires for each instruction\n"
          "  pass      run passes in order, then print\n"
          "  cfg       blocks and their successors, with edge kinds\n"
          "  dom       dominators, post-dominators, dominance frontiers\n"
          "  cdg       control dependence, over blocks\n"
          "  dfg       data dependence, over instructions\n"
          "  pdg       both kinds of dependence, in one graph\n"
          "  regions   the region overlay, as a nested rendering\n"
          "\n"
          "  --profile <name>   bce (default) or synth\n"
          "  --json             render diagnostics as JSON\n"
          "  --passes <list>    for `pass`: comma separated, in order\n",
          err);
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

static void cmd_cfg(job *j, FILE *out)
{
    for (size_t i = 0; i < ir_func_count(j->m); i++) {
        ir_func_t *f = ir_func_at(j->m, i);
        ir_pdg_t  *g = ir_pdg_of(f);

        fprintf(out, "func @%s\n", f->name);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *bb = f->blocks[b];
            ir_instr_t *t  = ir_block_terminator(bb);

            for (uint32_t s = 0; t && s < t->n_operands; s++) {
                ir_block_t *to = ir_succ_of(&t->operands[s]);

                if (to) {
                    fprintf(out, "  %s -> %s %s\n", bb->name, to->name,
                            ir_edge_kind_name(ir_edge_kind(g, bb, s)));
                }
            }

            if (!ir_succ_count(t)) {
                fprintf(out, "  %s -> exit\n", bb->name);
            }
        }
    }
}

static void cmd_dom(job *j, FILE *out)
{
    for (size_t i = 0; i < ir_func_count(j->m); i++) {
        ir_func_t *f = ir_func_at(j->m, i);
        ir_doms_t *d = ir_doms_of(f);

        fprintf(out, "func @%s\n", f->name);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *bb = f->blocks[b];
            ir_block_t *id = ir_idom_of(d, bb);
            ir_block_t *ip = ir_ipdom_of(d, bb);

            if (!ir_block_reachable(d, bb)) {
                fprintf(out, "  %s unreachable\n", bb->name);
                continue;
            }
            fprintf(out, "  %s idom %s ipdom %s\n", bb->name,
                    id ? id->name : "-", ip ? ip->name : "-");

            for (uint32_t k = 0; k < ir_df_count(d, bb); k++) {
                fprintf(out, "    frontier %s\n", ir_df_at(d, bb, k)->name);
            }
        }
    }
}

static void cmd_cdg(job *j, FILE *out)
{
    for (size_t i = 0; i < ir_func_count(j->m); i++) {
        ir_func_t *f = ir_func_at(j->m, i);
        ir_pdg_t  *g = ir_pdg_of(f);

        fprintf(out, "func @%s\n", f->name);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *bb = f->blocks[b];

            for (uint32_t k = 0; k < ir_controllers_of(g, bb); k++) {
                fprintf(out, "  %s on %s\n", bb->name,
                        ir_controller_at(g, bb, k)->name);
            }
        }
    }
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

static void cmd_graph(job *j, FILE *out, bool control, bool data)
{
    for (size_t i = 0; i < ir_func_count(j->m); i++) {
        ir_func_t *f = ir_func_at(j->m, i);
        ir_pdg_t  *g = ir_pdg_of(f);

        fprintf(out, "func @%s %s\n", f->name,
                ir_pdg_is_exact(g) ? "exact" : "may");
        put_edges(out, g, control, data);
    }
}

static void cmd_regions(job *j, FILE *out, FILE *err)
{
    if (!ir_structure(j->m, j->d)) {
        fprintf(err, "ir: the region overlay could not be built\n");

        return;
    }
    ir_module_establish(j->m, IR_PROP_STRUCTURED);

    char *text = ir_module_render_nested(j->m);

    if (text) {
        fputs(text, out);
        free(text);
    }
}

static void cmd_explain(job *j, FILE *out)
{
    ir_rules_t *rs = ir_module_rules(j->m);

    for (size_t i = 0; i < ir_func_count(j->m); i++) {
        ir_func_t *f = ir_func_at(j->m, i);

        fprintf(out, "func @%s\n", f->name);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
                ir_instr_t *in = f->blocks[b]->instrs[k];
                char        nm[128];
                char       *why = ir_rule_explain(j->m, rs, in);

                fprintf(out, "  %s\n", tag(in, nm, sizeof nm));

                if (why) {
                    fputs(why, out);
                    free(why);
                }
            }
        }
    }
}

static void cmd_verify(job *j, FILE *out)
{
    ir_props_t held = ir_verify(j->m, j->d);

    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); i++) {
        fprintf(out, "%-18s %s\n", ir_prop_name(PROPS[i]),
                held & PROPS[i] ? "holds" : "does not hold");
    }
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

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            help = true;
        }
        else if (!strcmp(argv[i], "--json")) {
            json = true;
        }
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            const char *want = argv[++i];

            if (!strcmp(want, "bce")) {
                only = &ir_profile_bce;
            }
            else if (!strcmp(want, "synth")) {
                only = &ir_profile_synth;
            }
            else {
                fprintf(err, "ir: no profile named %s\n", want);

                return IR_CLI_USAGE;
            }
        }
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) {
            list = argv[++i];
        }
        else if (argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "-")) {
            fprintf(err, "ir: unknown option %s\n", argv[i]);

            return IR_CLI_USAGE;
        }
        else if (!cmd) {
            cmd = argv[i];
        }
        else {
            path = argv[i];
        }
    }

    if (help || !cmd) {
        usage(err);

        return cmd || help ? IR_CLI_OK : IR_CLI_USAGE;
    }

    static const char *const KNOWN[] = {
        "parse", "print", "verify",  "analyze", "explain", "pass",
        "cfg",   "dom",   "cdg",     "dfg",     "pdg",     "regions",
    };
    bool known = false;

    for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
        known = known || !strcmp(cmd, KNOWN[i]);
    }

    if (!known) {
        fprintf(err, "ir: no command named %s\n", cmd);
        usage(err);

        return IR_CLI_USAGE;
    }

    if (!strcmp(cmd, "pass")) {
        if (!list) {
            fprintf(err, "ir: pass needs --passes\n");

            return IR_CLI_USAGE;
        }

        for (const char *at = list; at && *at;) {
            const char *comma = strchr(at, ',');
            size_t      n     = comma ? (size_t)(comma - at) : strlen(at);

            if (!pass_named(at, n)) {
                fprintf(err, "ir: no pass named %.*s\n", (int)n, at);

                return IR_CLI_USAGE;
            }
            at = comma ? comma + 1 : nullptr;
        }
    }

    if (!path) {
        fprintf(err, "ir: %s needs a file\n", cmd);

        return IR_CLI_USAGE;
    }

    job  j  = {0};
    bool ok = load(&j, path, only ? &only : BOTH, only ? 1 : 2, err);

    if (!ok) {
        int rc = j.text ? IR_CLI_DIAG : IR_CLI_USAGE;

        report(&j, out, json);
        unload(&j);

        return rc;
    }

    if (!strcmp(cmd, "parse")) {
        fprintf(out, "parsed %s: %zu function%s\n", j.path,
                ir_func_count(j.m), ir_func_count(j.m) == 1 ? "" : "s");
    }
    else if (!strcmp(cmd, "print")) {
        char *text = ir_module_print(j.m);

        if (text) {
            fputs(text, out);
            free(text);
        }
    }
    else if (!strcmp(cmd, "verify")) {
        cmd_verify(&j, out);
    }
    else if (!strcmp(cmd, "analyze")) {
        ir_analyze(j.m, j.d);
    }
    else if (!strcmp(cmd, "explain")) {
        cmd_explain(&j, out);
    }
    else if (!strcmp(cmd, "cfg")) {
        cmd_cfg(&j, out);
    }
    else if (!strcmp(cmd, "dom")) {
        cmd_dom(&j, out);
    }
    else if (!strcmp(cmd, "cdg")) {
        cmd_cdg(&j, out);
    }
    else if (!strcmp(cmd, "dfg")) {
        cmd_graph(&j, out, false, true);
    }
    else if (!strcmp(cmd, "pdg")) {
        cmd_graph(&j, out, true, true);
    }
    else if (!strcmp(cmd, "regions")) {
        cmd_regions(&j, out, err);
    }
    else if (!strcmp(cmd, "pass")) {
        int rc = cmd_pass(&j, out, err, list);

        if (rc != IR_CLI_OK) {
            report(&j, out, json);
            unload(&j);

            return rc;
        }
    }
    report(&j, out, json);

    int rc = ok && !ir_diags_failed(j.d) ? IR_CLI_OK : IR_CLI_DIAG;

    unload(&j);

    return rc;
}
