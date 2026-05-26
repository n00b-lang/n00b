#include "core/static_image.h"
#include "core/string.h"
#include "text/strings/string_ops.h"

n00b_string_t *
n00b_static_image_status_name(n00b_static_image_status_t status)
{
    switch (status) {
    case N00B_STATIC_IMAGE_OK:
        return r"ok";
    case N00B_STATIC_IMAGE_ERR_NULL_REQUEST:
        return r"null-request";
    case N00B_STATIC_IMAGE_ERR_VERSION:
        return r"version";
    case N00B_STATIC_IMAGE_ERR_ABI:
        return r"abi";
    case N00B_STATIC_IMAGE_ERR_PAYLOAD:
        return r"payload";
    case N00B_STATIC_IMAGE_ERR_ARGUMENT:
        return r"argument";
    case N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE:
        return r"unregistered-type";
    case N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY:
        return r"unsupported-policy";
    case N00B_STATIC_IMAGE_ERR_SCAN_KIND:
        return r"scan-kind";
    case N00B_STATIC_IMAGE_ERR_NO_INITIALIZER:
        return r"no-initializer";
    case N00B_STATIC_IMAGE_ERR_INITIALIZER:
        return r"initializer";
    }

    return r"unknown";
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
    // n00b_string_t * fields are GC-tracked; no manual free needed.
    // Clear the struct so a destroyed builder can't be reused
    // accidentally.
    *builder = (n00b_static_image_builder_t){};
}

void
n00b_static_image_builder_set_expr(n00b_static_image_builder_t *builder,
                                   n00b_string_t              *expr)
{
    if (!builder) {
        return;
    }
    builder->expr = expr;
}

void
n00b_static_image_builder_append(n00b_static_image_builder_t *builder,
                                 n00b_string_t              *chunk)
{
    if (!builder || !chunk) {
        return;
    }

    if (!builder->decls) {
        builder->decls = chunk;
        return;
    }

    builder->decls = n00b_unicode_str_cat(builder->decls, chunk);
}

n00b_static_image_status_t
n00b_static_image_builder_fail(n00b_static_image_builder_t *builder,
                               n00b_static_image_status_t   status,
                               n00b_string_t               *msg)
{
    if (!builder) {
        return status;
    }

    builder->status = status;
    builder->error  = msg;
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
        builder->error  = n00b_static_image_status_name(status);
        return status;
    }

    auto info_opt = n00b_type_lookup(request->type_hash);
    if (!n00b_option_is_set(info_opt)) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE,
            r"type is not registered");
    }

    n00b_type_info_t *info = n00b_option_get(info_opt);
    n00b_vtable_entry fn = info->core_vtable[N00B_BI_STATIC_INITIALIZER];
    if (!fn) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_NO_INITIALIZER,
            r"type has no static initializer");
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
            r"type static initializer does not accept positional arguments");
    }
    if (has_named && !info->static_init_takes_kargs) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_ARGUMENT,
            r"type static initializer does not accept keyword arguments");
    }

    status = ((n00b_static_initializer_fn)fn)(builder);
    if (status == N00B_STATIC_IMAGE_OK
        && (!builder->expr || !builder->decls)) {
        return n00b_static_image_builder_fail(
            builder, N00B_STATIC_IMAGE_ERR_INITIALIZER,
            r"static initializer did not produce an expression and declarations");
    }

    builder->status = status;
    return status;
}
