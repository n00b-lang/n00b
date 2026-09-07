#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/annot_args.h"
#include "parse/infer.h"
#include "harness.h"

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static const char *const CORPUS[] = {
    "`t",
    "$0",
    "$0 unify $1",
    "$0 unify $2",
    "$0 unify $2 unify int",
    "$0 unify $4",
    "$0 unify list[`t]|array[`t]",
    "$0[...$0]",
    "$1",
    "$1 unify $2",
    "$1 unify $3",
    "$1 unify int",
    "$2",
    "$2 unify $3",
    "$3 unify (list[$1] | array[$1])",
    "bool",
    "dict[$0, $1]",
    "element_of($0)",
    "list[`e]",
    "list[$0]",
    "list[$0] unify $1",
    "lookup($0)",
    "method_return($0)",
    "ref[$0]",
    "return_of($0)",
    "set[$0]",
    "set[$0] unify $1",
    "unwrap_result($0)",
    "$$",
    "$$ unify $1",
    "$return",
    "$return unify $1",
    "$$ unify $return",
    "return",
    "unwrap_result($0) unify $$",
    "$0[...$1]",
    "dict[$0, list[`t]]",
    "$0[]",
};

static void
test_the_corpus_parses(void)
{
    size_t n = sizeof(CORPUS) / sizeof(CORPUS[0]);

    for (size_t i = 0; i < n; i++) {
        parse_infer_expr_t *e = parse_infer_parse(S(CORPUS[i]));

        if (!e) {
            printf("FAIL  cannot parse @infer(\"%s\")\n", CORPUS[i]);
            fails++;

            continue;
        }

        char *printed = parse_infer_print(e);

        parse_infer_expr_t *again = parse_infer_parse(S(printed));

        if (!again) {
            printf("FAIL  \"%s\" prints as \"%s\", which does not parse\n",
                   CORPUS[i],
                   printed);
            fails++;
            core_free(printed);
            parse_infer_free(e);

            continue;
        }

        char *twice = parse_infer_print(again);

        if (strcmp(printed, twice) != 0) {
            printf("FAIL  \"%s\" is not stable: \"%s\" then \"%s\"\n",
                   CORPUS[i],
                   printed,
                   twice);
            fails++;
        }

        core_free(printed);
        core_free(twice);
        parse_infer_free(e);
        parse_infer_free(again);
    }
}

static void
test_shapes(void)
{
    parse_infer_expr_t *e = parse_infer_parse(S("$0 unify list[`t]|array[`t]"));

    check("a mixed operator chain parses", e != nullptr);

    if (e) {
        check("and associates to the left, so the outermost node is the sum",
              e->kind == PARSE_INFER_SUM);
        check("whose left side is the unification",
              e->lhs && e->lhs->kind == PARSE_INFER_UNIFY);
        check("of the child reference",
              e->lhs && e->lhs->lhs
                  && e->lhs->lhs->kind == PARSE_INFER_CHILD
                  && e->lhs->lhs->index == 0);
        check("with a parameterized name",
              e->lhs && e->lhs->rhs
                  && e->lhs->rhs->kind == PARSE_INFER_NAMED_APP
                  && e->lhs->rhs->n_args == 1
                  && e->lhs->rhs->args[0]->kind == PARSE_INFER_TVAR);
        parse_infer_free(e);
    }

    e = parse_infer_parse(S("$3 unify (list[$1] | array[$1])"));

    check("a group parses", e != nullptr);

    if (e) {
        check("and the group is the right side of the unification",
              e->kind == PARSE_INFER_UNIFY && e->rhs
                  && e->rhs->kind == PARSE_INFER_SUM);
        parse_infer_free(e);
    }

    e = parse_infer_parse(S("$0[...$1]"));

    check("a spread parses", e != nullptr);

    if (e) {
        check("as one argument of a child application",
              e->kind == PARSE_INFER_CHILD_APP && e->index == 0
                  && e->n_args == 1
                  && e->args[0]->kind == PARSE_INFER_SPREAD
                  && e->args[0]->index == 1);
        parse_infer_free(e);
    }

    e = parse_infer_parse(S("dict[$0, $1]"));

    if (e) {
        check("a two-argument application keeps both arguments in order",
              e->kind == PARSE_INFER_NAMED_APP && e->n_args == 2
                  && e->args[0]->index == 0 && e->args[1]->index == 1);
        parse_infer_free(e);
    }
    else {
        check("dict[$0, $1] parses", false);
    }

    e = parse_infer_parse(S("unwrap_result($2)"));

    if (e) {
        check("a projection keeps its operator and its child",
              e->kind == PARSE_INFER_PROJECT
                  && e->proj == PARSE_INFER_UNWRAP_RESULT && e->index == 2);
        parse_infer_free(e);
    }
    else {
        check("unwrap_result($2) parses", false);
    }

    e = parse_infer_parse(S("$$"));

    if (e) {
        check("$$ is the production's own type", e->kind == PARSE_INFER_SELF);
        parse_infer_free(e);
    }
    else {
        check("$$ parses", false);
    }

    e = parse_infer_parse(S("$$ unify $return"));

    if (e) {
        check("the production's type and the function's return type are "
              "different nodes",
              e->kind == PARSE_INFER_UNIFY && e->lhs
                  && e->lhs->kind == PARSE_INFER_SELF && e->rhs
                  && e->rhs->kind == PARSE_INFER_RETURN);
        parse_infer_free(e);
    }
    else {
        check("$$ unify $return parses", false);
    }

    e = parse_infer_parse(S("return"));

    if (e) {
        check("return is an ordinary name, not a reserved word",
              e->kind == PARSE_INFER_NAMED
                  && core_string_eq(e->name, S("return")));
        parse_infer_free(e);
    }
    else {
        check("return parses as a name", false);
    }
}

static void
test_rejections(void)
{
    static const char *const BAD[] = {
        "",
        "$",
        "$x",
        "list[",
        "list[]",
        "unify $1",
        "$0 unify",
        "`",
        "lookup(0)",
        "lookup($0",
        "...$0",
        "return_of($0",
        "$0[...]",
        "$0[...$]",
        "$$0",
        "$$$",
        "$result",
        "$0 unify $nope",
    };

    size_t n = sizeof(BAD) / sizeof(BAD[0]);

    for (size_t i = 0; i < n; i++) {
        parse_infer_expr_t *e = parse_infer_parse(S(BAD[i]));

        if (e) {
            char *p = parse_infer_print(e);

            printf("FAIL  \"%s\" is not an @infer expression, but parsed "
                   "as \"%s\"\n",
                   BAD[i],
                   p);
            fails++;
            core_free(p);
            parse_infer_free(e);
        }
    }
}

int
main(void)
{
    test_the_corpus_parses();
    test_shapes();
    test_rejections();

    parse_annot_grammar_shutdown();

    return report("fe infer");
}
