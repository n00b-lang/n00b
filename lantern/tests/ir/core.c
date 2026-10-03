#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "harness.h"

static void test_ops(void)
{
    check("op table is populated", IR_OP_COUNT > 0);

    ir_op_t op;

    check("lookup finds add", ir_op_lookup("add", &op) && op == IR_OP_ADD);
    check("lookup finds a dotted mnemonic",
          ir_op_lookup("variant.make", &op) && op == IR_OP_VARIANT_MAKE);
    check("lookup finds runtime type testing",
          ir_op_lookup("type.test", &op) && op == IR_OP_TYPE_TEST);
    check("lookup finds semantic array construction",
          ir_op_lookup("array.new", &op) && op == IR_OP_ARRAY_NEW);
    check("lookup rejects an unknown mnemonic",
          !ir_op_lookup("frobnicate", &op));

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        const ir_opinfo_t *info = ir_op_info((ir_op_t)i);
        ir_op_t            back;

        if (!info || !info->spelling || !ir_op_lookup(info->spelling, &back)
            || back != (ir_op_t)i) {
            check("every opcode round-trips through its spelling", false);
            break;
        }
    }

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < IR_OP_COUNT; j++) {
            if (!strcmp(ir_op_spelling((ir_op_t)i),
                        ir_op_spelling((ir_op_t)j))) {
                check("opcode spellings are unique", false);
                return;
            }
        }
    }

    check("add carries a result", ir_op_has_result(IR_OP_ADD));
    check("store carries no result", !ir_op_has_result(IR_OP_STORE));
    check("alloc carries an address result", ir_op_has_result(IR_OP_ALLOC));
    check("address.of carries an address result",
          ir_op_has_result(IR_OP_ADDRESS_OF));
    check("finalize and dealloc have no result",
          !ir_op_has_result(IR_OP_FINALIZE)
              && !ir_op_has_result(IR_OP_DEALLOC));
    check("branch is a terminator", ir_op_is_terminator(IR_OP_BRANCH));
    check("trap is a no-result zero-operand terminator",
          ir_op_is_terminator(IR_OP_TRAP)
              && !ir_op_has_result(IR_OP_TRAP)
              && ir_op_accepts_operands(IR_OP_TRAP, 0)
              && !ir_op_accepts_operands(IR_OP_TRAP, 1));
    check("add is not a terminator", !ir_op_is_terminator(IR_OP_ADD));

    check("add takes two operands", ir_op_accepts_operands(IR_OP_ADD, 2));
    check("add rejects one operand", !ir_op_accepts_operands(IR_OP_ADD, 1));
    check("add rejects three operands", !ir_op_accepts_operands(IR_OP_ADD, 3));
    check("const supports scalar or payload forms",
          ir_op_accepts_operands(IR_OP_CONST, 0)
              && ir_op_accepts_operands(IR_OP_CONST, 1)
              && !ir_op_accepts_operands(IR_OP_CONST, 2));
    check("call is variadic", ir_op_accepts_operands(IR_OP_CALL, 9));
    check("array.new accepts a variable number of extent values",
          ir_op_has_result(IR_OP_ARRAY_NEW)
              && ir_op_accepts_operands(IR_OP_ARRAY_NEW, 0)
              && ir_op_accepts_operands(IR_OP_ARRAY_NEW, 4)
              && ir_op_operand_kind(IR_OP_ARRAY_NEW, 3) == IR_OPD_VALUE);
    check("array.new.using reserves its first value for allocator selection",
          ir_op_has_result(IR_OP_ARRAY_NEW_USING)
              && ir_op_accepts_operands(IR_OP_ARRAY_NEW_USING, 1)
              && ir_op_operand_kind(IR_OP_ARRAY_NEW_USING, 0)
                  == IR_OPD_VALUE);
    check("return accepts a value and semantic copy-out values",
          ir_op_accepts_operands(IR_OP_RETURN, 0)
              && ir_op_accepts_operands(IR_OP_RETURN, 1)
              && ir_op_accepts_operands(IR_OP_RETURN, 2)
              && ir_op_accepts_operands(IR_OP_RETURN, 8));

    check("branch operand kinds come from the pattern",
          ir_op_operand_kind(IR_OP_BRANCH, 0) == IR_OPD_VALUE
              && ir_op_operand_kind(IR_OP_BRANCH, 1) == IR_OPD_BLOCK
              && ir_op_operand_kind(IR_OP_BRANCH, 2) == IR_OPD_BLOCK);
    check("field takes a value then a name",
          ir_op_operand_kind(IR_OP_FIELD, 0) == IR_OPD_VALUE
              && ir_op_operand_kind(IR_OP_FIELD, 1) == IR_OPD_NAME);
    check("sizeof takes a type",
          ir_op_operand_kind(IR_OP_SIZEOF, 0) == IR_OPD_TYPE);
    check("phi operands repeat as arms",
          ir_op_operand_kind(IR_OP_PHI, 0) == IR_OPD_PHI_ARM
              && ir_op_operand_kind(IR_OP_PHI, 7) == IR_OPD_PHI_ARM);

    check("overflow modifiers apply to add",
          ir_op_info(IR_OP_ADD)->mods & IR_MOD_OVERFLOW);
    check("overflow modifiers do not apply to and",
          !(ir_op_info(IR_OP_AND)->mods & IR_MOD_OVERFLOW));
    check("div takes both overflow and divzero",
          (ir_op_info(IR_OP_DIV)->mods & IR_MOD_OVERFLOW)
              && (ir_op_info(IR_OP_DIV)->mods & IR_MOD_DIVZERO));
}

static void test_props(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    check("a new module has no properties", ir_module_props(m) == 0);

    ir_module_establish(m, IR_PROP_PARSED);
    ir_module_establish(m, IR_PROP_TYPED);
    check("properties accumulate",
          ir_module_props(m) == (IR_PROP_PARSED | IR_PROP_TYPED));

    ir_module_invalidate(m, IR_PROP_TYPED);
    check("invalidation clears only what it names",
          ir_module_props(m) == IR_PROP_PARSED);

    check("property names are spelled",
          !strcmp(ir_prop_name(IR_PROP_SSA), "ssa")
              && !strcmp(ir_prop_name(IR_PROP_LAID_OUT), "laid-out"));

    ir_ctx_free(ctx);
}

static void test_types(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    check("module names its profile",
          !strcmp(ir_module_profile(m)->name, "bce"));
    check("module names its target", !strcmp(ir_module_target(m), "test"));

    th_type *a = ir_type_parse(m, "i32");
    th_type *b = ir_type_parse(m, "i32");

    check("a type parses", a != nullptr);
    check("interning returns one node for one type", a == b);
    check("equal types are equal by hash", ir_type_eq(m, a, b));
    check("one interned type is counted once", ir_type_count(m) == 1);

    th_type *c = ir_type_parse(m, "u32");

    check("different types are not equal", !ir_type_eq(m, a, c));
    check("a second type is counted", ir_type_count(m) == 2);

    th_ctx *types = ir_ctx_types(ctx);
    th_type *inferred = th_var(types);
    uint64_t unresolved_id = ir_type_id(m, inferred);
    check("an open type variable gets an identity", unresolved_id != 0);
    check("type identity follows later unification",
          th_unify_in_place(types, inferred, a)
              && ir_type_eq(m, inferred, a)
              && ir_type_id(m, inferred) == ir_type_id(m, a));

    th_type *r1 = ir_type_parse(m, "(x: i32, y: i32)");
    th_type *r2 = ir_type_parse(m, "(y: i32, x: i32)");

    check("field order does not affect identity", r1 == r2);

    th_type *v1 = ir_type_parse(m, "Vec[`a]");
    th_type *v2 = ir_type_parse(m, "Vec[`b]");

    check("alpha-equivalent types are one type", v1 == v2);

    bool found_a = false;
    bool found_c = false;

    for (size_t i = 0; i < ir_type_count(m); i++) {
        found_a = found_a || ir_type_at(m, i) == a;
        found_c = found_c || ir_type_at(m, i) == c;
    }
    check("the type table can be walked, and holds what was interned",
          found_a && found_c);
    check("and walking past the end gives nothing",
          ir_type_at(m, ir_type_count(m)) == nullptr);

    ir_ctx_free(ctx);
}

static char *wide(const char *open, const char *item, const char *sep,
                  const char *close, uint32_t n, bool number)
{
    size_t room = (size_t)n * 32 + strlen(open) + strlen(close) + 8;
    char  *s    = malloc(room);
    size_t at   = (size_t)snprintf(s, room, "%s", open);

    for (uint32_t i = 0; i < n; i++) {
        at += (size_t)snprintf(s + at, room - at, "%s", i ? sep : "");
        at += number ? (size_t)snprintf(s + at, room - at, item, i)
                     : (size_t)snprintf(s + at, room - at, "%s", item);
    }
    snprintf(s + at, room - at, "%s", close);

    return s;
}

static void test_a_type_has_no_size_limit(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    struct {
        const char *what;
        const char *open;
        const char *item;
        const char *sep;
        const char *close;
        bool        number;
    } shape[] = {
        {"record fields", "(", "f%u: i32", ", ", ")", true},
        {"tuple elements", "t(", "i32", ", ", ")", false},
        {"function parameters", "(", "i32", ", ", ") -> i32", false},
        {"keyword parameters", "(", "k%u: i32", ", ", ") -> i32", true},
        {"union arms", "", "N%u", " | ", "", true},
        {"forall binders", "forall[", "`v%u", ",", "] i32", true},
        {"free type variables", "t(", "`v%u", ", ", ")", true},
    };

    for (size_t i = 0; i < sizeof(shape) / sizeof(shape[0]); i++) {
        char *s = wide(shape[i].open, shape[i].item, shape[i].sep,
                       shape[i].close, 300, shape[i].number);
        th_type *t = ir_type_parse(m, s);

        if (!t) {
            printf("FAIL  300 %s do not parse\n  %.60s...\n", shape[i].what,
                   s);
            fails++;
            free(s);
            continue;
        }

        const char *back  = th_print(ir_ctx_types(ctx), t);
        th_type    *again = back ? ir_type_parse(m, back) : nullptr;

        if (again != t) {
            printf("FAIL  a type of 300 %s does not survive printing\n",
                   shape[i].what);
            fails++;
        }
        free(s);
    }
    ir_ctx_free(ctx);
}

static char *nested(uint32_t d, const char *leaf)
{
    size_t room = (size_t)d * 6 + 32;
    char  *s    = malloc(room);
    size_t at   = 0;

    for (uint32_t i = 0; i < d; i++) {
        at += (size_t)snprintf(s + at, room - at, "ptr[");
    }
    at += (size_t)snprintf(s + at, room - at, "%s", leaf);

    for (uint32_t i = 0; i < d; i++) {
        at += (size_t)snprintf(s + at, room - at, "]");
    }

    return s;
}

static void test_identity_is_the_whole_type(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");
    char        *a   = nested(400, "i32");
    char        *b   = nested(400, "i64");
    th_type     *ta  = ir_type_parse(m, a);
    th_type     *tb  = ir_type_parse(m, b);

    check("a deeply nested type parses", ta && tb);
    check("two types that differ only deep down are two types",
          ta != tb && !ir_type_eq(m, ta, tb));
    free(a);
    free(b);

    char    *deep = nested(100000, "i32");
    th_type *td   = ir_type_parse(m, deep);

    check("nesting is bounded by memory, not by a recursion limit", td);
    check("and a hundred thousand deep is still not the four hundred",
          td && td != ta && !ir_type_eq(m, td, ta));
    free(deep);
    ir_ctx_free(ctx);
}

static void test_machine_classes(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    struct {
        const char *text;
        ir_class_t  class;
        uint32_t    width;
    } cases[] = {
        {"i32", IR_CLASS_SINT, 32},
        {"u64", IR_CLASS_UINT, 64},
        {"f64", IR_CLASS_FLOAT, 64},
        {"bool", IR_CLASS_BOOL, 1},
        {"void", IR_CLASS_VOID, 0},
        {"bitint[7]", IR_CLASS_SINT, 7},
        {"ubitint[13]", IR_CLASS_UINT, 13},
        {"ptr[i32]", IR_CLASS_PTR, 0},
        {"array[i32; 4]", IR_CLASS_AGGREGATE, 0},
        {"(x: i32)", IR_CLASS_AGGREGATE, 0},
        {"(i32) -> i32", IR_CLASS_PTR, 0},
        {"(i32 | u32)", IR_CLASS_AGGREGATE, 0},
        {"`a", IR_CLASS_OPAQUE, 0},
        {"forall[`a] Vec[`a]", IR_CLASS_OPAQUE, 0},
        {"Widget", IR_CLASS_OPAQUE, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        th_type     *t = ir_type_parse(m, cases[i].text);
        ir_machine_t g = ir_machine_of(m, t);

        if (g.class != cases[i].class || g.width != cases[i].width) {
            printf("FAIL  machine class of %s: got class %d width %u\n",
                   cases[i].text, (int)g.class, g.width);
            fails++;
        }
    }

    check("a profiled name is in the profile",
          ir_profile_has(m, ir_type_parse(m, "i32")));
    check("an unprofiled name is not",
          !ir_profile_has(m, ir_type_parse(m, "Widget")));

    th_ctx *types = ir_ctx_types(ctx);
    th_type *inferred = th_var(types);
    check("an unresolved variable has no machine class",
          ir_machine_of(m, inferred).class == IR_CLASS_OPAQUE);
    check("unifying an inferred type exposes its machine class",
          th_unify_in_place(types, inferred, ir_type_parse(m, "i32"))
              && ir_machine_of(m, inferred).class == IR_CLASS_SINT
              && ir_machine_of(m, inferred).width == 32);
    check("a bound inferred type is recognized by the profile",
          ir_profile_has(m, inferred));

    ir_ctx_free(ctx);
}

static void test_second_profile(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_synth, "test");

    ir_machine_t i = ir_machine_of(m, ir_type_parse(m, "int"));

    check("the synthetic profile makes int 64-bit signed",
          i.class == IR_CLASS_SINT && i.width == 64);

    ir_machine_t w = ir_machine_of(m, ir_type_parse(m, "word"));

    check("the synthetic profile has its own unsigned name",
          w.class == IR_CLASS_UINT && w.width == 32);

    ir_machine_t g = ir_machine_of(m, ir_type_parse(m, "i32"));

    check("i32 is opaque under the synthetic profile",
          g.class == IR_CLASS_OPAQUE);

    ir_machine_t b = ir_machine_of(m, ir_type_parse(m, "bitint[7]"));

    check("bitint ignores the profile entirely",
          b.class == IR_CLASS_SINT && b.width == 7);

    ir_ctx_free(ctx);
}

static void test_files_and_spans(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    int32_t f0 = ir_file_add(m, "a.bce");
    int32_t f1 = ir_file_add(m, "b.bce");
    int32_t f0b = ir_file_add(m, "a.bce");

    check("file ids start at zero", f0 == 0);
    check("a second file gets a new id", f1 == 1);
    check("adding a path twice reuses the id", f0b == f0);
    check("file count reflects distinct paths", ir_file_count(m) == 2);
    check("paths come back", !strcmp(ir_file_path(m, f1), "b.bce"));
    check("an out-of-range id has no path", ir_file_path(m, 7) == nullptr);
    check("an unknown id has no path", ir_file_path(m, -1) == nullptr);

    ir_span_t unknown = IR_SPAN_UNKNOWN;

    check("the unknown span is unknown", !ir_span_known(unknown));
    check("the unknown file id is -1", unknown.file == -1);

    ir_span_t s = ir_span_make(f0, 10, 20, 1, 1, 1, 11);

    check("a made span is known", ir_span_known(s));
    check("spans keep byte offsets", s.start_byte == 10 && s.end_byte == 20);
    check("spans keep line and column", s.start_line == 1 && s.start_col == 1);

    ir_ctx_free(ctx);
}

static void test_origins(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");
    int32_t      f   = ir_file_add(m, "a.bce");
    ir_span_t    s   = ir_span_make(f, 0, 1, 1, 1, 1, 2);

    ir_origin_t *src = ir_origin_new(ctx, IR_ORIGIN_SOURCE, s, nullptr,
                                     nullptr);

    check("a source origin needs no agent", src != nullptr);
    check("a source origin has depth one", ir_origin_depth(src) == 1);

    check("a non-source origin requires an agent",
          ir_origin_new(ctx, IR_ORIGIN_GENERATED_BY, s, nullptr, nullptr)
              == nullptr);

    ir_origin_t *gen = ir_origin_new(ctx, IR_ORIGIN_GENERATED_BY, s, "promote",
                                     src);

    check("an origin chains to its parent",
          gen && gen->parent == src && ir_origin_depth(gen) == 2);
    check("the agent is kept", !strcmp(gen->agent, "promote"));

    ir_origin_t *inl = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, s, "inline",
                                     gen);

    check("chains nest", ir_origin_depth(inl) == 3);

    ir_origin_t loop = {.kind = IR_ORIGIN_GENERATED_BY, .agent = "x"};

    loop.parent = &loop;
    check("a cyclic parent chain is rejected",
          ir_origin_new(ctx, IR_ORIGIN_GENERATED_BY, s, "y", &loop)
              == nullptr);

    ir_ctx_free(ctx);
}

static void test_annots(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    check("ir is a reserved namespace", ir_annot_key_ok("ir.rule", "bce"));
    check("the profile namespace is allowed",
          ir_annot_key_ok("bce.gcroot", "bce"));
    check("another profile's namespace is not",
          !ir_annot_key_ok("n00b.concat", "bce"));
    check("an unnamespaced key is rejected", !ir_annot_key_ok("rule", "bce"));
    check("an empty namespace is rejected", !ir_annot_key_ok(".rule", "bce"));
    check("an empty name is rejected", !ir_annot_key_ok("ir.", "bce"));

    ir_annots_t *am = ir_module_annots(m);

    check("a new node has no annotations", ir_annot_count(am) == 0);

    ir_annot_set(ctx, am,
                 (ir_annot_t){.key = "bce.pinned", .kind = IR_ANNOT_BOOL,
                              .b = true});
    check("an authored annotation lands", ir_annot_count(am) == 1);
    check("an annotation reads back",
          ir_annot_get(am, "bce.pinned")->b == true);
    check("an authored annotation has no provenance",
          ir_annot_get(am, "bce.pinned")->derived_from == 0);

    ir_annot_set(ctx, am,
                 (ir_annot_t){.key = "ir.rule", .kind = IR_ANNOT_STR,
                              .s = "sint", .derived_from = IR_PROP_TYPED});
    check("a derived annotation names its property",
          ir_annot_get(am, "ir.rule")->derived_from == IR_PROP_TYPED);
    check("two annotations coexist", ir_annot_count(am) == 2);

    check("a derived write may not overwrite an authored annotation",
          !ir_annot_set(ctx, am,
                        (ir_annot_t){.key = "bce.pinned",
                                     .kind = IR_ANNOT_BOOL, .b = false,
                                     .derived_from = IR_PROP_TYPED}));
    check("the authored value survives",
          ir_annot_get(am, "bce.pinned")->b == true);

    ir_module_invalidate(m, IR_PROP_TYPED);
    check("invalidating a property drops what it derived",
          ir_annot_get(am, "ir.rule") == nullptr);
    check("the authored annotation is untouched",
          ir_annot_get(am, "bce.pinned") != nullptr);
    check("only the derived one was dropped", ir_annot_count(am) == 1);

    ir_ctx_free(ctx);
}

static void test_structure(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    check("module owns its name", !strcmp(ir_module_name(m), "m"));

    th_type     *gt = ir_type_parse(m, "i32");
    ir_global_t *g  = ir_global_add(m, "counter", gt);

    check("a global is added", g != nullptr);
    check("a duplicate global is refused",
          ir_global_add(m, "counter", gt) == nullptr);
    check("globals are counted", ir_global_count(m) == 1);
    check("globals are found by name", ir_global_find(m, "counter") == g);
    check("a global carries a span slot", !ir_span_known(g->span));

    th_type   *ft = ir_type_parse(m, "(i32) -> i32");
    ir_func_t *f  = ir_func_add(m, "sum", ft, IR_LINK_EXTERNAL);

    check("a function is added", f != nullptr);
    check("a duplicate function is refused",
          ir_func_add(m, "sum", ft, IR_LINK_EXTERNAL) == nullptr);
    check("a non-function type is refused",
          ir_func_add(m, "bad", gt, IR_LINK_EXTERNAL) == nullptr);
    check("functions are counted", ir_func_count(m) == 1);
    check("functions are found by name", ir_func_find(m, "sum") == f);

    ir_value_t *p = ir_param_add(f, "n", gt);

    check("a parameter is added", p && f->n_params == 1);
    check("a parameter is a value with a type",
          p->kind == IR_VAL_PARAM && p->type == gt);

    ir_local_t *l = ir_local_add(f, "acc", gt);

    check("a local is added", l && f->n_locals == 1);
    check("a duplicate local is refused",
          ir_local_add(f, "acc", gt) == nullptr);
    check("locals are found by name", ir_local_find(f, "acc") == l);

    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *body  = ir_block_add(f, "body");

    check("blocks are added", entry && body && f->n_blocks == 2);
    check("the first block is the entry", ir_block_entry(f) == entry);
    check("a duplicate block is refused", ir_block_add(f, "entry") == nullptr);
    check("blocks are found by name", ir_block_find(f, "body") == body);
    check("blocks know their index", entry->index == 0 && body->index == 1);

    ir_instr_t *add = ir_instr_add(entry, IR_OP_ADD);

    check("an instruction is added", add && entry->n_instrs == 1);
    check("a fresh instruction has no terminator",
          ir_block_terminator(entry) == nullptr);

    check("a value destination is set",
          ir_instr_set_dest(add, "t0", gt) && add->dest_kind == IR_DEST_VALUE);
    check("the destination is a value with the given type",
          add->dest->kind == IR_VAL_INSTR && add->dest->type == gt);

    check("value operands push", ir_instr_operand_value(add, p));
    check("a second value operand pushes", ir_instr_operand_value(add, p));
    check("a third operand is refused by arity",
          !ir_instr_operand_value(add, p));
    check("a block operand is refused where a value is expected",
          !ir_instr_operand_block(add, body));

    check("modifiers push", ir_instr_add_mod(add, "wrap"));
    check("modifiers read back", ir_instr_has_mod(add, "wrap"));
    check("an absent modifier is absent", !ir_instr_has_mod(add, "trap"));

    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);

    check("a value is refused where a block is expected",
          !ir_instr_operand_value(jump, p));
    check("a block operand pushes", ir_instr_operand_block(jump, body));
    check("the terminator is found", ir_block_terminator(entry) == jump);

    check("a destination is refused on a resultless op",
          !ir_instr_set_dest(jump, "x", gt));

    ir_instr_t *store = ir_instr_add(body, IR_OP_STORE);

    check("a local destination is refused on a resultless op",
          !ir_instr_set_dest_local(store, l));

    ir_instr_t *bump = ir_instr_add(body, IR_OP_ADD);

    check("a local destination is set",
          ir_instr_set_dest_local(bump, l)
              && bump->dest_kind == IR_DEST_LOCAL && bump->dest_local == l);

    ir_instr_t *fld = ir_instr_add(body, IR_OP_FIELD);

    check("field takes a value then a name",
          ir_instr_operand_value(fld, p) && ir_instr_operand_name(fld, "x"));
    check("field refuses a name in the value slot",
          !ir_instr_operand_name(ir_instr_add(body, IR_OP_FIELD), "x"));

    ir_ctx_free(ctx);
}

static void test_cross_module_global_address(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *owner = ir_module_new(ctx, "owner", &ir_profile_bce,
                                       "test");
    ir_module_t *consumer = ir_module_new(ctx, "consumer", &ir_profile_bce,
                                          "test");
    th_type *i32 = ir_type_parse(owner, "i32");
    th_type *u32 = ir_type_parse(consumer, "u32");
    ir_global_t *canonical = ir_global_add(owner, "shared", i32);
    ir_global_t *wrong_type_canonical = ir_global_add(owner, "wrong_type",
                                                       i32);
    ir_global_t *not_external_canonical = ir_global_add(owner,
                                                         "not_external", i32);
    ir_global_t *external = ir_global_add(consumer, "shared", i32);
    ir_global_t *wrong_type = ir_global_add(consumer, "wrong_type", u32);
    ir_global_t *not_external = ir_global_add(consumer, "not_external", i32);
    if (external) external->is_extern = true;
    if (wrong_type) wrong_type->is_extern = true;

    check("globals retain their declaring module and begin with self identity",
          canonical && external && ir_global_owner(canonical) == owner
              && ir_global_identity(canonical) == canonical
              && ir_global_identity(external) == external);
    check("matching globals have equal names and types",
          external && canonical && !strcmp(external->name, canonical->name)
              && ir_type_eq(consumer, external->type, canonical->type));
    check("a module global can bind to a canonical cross-module identity",
          external && ir_global_set_identity(ctx, external, canonical)
              && ir_global_identity(external) == canonical);
    check("global identity binding rejects a name or type mismatch",
          !ir_global_set_identity(ctx, not_external, canonical)
              && !ir_global_set_identity(ctx, wrong_type, canonical));

    ir_value_t *resolved = ir_global_address(consumer, canonical);
    check("a canonical foreign global resolves to the consumer extern",
          resolved && resolved->kind == IR_VAL_GLOBAL
              && resolved->global == external);
    check("a foreign global without a local declaration is rejected",
          ir_global_address(consumer, ir_global_add(owner, "missing", i32))
              == nullptr);
    check("a mismatched extern type is rejected",
          ir_global_address(consumer, wrong_type_canonical) == nullptr);
    check("a same-name non-extern is not accepted as a foreign binding",
          not_external && !not_external->is_extern
              && ir_global_address(consumer, not_external_canonical)
                     == nullptr);
    check("a module-owned global address still resolves directly",
          ir_global_address(owner, canonical)->global == canonical);

    ir_ctx_free(ctx);
}

static void test_node_annots(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");
    th_type     *t   = ir_type_parse(m, "i32");
    th_type     *ft  = ir_type_parse(m, "(i32) -> i32");
    ir_func_t   *f   = ir_func_add(m, "f", ft, IR_LINK_EXTERNAL);
    ir_local_t  *l   = ir_local_add(f, "x", t);
    ir_block_t  *b   = ir_block_add(f, "entry");
    ir_instr_t  *in  = ir_instr_add(b, IR_OP_ADD);
    ir_global_t *g   = ir_global_add(m, "g", t);

    ir_annots_t *maps[] = {
        ir_module_annots(m), &f->annots, &b->annots,
        &in->annots,         &l->annots, &g->annots,
    };
    const char *names[] = {"module", "function", "block",
                           "instruction", "local", "global"};

    for (size_t i = 0; i < 6; i++) {
        ir_annot_set(ctx, maps[i],
                     (ir_annot_t){.key = "ir.mark", .kind = IR_ANNOT_INT,
                                  .i = ir_lit_of((ir_int_t)i)});

        ir_annot_t *got = ir_annot_get(maps[i], "ir.mark");

        if (!got || ir_lit_signed(got->i) != (ir_int_t)i) {
            printf("FAIL  %s carries an annotation map\n", names[i]);
            fails++;
        }
    }

    ir_ctx_free(ctx);
}

int main(void)
{
    test_ops();
    test_props();
    test_types();
    test_a_type_has_no_size_limit();
    test_identity_is_the_whole_type();
    test_machine_classes();
    test_second_profile();
    test_files_and_spans();
    test_origins();
    test_annots();
    test_structure();
    test_cross_module_global_address();
    test_node_annots();

    return report("ir core");
}
