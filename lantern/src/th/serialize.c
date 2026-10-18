#include <string.h>

#include "private/th/internal.h"
#include "lib/alloc.h"
#include "lib/dict.h"

struct th_type_writer {
    core_list_t(th_type_ptr_t) nodes;
    core_dict_t                index;
};

struct th_type_reader {
    th_ctx   *ctx;
    th_type **nodes;
    size_t    count;
};

th_type_writer_t *
th_type_writer_new(void)
{
    th_type_writer_t *writer = core_alloc(th_type_writer_t);
    if (!writer) return nullptr;
    writer->nodes = core_list_new(th_type_ptr_t);
    core_dict_init(&writer->index, nullptr, nullptr);
    return writer;
}

void
th_type_writer_free(th_type_writer_t *writer)
{
    if (!writer) return;
    core_list_free(writer->nodes);
    core_dict_free(&writer->index);
    core_free(writer);
}

static uint32_t
writer_node(th_type_writer_t *writer, const th_type *type)
{
    if (!type) return 0;
    bool found = false;
    void *slot = core_dict_get(&writer->index, (void *)type, &found);
    if (found) return (uint32_t)(uintptr_t)slot;
    core_list_push(writer->nodes, (th_type *)type);
    uint32_t index = (uint32_t)writer->nodes.len;
    core_dict_put(&writer->index, (void *)type, (void *)(uintptr_t)index);
    return index;
}

static void
visit_constraint(th_type_writer_t *writer, const th_constraint *constraint)
{
    switch (constraint->kind) {
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        writer_node(writer, constraint->target);
        break;
    case TH_CON_ONE_OF:
        for (size_t i = 0; i < constraint->one_of.n; i++)
            writer_node(writer, constraint->one_of.types[i]);
        break;
    case TH_CON_HAS_FIELD:
        writer_node(writer, constraint->field.type);
        break;
    case TH_CON_HAS_PARAM:
        writer_node(writer, constraint->param.type);
        break;
    case TH_CON_METHOD:
        writer_node(writer, constraint->method.result);
        writer_node(writer, constraint->method.call);
        break;
    case TH_CON_IMPLEMENTS:
    case TH_CON_KIND:
        break;
    }
}

static void
visit_children(th_type_writer_t *writer, const th_type *type)
{
    switch (type->kind) {
    case TH_NAMED:
        for (size_t i = 0; i < type->named.nargs; i++)
            if (type->named.args[i].kind == TH_ARG_TYPE)
                writer_node(writer, type->named.args[i].type);
        break;
    case TH_INDIRECTION:
        writer_node(writer, type->ind.referent);
        break;
    case TH_SEQUENCE:
        writer_node(writer, type->seq.elem);
        break;
    case TH_RECORD:
        for (size_t i = 0; i < type->record.n; i++)
            writer_node(writer, type->record.members[i].type);
        break;
    case TH_TRAIT:
        for (size_t i = 0; i < type->trait.n; i++)
            writer_node(writer, type->trait.methods[i].type);
        for (size_t i = 0; i < type->trait.nr; i++) {
            writer_node(writer, type->trait.requirements[i].subject);
            writer_node(writer, type->trait.requirements[i].target);
        }
        break;
    case TH_FUNCTION:
        writer_node(writer, type->fn.ret);
        writer_node(writer, type->fn.error);
        for (size_t i = 0; i < type->fn.np; i++)
            writer_node(writer, type->fn.params[i].type);
        if (type->fn.rest) writer_node(writer, type->fn.rest->type);
        for (size_t i = 0; i < type->fn.nkw; i++)
            writer_node(writer, type->fn.kw[i].type);
        break;
    case TH_BINDER:
        writer_node(writer, type->binder.body);
        for (uint64_t i = 0; type->binder.qvars && i < type->binder.arity; i++)
            writer_node(writer, type->binder.qvars[i]);
        break;
    case TH_SUM:
        for (size_t i = 0; i < type->sum.n; i++)
            writer_node(writer, type->sum.arms[i]);
        break;
    case TH_VAR:
        writer_node(writer, type->var.ref);
        for (size_t i = 0; i < type->var.cons.len; i++)
            visit_constraint(writer, &type->var.cons.data[i]);
        break;
    default:
        break;
    }
}

uint32_t
th_type_writer_add(th_type_writer_t *writer, const th_type *type)
{
    return writer && type ? writer_node(writer, type) : 0;
}

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static void
put_u64(core_buffer_t *out, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 8);
}

static void
put_bytes(core_buffer_t *out, const char *data, size_t length)
{
    put_u64(out, data ? length : UINT64_MAX);
    if (data && length) core_buffer_append(out, data, length);
}

static void
put_text(core_buffer_t *out, const char *text)
{
    put_bytes(out, text, text ? strlen(text) : 0);
}

static void
put_ref(th_type_writer_t *writer, core_buffer_t *out, const th_type *type)
{
    put_u32(out, writer_node(writer, type));
}

static void
put_constraint(th_type_writer_t *writer, core_buffer_t *out,
               const th_constraint *constraint)
{
    put_u32(out, (uint32_t)constraint->kind);
    put_u32(out, constraint->has_scope);
    put_text(out, constraint->scope_module);
    put_text(out, constraint->scope_package);
    put_text(out, constraint->span.file);
    put_u32(out, constraint->span.start_line);
    put_u32(out, constraint->span.start_col);
    put_u32(out, constraint->span.end_line);
    put_u32(out, constraint->span.end_col);
    switch (constraint->kind) {
    case TH_CON_KIND:
        put_u32(out, (uint32_t)constraint->type_kind);
        break;
    case TH_CON_IMPLEMENTS:
        put_text(out, constraint->iface);
        break;
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        put_ref(writer, out, constraint->target);
        break;
    case TH_CON_ONE_OF:
        put_u64(out, constraint->one_of.n);
        for (size_t i = 0; i < constraint->one_of.n; i++)
            put_ref(writer, out, constraint->one_of.types[i]);
        break;
    case TH_CON_HAS_FIELD:
        put_text(out, constraint->field.name);
        put_ref(writer, out, constraint->field.type);
        put_u32(out, constraint->field.auto_deref);
        break;
    case TH_CON_HAS_PARAM:
        put_u64(out, constraint->param.index);
        put_ref(writer, out, constraint->param.type);
        break;
    case TH_CON_METHOD:
        put_text(out, constraint->method.name);
        put_ref(writer, out, constraint->method.result);
        put_ref(writer, out, constraint->method.call);
        put_text(out, constraint->method.in_place);
        break;
    }
}

static void
put_node(th_type_writer_t *writer, core_buffer_t *out, const th_type *type)
{
    put_u32(out, (uint32_t)type->kind);
    switch (type->kind) {
    case TH_NAMED:
        put_bytes(out, type->named.name, type->named.nlen);
        put_u32(out, type->named.is_variant);
        put_u64(out, type->named.nargs);
        for (size_t i = 0; i < type->named.nargs; i++) {
            const th_arg *arg = &type->named.args[i];
            put_u32(out, (uint32_t)arg->kind);
            switch (arg->kind) {
            case TH_ARG_TYPE: put_ref(writer, out, arg->type); break;
            case TH_ARG_UINT: put_u64(out, arg->u); break;
            case TH_ARG_SINT: put_u64(out, (uint64_t)arg->s); break;
            case TH_ARG_STR: put_bytes(out, arg->str.p, arg->str.n); break;
            }
        }
        break;
    case TH_BITINT:
        put_u64(out, type->bitint.width);
        put_u32(out, (uint32_t)type->bitint.flavor);
        break;
    case TH_INDIRECTION:
        put_u32(out, type->ind.attrs);
        put_ref(writer, out, type->ind.referent);
        break;
    case TH_SEQUENCE:
        put_u32(out, type->seq.has_extent);
        put_u64(out, type->seq.extent);
        put_ref(writer, out, type->seq.elem);
        break;
    case TH_RECORD:
        put_u64(out, type->record.n);
        for (size_t i = 0; i < type->record.n; i++) {
            put_text(out, type->record.members[i].name);
            put_ref(writer, out, type->record.members[i].type);
        }
        break;
    case TH_TRAIT:
        put_bytes(out, type->trait.name, type->trait.nlen);
        put_u64(out, type->trait.n);
        for (size_t i = 0; i < type->trait.n; i++) {
            put_text(out, type->trait.methods[i].name);
            put_ref(writer, out, type->trait.methods[i].type);
        }
        put_u64(out, type->trait.nr);
        for (size_t i = 0; i < type->trait.nr; i++) {
            const th_trait_requirement *requirement =
                &type->trait.requirements[i];
            put_ref(writer, out, requirement->subject);
            put_u32(out, (uint32_t)requirement->kind);
            put_ref(writer, out, requirement->target);
            put_text(out, requirement->iface);
        }
        break;
    case TH_FUNCTION:
        put_ref(writer, out, type->fn.ret);
        put_ref(writer, out, type->fn.error);
        put_u64(out, type->fn.np);
        for (size_t i = 0; i < type->fn.np; i++) {
            put_u32(out, (uint32_t)type->fn.params[i].mode);
            put_ref(writer, out, type->fn.params[i].type);
        }
        put_u32(out, type->fn.rest != nullptr);
        if (type->fn.rest) {
            put_u32(out, (uint32_t)type->fn.rest->mode);
            put_ref(writer, out, type->fn.rest->type);
        }
        put_u64(out, type->fn.nkw);
        for (size_t i = 0; i < type->fn.nkw; i++) {
            put_text(out, type->fn.kw[i].name);
            put_u32(out, (uint32_t)type->fn.kw[i].mode);
            put_ref(writer, out, type->fn.kw[i].type);
        }
        break;
    case TH_BINDER:
        put_u64(out, type->binder.arity);
        put_ref(writer, out, type->binder.body);
        put_u32(out, type->binder.qvars != nullptr);
        for (uint64_t i = 0; type->binder.qvars && i < type->binder.arity; i++)
            put_ref(writer, out, type->binder.qvars[i]);
        break;
    case TH_BOUND:
        put_u64(out, type->bound.distance);
        put_u64(out, type->bound.position);
        break;
    case TH_SUM:
        put_u32(out, (uint32_t)type->sum.comb);
        put_u64(out, type->sum.n);
        for (size_t i = 0; i < type->sum.n; i++)
            put_ref(writer, out, type->sum.arms[i]);
        break;
    case TH_VAR:
        put_u64(out, type->var.index);
        put_u64(out, type->var.origin);
        put_u32(out, type->var.has_index);
        put_text(out, type->var.name);
        put_ref(writer, out, type->var.ref);
        put_u64(out, type->var.cons.len);
        for (size_t i = 0; i < type->var.cons.len; i++)
            put_constraint(writer, out, &type->var.cons.data[i]);
        break;
    default:
        break;
    }
}

bool
th_type_writer_finish(th_type_writer_t *writer, core_buffer_t *out)
{
    if (!writer || !out) return false;
    for (size_t i = 0; i < writer->nodes.len; i++)
        visit_children(writer, writer->nodes.data[i]);
    put_u64(out, writer->nodes.len);
    for (size_t i = 0; i < writer->nodes.len; i++)
        put_node(writer, out, writer->nodes.data[i]);
    return true;
}

typedef struct {
    const uint8_t *data;
    size_t         length;
    size_t         at;
    bool           failed;
} cursor_t;

static uint32_t
get_u32(cursor_t *cursor)
{
    if (cursor->failed || cursor->length - cursor->at < 4) {
        cursor->failed = true;
        return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= (uint32_t)cursor->data[cursor->at + i] << (8 * i);
    cursor->at += 4;
    return value;
}

static uint64_t
get_u64(cursor_t *cursor)
{
    if (cursor->failed || cursor->length - cursor->at < 8) {
        cursor->failed = true;
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)cursor->data[cursor->at + i] << (8 * i);
    cursor->at += 8;
    return value;
}

static const char *
get_bytes(cursor_t *cursor, th_ctx *ctx, size_t *length)
{
    uint64_t size = get_u64(cursor);
    if (length) *length = 0;
    if (cursor->failed || size == UINT64_MAX) return nullptr;
    if (size > cursor->length - cursor->at) {
        cursor->failed = true;
        return nullptr;
    }
    char *copy = th_strdup(ctx, (const char *)cursor->data + cursor->at,
                           (size_t)size);
    cursor->at += (size_t)size;
    if (!copy) cursor->failed = true;
    if (length) *length = (size_t)size;
    return copy;
}

static th_type *
get_ref(cursor_t *cursor, th_type_reader_t *reader)
{
    uint32_t index = get_u32(cursor);
    if (!index) return nullptr;
    if (index > reader->count) {
        cursor->failed = true;
        return nullptr;
    }
    return reader->nodes[index - 1];
}

static void
get_constraint(cursor_t *cursor, th_type_reader_t *reader,
               th_constraint *constraint)
{
    th_ctx *ctx = reader->ctx;
    *constraint = (th_constraint){0};
    constraint->kind = (th_con_kind)get_u32(cursor);
    constraint->has_scope = get_u32(cursor) != 0;
    constraint->scope_module = get_bytes(cursor, ctx, nullptr);
    constraint->scope_package = get_bytes(cursor, ctx, nullptr);
    constraint->span.file = get_bytes(cursor, ctx, nullptr);
    constraint->span.start_line = get_u32(cursor);
    constraint->span.start_col = get_u32(cursor);
    constraint->span.end_line = get_u32(cursor);
    constraint->span.end_col = get_u32(cursor);
    switch (constraint->kind) {
    case TH_CON_KIND:
        constraint->type_kind = (th_kind)get_u32(cursor);
        break;
    case TH_CON_IMPLEMENTS:
        constraint->iface = get_bytes(cursor, ctx, nullptr);
        break;
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        constraint->target = get_ref(cursor, reader);
        break;
    case TH_CON_ONE_OF: {
        uint64_t count = get_u64(cursor);
        if (count > cursor->length) {
            cursor->failed = true;
            break;
        }
        constraint->one_of.n = (size_t)count;
        constraint->one_of.types = count
            ? th_alloc(ctx, (size_t)count * sizeof(th_type *)) : nullptr;
        for (size_t i = 0; i < count && constraint->one_of.types; i++)
            constraint->one_of.types[i] = get_ref(cursor, reader);
        break;
    }
    case TH_CON_HAS_FIELD:
        constraint->field.name = get_bytes(cursor, ctx, nullptr);
        constraint->field.type = get_ref(cursor, reader);
        constraint->field.auto_deref = get_u32(cursor) != 0;
        break;
    case TH_CON_HAS_PARAM:
        constraint->param.index = get_u64(cursor);
        constraint->param.type = get_ref(cursor, reader);
        break;
    case TH_CON_METHOD:
        constraint->method.name = get_bytes(cursor, ctx, nullptr);
        constraint->method.result = get_ref(cursor, reader);
        constraint->method.call = get_ref(cursor, reader);
        constraint->method.in_place = get_bytes(cursor, ctx, nullptr);
        break;
    default:
        cursor->failed = true;
        break;
    }
}

static void *
get_array(cursor_t *cursor, th_ctx *ctx, uint64_t count, size_t element)
{
    if (!count) return nullptr;
    if (count > cursor->length || count > SIZE_MAX / element) {
        cursor->failed = true;
        return nullptr;
    }
    void *array = th_alloc(ctx, (size_t)count * element);
    if (!array) cursor->failed = true;
    else memset(array, 0, (size_t)count * element);
    return array;
}

static void
get_node(cursor_t *cursor, th_type_reader_t *reader, th_type *type)
{
    th_ctx *ctx = reader->ctx;
    switch (type->kind) {
    case TH_NAMED: {
        type->named.name = get_bytes(cursor, ctx, &type->named.nlen);
        type->named.is_variant = get_u32(cursor) != 0;
        uint64_t count = get_u64(cursor);
        type->named.args = get_array(cursor, ctx, count, sizeof(th_arg));
        type->named.nargs = cursor->failed ? 0 : (size_t)count;
        for (size_t i = 0; i < type->named.nargs; i++) {
            th_arg *arg = &type->named.args[i];
            arg->kind = (th_argkind)get_u32(cursor);
            switch (arg->kind) {
            case TH_ARG_TYPE: arg->type = get_ref(cursor, reader); break;
            case TH_ARG_UINT: arg->u = get_u64(cursor); break;
            case TH_ARG_SINT: arg->s = (int64_t)get_u64(cursor); break;
            case TH_ARG_STR:
                arg->str.p = get_bytes(cursor, ctx, &arg->str.n);
                break;
            default: cursor->failed = true; break;
            }
        }
        if (!type->named.name) cursor->failed = true;
        else (void)th_builtin_type(ctx, type);
        break;
    }
    case TH_BITINT:
        type->bitint.width = get_u64(cursor);
        type->bitint.flavor = (th_bitint_flavor)get_u32(cursor);
        break;
    case TH_INDIRECTION:
        type->ind.attrs = (uint8_t)get_u32(cursor);
        type->ind.referent = get_ref(cursor, reader);
        break;
    case TH_SEQUENCE:
        type->seq.has_extent = get_u32(cursor) != 0;
        type->seq.extent = get_u64(cursor);
        type->seq.elem = get_ref(cursor, reader);
        break;
    case TH_RECORD: {
        uint64_t count = get_u64(cursor);
        type->record.members = get_array(cursor, ctx, count,
                                         sizeof(th_member));
        type->record.n = cursor->failed ? 0 : (size_t)count;
        for (size_t i = 0; i < type->record.n; i++) {
            type->record.members[i].name = get_bytes(cursor, ctx, nullptr);
            type->record.members[i].type = get_ref(cursor, reader);
        }
        break;
    }
    case TH_TRAIT: {
        type->trait.name = get_bytes(cursor, ctx, &type->trait.nlen);
        uint64_t count = get_u64(cursor);
        type->trait.methods = get_array(cursor, ctx, count,
                                        sizeof(th_member));
        type->trait.n = cursor->failed ? 0 : (size_t)count;
        for (size_t i = 0; i < type->trait.n; i++) {
            type->trait.methods[i].name = get_bytes(cursor, ctx, nullptr);
            type->trait.methods[i].type = get_ref(cursor, reader);
        }
        uint64_t requirements = get_u64(cursor);
        type->trait.requirements = get_array(cursor, ctx, requirements,
                                             sizeof(th_trait_requirement));
        type->trait.nr = cursor->failed ? 0 : (size_t)requirements;
        for (size_t i = 0; i < type->trait.nr; i++) {
            th_trait_requirement *requirement = &type->trait.requirements[i];
            requirement->subject = get_ref(cursor, reader);
            requirement->kind = (th_con_kind)get_u32(cursor);
            requirement->target = get_ref(cursor, reader);
            requirement->iface = get_bytes(cursor, ctx, nullptr);
        }
        break;
    }
    case TH_FUNCTION: {
        type->fn.ret = get_ref(cursor, reader);
        type->fn.error = get_ref(cursor, reader);
        uint64_t count = get_u64(cursor);
        type->fn.params = get_array(cursor, ctx, count, sizeof(th_param));
        type->fn.np = cursor->failed ? 0 : (size_t)count;
        for (size_t i = 0; i < type->fn.np; i++) {
            type->fn.params[i].mode = (th_mode)get_u32(cursor);
            type->fn.params[i].type = get_ref(cursor, reader);
        }
        if (get_u32(cursor)) {
            type->fn.rest = get_array(cursor, ctx, 1, sizeof(th_param));
            if (type->fn.rest) {
                type->fn.rest->mode = (th_mode)get_u32(cursor);
                type->fn.rest->type = get_ref(cursor, reader);
            }
        }
        uint64_t keywords = get_u64(cursor);
        type->fn.kw = get_array(cursor, ctx, keywords, sizeof(th_kwparam));
        type->fn.nkw = cursor->failed ? 0 : (size_t)keywords;
        for (size_t i = 0; i < type->fn.nkw; i++) {
            type->fn.kw[i].name = get_bytes(cursor, ctx, nullptr);
            type->fn.kw[i].mode = (th_mode)get_u32(cursor);
            type->fn.kw[i].type = get_ref(cursor, reader);
        }
        break;
    }
    case TH_BINDER:
        type->binder.arity = get_u64(cursor);
        type->binder.body = get_ref(cursor, reader);
        if (get_u32(cursor)) {
            type->binder.qvars = get_array(cursor, ctx, type->binder.arity,
                                           sizeof(th_type *));
            for (uint64_t i = 0; type->binder.qvars
                 && i < type->binder.arity; i++)
                type->binder.qvars[i] = get_ref(cursor, reader);
        }
        break;
    case TH_BOUND:
        type->bound.distance = get_u64(cursor);
        type->bound.position = get_u64(cursor);
        break;
    case TH_SUM: {
        type->sum.comb = (th_comb)get_u32(cursor);
        uint64_t count = get_u64(cursor);
        type->sum.arms = get_array(cursor, ctx, count, sizeof(th_type *));
        type->sum.n = cursor->failed ? 0 : (size_t)count;
        for (size_t i = 0; i < type->sum.n; i++)
            type->sum.arms[i] = get_ref(cursor, reader);
        break;
    }
    case TH_VAR: {
        type->var.index = get_u64(cursor);
        type->var.origin = get_u64(cursor);
        type->var.has_index = get_u32(cursor) != 0;
        type->var.name = get_bytes(cursor, ctx, nullptr);
        type->var.ref = get_ref(cursor, reader);
        uint64_t count = get_u64(cursor);
        if (count > cursor->length) {
            cursor->failed = true;
            break;
        }
        type->var.cons = (core_list_t(th_constraint)){0};
        for (uint64_t i = 0; !cursor->failed && i < count; i++) {
            th_constraint constraint;
            get_constraint(cursor, reader, &constraint);
            core_list_push(type->var.cons, constraint);
        }
        break;
    }
    default:
        break;
    }
}

th_type_reader_t *
th_type_reader_open(th_ctx *ctx, const uint8_t *data, size_t length,
                    size_t *consumed)
{
    if (!ctx || (length && !data)) return nullptr;
    cursor_t cursor = {.data = data, .length = length};
    uint64_t count = get_u64(&cursor);
    if (cursor.failed || count > length) return nullptr;
    th_type_reader_t *reader = core_alloc(th_type_reader_t);
    if (!reader) return nullptr;
    reader->ctx = ctx;
    reader->count = (size_t)count;
    reader->nodes = count ? core_alloc_array(th_type *, (size_t)count)
                          : nullptr;
    bool valid = !count || reader->nodes;
    for (size_t i = 0; valid && i < reader->count; i++) {
        reader->nodes[i] = th_alloc(ctx, sizeof(th_type));
        valid = reader->nodes[i] != nullptr;
        if (valid) memset(reader->nodes[i], 0, sizeof(th_type));
    }
    for (size_t i = 0; valid && i < reader->count; i++) {
        th_kind kind = (th_kind)get_u32(&cursor);
        reader->nodes[i]->kind = kind;
        get_node(&cursor, reader, reader->nodes[i]);
        valid = !cursor.failed;
    }
    if (!valid) {
        th_type_reader_free(reader);
        return nullptr;
    }
    if (consumed) *consumed = cursor.at;
    return reader;
}

th_type *
th_type_reader_get(const th_type_reader_t *reader, uint32_t index)
{
    return reader && index && index <= reader->count
        ? reader->nodes[index - 1] : nullptr;
}

void
th_type_reader_free(th_type_reader_t *reader)
{
    if (!reader) return;
    core_free(reader->nodes);
    core_free(reader);
}
