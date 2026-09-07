#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"
#include "ir_test.h"

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce,
                                               &ir_profile_synth};

typedef struct {
    const char *production;
    const char *what;
    const char *src;
    const char *survives;
} rule_t;

static bool round_trips(const char *src, char **printed, char **why)
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

    char        *once = ir_module_print(m);
    ir_diags_t  *d2   = ir_diags_new(ctx, nullptr);
    ir_module_t *m2   = ir_module_parse(ctx, once, 0, PROFILES, 2, d2);

    if (!m2) {
        ir_render_t r = {0};
        char       *e = ir_diags_render_text(d2, &r);
        size_t      n = strlen(e) + strlen(once) + 64;

        *why = malloc(n);
        snprintf(*why, n, "reparse failed: %s---- printed ----\n%s", e, once);
        free(e);
        free(once);
        ir_ctx_free(ctx);

        return false;
    }

    char *twice = ir_module_print(m2);
    bool  same  = !strcmp(once, twice);

    if (!same) {
        size_t n = strlen(once) + strlen(twice) + 64;

        *why = malloc(n);
        snprintf(*why, n, "unstable:\n---- once ----\n%s---- twice ----\n%s",
                 once, twice);
    }

    if (printed) {
        *printed = strdup(once);
    }
    free(once);
    free(twice);
    ir_ctx_free(ctx);

    return same;
}

static void covers(const rule_t *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char *why  = nullptr;
        char *text = nullptr;

        if (!round_trips(r[i].src, &text, &why)) {
            printf("FAIL  %-16s %s\n%s\n", r[i].production, r[i].what,
                   why ? why : "(no reason)");
            fails++;
        }
        else if (r[i].survives && (!text || !strstr(text, r[i].survives))) {
            printf("FAIL  %-16s %s: [%s] did not survive printing\n"
                   "---- printed ----\n%s",
                   r[i].production, r[i].what, r[i].survives,
                   text ? text : "(nothing)");
            fails++;
        }
        free(why);
        free(text);
    }
}

#define HEAD "module m {\n  profile bce\n  target t\n"

static const rule_t MODULE_ITEMS[] = {
    {"<module>", "a module with no items at all",
     "module m {\n  profile bce\n}\n", nullptr},
    {"<target-decl>", "target is optional", "module m {\n  profile bce\n}\n", nullptr},
    {"<target-decl>", "target is accepted", HEAD "}\n", nullptr},
    {"<type-decl>", "a record definition",
     HEAD "\n  type Point = (x: i32, y: i32)\n}\n",
     "type Point = (x: i32, y: i32)"},
    {"<type-decl>", "a variant definition",
     HEAD "\n  type R = (ok: i32, err: u64)\n}\n",
     "type R = (ok: i32, err: u64)"},
    {"<global-decl>", "a bare global", HEAD "\n  global @g: i32\n}\n",
     "global @g: i32"},
    {"<global-decl>", "a static global",
     HEAD "\n  static global @g: i32\n}\n", "static global @g: i32"},
    {"<global-decl>", "an extern global",
     HEAD "\n  extern global @g: i32\n}\n", "extern global @g: i32"},
    {"<global-decl>", "a global with an initializer",
     HEAD "\n  global @g: i32 = 7\n}\n", "global @g: i32 = 7"},
    {"<global-decl>", "a global with a float initializer",
     HEAD "\n  global @g: f64 = 1.5\n}\n", "global @g: f64 = 1.5"},
};

static const rule_t LAYOUT_ITEMS[] = {
    {"<layout-item>", "size",
     HEAD "\n  layout i32 {\n    size = 4\n  }\n}\n", "size = 4"},
    {"<layout-item>", "align",
     HEAD "\n  layout i32 {\n    align = 4\n  }\n}\n", "align = 4"},
    {"<layout-item>", "stride",
     HEAD "\n  layout array[i32; 4] {\n    stride = 4\n  }\n}\n",
     "stride = 4"},
    {"<layout-item>", "a member bit range",
     HEAD "\n  type P = (x: i32)\n\n  layout P {\n    x @ bits 0..32\n  }\n}\n",
     "x @ bits 0..32"},
    {"<layout-item>", "every item at once",
     HEAD "\n  type P = (x: i32, y: i32)\n\n  layout P {\n    size = 8\n"
     "    align = 4\n    stride = 8\n    x @ bits 0..32\n    y @ bits 32..64\n"
     "  }\n}\n", nullptr},
    {"<layout-val>", "sizeof",
     HEAD "\n  layout ptr[i32] {\n    size = sizeof(ptr[i32])\n  }\n}\n",
     "size = sizeof(ptr[i32])"},
    {"<layout-val>", "alignof",
     HEAD "\n  layout ptr[i32] {\n    align = alignof(ptr[i32])\n  }\n}\n",
     "align = alignof(ptr[i32])"},
    {"<layout-val>", "offsetof",
     HEAD "\n  type P = (x: i32)\n\n  layout ptr[P] {\n"
     "    size = offsetof(P, \"x\")\n  }\n}\n",
     "size = offsetof(P, \"x\")"},
};

#define FUNC(body) HEAD "\n  func @f() -> void {\n    block entry:\n" body \
                        "  }\n}\n"

static const rule_t FUNC_ITEMS[] = {
    {"<func>", "no parameters", FUNC("      unreachable\n"), nullptr},
    {"<func>", "one parameter",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      unreachable\n  }\n}\n", nullptr},
    {"<func>", "several parameters",
     HEAD "\n  func @f(%a: i32, %b: u64, %c: bool) -> void {\n"
          "    block entry:\n      unreachable\n  }\n}\n", nullptr},
    {"<linkage>", "static", HEAD "\n  static func @f() -> void {\n"
                                 "    block entry:\n      unreachable\n  }\n}\n", nullptr},
    {"<linkage>", "extern", HEAD "\n  extern func @f() -> void {\n"
                                 "    block entry:\n      unreachable\n  }\n}\n", nullptr},
    {"<local>", "a local",
     HEAD "\n  func @f() -> void {\n    local $t: i32\n    block entry:\n"
          "      unreachable\n  }\n}\n", nullptr},
    {"<local>", "a local with an initializer",
     HEAD "\n  func @f() -> void {\n    local $t: i32 = 7\n"
          "    block entry:\n      unreachable\n  }\n}\n",
     "local $t: i32 = 7"},
    {"<local>", "several locals",
     HEAD "\n  func @f() -> void {\n    local $t: i32\n    local $u: u64\n"
          "    block entry:\n      unreachable\n  }\n}\n", nullptr},
    {"<block>", "several blocks",
     HEAD "\n  func @f() -> void {\n    block entry:\n      jump second\n"
          "    block second:\n      unreachable\n  }\n}\n", nullptr},
};

static const rule_t OPERANDS[] = {
    {"<dest>", "a value destination",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      %s: i32 = copy %a\n      unreachable\n  }\n}\n", nullptr},
    {"<dest>", "a local destination",
     HEAD "\n  func @f(%a: i32) -> void {\n    local $t: i32\n"
          "    block entry:\n      $t = copy %a\n      unreachable\n  }\n}\n", nullptr},
    {"<modifier>", "one modifier",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      %s: i32 = add wrap %a, %a\n      unreachable\n  }\n}\n", nullptr},
    {"<modifier>", "several modifiers",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      %s: i32 = div wrap trap %a, %a\n      unreachable\n  }\n}\n",
     "div wrap trap"},
    {"<operand>", "a value",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      %s: i32 = copy %a\n      unreachable\n  }\n}\n", nullptr},
    {"<operand>", "a literal",
     FUNC("      %k: i32 = const 7\n      unreachable\n"), nullptr},
    {"<operand>", "a float literal",
     FUNC("      %k: f64 = const 1.5\n      unreachable\n"), "const 1.5"},
    {"<operand>", "a block reference",
     HEAD "\n  func @f() -> void {\n    block entry:\n      jump out\n"
          "    block out:\n      unreachable\n  }\n}\n", nullptr},
    {"<operand>", "a quoted name",
     HEAD "\n  func @f(%p: ptr[(x: i32)]) -> void {\n    block entry:\n"
          "      %q: ptr[i32] = field %p, \"x\"\n      unreachable\n  }\n}\n", nullptr},
    {"<operand>", "a type",
     FUNC("      %z: u64 = sizeof i32\n      unreachable\n"), nullptr},
    {"<operand>", "a named argument",
     HEAD "\n  func @g(%x: i32, y: u64) -> void {\n    block entry:\n"
          "      unreachable\n  }\n"
          "\n  func @f(%a: i32, %b: u64) -> void {\n    block entry:\n"
          "      %r: void = call @g, %a, y = %b\n      unreachable\n  }\n}\n",
     "y = %b"},
    {"<operand>", "a phi arm",
     HEAD "\n  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
          "      branch %c, l, l\n    block l:\n"
          "      %p: i32 = phi [entry: %a]\n      unreachable\n  }\n}\n", nullptr},
    {"<operand>", "a switch arm and a default block",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      switch %a, default: out, [1: out], [2: out]\n"
          "    block out:\n      unreachable\n  }\n}\n",
     "switch %a, default: out, [1: out], [2: out]"},
    {"<value>", "a global as an operand",
     HEAD "\n  global @g: i32\n\n  func @f() -> void {\n    block entry:\n"
          "      %v: i32 = load @g\n      unreachable\n  }\n}\n", nullptr},
    {"<value>", "a local as an operand",
     HEAD "\n  func @f() -> void {\n    local $t: i32\n    block entry:\n"
          "      %v: i32 = copy $t\n      unreachable\n  }\n}\n", nullptr},
};

static const rule_t META[] = {
    {"<loc>", "a location",
     FUNC("      unreachable !loc(0, 1, 2, 3, 4, 5, 6)\n"),
     "!loc(0, 1, 2, 3, 4, 5, 6)"},
    {"<origin>", "source", FUNC("      unreachable "
                                "!origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)\n"), nullptr},
    {"<origin>", "inlined", FUNC("      unreachable "
                                 "!origin(inlined, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"), nullptr},
    {"<origin>", "expanded", FUNC("      unreachable "
                                  "!origin(expanded, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"), nullptr},
    {"<origin>", "lowered", FUNC("      unreachable "
                                 "!origin(lowered, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"), nullptr},
    {"<origin>", "generated", FUNC("      unreachable "
                                   "!origin(generated, \"p\", 0, 1, 2, 3, 4, 5, 6)\n"), nullptr},
    {"<annot-value>", "true", FUNC("      unreachable !bce.a(true)\n"), nullptr},
    {"<annot-value>", "false", FUNC("      unreachable !bce.a(false)\n"), nullptr},
    {"<annot-value>", "an integer", FUNC("      unreachable !bce.a(7)\n"), nullptr},
    {"<annot-value>", "a negative integer",
     FUNC("      unreachable !bce.a(-7)\n"), "!bce.a(-7)"},
    {"<annot-value>", "a string", FUNC("      unreachable !bce.a(\"s\")\n"), nullptr},
    {"<annot-value>", "a type", FUNC("      unreachable !bce.a(ptr[i32])\n"),
     "!bce.a(ptr[i32])"},
    {"<annot-value>", "a value",
     HEAD "\n  func @f(%a: i32) -> void {\n    block entry:\n"
          "      unreachable !bce.a(%a)\n  }\n}\n", "!bce.a(%a)"},
    {"<annot-value>", "with a provenance",
     FUNC("      unreachable !bce.a(true from typed)\n"),
     "!bce.a(true from typed)"},
    {"<meta>", "several at once",
     FUNC("      unreachable !loc(0, 1, 2, 3, 4, 5, 6) !bce.a(true)\n"), nullptr},
    {"<meta>", "on a function",
     HEAD "\n  func @f() -> void !bce.a(true) {\n    block entry:\n"
          "      unreachable\n  }\n}\n", nullptr},
    {"<meta>", "on a block",
     HEAD "\n  func @f() -> void {\n    block entry: !bce.a(true)\n"
          "      unreachable\n  }\n}\n", nullptr},
    {"<meta>", "on a local",
     HEAD "\n  func @f() -> void {\n    local $t: i32 !bce.a(true)\n"
          "    block entry:\n      unreachable\n  }\n}\n", nullptr},
    {"<meta>", "on a global",
     HEAD "\n  global @g: i32 !bce.a(true)\n}\n", nullptr},
    {"<meta>", "on the module",
     "module m {\n  profile bce\n  target t\n  meta !bce.a(true)\n}\n", nullptr},
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
