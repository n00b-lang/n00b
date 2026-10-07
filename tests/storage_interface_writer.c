#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "th/environment.h"
#include "th/typehash.h"
#include "global_metadata.h"

static void
append_u32_le(core_buffer_t *buffer, uint32_t value)
{
    for (size_t i = 0; i < 4; i++)
        core_buffer_putc(buffer, (char)(value >> (i * 8)));
}

static void
append_export_origin(core_buffer_t *buffer, const char *module,
                     const char *declaration)
{
    size_t module_size = strlen(module);
    size_t declaration_size = strlen(declaration);
    core_buffer_append(buffer, "N0EO", 4);
    append_u32_le(buffer, (uint32_t)module_size);
    append_u32_le(buffer, (uint32_t)declaration_size);
    core_buffer_append(buffer, module, module_size);
    core_buffer_append(buffer, declaration, declaration_size);
}

static int
write_origin_conflict(const char *path, th_ctx *types, th_env *environment)
{
    th_type *integer = th_name(types, "int");
    th_type *string = th_name(types, "string");
    th_member left_member = {.name = "value", .type = integer};
    th_member right_member = {.name = "value", .type = string};
    th_type *left_shape = th_record(types, &left_member, 1);
    th_type *right_shape = th_record(types, &right_member, 1);
    uint8_t exposed = 1;
    core_buffer_t origin = {0};
    append_export_origin(&origin, "conflict.impl", "conflict.impl.Token");
    core_object_archive_error_t error = CORE_OBJECT_ARCHIVE_OK;
    bool ok = integer && string && left_shape && right_shape
        && th_env_register_type(environment, "conflict.left.Token",
            "conflict.left", "default", TH_ENV_PUBLIC)
        && th_env_register_type_shape(environment, "conflict.left.Token",
            TH_ENV_TYPE_SHAPE_RECORD, left_shape)
        && th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            "conflict.left.Token", "n00b.type-exposure", 1,
            &exposed, sizeof(exposed))
        && th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            "conflict.left.Token", "n00b.export-origin", 1,
            origin.data, origin.byte_len)
        && th_env_register_type(environment, "conflict.right.Token",
            "conflict.right", "default", TH_ENV_PUBLIC)
        && th_env_register_type_shape(environment, "conflict.right.Token",
            TH_ENV_TYPE_SHAPE_RECORD, right_shape)
        && th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            "conflict.right.Token", "n00b.type-exposure", 1,
            &exposed, sizeof(exposed))
        && th_env_register_metadata(environment, TH_ENV_METADATA_TYPE,
            "conflict.right.Token", "n00b.export-origin", 1,
            origin.data, origin.byte_len)
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_ENVIRONMENT, "conflict.left",
            "n00b.package-interface", 1, "default", sizeof("default"))
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_ENVIRONMENT, "conflict.right",
            "n00b.package-interface", 1, "default", sizeof("default"))
        && th_env_freeze(environment)
        && th_env_save(environment, path, 0, &error)
        && error == CORE_OBJECT_ARCHIVE_OK;
    core_buffer_release(&origin);
    if (!ok)
        fprintf(stderr, "could not write origin-conflict interface: %d\n",
                (int)error);
    return ok ? 0 : 1;
}

static int
write_absent_failure(const char *path, th_ctx *types, th_env *environment)
{
    static const char name[] = "generic_absent.answer";
    static const uint8_t parameters[] = {
        'N', '0', 'F', 'P', 1, 0, 0, 0, 0,
    };
    th_type *result = th_name(types, "int");
    th_type *signature = result
        ? th_func(types, result, NULL, 0, NULL, NULL, 0) : NULL;
    core_buffer_t origin = {0};
    append_export_origin(&origin, "generic_absent", name);
    core_object_archive_error_t error = CORE_OBJECT_ARCHIVE_OK;
    bool ok = signature
        && th_env_register_function_contract(environment, name, "answer",
            "generic_absent", "default", TH_ENV_PUBLIC, signature)
        && th_env_register_metadata(environment, TH_ENV_METADATA_FUNCTION,
            name, "n00b.export-origin", 1, origin.data, origin.byte_len)
        && th_env_register_metadata(environment, TH_ENV_METADATA_FUNCTION,
            name, "n00b.function-params", 1, parameters,
            sizeof(parameters))
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_ENVIRONMENT, "generic_absent",
            "n00b.package-interface", 1, "default", sizeof("default"))
        && th_env_freeze(environment)
        && th_env_save(environment, path, 0, &error)
        && error == CORE_OBJECT_ARCHIVE_OK;
    core_buffer_release(&origin);
    if (!ok)
        fprintf(stderr, "could not write absent-failure interface: %d\n",
                (int)error);
    return ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    bool missing_class = argc == 3
        && strcmp(argv[2], "--missing-class") == 0;
    bool origin_conflict = argc == 3
        && strcmp(argv[2], "--origin-conflict") == 0;
    bool absent_failure = argc == 3
        && strcmp(argv[2], "--absent-failure") == 0;
    if (argc == 3 && !missing_class && !origin_conflict
        && !absent_failure) return 2;

    th_ctx *types = th_ctx_new();
    th_env *environment = th_env_new();
    if (origin_conflict) {
        int result = types && environment
            ? write_origin_conflict(argv[1], types, environment) : 1;
        th_env_free(environment);
        th_ctx_free(types);
        return result;
    }
    if (absent_failure) {
        int result = types && environment
            ? write_absent_failure(argv[1], types, environment) : 1;
        th_env_free(environment);
        th_ctx_free(types);
        return result;
    }
    th_type *integer = types ? th_name(types, "int") : NULL;
    uint8_t global_metadata[12] = {0};
    const char *requested_class = missing_class
        ? "" : "vendor.pool/opaque";
    size_t requested_size = strlen(requested_class);
    global_metadata[0] = 0;
    global_metadata[1] = 2;
    global_metadata[2] = 2;
    global_metadata[3] = 3;
    global_metadata[4] = 3;
    core_buffer_t payload = {0};
    core_string_t requested = core_string_borrow(requested_class,
                                                  requested_size);
    bool encoded = n00bb_global_metadata_append_storage(&payload,
        global_metadata, requested, true);
    static const char origin_module[] = "required_storage_dep";
    static const char origin_declaration[] = "required_storage_dep.slot";
    core_buffer_t origin = {0};
    append_export_origin(&origin, origin_module, origin_declaration);

    core_object_archive_error_t error = CORE_OBJECT_ARCHIVE_OK;
    int ok = environment && integer && encoded
        && th_env_register_value(environment,
            "required_storage_dep.slot", "slot", "required_storage_dep",
            "default", TH_ENV_PUBLIC, integer)
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_VALUE,
            "required_storage_dep.slot", "n00b.global", 1,
            payload.data, payload.byte_len)
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_VALUE,
            "required_storage_dep.slot", "n00b.export-origin", 1,
            origin.data, origin.byte_len)
        && th_env_register_metadata(environment,
            TH_ENV_METADATA_ENVIRONMENT,
            "required_storage_dep", "n00b.package-interface", 1,
            "default", sizeof("default"))
        && th_env_freeze(environment)
        && th_env_save(environment, argv[1], 0, &error)
        && error == CORE_OBJECT_ARCHIVE_OK;

    if (!ok)
        fprintf(stderr, "could not write storage test interface: %d\n",
                (int)error);
    core_buffer_release(&payload);
    core_buffer_release(&origin);
    th_env_free(environment);
    th_ctx_free(types);
    return ok ? 0 : 1;
}
