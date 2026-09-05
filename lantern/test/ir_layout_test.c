#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"

static int  fails = 0;
static bool produced[IR_DIAG_COUNT];

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
} env;

static env *open_env(const char *target)
{
    env *e = calloc(1, sizeof(env));

    e->ctx = ir_ctx_new();
    e->m   = ir_module_new(e->ctx, "m", &ir_profile_bce, target);
    e->d   = ir_diags_new(e->ctx, nullptr);

    ir_module_set_span(e->m, ir_span_make(0, 0, 6, 1, 1, 1, 7));
    ir_module_establish(e->m, IR_PROP_PARSED | IR_PROP_TYPED);

    return e;
}

static bool told(env *e, const char *text)
{
    for (uint32_t i = 0; i < ir_diags_count(e->d); i++) {
        const ir_diag_t *dg = ir_diags_at(e->d, i);

        if ((dg->primary.label && strstr(dg->primary.label, text))
            || (dg->message && strstr(dg->message, text))) {
            return true;
        }
    }

    return false;
}

static void harvest(env *e)
{
    for (uint32_t i = 0; i < ir_diags_count(e->d); i++) {
        produced[ir_diags_at(e->d, i)->code] = true;
    }
}

static void close_env(env *e)
{
    harvest(e);
    ir_ctx_free(e->ctx);
    free(e);
}

static th_type *ty(env *e, const char *t)
{
    return ir_type_parse(e->m, t);
}

static ir_func_t *touching(env *e, const char *const *types, size_t n)
{
    ir_func_t  *f = ir_func_add(e->m, "f", ty(e, "() -> void"),
                                IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, "entry");

    for (size_t i = 0; i < n; i++) {
        ir_instr_t *in = ir_instr_add(b, IR_OP_SIZEOF);

        ir_instr_push_type(in, ty(e, types[i]));
    }
    ir_instr_add(b, IR_OP_UNREACHABLE);

    return f;
}

static void test_descriptors_are_keyed_and_scoped(void)
{
    env *e = open_env("x86_64");

    th_type *t = ty(e, "i32");

    ir_descriptor_t *a = ir_layout_ensure(e->m, t, "x86_64");
    ir_descriptor_t *b = ir_layout_ensure(e->m, t, "wasm32");

    check("one type can carry a descriptor per target", a && b && a != b);
    check("and they are found back by target",
          ir_layout_of(e->m, t, "x86_64") == a
              && ir_layout_of(e->m, t, "wasm32") == b);
    check("the module's own target is the default",
          ir_layout_of(e->m, t, nullptr) == a);
    check("two descriptors are counted as two", ir_layout_count(e->m, t) == 2);

    uint64_t before = th_hash(ir_ctx_types(e->ctx), t);

    a->size  = (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = 4};
    b->size  = (ir_layout_val_t){.state = IR_LAYOUT_RESOLVED, .value = 99};

    check("changing a descriptor does not change the type's hash",
          th_hash(ir_ctx_types(e->ctx), t) == before);
    check("a second spelling of the same type finds the same descriptor",
          ir_layout_of(e->m, ty(e, "i32"), "x86_64") == a);
    close_env(e);
}

static void test_the_pass_computes_around_pins(void)
{
    const char *types[] = {"i32", "u8", "f64", "bool", "(x: i32, y: u64)",
                           "array[i32; 4]", "void"};
    env        *e       = open_env("x86_64");

    touching(e, types, 7);
    check("the layout pass runs", ir_layout_run(e->m, e->d));

    struct {
        const char *type;
        uint64_t    size;
        uint64_t    align;
    } want[] = {
        {"i32", 4, 4}, {"u8", 1, 1}, {"f64", 8, 8}, {"bool", 1, 1},
        {"(x: i32, y: u64)", 16, 8}, {"array[i32; 4]", 16, 4},
        {"void", 0, 1},
    };

    for (size_t i = 0; i < 7; i++) {
        ir_descriptor_t *d = ir_layout_of(e->m, ty(e, want[i].type), nullptr);

        if (!d || d->size.value != want[i].size
            || d->align.value != want[i].align) {
            printf("FAIL  %s lays out as %llu/%llu, wanted %llu/%llu\n",
                   want[i].type,
                   (unsigned long long)(d ? d->size.value : 0),
                   (unsigned long long)(d ? d->align.value : 0),
                   (unsigned long long)want[i].size,
                   (unsigned long long)want[i].align);
            fails++;
        }
    }

    ir_descriptor_t *rec = ir_layout_of(e->m, ty(e, "(x: i32, y: u64)"),
                                        nullptr);

    check("a member's bit offset and width are both recorded",
          rec && rec->n_members == 2
              && rec->members[0].bit_offset.value == 0
              && rec->members[0].bit_width.value == 32
              && rec->members[1].bit_offset.value == 64
              && rec->members[1].bit_width.value == 64);
    check("padding shows as a gap between members, not as a member",
          rec && rec->n_members == 2);
    close_env(e);
}

static void test_pins_are_honored(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    check("a size pin is accepted", ir_layout_pin_size(e->m, t, 16));
    check("an alignment pin is accepted", ir_layout_pin_align(e->m, t, 16));
    check("a member pin is accepted",
          ir_layout_pin_member(e->m, t, "y", 96, 32));
    check("an alignment that is not a power of two is refused",
          !ir_layout_pin_align(e->m, t, 6));
    check("a pin that contradicts one already set is refused",
          !ir_layout_pin_size(e->m, t, 8));

    check("the pass fills around the pins", ir_layout_run(e->m, e->d));

    ir_descriptor_t *d = ir_layout_of(e->m, t, nullptr);

    check("the pinned size survives",
          d && d->size.state == IR_LAYOUT_PINNED && d->size.value == 16);
    check("the pinned alignment survives",
          d && d->align.state == IR_LAYOUT_PINNED && d->align.value == 16);

    ir_member_layout_t *y = nullptr;
    ir_member_layout_t *x = nullptr;

    for (uint32_t i = 0; d && i < d->n_members; i++) {
        if (!strcmp(d->members[i].name, "y")) {
            y = &d->members[i];
        }
        else {
            x = &d->members[i];
        }
    }

    check("the pinned member stays where it was put",
          y && y->bit_offset.state == IR_LAYOUT_PINNED
              && y->bit_offset.value == 96);
    check("and the unpinned one is placed by the pass",
          x && x->bit_offset.state == IR_LAYOUT_RESOLVED);
    close_env(e);
}

static void test_an_unsatisfiable_pin_fails_loudly(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    ir_layout_pin_size(e->m, t, 4);

    bool ran = ir_layout_run(e->m, e->d);

    harvest(e);

    check("a size too small for the members fails the pass", !ran);
    check("and says so rather than overriding the pin",
          told(e, "smaller than"));
    check("and the pin is left as the author set it",
          ir_layout_of(e->m, t, nullptr)->size.value == 4);
    check("so the module is not laid out",
          !(ir_module_props(e->m) & IR_PROP_LAID_OUT));
    close_env(e);
}

static void test_symbolic_survives_a_target_change(void)
{
    const char *types[] = {"ptr[i32]", "Widget"};
    env        *e       = open_env("x86_64");

    touching(e, types, 2);
    ir_layout_run(e->m, e->d);

    ir_descriptor_t *p = ir_layout_of(e->m, ty(e, "ptr[i32]"), nullptr);
    ir_descriptor_t *w = ir_layout_of(e->m, ty(e, "Widget"), nullptr);

    check("a pointer's size is symbolic, not a number the pass invented",
          p && p->size.state == IR_LAYOUT_SYMBOLIC
              && p->size.sym == IR_SYM_SIZEOF);
    check("and so is an opaque named type's",
          w && w->size.state == IR_LAYOUT_SYMBOLIC);
    check("a symbolic value names the type it is about",
          p && ir_type_eq(e->m, p->size.of, ty(e, "ptr[i32]")));
    check("a module with symbolic layout is not laid out for its target",
          !(ir_module_props(e->m) & IR_PROP_LAID_OUT));

    ir_descriptor_t *other = ir_layout_ensure(e->m, ty(e, "ptr[i32]"),
                                              "wasm32");

    check("the same symbolic expression is meaningful under another target",
          other && other->size.state == IR_LAYOUT_UNSET);
    close_env(e);
}

static void test_laid_out_is_verified(void)
{
    const char *types[] = {"i32", "(x: i32, y: u64)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 2);
    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    check("a fully resolved module verifies as laid out",
          (ir_verify(e->m, e->d) & IR_PROP_LAID_OUT) != 0);
    close_env(e);

    const char *unset[] = {"ptr[i32]"};
    env        *u       = open_env("x86_64");

    touching(u, unset, 1);
    ir_layout_run(u->m, u->d);
    ir_module_establish(u->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(u->m, u->d);

    harvest(u);

    check("a module with an unresolved value is not laid out",
          !(held & IR_PROP_LAID_OUT));
    check("and the verifier names the rule",
          told(u, "laid-out.descriptor"));
    close_env(u);
}

static void test_overlapping_members_are_caught(void)
{
    const char *types[] = {"(x: i32, y: i32)"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    th_type *t = ty(e, "(x: i32, y: i32)");

    ir_layout_pin_member(e->m, t, "x", 0, 32);
    ir_layout_pin_member(e->m, t, "y", 16, 32);
    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(e->m, e->d);

    harvest(e);

    check("members whose bits overlap are caught",
          !(held & IR_PROP_LAID_OUT) && told(e, "laid-out.overlap"));
    close_env(e);
}

static void test_bitcast_sizes_agree(void)
{
    env        *e = open_env("x86_64");
    ir_func_t  *f = ir_func_add(e->m, "f", ty(e, "(u64) -> void"),
                                IR_LINK_EXTERNAL);
    ir_value_t *a = ir_param_add(f, "a", ty(e, "u64"));
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *c = ir_instr_add(b, IR_OP_BITCAST);

    ir_instr_set_dest(c, "s", ty(e, "u32"));
    ir_instr_push_value(c, a);
    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_layout_run(e->m, e->d);
    ir_module_establish(e->m, IR_PROP_LAID_OUT);

    ir_props_t held = ir_verify(e->m, e->d);

    harvest(e);

    check("a bitcast between differently sized types is caught once laid out",
          !(held & IR_PROP_LAID_OUT) && told(e, "laid-out.bitcast"));
    close_env(e);
}

static void test_the_pass_declares_what_the_spec_says(void)
{
    check("the layout pass declares requires typed, establishes laid-out",
          ir_pass_layout.requires == IR_PROP_TYPED
              && ir_pass_layout.establishes == IR_PROP_LAID_OUT
              && ir_pass_declares(&ir_pass_layout));

    const char *types[] = {"i32"};
    env        *e       = open_env("x86_64");

    touching(e, types, 1);

    ir_props_t unmet = 0;

    check("it runs through the pass manager",
          ir_pass_run_checked(&ir_pass_layout, e->m, e->d, &unmet, e->d)
              == IR_PASS_OK);
    check("and establishes the property it declared",
          (ir_module_props(e->m) & IR_PROP_LAID_OUT) != 0);
    close_env(e);
}

static void test_comptime_is_symbolic_until_laid_out(void)
{
    const char *types[] = {"i32"};
    env        *e       = open_env("x86_64");
    ir_func_t  *f       = touching(e, types, 1);
    ir_instr_t *sz      = ir_block_entry(f)->instrs[0];
    ir_rules_t *rs      = ir_module_rules(e->m);

    ir_rule_apply(e->m, rs, sz, e->d);
    check("sizeof has no value before the layout pass runs",
          ir_annot_get(&sz->annots, "ir.const") == nullptr);

    ir_layout_run(e->m, e->d);
    ir_module_invalidate(e->m, IR_PROP_TYPED);
    ir_rule_apply(e->m, rs, sz, e->d);

    ir_annot_t *k = ir_annot_get(&sz->annots, "ir.const");

    check("and takes one once the type is laid out",
          k && k->kind == IR_ANNOT_INT && k->i.mag == 4);

    ir_instr_t *al = ir_instr_add(ir_block_entry(f), IR_OP_ALIGNOF);

    ir_instr_push_type(al, ty(e, "i32"));
    ir_rule_apply(e->m, rs, al, e->d);

    ir_annot_t *a = ir_annot_get(&al->annots, "ir.const");

    check("alignof resolves the same way", a && a->i.mag == 4);

    ir_instr_t *of = ir_instr_add(ir_block_entry(f), IR_OP_OFFSETOF);

    ir_instr_push_type(of, ty(e, "(x: i32, y: u64)"));
    ir_instr_push_name(of, "y");
    ir_layout_run(e->m, e->d);
    ir_rule_apply(e->m, rs, of, e->d);

    ir_annot_t *o = ir_annot_get(&of->annots, "ir.const");

    check("and so does offsetof, in bits", o && o->i.mag == 64);

    ir_instr_t *sp = ir_instr_add(ir_block_entry(f), IR_OP_SIZEOF);

    ir_instr_push_type(sp, ty(e, "ptr[i32]"));
    ir_layout_run(e->m, e->d);
    ir_rule_apply(e->m, rs, sp, e->d);
    check("a type whose size stays symbolic gets no value",
          ir_annot_get(&sp->annots, "ir.const") == nullptr);
    close_env(e);
}

static void test_a_module_with_no_target(void)
{
    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    ir_ctx_t                 *ctx        = ir_ctx_new();
    ir_diags_t               *d          = ir_diags_new(ctx, nullptr);
    const char               *src        = "module m {\n  profile bce\n\n"
                                           "  func @f(%a: i32) -> i32 {\n"
                                           "    block entry:\n"
                                           "      %r: i32 = add wrap %a, %a\n"
                                           "      return %r\n"
                                           "  }\n}\n";
    ir_module_t              *m = ir_module_parse(ctx, src, 0, profiles, 1, d);

    check("a module may name no target", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);
    check("and laying it out does not crash", ir_layout_run(m, d));

    th_type *i32 = ir_type_parse(m, "i32");

    check("its descriptors are keyed to having no target",
          ir_layout_of(m, i32, nullptr) != nullptr);
    check("and are not confused with a named target's",
          ir_layout_of(m, i32, "host") == nullptr);

    char *text = ir_module_print(m);

    check("and printing it does not crash", text != nullptr);
    check("and it still says no target",
          text && !strstr(text, "target"));
    free(text);
    ir_ctx_free(ctx);
}

static void test_variant_and_indirection_are_recorded(void)
{
    const char *types[] = {"variant R[i32]", "ptr[i32]"};
    env        *e       = open_env("x86_64");

    ir_type_define(e->m, ty(e, "variant R[i32]"), ty(e, "(ok: i32, err: u8)"));
    touching(e, types, 2);
    ir_layout_run(e->m, e->d);

    ir_descriptor_t *v = ir_layout_of(e->m, ty(e, "variant R[i32]"), nullptr);

    check("a variant records where its tag lives",
          v && ir_layout_is_settled(&v->variant.tag_bit_offset)
              && ir_layout_is_settled(&v->variant.tag_bit_width));
    check("and is not using a niche unless told to",
          v && !v->variant.niche);
    check("its payload is sized by the widest arm",
          v && v->size.value >= 12);

    ir_variant_repr_t niche = {.niche = true, .tag_bit_width = {.value = 1}};

    check("variant representation is settable",
          ir_layout_pin_variant(e->m, ty(e, "variant R[i32]"), niche));
    check("and the niche choice is recorded",
          ir_layout_of(e->m, ty(e, "variant R[i32]"), nullptr)->variant.niche);

    ir_descriptor_t *p = ir_layout_of(e->m, ty(e, "ptr[i32]"), nullptr);

    check("an indirection records its representation",
          p && !p->indirect.fat
              && ir_layout_is_settled(&p->indirect.words)
              && p->indirect.words.value == 1);
    close_env(e);
}

static void write_produced(const char *dir)
{
    size_t n    = strlen(dir) + 32;
    char  *path = malloc(n);

    snprintf(path, n, "%s/produced-codes-layout.txt", dir);

    FILE *f = fopen(path, "w");

    if (f) {
        for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
            if (produced[i]) {
                fprintf(f, "%s\n", ir_diag_code((ir_diagcode_t)i));
            }
        }
        fclose(f);
    }
    free(path);
}

int main(int argc, char **argv)
{
    test_descriptors_are_keyed_and_scoped();
    test_the_pass_computes_around_pins();
    test_pins_are_honored();
    test_an_unsatisfiable_pin_fails_loudly();
    test_symbolic_survives_a_target_change();
    test_laid_out_is_verified();
    test_overlapping_members_are_caught();
    test_bitcast_sizes_agree();
    test_the_pass_declares_what_the_spec_says();
    test_comptime_is_symbolic_until_laid_out();
    test_a_module_with_no_target();
    test_variant_and_indirection_are_recorded();

    if (argc > 1) {
        write_produced(argv[1]);
    }

    if (!fails) {
        printf("ir layout: all checks passed\n");
    }

    return fails ? 1 : 0;
}
