#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ir/cli.h"

static int  fails = 0;
static char path[256];

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static char *drain(FILE *f)
{
    fflush(f);

    long n = ftell(f);

    rewind(f);

    char *p = malloc((size_t)n + 1);

    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        p[0] = 0;

        return p;
    }
    p[n] = 0;

    return p;
}

typedef struct {
    int   rc;
    char *out;
    char *err;
} run_t;

static run_t run(const char **args, int n)
{
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    run_t r   = {0};

    r.rc  = ir_cli(n, (char **)args, out, err);
    r.out = drain(out);
    r.err = drain(err);
    fclose(out);
    fclose(err);

    return r;
}

static void done(run_t *r)
{
    free(r->out);
    free(r->err);
}

static bool said(run_t *r, const char *text)
{
    return strstr(r->out, text) || strstr(r->err, text);
}

static void write_module(const char *text)
{
    FILE *f = fopen(path, "wb");

    fputs(text, f);
    fclose(f);
}

#define MOD(...) "module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n"

static const char *DIAMOND = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    local $x: i32\n"
    "    block entry:\n"
    "      $x = copy %a\n"
    "      branch %c, body, out\n"
    "    block body:\n"
    "      $x = add wrap %a, %a\n"
    "      jump out\n"
    "    block out:\n"
    "      %res: i32 = copy $x\n"
    "      return %res\n"
    "  }\n");

static void test_every_command_the_plan_names_exists(void)
{
    static const char *cmds[] = {"parse", "print",   "verify", "analyze",
                                 "explain", "cfg",   "dom",    "cdg",
                                 "dfg",   "pdg",     "regions"};

    write_module(DIAMOND);

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        const char *args[] = {"ir", cmds[i], path};
        run_t       r      = run(args, 3);

        if (r.rc != 0) {
            printf("FAIL  ir %s exits %d\n  %s%s\n", cmds[i], r.rc, r.out,
                   r.err);
            fails++;
        }
        done(&r);
    }

    const char *pass[] = {"ir", "pass", "--passes", "promote", path};
    run_t       r      = run(pass, 5);

    check("ir pass runs the pass it is given", r.rc == 0 && said(&r, "ran promote"));
    done(&r);
}

static void test_usage_and_dispatch_agree(void)
{
    const char *help[] = {"ir", "--help"};
    run_t       h      = run(help, 2);
    uint32_t    listed = 0;

    write_module(DIAMOND);

    for (char *line = strtok(h.err, "\n"); line; line = strtok(nullptr, "\n")) {
        char name[32];

        if (line[0] != ' ' || line[1] != ' ' || line[2] == ' '
            || line[2] == '-' || sscanf(line, " %31s", name) != 1) {
            continue;
        }

        const char *args[] = {"ir", name, path};
        run_t       r      = run(args, 3);

        listed++;

        if (said(&r, "no command named")) {
            printf("FAIL  usage lists %s and it does not dispatch\n", name);
            fails++;
        }
        done(&r);
    }
    check("the usage lists every command", listed == 12);
    done(&h);
}

static void test_the_status_says_which_kind_of_wrong(void)
{
    write_module(DIAMOND);

    const char *ok[] = {"ir", "parse", path};
    run_t       a    = run(ok, 3);

    check("a module that loads exits zero", a.rc == 0);
    done(&a);

    write_module(MOD("  func @f(%a: i32) -> i32 {\n"
                     "    block entry:\n"
                     "      %r: i32 = copy $nope\n"
                     "      return %r\n"
                     "  }\n"));

    const char *bad[] = {"ir", "verify", path};
    run_t       b     = run(bad, 3);

    check("a module the parser refuses exits one", b.rc == 1);
    check("and the reason is on stdout, where a reader can pipe it",
          strstr(b.out, "unknown local") != nullptr);
    check("and it points at the operand, not the line after it",
          strstr(b.out, "copy $nope") && strstr(b.out, ":7:"));
    done(&b);

    const char *nocmd[] = {"ir", "frobnicate", path};
    run_t       c       = run(nocmd, 3);

    check("an unknown command exits two, not one", c.rc == 2);
    check("and says so", said(&c, "no command named frobnicate"));
    done(&c);

    const char *nofile[] = {"ir", "print", "/nonexistent/nope.ir"};
    run_t       d        = run(nofile, 3);

    check("a file that will not open exits two", d.rc == 2);
    done(&d);

    const char *nopass[] = {"ir", "pass", "--passes", "nope", path};
    run_t       e        = run(nopass, 5);

    check("a pass that does not exist exits two", e.rc == 2);
    check("and names it", said(&e, "no pass named nope"));
    done(&e);

    const char *bare[] = {"ir", "pass", path};
    run_t       g      = run(bare, 3);

    check("pass without a list exits two", g.rc == 2);
    done(&g);

    const char *nothing[] = {"ir"};
    run_t       h         = run(nothing, 1);

    check("no command at all exits two and prints usage",
          h.rc == 2 && said(&h, "usage: ir"));
    done(&h);

    const char *help[] = {"ir", "--help"};
    run_t       k      = run(help, 2);

    check("asking for help is not an error", k.rc == 0 && said(&k, "usage: ir"));
    done(&k);
}

static void test_explain_names_the_rule_that_fires(void)
{
    write_module(DIAMOND);

    const char *args[] = {"ir", "explain", path};
    run_t       r      = run(args, 3);

    check("explain reports the ladder", said(&r, "exact type"));
    check("and the rung that matched", said(&r, "MATCH"));
    check("and what it resolved to", said(&r, "resolves to ir.add"));
    check("and it says which instruction it is talking about",
          said(&r, "entry#0:x"));
    done(&r);
}

static void test_the_graphs(void)
{
    write_module(DIAMOND);

    const char *cfg[] = {"ir", "cfg", path};
    run_t       a     = run(cfg, 3);

    check("cfg gives every edge a kind",
          said(&a, "entry -> body branch-true")
              && said(&a, "entry -> out branch-false")
              && said(&a, "body -> out fallthrough"));
    check("and says where control leaves", said(&a, "out -> exit"));
    done(&a);

    const char *dom[] = {"ir", "dom", path};
    run_t       b     = run(dom, 3);

    check("dom gives the dominator and post-dominator of each block",
          said(&b, "body idom entry ipdom out"));
    check("and the dominance frontier", said(&b, "frontier out"));
    done(&b);

    const char *cdg[] = {"ir", "cdg", path};
    run_t       c     = run(cdg, 3);

    check("cdg is over blocks", said(&c, "body on entry"));
    done(&c);

    const char *dfg[] = {"ir", "dfg", path};
    run_t       d     = run(dfg, 3);

    check("dfg names the local a may-edge flows through",
          said(&d, "via $x") && said(&d, "~>"));
    check("and marks a single reaching definition exact", said(&d, "=>"));
    check("and carries no control edges", !said(&d, "control"));
    done(&d);

    const char *pdg[] = {"ir", "pdg", path};
    run_t       e     = run(pdg, 3);

    check("pdg carries both kinds in one graph",
          said(&e, "control") && said(&e, "data"));
    check("and says whether the graph is exact", said(&e, "may"));
    done(&e);

    const char *rg[] = {"ir", "regions", path};
    run_t       g    = run(rg, 3);

    check("regions renders the overlay", said(&g, "if at entry"));
    check("and says it is a rendering, not the format",
          said(&g, "not its format"));
    done(&g);
}

static void test_both_profiles(void)
{
    write_module(DIAMOND);

    const char *bce[] = {"ir", "parse", "--profile", "bce", path};
    run_t       a     = run(bce, 5);

    check("a bce module loads under bce", a.rc == 0);
    done(&a);

    write_module("module m {\n  profile synth\n  target t\n\n"
                 "  func @f(%a: i32) -> i32 {\n"
                 "    block entry:\n"
                 "      return %a\n"
                 "  }\n}\n");

    const char *any[] = {"ir", "verify", path};
    run_t       b     = run(any, 3);

    check("a synth module loads when the command is given both profiles",
          b.rc == 0);
    done(&b);

    const char *only[] = {"ir", "parse", "--profile", "bce", path};
    run_t       c      = run(only, 5);

    check("and is refused when the command is given only the other one",
          c.rc == 1 && said(&c, "unknown profile"));
    done(&c);

    const char *nope[] = {"ir", "parse", "--profile", "nope", path};
    run_t       d      = run(nope, 5);

    check("a profile that does not exist is a usage error",
          d.rc == 2 && said(&d, "no profile named nope"));
    done(&d);
}

static void test_diagnostics_render_both_ways(void)
{
    write_module(MOD("  func @f(%a: i32) -> i32 {\n"
                     "    block entry:\n"
                     "      %r: i32 = copy $nope\n"
                     "      return %r\n"
                     "  }\n"));

    const char *text[] = {"ir", "parse", path};
    run_t       a      = run(text, 3);

    check("the human rendering quotes the source line",
          strstr(a.out, "copy $nope") && strstr(a.out, "error[IR-E0009]"));
    done(&a);

    const char *json[] = {"ir", "parse", "--json", path};
    run_t       b      = run(json, 4);

    check("the machine rendering is JSON with the same code",
          strstr(b.out, "\"code\":\"IR-E0009\"")
              && strstr(b.out, "\"start_line\":7"));
    done(&b);
}

static void test_analyze_is_reachable(void)
{
    write_module(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                     "    local $x: i32\n"
                     "    local $y: i32\n"
                     "    block entry:\n"
                     "      $y = copy %a\n"
                     "      branch %c, set, use\n"
                     "    block set:\n"
                     "      $x = copy %a\n"
                     "      jump use\n"
                     "    block use:\n"
                     "      %res: i32 = copy $x\n"
                     "      return %res\n"
                     "    block orphan:\n"
                     "      return %a\n"
                     "  }\n"));

    const char *args[] = {"ir", "analyze", path};
    run_t       r      = run(args, 3);

    check("analyze reports the unreachable block",
          said(&r, "orphan") && said(&r, "IR-W0030"));
    check("and the local read on a path that does not assign it",
          said(&r, "IR-W0027") && said(&r, "$x"));
    check("and the local assigned and never read",
          said(&r, "IR-W0031") && said(&r, "$y"));
    check("and warnings alone are not a failure", r.rc == 0);
    done(&r);
}

static void test_pass_refuses_what_it_cannot_run(void)
{
    write_module(DIAMOND);

    const char *two[] = {"ir", "pass", "--passes", "promote,structure", path};
    run_t       a     = run(two, 5);

    check("passes run in the order given",
          a.rc == 0 && said(&a, "ran promote") && said(&a, "ran structure"));

    const char *pr = strstr(a.out, "ran promote");
    const char *st = strstr(a.out, "ran structure");

    check("and the order is the order printed", pr && st && pr < st);
    check("and the module is printed afterwards", said(&a, "module m"));
    done(&a);
}

int main(void)
{
    snprintf(path, sizeof path, "%s", "ir_cli_test_module.ir");
    test_every_command_the_plan_names_exists();
    test_usage_and_dispatch_agree();
    test_the_status_says_which_kind_of_wrong();
    test_explain_names_the_rule_that_fires();
    test_the_graphs();
    test_both_profiles();
    test_diagnostics_render_both_ways();
    test_analyze_is_reachable();
    test_pass_refuses_what_it_cannot_run();
    unlink(path);

    if (!fails) {
        printf("ir cli: all checks passed\n");
    }

    return fails ? 1 : 0;
}
