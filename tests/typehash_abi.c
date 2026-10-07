#include <inttypes.h>
#include <stdio.h>

#include "th/typehash.h"

static bool expect_hash(th_ctx *ctx, const char *name, th_type *type,
                        uint64_t expected)
{
    uint64_t actual = type ? th_hash(ctx, type) : 0;
    if (actual == expected) return true;
    fprintf(stderr, "%s: expected %016" PRIx64 ", got %016" PRIx64 "\n",
            name, expected, actual);
    return false;
}

int main(void)
{
    th_ctx *ctx = th_ctx_new();
    if (!ctx) return 1;
    th_install_nfc(ctx);

    th_type *nil = th_name(ctx, "nil");
    th_type *ch = th_name(ctx, "char");
    th_type *boolean = th_name(ctx, "bool");
    th_type *array = th_array(ctx, ch);
    th_param parameter = {.mode = TH_IN, .type = ch};
    th_type *function = th_func_with_error(ctx, ch, nil, &parameter, 1,
                                           nullptr, nullptr, 0);
    th_type *nominal = th_name(ctx, "example.geometry.Point");
    th_arg argument = {.kind = TH_ARG_TYPE, .type = ch};
    th_type *generic = th_apply(ctx, "example.Box", &argument, 1);
    th_type *recursive = th_record_stub(ctx);
    th_type *self_ref = th_ind(ctx, 0, recursive);
    th_member next = {.name = "next", .type = self_ref};
    th_record_fill(ctx, recursive, &next, 1);
    th_member ab[] = {{.name = "a", .type = ch},
                      {.name = "b", .type = boolean}};
    th_member ba[] = {{.name = "b", .type = boolean},
                      {.name = "a", .type = ch}};
    th_type *record_ab = th_record(ctx, ab, 2);
    th_type *record_ba = th_record(ctx, ba, 2);
    th_type *unsigned8 = th_bitint(ctx, 8, false);
    th_type *bits8 = th_bits(ctx, 8);
    bool ok = expect_hash(ctx, "nil", nil, UINT64_C(0x0ea862bddb8ce94c))
        && expect_hash(ctx, "char", ch, UINT64_C(0x1b86a1ac31af39e4))
        && expect_hash(ctx, "Array[char]", array,
                       UINT64_C(0xc615a3440ff70106))
        && expect_hash(ctx, "(char) -> char fail: nil", function,
                       UINT64_C(0x3d8888d8b8cbcc49))
        && expect_hash(ctx, "example.geometry.Point", nominal,
                       UINT64_C(0x90314240c32667b8))
        && expect_hash(ctx, "example.Box[char]", generic,
                       UINT64_C(0x36d25f644e5be9ba))
        && expect_hash(ctx, "recursive next: Ref[Self]", recursive,
                       UINT64_C(0x2e980f80af722062))
        && expect_hash(ctx, "{a: char, b: bool}", record_ab,
                       UINT64_C(0xf7b2a7ba2cb9882b))
        && expect_hash(ctx, "{b: bool, a: char}", record_ba,
                       UINT64_C(0xf7b2a7ba2cb9882b))
        && expect_hash(ctx, "u:8", unsigned8,
                       UINT64_C(0xb72908f4367c057e))
        && expect_hash(ctx, "bits:8", bits8,
                       UINT64_C(0x3d3d83140a59b89b));
    th_type *omitted = th_func(ctx, ch, &parameter, 1, nullptr, nullptr, 0);
    th_type *failable = th_func_with_error(ctx, ch, th_name(ctx, "Error"),
                                           &parameter, 1, nullptr, nullptr, 0);
    parameter.mode = TH_OUT;
    th_type *out_mode = th_func_with_error(ctx, ch, nil, &parameter, 1,
                                           nullptr, nullptr, 0);
    if (ok) {
        uint64_t canonical = th_hash(ctx, function);
        th_type *other_nominal = th_name(ctx, "other.geometry.Point");
        argument.type = boolean;
        th_type *other_generic = th_apply(ctx, "example.Box", &argument, 1);
        ok = th_hash(ctx, omitted) != canonical
            && th_hash(ctx, failable) != canonical
            && th_hash(ctx, out_mode) != canonical
            && th_hash(ctx, other_nominal) != th_hash(ctx, nominal)
            && th_hash(ctx, other_generic) != th_hash(ctx, generic)
            && th_hash(ctx, unsigned8) != th_hash(ctx, bits8);
        if (!ok) fprintf(stderr, "distinct ABI type identities collapsed\n");
    }
    th_type *left_var = th_var(ctx);
    th_type *right_var = th_var(ctx);
    th_arg left_arg = {.kind = TH_ARG_TYPE, .type = left_var};
    th_arg right_arg = {.kind = TH_ARG_TYPE, .type = right_var};
    th_type *left_array = th_apply(ctx, "array", &left_arg, 1);
    th_type *right_array = th_apply(ctx, "array", &right_arg, 1);
    th_param left_param = {.mode = TH_IN, .type = left_array};
    th_param right_param = {.mode = TH_IN, .type = right_array};
    th_type *left_function = th_func_with_error(
        ctx, left_array, nil, &left_param, 1, nullptr, nullptr, 0);
    th_type *right_function = th_func_with_error(
        ctx, right_array, nil, &right_param, 1, nullptr, nullptr, 0);
    if (ok) {
        ok = left_var != right_var
            && th_hash(ctx, left_array) == th_hash(ctx, right_array)
            && th_hash(ctx, left_function) == th_hash(ctx, right_function);
        if (!ok) fprintf(stderr,
            "independent generic declarations should share shape hashes\n");
    }
    th_ctx_free(ctx);
    return ok ? 0 : 1;
}
