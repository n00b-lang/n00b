#include <stdio.h>
#include <stdlib.h>

#include "ir/layout.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)
#define NONE ((core_string_t){})

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce,
                                               &ir_profile_synth};

typedef struct {
    core_string_t production;
    core_string_t what;
    core_string_t src;
    core_string_t survives;
} rule_t;

static bool round_trips(core_string_t src, core_string_t *printed,
                        core_string_t *why)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx, src, 0, PROFILES, 2, d);

    if (!m) {
        ir_render_t r = {0};

        *why = ir_diags_render_text(d, &r);
        ir_ctx_free(ctx);

        return false;
    }

    core_string_t once = ir_module_print(m);
    ir_diags_t   *d2   = ir_diags_new(ctx, nullptr);
    ir_module_t  *m2   = ir_module_parse(ctx, once, 0, PROFILES, 2, d2);

    if (!m2) {
        ir_render_t    r = {0};
        core_string_t  e = ir_diags_render_text(d2, &r);
        core_buffer_t *b = core_buffer_empty();

        core_buffer_printf(b, "reparse failed: %.*s---- printed ----\n%.*s",
                           (int)e.u8_bytes, e.data, (int)once.u8_bytes,
                           once.data);
        *why = core_buffer_take_string(b);
        core_free(e.data);
        core_free(once.data);
        ir_ctx_free(ctx);

        return false;
    }

    core_string_t twice = ir_module_print(m2);
    bool          same  = core_string_eq(once, twice);

    if (!same) {
        core_buffer_t *b = core_buffer_empty();

        core_buffer_printf(b,
                           "unstable:\n---- once ----\n%.*s---- twice ----\n"
                           "%.*s",
                           (int)once.u8_bytes, once.data, (int)twice.u8_bytes,
                           twice.data);
        *why = core_buffer_take_string(b);
    }

    if (printed) {
        *printed = once;
    }
    else {
        core_free(once.data);
    }
    core_free(twice.data);
    ir_ctx_free(ctx);

    return same;
}

static void covers(const rule_t *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        core_string_t why  = {};
        core_string_t text = {};

        if (!round_trips(r[i].src, &text, &why)) {
            core_string_t reason = why.data ? why : S("(no reason)");

            printf("FAIL  %-16.*s %.*s\n%.*s\n",
                   (int)r[i].production.u8_bytes, r[i].production.data,
                   (int)r[i].what.u8_bytes, r[i].what.data,
                   (int)reason.u8_bytes, reason.data);
            fails++;
        }
        else if (r[i].survives.data
                 && (!text.data
                     || core_string_find(text, r[i].survives) < 0)) {
            core_string_t shown = text.data ? text : S("(nothing)");

            printf("FAIL  %-16.*s %.*s: [%.*s] did not survive printing\n"
                   "---- printed ----\n%.*s",
                   (int)r[i].production.u8_bytes, r[i].production.data,
                   (int)r[i].what.u8_bytes, r[i].what.data,
                   (int)r[i].survives.u8_bytes, r[i].survives.data,
                   (int)shown.u8_bytes, shown.data);
            fails++;
        }
        core_free(why.data);
        core_free(text.data);
    }
}

#define HEAD "module m {\n  profile bce\n  target t\n"

static const rule_t MODULE_ITEMS[] = {
    {S("<module>"), S("a module with no items at all"),
     S("module m {\n  profile bce\n}\n"), NONE},
    {S("<target-decl>"), S("target is optional"),
     S("module m {\n  profile bce\n}\n"), NONE},
    {S("<target-decl>"), S("target is accepted"), S(HEAD "}\n"), NONE},
    {S("<type-decl>"), S("a record definition"),
     S(HEAD "\n  type Point = (x: i32, y: i32)\n}\n"),
     S("type Point = (x: i32, y: i32)")},
    {S("<type-decl>"), S("a variant definition"),
     S(HEAD "\n  type R = (ok: i32, err: u64)\n}\n"),
     S("type R = (ok: i32, err: u64)")},
    {S("<global-decl>"), S("a bare global"),
     S(HEAD "\n  global @g: i32\n}\n"), S("global @g: i32")},
    {S("<global-decl>"), S("a static global"),
     S(HEAD "\n  static global @g: i32\n}\n"), S("static global @g: i32")},
    {S("<global-decl>"), S("an extern global"),
     S(HEAD "\n  extern global @g: i32\n}\n"), S("extern global @g: i32")},
    {S("<global-decl>"), S("a global with an initializer"),
     S(HEAD "\n  global @g: i32 = 7\n}\n"), S("global @g: i32 = 7")},
    {S("<global-decl>"), S("a global with a float initializer"),
     S(HEAD "\n  global @g: f64 = 1.5\n}\n"), S("global @g: f64 = 1.5")},
};

static const rule_t LAYOUT_ITEMS[] = {
    {S("<layout-item>"), S("size"),
     S(HEAD "\n  layout i32 {\n    size = 4\n  }\n}\n"), S("size = 4")},
    {S("<layout-item>"), S("align"),
     S(HEAD "\n  layout i32 {\n    align = 4\n  }\n}\n"), S("align = 4")},
    {S("<layout-item>"), S("stride"),
     S(HEAD "\n  layout array[i32; 4] {\n    stride = 4\n  }\n}\n"),
     S("stride = 4")},
    {S("<layout-item>"), S("a member bit range"),
     S(HEAD "\n  type P = (x: i32)\n\n  layout P {\n    x @ bits 0..32\n  }\n}\n"),
     S("x @ bits 0..32")},
    {S("<layout-item>"), S("every item at once"),
     S(HEAD "\n  type P = (x: i32, y: i32)\n\n  layout P {\n    size = 8\n"
            "    align = 4\n    stride = 8\n    x @ bits 0..32\n"
            "    y @ bits 32..64\n  }\n}\n"),
     NONE},
    {S("<layout-val>"), S("sizeof"),
     S(HEAD "\n  layout ptr[i32] {\n    size = sizeof(ptr[i32])\n  }\n}\n"),
     S("size = sizeof(ptr[i32])")},
    {S("<layout-val>"), S("alignof"),
     S(HEAD "\n  layout ptr[i32] {\n    align = alignof(ptr[i32])\n  }\n}\n"),
     S("align = alignof(ptr[i32])")},
    {S("<layout-val>"), S("offsetof"),
     S(HEAD "\n  type P = (x: i32)\n\n  layout ptr[P] {\n"
            "    size = offsetof(P, \"x\")\n  }\n}\n"),
     S("size = offsetof(P, \"x\")")},
};

#define FUNC(body) \
    S(HEAD "\n  func @f() -> void {\n    block entry:\n" body "  }\n}\n")

static const rule_t FUNC_ITEMS[] = {
    {S("<func>"), S("no parameters"), FUNC("      unreachable\n"), NONE},
    {S("<func>"), S("one parameter"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      unreachable\n  }\n}\n"),
     NONE},
    {S("<func>"), S("several parameters"),
     S(HEAD "\n  func @f(%a: i32, %b: u64, %c: bool) -> void {\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<linkage>"), S("static"),
     S(HEAD "\n  static func @f() -> void {\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<linkage>"), S("extern"),
     S(HEAD "\n  extern func @f() -> void {\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<local>"), S("a local"),
     S(HEAD "\n  func @f() -> void {\n    local $t: i32\n    block entry:\n"
            "      unreachable\n  }\n}\n"),
     NONE},
    {S("<local>"), S("a local with an initializer"),
     S(HEAD "\n  func @f() -> void {\n    local $t: i32 = 7\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     S("local $t: i32 = 7")},
    {S("<local>"), S("several locals"),
     S(HEAD "\n  func @f() -> void {\n    local $t: i32\n    local $u: u64\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<block>"), S("several blocks"),
     S(HEAD "\n  func @f() -> void {\n    block entry:\n      jump second\n"
            "    block second:\n      unreachable\n  }\n}\n"),
     NONE},
};

static const rule_t OPERANDS[] = {
    {S("<dest>"), S("a value destination"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      %s: i32 = copy %a\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<dest>"), S("a local destination"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    local $t: i32\n"
            "    block entry:\n      $t = copy %a\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<modifier>"), S("one modifier"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      %s: i32 = add wrap %a, %a\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<modifier>"), S("several modifiers"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      %s: i32 = div wrap trap %a, %a\n      unreachable\n  }\n}\n"),
     S("div wrap trap")},
    {S("<operand>"), S("a value"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      %s: i32 = copy %a\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<operand>"), S("a literal"),
     FUNC("      %k: i32 = const 7\n      unreachable\n"), NONE},
    {S("<operand>"), S("a float literal"),
     FUNC("      %k: f64 = const 1.5\n      unreachable\n"), S("const 1.5")},
    {S("<operand>"), S("a block reference"),
     S(HEAD "\n  func @f() -> void {\n    block entry:\n      jump out\n"
            "    block out:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<operand>"), S("a quoted name"),
     S(HEAD "\n  func @f(%p: ptr[(x: i32)]) -> void {\n    block entry:\n"
            "      %q: ptr[i32] = field %p, \"x\"\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<operand>"), S("a type"),
     FUNC("      %z: u64 = sizeof i32\n      unreachable\n"), NONE},
    {S("<operand>"), S("a named argument"),
     S(HEAD "\n  func @g(%x: i32, y: u64) -> void {\n    block entry:\n"
            "      unreachable\n  }\n"
            "\n  func @f(%a: i32, %b: u64) -> void {\n    block entry:\n"
            "      %r: void = call @g, %a, y = %b\n      unreachable\n  }\n}\n"),
     S("y = %b")},
    {S("<operand>"), S("a phi arm"),
     S(HEAD "\n  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
            "      branch %c, l, l\n    block l:\n"
            "      %p: i32 = phi [entry: %a]\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<operand>"), S("a switch arm and a default block"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      switch %a, default: out, [1: out], [2: out]\n"
            "    block out:\n      unreachable\n  }\n}\n"),
     S("switch %a, default: out, [1: out], [2: out]")},
    {S("<value>"), S("a global as an operand"),
     S(HEAD "\n  global @g: i32\n\n  func @f() -> void {\n    block entry:\n"
            "      %v: i32 = load @g\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<value>"), S("a local as an operand"),
     S(HEAD "\n  func @f() -> void {\n    local $t: i32\n    block entry:\n"
            "      %v: i32 = copy $t\n      unreachable\n  }\n}\n"),
     NONE},
};

static const rule_t META[] = {
    {S("<loc>"), S("a location"),
     FUNC("      unreachable !loc(0, 1, 2, 3, 4, 5, 6)\n"),
     S("!loc(0, 1, 2, 3, 4, 5, 6)")},
    {S("<origin>"), S("source"),
     FUNC("      unreachable "
          "!origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)\n"),
     NONE},
    {S("<origin>"), S("inlined"),
     FUNC("      unreachable "
          "!origin(inlined, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"),
     NONE},
    {S("<origin>"), S("expanded"),
     FUNC("      unreachable "
          "!origin(expanded, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"),
     NONE},
    {S("<origin>"), S("lowered"),
     FUNC("      unreachable "
          "!origin(lowered, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"),
     NONE},
    {S("<origin>"), S("generated"),
     FUNC("      unreachable "
          "!origin(generated, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"),
     NONE},
    {S("<annot-value>"), S("true"), FUNC("      unreachable !bce.a(true)\n"),
     NONE},
    {S("<annot-value>"), S("false"), FUNC("      unreachable !bce.a(false)\n"),
     NONE},
    {S("<annot-value>"), S("an integer"), FUNC("      unreachable !bce.a(7)\n"),
     NONE},
    {S("<annot-value>"), S("a negative integer"),
     FUNC("      unreachable !bce.a(-7)\n"), S("!bce.a(-7)")},
    {S("<annot-value>"), S("a string"),
     FUNC("      unreachable !bce.a(\"s\")\n"), NONE},
    {S("<annot-value>"), S("a type"),
     FUNC("      unreachable !bce.a(ptr[i32])\n"), S("!bce.a(ptr[i32])")},
    {S("<annot-value>"), S("a value"),
     S(HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
            "      unreachable !bce.a(%a)\n  }\n}\n"),
     S("!bce.a(%a)")},
    {S("<annot-value>"), S("with a provenance"),
     FUNC("      unreachable !bce.a(true from typed)\n"),
     S("!bce.a(true from typed)")},
    {S("<meta>"), S("several at once"),
     FUNC("      unreachable !loc(0, 1, 2, 3, 4, 5, 6) !bce.a(true)\n"), NONE},
    {S("<meta>"), S("on a function"),
     S(HEAD "\n  func @f() -> void !bce.a(true) {\n    block entry:\n"
            "      unreachable\n  }\n}\n"),
     NONE},
    {S("<meta>"), S("on a block"),
     S(HEAD "\n  func @f() -> void {\n    block entry: !bce.a(true)\n"
            "      unreachable\n  }\n}\n"),
     NONE},
    {S("<meta>"), S("on a local"),
     S(HEAD "\n  func @f() -> void {\n    local $t: i32 !bce.a(true)\n"
            "    block entry:\n      unreachable\n  }\n}\n"),
     NONE},
    {S("<meta>"), S("on a global"),
     S(HEAD "\n  global @g: i32 !bce.a(true)\n}\n"), NONE},
    {S("<meta>"), S("on the module"),
     S("module m {\n  profile bce\n  target t\n  meta !bce.a(true)\n}\n"),
     NONE},
};

int main(void)
{
    covers(MODULE_ITEMS, sizeof(MODULE_ITEMS) / sizeof(MODULE_ITEMS[0]));
    covers(LAYOUT_ITEMS, sizeof(LAYOUT_ITEMS) / sizeof(LAYOUT_ITEMS[0]));
    covers(FUNC_ITEMS, sizeof(FUNC_ITEMS) / sizeof(FUNC_ITEMS[0]));
    covers(OPERANDS, sizeof(OPERANDS) / sizeof(OPERANDS[0]));
    covers(META, sizeof(META) / sizeof(META[0]));

    if (!fails) {
        printf("ir grammar: every production round-trips\n");
    }

    return fails ? 1 : 0;
}
