#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "th/environment.h"
#include "th/typehash.h"
#include "global_metadata.h"

int
main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) return 2;
    bool missing_class = argc == 3
        && strcmp(argv[2], "--missing-class") == 0;
    if (argc == 3 && !missing_class) return 2;

    th_ctx *types = th_ctx_new();
    th_env *environment = th_env_new();
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
    th_env_free(environment);
    th_ctx_free(types);
    return ok ? 0 : 1;
}
