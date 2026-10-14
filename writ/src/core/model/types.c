#include "model.h"

#include "lib/hash.h"

typedef struct {
    const char      *name;
    const char      *c_spelling;
    writ_type_kind_t kind;
    uint32_t         width;
    bool             is_signed;
} primitive_t;

enum {
    WIDTH_LONG    = 100,
    WIDTH_POINTER = 101,
    WIDTH_VA_LIST = 102,
};

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
    {"usize", "size_t", WRIT_TYPE_INT, WIDTH_POINTER, false},
    {"isize", "ptrdiff_t", WRIT_TYPE_INT, WIDTH_POINTER, true},
    {"uptr", "uintptr_t", WRIT_TYPE_INT, WIDTH_POINTER, false},
    {"f32", "float", WRIT_TYPE_FLOAT, 4, true},
    {"f64", "double", WRIT_TYPE_FLOAT, 8, true},
    {"c_char", "char", WRIT_TYPE_INT, 1, true},
    {"c_schar", "signed char", WRIT_TYPE_INT, 1, true},
    {"c_uchar", "unsigned char", WRIT_TYPE_INT, 1, false},
    {"c_short", "short", WRIT_TYPE_INT, 2, true},
    {"c_ushort", "unsigned short", WRIT_TYPE_INT, 2, false},
    {"c_int", "int", WRIT_TYPE_INT, 4, true},
    {"c_uint", "unsigned int", WRIT_TYPE_INT, 4, false},
    {"c_long", "long", WRIT_TYPE_INT, WIDTH_LONG, true},
    {"c_ulong", "unsigned long", WRIT_TYPE_INT, WIDTH_LONG, false},
    {"c_longlong", "long long", WRIT_TYPE_INT, 8, true},
    {"c_ulonglong", "unsigned long long", WRIT_TYPE_INT, 8, false},
    {"cstr", "char", WRIT_TYPE_CSTR, 1, true},
    {"va_list", "va_list", WRIT_TYPE_VA_LIST, WIDTH_VA_LIST, false},
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

static uint32_t
pointer_size(const writ_model_t *model)
{
    return model->platform->pointer_width / 8;
}

static uint32_t
va_list_size(const writ_model_t *model)
{
    const writ_platform_t *platform = model->platform;
    if (platform->os != writ_intern(model->session, "linux", 5)) {
        return pointer_size(model);
    }
    return platform->arch == writ_intern(model->session, "x86_64", 6) ? 24 : 32;
}

writ_type_id
writ_type_intern(writ_model_t *model, writ_type_key_t key)
{
    writ_type_key_t probe = {0};
    probe.kind = key.kind;
    probe.nullable = key.nullable;
    probe.target = key.target;
    probe.name = key.name;
    probe.count = key.count;
    const core_indexed_entry_t *found = core_indexed_table_find(model->type_index, &probe);
    if (found) {
        return found->index;
    }
    writ_type_key_t *stored = writ_alloc(model->session, sizeof(writ_type_key_t));
    *stored = probe;
    writ_type_rec_t record = {.key = probe, .decl = WRIT_NONE};
    const writ_type_rec_t *target = writ_type(model, probe.target);
    switch (probe.kind) {
    case WRIT_TYPE_POINTER:
    case WRIT_TYPE_ARRAY:
    case WRIT_TYPE_CALLBACK:
        record.size = record.align = pointer_size(model);
        break;
    case WRIT_TYPE_VIEW:
    case WRIT_TYPE_HANDLE:
    case WRIT_TYPE_ENUM:
        record.size = target->size;
        record.align = target->align;
        record.is_signed = target->is_signed;
        record.c_spelling = target->c_spelling;
        break;
    case WRIT_TYPE_INLINE:
        record.size = target->size * probe.count;
        record.align = target->align;
        break;
    default:
        break;
    }
    core_list_push(model->types, record);
    return core_indexed_table_intern(model->type_index, stored, stored, nullptr)->index;
}

void
writ_types_init(writ_model_t *model)
{
    model->types = core_list_new(writ_type_rec_t);
    core_list_push(model->types, ((writ_type_rec_t){.decl = WRIT_NONE}));
    model->type_index = core_indexed_table_new(1, UINT32_MAX - 1, type_key_hash, type_key_eq);
    core_dict_init(&model->type_names, nullptr, nullptr);
    for (size_t i = 0; i < sizeof(primitives) / sizeof(primitives[0]); i++) {
        const primitive_t *p = &primitives[i];
        writ_sym_t name = writ_intern(model->session, p->name, strlen(p->name));
        writ_type_id id = writ_type_intern(model, (writ_type_key_t){.kind = p->kind, .name = name});
        writ_type_rec_t *record = &model->types.data[id];
        record->size = p->width == WIDTH_LONG    ? model->platform->long_width
                     : p->width == WIDTH_POINTER ? pointer_size(model)
                     : p->width == WIDTH_VA_LIST ? va_list_size(model)
                                                 : p->width;
        record->align = record->size ? (record->size > 8 ? 8 : record->size) : 1;
        record->is_signed = p->is_signed;
        record->c_spelling = writ_intern(model->session, p->c_spelling, strlen(p->c_spelling));
        core_dict_put(&model->type_names, name, id);
    }
}

bool
writ_type_declare(writ_model_t *model, writ_sym_t name, writ_type_id type, writ_loc_t loc)
{
    if (!core_dict_add(&model->type_names, name, type)) {
        writ_str_t text = writ_sym_str(model->session, name);
        writ_report(model->session, WRIT_D_DUPLICATE, loc, "type `%.*s`", (int)text.length, text.data);
        return false;
    }
    return true;
}

writ_type_id
writ_type_named(const writ_model_t *model, writ_sym_t name)
{
    bool found = false;
    uintptr_t id = (uintptr_t)core_dict_get((core_dict_t *)&model->type_names, name, &found);
    return found ? (writ_type_id)id : 0;
}

writ_type_id
writ_type_builtin(writ_model_t *model, const char *name)
{
    return writ_type_named(model, writ_intern(model->session, name, strlen(name)));
}

const writ_type_rec_t *
writ_type(const writ_model_t *model, writ_type_id id)
{
    return &model->types.data[id < model->types.len ? id : 0];
}

writ_type_id
writ_type_physical(const writ_model_t *model, writ_type_id id)
{
    const writ_type_rec_t *type = writ_type(model, id);
    while (type->key.kind == WRIT_TYPE_VIEW || type->key.kind == WRIT_TYPE_ENUM || type->key.kind == WRIT_TYPE_HANDLE) {
        id = type->key.target;
        type = writ_type(model, id);
    }
    return id;
}

bool
writ_type_is_integer(const writ_model_t *model, writ_type_id id)
{
    writ_type_kind_t kind = writ_type(model, writ_type_physical(model, id))->key.kind;
    return kind == WRIT_TYPE_INT || kind == WRIT_TYPE_BOOL;
}

bool
writ_type_is_pointer(const writ_model_t *model, writ_type_id id)
{
    writ_type_kind_t kind = writ_type(model, writ_type_physical(model, id))->key.kind;
    return kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_ARRAY || kind == WRIT_TYPE_CALLBACK;
}

static uint32_t
align_up(uint32_t value, uint32_t align)
{
    return align ? (value + align - 1) / align * align : value;
}

static void layout_record(writ_model_t *model, uint32_t index, uint8_t *state);

static const writ_type_rec_t *
laid_out(writ_model_t *model, writ_type_id id, uint8_t *state)
{
    const writ_type_rec_t *type = writ_type(model, id);
    if (type->key.kind == WRIT_TYPE_INLINE) {
        const writ_type_rec_t *element = laid_out(model, type->key.target, state);
        writ_type_rec_t *inline_type = &model->types.data[id];
        inline_type->size = element->size * inline_type->key.count;
        inline_type->align = element->align;
        return inline_type;
    }
    if (type->key.kind == WRIT_TYPE_RECORD && type->decl != WRIT_NONE && state[type->decl] != 2) {
        layout_record(model, type->decl, state);
    }
    return writ_type(model, id);
}

static void
layout_record(writ_model_t *model, uint32_t index, uint8_t *state)
{
    if (state[index]) {
        return;
    }
    state[index] = 1;
    writ_record_rec_t *record = &model->records.data[index];
    uint32_t offset = 0;
    uint32_t max_align = 1;
    for (uint32_t i = 0; i < record->field_count; i++) {
        writ_slot_rec_t *field = &model->slots.data[record->first_field + i];
        const writ_type_rec_t *type = laid_out(model, field->type, state);
        uint32_t align = type->align ? type->align : 1;
        if (record->pack && align > record->pack) {
            align = record->pack;
        }
        offset = align_up(offset, align);
        field->offset = offset;
        offset += type->size;
        max_align = align > max_align ? align : max_align;
    }
    if (record->align_assert > max_align) {
        max_align = record->align_assert;
    }
    writ_type_rec_t *self = &model->types.data[record->type];
    self->size = align_up(offset, max_align);
    self->align = max_align;
    state[index] = 2;
}

void
writ_type_layout_records(writ_model_t *model)
{
    uint8_t *state = writ_alloc(model->session, model->records.len + 1);
    memset(state, 0, model->records.len + 1);
    for (uint32_t i = 0; i < model->records.len; i++) {
        layout_record(model, i, state);
    }
}
