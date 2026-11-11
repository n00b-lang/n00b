#include <stdio.h>

#include "th/environment.h"
#include "th/typehash.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)


static void test_generic_record_definitions(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *key = th_var_named(ctx, S("K"));
    th_type *value = th_var_named(ctx, S("V"));
    th_arg parameters[] = {
        {.kind = TH_ARG_TYPE, .type = key},
        {.kind = TH_ARG_TYPE, .type = value},
    };
    th_type *template = th_apply(ctx, S("Pair"), parameters, 2);
    th_member members[] = {
        {.name = S("left"), .type = key},
        {.name = S("right"), .type = value},
    };
    th_type *definition = th_record(ctx, members, 2);
    th_type *integer = th_name(ctx, S("int"));
    th_type *string = th_name(ctx, S("string"));
    th_arg arguments[] = {
        {.kind = TH_ARG_TYPE, .type = integer},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_type *concrete = th_apply(ctx, S("Pair"), arguments, 2);
    th_type *bare = th_name(ctx, S("Pair"));
    check("generic records retain their ordered field template",
          th_type_define(ctx, template, definition)
          && th_type_definition(ctx, template) == definition
          && th_type_definition(ctx, bare) == nullptr);
    th_type *specialized = th_type_definition(ctx, concrete);
    check("generic record fields specialize without refining the template",
          specialized && th_kind_of(specialized) == TH_RECORD
          && th_hash(ctx, th_record_lookup_in(ctx, concrete, S("left")))
              == th_hash(ctx, integer)
          && th_hash(ctx, th_record_lookup_in(ctx, concrete, S("right")))
              == th_hash(ctx, string)
          && th_record_lookup(definition, S("left")) == key
          && th_record_lookup(definition, S("right")) == value
          && th_type_definition(ctx, concrete) == specialized);
    th_type *first = th_var_named(ctx, S("First"));
    th_type *second = th_var_named(ctx, S("Second"));
    th_arg first_args[] = {
        {.kind = TH_ARG_TYPE, .type = first},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_arg second_args[] = {
        {.kind = TH_ARG_TYPE, .type = second},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_type *first_pair = th_apply(ctx, S("Pair"), first_args, 2);
    th_type *second_pair = th_apply(ctx, S("Pair"), second_args, 2);
    check("open generic records preserve distinct variable identities",
          th_record_lookup_in(ctx, first_pair, S("left")) == first
          && th_record_lookup_in(ctx, second_pair, S("left")) == second);
    th_ctx_free(ctx);
}

static core_string_t display_type_name(core_string_t name, void *context)
{
    (void)context;
    return core_string_eq(name, S("list")) ? S("List") : (core_string_t){};
}

static void test_configurable_type_printing(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *item = th_name(ctx, S("int"));
    th_arg argument = {.kind = TH_ARG_TYPE, .type = item};
    th_type *list = th_apply(ctx, S("list"), &argument, 1);
    th_type *reference = th_ind(ctx, 0, list);
    th_type *sequence = th_array(ctx, reference);
    th_print_options_t options = {
        .named = display_type_name,
        .sequence = S("Array"),
        .reference = S("Ref"),
    };
    check("type printing accepts language-specific display spellings",
          core_string_eq(th_print_with_options(ctx, sequence, &options),
                         S("Array[Ref[List[int]]]"))
              && core_string_eq(th_print(ctx, sequence),
                                S("array[ref[list[int]]]")));
    th_ctx_free(ctx);
}

static void test_name_syntax(void)
{
    const char invalid_utf8[] = {'x', (char)0xc0, (char)0xaf};
    check("typehash names accept qualified identifiers",
          th_name_syntax_valid(S("package.module.Type")));
    check("typehash names accept well-formed Unicode spelling",
          th_name_syntax_valid(S("caf\xc3\xa9")));
    check("typehash names reject punctuation in a component",
          !th_name_syntax_valid(S("my-module.Type")));
    check("typehash names reject malformed UTF-8",
          !th_name_syntax_valid(
              core_string_borrow(invalid_utf8, sizeof(invalid_utf8))));
    check("typehash names reject empty components",
          !th_name_syntax_valid(S("package..Type")));
    check("typehash names reject missing and numeric starts",
          !th_name_syntax_valid(core_string_borrow(nullptr, 1))
              && !th_name_syntax_valid(S(""))
              && !th_name_syntax_valid(S("1Type")));
}

static void test_clone_replacing(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *original = th_var_named(ctx, S("T"));
    th_type *declared = th_var_named(ctx, S("T"));
    th_type *sequence = th_array(ctx, original);
    const th_type *from[] = {original};
    th_type *to[] = {declared};
    th_type *copy = th_type_clone_replacing(ctx, sequence, from, to, 1);
    check("clone replacement rebases a nested variable without changing source",
          copy && copy != sequence && th_seq_elem(copy) == declared
              && th_seq_elem(sequence) == original);
    check("clone replacement can replace its root",
          th_type_clone_replacing(ctx, original, from, to, 1) == declared);
    th_type *alias = th_type_clone(ctx, original);
    th_type *aliased_sequence = alias ? th_array(ctx, alias) : nullptr;
    th_type *aliased_copy = aliased_sequence
        ? th_type_clone_replacing(ctx, aliased_sequence, from, to, 1)
        : nullptr;
    check("clone replacement follows copied variable identity",
          aliased_copy && th_seq_elem(aliased_copy) == declared);
    th_type *unrelated = th_var_named(ctx, S("T"));
    th_type *unrelated_sequence = th_array(ctx, unrelated);
    th_type *unrelated_copy = th_type_clone_replacing(ctx,
        unrelated_sequence, from, to, 1);
    check("clone replacement leaves a distinct same-name variable alone",
          unrelated_copy && th_seq_elem(unrelated_copy) != declared);
    const th_type *conflicting_from[] = {original, original};
    th_type *conflicting_to[] = {declared, original};
    check("clone replacement rejects conflicting mappings",
          !th_type_clone_replacing(ctx, sequence, conflicting_from,
                                   conflicting_to, 2));
    th_ctx_free(ctx);
}

int main(void)
{
    test_generic_record_definitions();
    test_configurable_type_printing();
    test_name_syntax();
    test_clone_replacing();

    return report("th types");
}
