/**
 * @file n00b_dwarf_codegen.c
 * @brief Generate C source text from parsed DWARF type definitions.
 *
 * Produces struct/union definitions, enum definitions, typedefs, and
 * complete C headers from `n00b_dwarf_type_def_t` data.
 *
 * Ported from slop/src/demangle/dwarf/dwarf_codegen.c.
 */

#include <string.h>
#include <stdio.h>
#include "compiler/objfile/dwarf.h"

// ============================================================================
// String buffer helper (like strbuf_t from demangle)
// ============================================================================

typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} strbuf_t;

static void
strbuf_init(strbuf_t *buf)
{
    buf->cap  = 256;
    buf->size = 0;
    buf->data = n00b_alloc_array(char, buf->cap);
    buf->data[0] = '\0';
}

static void
strbuf_append(strbuf_t *buf, const char *str)
{
    size_t len = strlen(str);
    if (buf->size + len + 1 > buf->cap) {
        while (buf->size + len + 1 > buf->cap) {
            buf->cap *= 2;
        }
        char *nd = n00b_alloc_array(char, buf->cap);
        memcpy(nd, buf->data, buf->size);
        buf->data = nd;
    }
    memcpy(buf->data + buf->size, str, len + 1);
    buf->size += len;
}

static void
strbuf_appendf(strbuf_t *buf, const char *fmt, ...)
{
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n > 0) {
        strbuf_append(buf, tmp);
    }
}

// ============================================================================
// Code generation
// ============================================================================

n00b_string_t *
n00b_dwarf_generate_struct(const n00b_dwarf_type_def_t *type)
{
    if (!type) {
        return n00b_string_empty();
    }

    strbuf_t buf;
    strbuf_init(&buf);

    const char *keyword = "struct";
    if (type->kind == N00B_DWARF_TYPE_UNION) {
        keyword = "union";
    } else if (type->kind == N00B_DWARF_TYPE_CLASS) {
        keyword = "struct";  // C doesn't have class keyword.
    }

    if (type->name) {
        strbuf_appendf(&buf, "%s %s {\n", keyword, type->name->data);
    } else {
        strbuf_appendf(&buf, "%s {\n", keyword);
    }

    for (size_t i = 0; i < type->num_members; i++) {
        const n00b_dwarf_member_t *m = &type->members[i];
        const char *tname = m->type_name ? m->type_name->data : "?";
        const char *mname = m->name ? m->name->data : "?";

        if (m->bit_size > 0) {
            strbuf_appendf(&buf, "    %s %s : %u;", tname, mname,
                           m->bit_size);
        } else {
            strbuf_appendf(&buf, "    %s %s;", tname, mname);
        }

        strbuf_appendf(&buf, " // offset: %lu", (unsigned long)m->offset);
        if (m->size > 0) {
            strbuf_appendf(&buf, ", size: %lu", (unsigned long)m->size);
        }
        strbuf_append(&buf, "\n");
    }

    strbuf_append(&buf, "}");
    if (type->byte_size > 0) {
        strbuf_appendf(&buf, "; // size: %lu", (unsigned long)type->byte_size);
        if (type->alignment > 0) {
            strbuf_appendf(&buf, ", alignment: %lu",
                           (unsigned long)type->alignment);
        }
    } else {
        strbuf_append(&buf, ";");
    }
    strbuf_append(&buf, "\n");

    return n00b_string_from_cstr(buf.data);
}

n00b_string_t *
n00b_dwarf_generate_enum(const n00b_dwarf_type_def_t *type)
{
    if (!type) {
        return n00b_string_empty();
    }

    strbuf_t buf;
    strbuf_init(&buf);

    if (type->name) {
        strbuf_appendf(&buf, "enum %s {\n", type->name->data);
    } else {
        strbuf_append(&buf, "enum {\n");
    }

    for (size_t i = 0; i < type->num_enumerators; i++) {
        const n00b_dwarf_enumerator_t *e = &type->enumerators[i];
        const char *ename = e->name ? e->name->data : "?";
        strbuf_appendf(&buf, "    %s = %lld,\n", ename,
                       (long long)e->value);
    }

    strbuf_append(&buf, "};\n");

    return n00b_string_from_cstr(buf.data);
}

n00b_string_t *
n00b_dwarf_generate_typedef(const n00b_dwarf_type_def_t *type)
{
    if (!type || !type->name) {
        return n00b_string_empty();
    }

    const char *target = type->aliased_type
                       ? type->aliased_type->data
                       : "/* unknown */";

    strbuf_t buf;
    strbuf_init(&buf);
    strbuf_appendf(&buf, "typedef %s %s;\n", target, type->name->data);

    return n00b_string_from_cstr(buf.data);
}

n00b_string_t *
n00b_dwarf_generate_header(n00b_dwarf_info_t *info)
{
    if (!info) {
        return n00b_string_empty();
    }

    if (!info->type_index_built) {
        n00b_dwarf_build_type_index(info);
    }

    strbuf_t buf;
    strbuf_init(&buf);

    strbuf_append(&buf, "#pragma once\n\n");
    strbuf_append(&buf, "#include <stdint.h>\n");
    strbuf_append(&buf, "#include <stdbool.h>\n");
    strbuf_append(&buf, "#include <stddef.h>\n\n");

    // Forward declarations for structs/unions.
    for (size_t i = 0; i < info->num_types; i++) {
        const n00b_dwarf_type_def_t *t = &info->types[i];
        if (!t->name) {
            continue;
        }
        if (t->kind == N00B_DWARF_TYPE_STRUCT
            || t->kind == N00B_DWARF_TYPE_CLASS) {
            strbuf_appendf(&buf, "struct %s;\n", t->name->data);
        } else if (t->kind == N00B_DWARF_TYPE_UNION) {
            strbuf_appendf(&buf, "union %s;\n", t->name->data);
        }
    }
    strbuf_append(&buf, "\n");

    // Enumerations.
    for (size_t i = 0; i < info->num_types; i++) {
        if (info->types[i].kind == N00B_DWARF_TYPE_ENUM) {
            n00b_string_t *s = n00b_dwarf_generate_enum(&info->types[i]);
            strbuf_append(&buf, s->data);
            strbuf_append(&buf, "\n");
        }
    }

    // Struct/union definitions.
    for (size_t i = 0; i < info->num_types; i++) {
        if (info->types[i].kind == N00B_DWARF_TYPE_STRUCT
            || info->types[i].kind == N00B_DWARF_TYPE_CLASS
            || info->types[i].kind == N00B_DWARF_TYPE_UNION) {
            n00b_string_t *s = n00b_dwarf_generate_struct(&info->types[i]);
            strbuf_append(&buf, s->data);
            strbuf_append(&buf, "\n");
        }
    }

    // Typedefs.
    for (size_t i = 0; i < info->num_types; i++) {
        if (info->types[i].kind == N00B_DWARF_TYPE_TYPEDEF) {
            n00b_string_t *s = n00b_dwarf_generate_typedef(&info->types[i]);
            strbuf_append(&buf, s->data);
        }
    }

    return n00b_string_from_cstr(buf.data);
}
