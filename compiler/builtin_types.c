#include "builtin_types.h"

#include "th/typehash.h"

static const ir_scalar_t n00bb_scalars[] = {
    {"int", IR_CLASS_SINT, 64, IR_ROLE_NONE},
    {"uint", IR_CLASS_UINT, 64, IR_ROLE_NONE},
    {"bool", IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {"byte", IR_CLASS_UINT, 8, IR_ROLE_BYTE},
    {"f64", IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {"f32", IR_CLASS_FLOAT, 32, IR_ROLE_NONE},
    {"float", IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {"nil", IR_CLASS_VOID, 0, IR_ROLE_NONE},
    {"i8", IR_CLASS_SINT, 8, IR_ROLE_NONE},
    {"i16", IR_CLASS_SINT, 16, IR_ROLE_NONE},
    {"i32", IR_CLASS_SINT, 32, IR_ROLE_NONE},
    {"i64", IR_CLASS_SINT, 64, IR_ROLE_NONE},
    {"i128", IR_CLASS_SINT, 128, IR_ROLE_NONE},
    {"u8", IR_CLASS_UINT, 8, IR_ROLE_NONE},
    {"u16", IR_CLASS_UINT, 16, IR_ROLE_NONE},
    {"u32", IR_CLASS_UINT, 32, IR_ROLE_NONE},
    {"typespec", IR_CLASS_UINT, 32, IR_ROLE_TYPESPEC},
    {"char", IR_CLASS_UINT, 24, IR_ROLE_CHAR},
    {"u64", IR_CLASS_UINT, 64, IR_ROLE_NONE},
    {"u128", IR_CLASS_UINT, 128, IR_ROLE_NONE},
};

static const ir_profile_t n00bb_profile = {
    .name = "n00bb",
    .n_scalars = sizeof(n00bb_scalars) / sizeof(n00bb_scalars[0]),
    .scalars = n00bb_scalars,
};

const ir_profile_t *
n00bb_type_profile(void)
{
    return &n00bb_profile;
}

static const th_builtin_type_t n00bb_constructors[] = {
    {"string", TH_BUILTIN_STRING, 0, nullptr},
    {"Atomic", TH_BUILTIN_ATOMIC, 1, nullptr},
    {"Array", TH_BUILTIN_ARRAY, 1, nullptr},
    {"list", TH_BUILTIN_LIST, 1, nullptr},
    {"set", TH_BUILTIN_SET, 1, nullptr},
    {"dict", TH_BUILTIN_DICT, 2, nullptr},
    {"builtin.containers.list.List", TH_BUILTIN_CONTAINER_LIST, 1, nullptr},
    {"builtin.containers.set.Set", TH_BUILTIN_CONTAINER_SET, 1, nullptr},
    {"builtin.containers.dict.Dict", TH_BUILTIN_CONTAINER_DICT, 2, nullptr},
    {"builtin.containers.view.View", TH_BUILTIN_VIEW, 1, nullptr},
    {"Any", TH_BUILTIN_ANY, 0, nullptr},
    {"Allocator", TH_BUILTIN_ALLOCATOR, 0, nullptr},
};

#define N00BB_SCALAR_COUNT (sizeof(n00bb_scalars) / sizeof(n00bb_scalars[0]))
#define N00BB_CONSTRUCTOR_COUNT \
    (sizeof(n00bb_constructors) / sizeof(n00bb_constructors[0]))

static th_builtin_type_t n00bb_builtin_types[N00BB_SCALAR_COUNT
                                             + N00BB_CONSTRUCTOR_COUNT];

void
n00bb_register_builtin_types(void)
{
    for (size_t i = 0; i < N00BB_SCALAR_COUNT; i++) {
        const ir_scalar_t *scalar = &n00bb_scalars[i];
        th_builtin_kind_t kind = scalar->class == IR_CLASS_VOID
            ? TH_BUILTIN_NIL
            : scalar->role == IR_ROLE_TYPESPEC ? TH_BUILTIN_TYPESPEC
            : TH_BUILTIN_SCALAR;
        n00bb_builtin_types[i] = (th_builtin_type_t){
            .name = scalar->type_text,
            .kind = kind,
            .arity = 0,
            .scalar = scalar,
        };
    }
    for (size_t i = 0; i < N00BB_CONSTRUCTOR_COUNT; i++)
        n00bb_builtin_types[N00BB_SCALAR_COUNT + i] = n00bb_constructors[i];
    th_set_default_builtin_types(n00bb_builtin_types,
                                 N00BB_SCALAR_COUNT + N00BB_CONSTRUCTOR_COUNT);
}
