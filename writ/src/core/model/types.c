#include "model.h"

#include "lib/hash.h"

typedef struct {
    const char      *name;
    const char      *c_spelling;
    writ_type_kind_t kind;
    uint32_t         width;
    bool             is_signed;
} primitive_t;

static const primitive_t primitives[] = {
    {"void", "void", WRIT_TYPE_VOID, 0, false},
    {"bool", "bool", WRIT_TYPE_BOOL, 1, false},
    {"u8", "uint8_t", WRIT_TYPE_INT, 1, false},
    {"u16", "uint16_t", WRIT_TYPE_INT, 2, false},
    {"u32", "uint32_t", WRIT_TYPE_INT, 4, false},
    {"u64", "uint64_t", WRIT_TYPE_INT, 8, false},
    {"i8", "int8_t", WRIT_TYPE_INT, 1, true},
    {"i16", "int16_t", WRIT_TYPE_INT, 2, true},
    {"i32", "int32_t", WRIT_TYPE_INT, 4, true},
    {"i64", "int64_t", WRIT_TYPE_INT, 8, true},
    {"usize", "size_t", WRIT_TYPE_INT, 8, false},
    {"isize", "ptrdiff_t", WRIT_TYPE_INT, 8, true},
    {"uptr", "uintptr_t", WRIT_TYPE_INT, 8, false},
    {"f32", "float", WRIT_TYPE_FLOAT, 4, true},
    {"f64", "double", WRIT_TYPE_FLOAT, 8, true},
    {"c_char", "char", WRIT_TYPE_INT, 1, true},
    {"c_schar", "signed char", WRIT_TYPE_INT, 1, true},
    {"c_uchar", "unsigned char", WRIT_TYPE_INT, 1, false},
    {"c_short", "short", WRIT_TYPE_INT, 2, true},
    {"c_ushort", "unsigned short", WRIT_TYPE_INT, 2, false},
    {"c_int", "int", WRIT_TYPE_INT, 4, true},
    {"c_uint", "unsigned int", WRIT_TYPE_INT, 4, false},
    {"c_long", "long", WRIT_TYPE_INT, 8, true},
    {"c_ulong", "unsigned long", WRIT_TYPE_INT, 8, false},
    {"c_longlong", "long long", WRIT_TYPE_INT, 8, true},
    {"c_ulonglong", "unsigned long long", WRIT_TYPE_INT, 8, false},
};

static core_hash_value_t
type_key_hash(void *key)
{
    return core_hash_raw(key, sizeof(writ_type_key_t));
}

static bool
type_key_eq(const void *a, const void *b)
{
    return !memcmp(a, b, sizeof(writ_type_key_t));
}

writ_type_id
writ_type_intern(writ_model_t *model, writ_type_key_t key)
{
    writ_type_key_t probe = {0};
    probe.kind = key.kind;
    probe.nullable = key.nullable;
    probe.target = key.target;
    probe.name = key.name;
    const core_indexed_entry_t *found = core_indexed_table_find(model->type_index, &probe);
    if (found) {
        return found->index;
    }
    writ_type_key_t *stored = writ_alloc(model->session, sizeof(writ_type_key_t));
    *stored = probe;
    writ_type_rec_t record = {.key = probe};
    if (probe.kind == WRIT_TYPE_POINTER || probe.kind == WRIT_TYPE_ARRAY) {
        record.size = record.align = 8;
    }
    core_list_push(model->types, record);
    return core_indexed_table_intern(model->type_index, stored, stored, nullptr)->index;
}

void
writ_types_init(writ_model_t *model)
{
    model->types = core_list_new(writ_type_rec_t);
    core_list_push(model->types, ((writ_type_rec_t){0}));
    model->type_index = core_indexed_table_new(1, UINT32_MAX - 1, type_key_hash, type_key_eq);
    core_dict_init(&model->type_names, nullptr, nullptr);
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++) {
        const primitive_t *p = &primitives[i];
        writ_sym_t name = writ_intern(model->session, p->name, strlen(p->name));
        writ_type_id id = writ_type_intern(model, (writ_type_key_t){.kind = p->kind, .name = name});
        writ_type_rec_t *record = &model->types.data[id];
        record->size = p->width;
        record->align = p->width ? p->width : 1;
        record->is_signed = p->is_signed;
        record->c_spelling = writ_intern(model->session, p->c_spelling, strlen(p->c_spelling));
        core_dict_put(&model->type_names, name, id);
    }
}

writ_type_id
writ_type_named(writ_model_t *model, writ_sym_t name)
{
    bool found = false;
    uintptr_t id = (uintptr_t)core_dict_get(&model->type_names, name, &found);
    return found ? (writ_type_id)id : 0;
}

const writ_type_rec_t *
writ_type(const writ_model_t *model, writ_type_id id)
{
    return &model->types.data[id < model->types.len ? id : 0];
}

bool
writ_type_is_integer(const writ_model_t *model, writ_type_id id)
{
    writ_type_kind_t kind = writ_type(model, id)->key.kind;
    return kind == WRIT_TYPE_INT || kind == WRIT_TYPE_BOOL;
}
