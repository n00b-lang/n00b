#include "core/static_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
n00b_static_image_status_name(n00b_static_image_status_t status)
{
    switch (status) {
    case N00B_STATIC_IMAGE_OK:
        return "ok";
    case N00B_STATIC_IMAGE_ERR_NULL_REQUEST:
        return "null-request";
    case N00B_STATIC_IMAGE_ERR_VERSION:
        return "version";
    case N00B_STATIC_IMAGE_ERR_ABI:
        return "abi";
    case N00B_STATIC_IMAGE_ERR_PAYLOAD:
        return "payload";
    case N00B_STATIC_IMAGE_ERR_ARGUMENT:
        return "argument";
    case N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE:
        return "unregistered-type";
    case N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY:
        return "unsupported-policy";
    case N00B_STATIC_IMAGE_ERR_SCAN_KIND:
        return "scan-kind";
    case N00B_STATIC_IMAGE_ERR_NO_INITIALIZER:
        return "no-initializer";
    case N00B_STATIC_IMAGE_ERR_INITIALIZER:
        return "initializer";
    }

    return "unknown";
}

bool
n00b_static_image_abi_matches_host(const n00b_static_image_abi_t *abi)
{
    if (!abi) {
        return false;
    }

    return abi->version == N00B_STATIC_IMAGE_CONTRACT_VERSION
        && abi->pointer_bytes == sizeof(void *)
        && abi->size_t_bytes == sizeof(size_t)
        && abi->char_bits == 8
        && abi->endian == N00B_STATIC_IMAGE_HOST_ENDIAN;
}

n00b_static_image_status_t
n00b_static_image_validate_request(const n00b_static_image_request_t *request)
{
    if (!request) {
        return N00B_STATIC_IMAGE_ERR_NULL_REQUEST;
    }

    if (request->version != N00B_STATIC_IMAGE_CONTRACT_VERSION) {
        return N00B_STATIC_IMAGE_ERR_VERSION;
    }

    if (!n00b_static_image_abi_matches_host(&request->target_abi)) {
        return N00B_STATIC_IMAGE_ERR_ABI;
    }

    if (request->payload_kind != N00B_STATIC_IMAGE_PAYLOAD_NONE
        && request->payload_kind != N00B_STATIC_IMAGE_PAYLOAD_BYTES) {
        return N00B_STATIC_IMAGE_ERR_PAYLOAD;
    }

    if (request->payload_kind == N00B_STATIC_IMAGE_PAYLOAD_BYTES
        && !request->payload && request->payload_len != 0) {
        return N00B_STATIC_IMAGE_ERR_PAYLOAD;
    }

    if (request->arg_count != 0 && !request->args) {
        return N00B_STATIC_IMAGE_ERR_ARGUMENT;
    }

    auto layout_opt = n00b_type_static_layout(request->type_hash);
    if (!n00b_option_is_set(layout_opt)) {
        return N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE;
    }

    n00b_static_layout_info_t *layout = n00b_option_get(layout_opt);
    if (layout->policy != N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE) {
        return N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY;
    }

    if (layout->scan_kind != request->required_scan_kind) {
        return N00B_STATIC_IMAGE_ERR_SCAN_KIND;
    }

    return N00B_STATIC_IMAGE_OK;
}

static char *
dup_cstr(const char *s)
{
    if (!s) {
        return nullptr;
    }
    size_t len = strlen(s);
    char  *out = malloc(len + 1);
    if (!out) {
        return nullptr;
    }
    memcpy(out, s, len + 1);
    return out;
}

static char *
vformat_alloc(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        return nullptr;
    }

    char *out = malloc((size_t)n + 1);
    if (!out) {
        return nullptr;
    }
    vsnprintf(out, (size_t)n + 1, fmt, args);
    return out;
}

static void
append_vformat(char **target, const char *fmt, va_list args)
{
    char *piece = vformat_alloc(fmt, args);
    if (!piece) {
        return;
    }

    size_t old_len = *target ? strlen(*target) : 0;
    size_t add_len = strlen(piece);
    char  *out     = realloc(*target, old_len + add_len + 1);
    if (!out) {
        free(piece);
        return;
    }

    memcpy(out + old_len, piece, add_len + 1);
    *target = out;
    free(piece);
}

void
n00b_static_image_builder_init(n00b_static_image_builder_t *builder,
                               const n00b_static_image_request_t *request)
{
    if (!builder) {
        return;
    }
    *builder = (n00b_static_image_builder_t){
        .request = request,
        .status  = N00B_STATIC_IMAGE_OK,
    };
}

void
n00b_static_image_builder_destroy(n00b_static_image_builder_t *builder)
{
    if (!builder) {
        return;
    }
    free(builder->expr);
    free(builder->decls);
    free(builder->error);
    *builder = (n00b_static_image_builder_t){0};
}

void
n00b_static_image_builder_set_expr(n00b_static_image_builder_t *builder,
                                   const char *fmt, ...)
{
    if (!builder) {
        return;
    }
    free(builder->expr);
    builder->expr = nullptr;

    va_list args;
    va_start(args, fmt);
    builder->expr = vformat_alloc(fmt, args);
    va_end(args);
}

void
n00b_static_image_builder_append(n00b_static_image_builder_t *builder,
                                 const char *fmt, ...)
{
    if (!builder) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    append_vformat(&builder->decls, fmt, args);
    va_end(args);
}

n00b_static_image_status_t
n00b_static_image_builder_fail(n00b_static_image_builder_t *builder,
                               n00b_static_image_status_t status,
                               const char *fmt, ...)
{
    if (!builder) {
        return status;
    }

    free(builder->error);
    builder->error  = nullptr;
    builder->status = status;

    va_list args;
    va_start(args, fmt);
    builder->error = vformat_alloc(fmt, args);
    va_end(args);

    return status;
}

n00b_static_image_status_t
n00b_static_image_build(const n00b_static_image_request_t *request,
                        n00b_static_image_builder_t *builder)
{
    if (!builder) {
        return N00B_STATIC_IMAGE_ERR_NULL_REQUEST;
    }

    n00b_static_image_builder_init(builder, request);

    n00b_static_image_status_t status = n00b_static_image_validate_request(request);
    if (status != N00B_STATIC_IMAGE_OK) {
        builder->status = status;
        builder->error  = dup_cstr(n00b_static_image_status_name(status));
        return status;
    }

    auto info_opt = n00b_type_lookup(request->type_hash);
    if (!n00b_option_is_set(info_opt)) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE,
            "type is not registered");
    }

    n00b_type_info_t *info = n00b_option_get(info_opt);
    n00b_vtable_entry fn = info->core_vtable[N00B_BI_STATIC_INITIALIZER];
    if (!fn) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_NO_INITIALIZER,
            "type has no static initializer");
    }

    bool has_positional = false;
    bool has_named      = false;
    for (uint64_t i = 0; i < request->arg_count; i++) {
        if (request->args[i].name) {
            has_named = true;
        }
        else {
            has_positional = true;
        }
    }

    if (has_positional && !info->static_init_takes_vargs) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "type static initializer does not accept positional arguments");
    }
    if (has_named && !info->static_init_takes_kargs) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            "type static initializer does not accept keyword arguments");
    }

    status = ((n00b_static_initializer_fn)fn)(builder);
    if (status == N00B_STATIC_IMAGE_OK
        && (!builder->expr || !builder->decls)) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_INITIALIZER,
            "static initializer did not produce an expression and declarations");
    }

    builder->status = status;
    return status;
}
