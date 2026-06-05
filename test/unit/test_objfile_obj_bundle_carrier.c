#include "n00b.h"
#include "core/runtime.h"
#include "util/assert.h"
#include "compiler/objfile/obj_bundle.h"

#define N00B_TEST_REQUIRE(expr) n00b_require((expr), #expr)

static n00b_buffer_t *
make_object_bytes(void)
{
    n00b_buffer_t *bytes = n00b_buffer_new(8);
    uint8_t       *data  = (uint8_t *)bytes->data;

    data[0] = 0x7f;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 1;
    data[5] = 2;
    data[6] = 3;
    data[7] = 4;

    return bytes;
}

static n00b_obj_bundle_t *
make_bundle(void)
{
    auto result = n00b_obj_bundle_new();

    N00B_TEST_REQUIRE(n00b_result_is_ok(result));
    return n00b_result_get(result);
}

static void
assert_object_bytes_unchanged(n00b_buffer_t *bytes)
{
    const uint8_t *data = (const uint8_t *)bytes->data;

    N00B_TEST_REQUIRE(bytes->byte_len == 8);
    N00B_TEST_REQUIRE(data[0] == 0x7f);
    N00B_TEST_REQUIRE(data[1] == 'E');
    N00B_TEST_REQUIRE(data[2] == 'L');
    N00B_TEST_REQUIRE(data[3] == 'F');
    N00B_TEST_REQUIRE(data[4] == 1);
    N00B_TEST_REQUIRE(data[5] == 2);
    N00B_TEST_REQUIRE(data[6] == 3);
    N00B_TEST_REQUIRE(data[7] == 4);
}

static void
assert_string_eq(n00b_string_t *actual, n00b_string_t *expected)
{
    N00B_TEST_REQUIRE(actual != nullptr);
    N00B_TEST_REQUIRE(expected != nullptr);
    N00B_TEST_REQUIRE(actual->u8_bytes == expected->u8_bytes);
    N00B_TEST_REQUIRE(memcmp(actual->data,
                             expected->data,
                             actual->u8_bytes) == 0);
}

static n00b_obj_bundle_error_t *
require_read_error(n00b_result_t(n00b_obj_bundle_t *) result,
                   n00b_obj_bundle_error_code_t       expected)
{
    N00B_TEST_REQUIRE(n00b_result_is_err(result));
    N00B_TEST_REQUIRE(
        n00b_result_is_err_payload(n00b_obj_bundle_error_t *, result));

    n00b_obj_bundle_error_t *error =
        n00b_result_get_err_payload(n00b_obj_bundle_error_t *, result);

    N00B_TEST_REQUIRE(n00b_obj_bundle_error_code(error) == expected);
    return error;
}

static n00b_obj_bundle_error_t *
require_write_error(n00b_result_t(n00b_buffer_t *) result,
                    n00b_obj_bundle_error_code_t   expected)
{
    N00B_TEST_REQUIRE(n00b_result_is_err(result));
    N00B_TEST_REQUIRE(
        n00b_result_is_err_payload(n00b_obj_bundle_error_t *, result));

    n00b_obj_bundle_error_t *error =
        n00b_result_get_err_payload(n00b_obj_bundle_error_t *, result);

    N00B_TEST_REQUIRE(n00b_obj_bundle_error_code(error) == expected);
    return error;
}

static void
assert_format(n00b_obj_bundle_error_t *error, n00b_format_t expected)
{
    auto format = n00b_obj_bundle_error_format(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(format));
    N00B_TEST_REQUIRE(n00b_option_get(format) == expected);
}

static void
assert_no_format(n00b_obj_bundle_error_t *error)
{
    N00B_TEST_REQUIRE(!n00b_option_is_set(
        n00b_obj_bundle_error_format(error)));
}

static void
assert_carrier(n00b_obj_bundle_error_t *error,
               n00b_obj_bundle_carrier_t expected)
{
    auto carrier = n00b_obj_bundle_error_carrier(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(carrier));
    N00B_TEST_REQUIRE(n00b_option_get(carrier) == expected);
}

static void
assert_no_carrier(n00b_obj_bundle_error_t *error)
{
    N00B_TEST_REQUIRE(!n00b_option_is_set(
        n00b_obj_bundle_error_carrier(error)));
}

static void
test_carrier_error_strings(void)
{
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_BUNDLE_NOT_FOUND),
        r"object bundle: bundle carrier not found");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER),
        r"object bundle: duplicate bundle carrier");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER),
        r"object bundle: malformed bundle carrier");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_REPLACE_REQUIRED),
        r"object bundle: replacement required");
    assert_string_eq(
        n00b_obj_bundle_err_str(
            N00B_OBJ_BUNDLE_ERR_RESERVED_NAMESPACE_OCCUPIED),
        r"object bundle: reserved namespace occupied");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_FOREIGN_LEGACY_BUNDLE),
        r"object bundle: foreign legacy bundle");
    assert_string_eq(
        n00b_obj_bundle_err_str(
            N00B_OBJ_BUNDLE_ERR_ALREADY_WRAPPED_OR_RESERVED),
        r"object bundle: already wrapped or reserved");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_GUARD_SECTION_PRESENT),
        r"object bundle: guard section present");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER),
        r"object bundle: unsupported carrier");
    assert_string_eq(
        n00b_obj_bundle_err_str(N00B_OBJ_BUNDLE_ERR_REWRITE_FAILURE),
        r"object bundle: rewrite failure");
}

static void
test_read_invalid_arguments(void)
{
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(nullptr),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);

    assert_no_format(error);
    assert_no_carrier(error);

    n00b_buffer_t null_data = {};

    error = require_read_error(n00b_obj_bundle_read(&null_data),
                               N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_no_format(error);
    assert_no_carrier(error);

    n00b_buffer_t *bytes = make_object_bytes();

    error = require_read_error(
        n00b_obj_bundle_read(bytes, .format = (n00b_format_t)99),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_format(error, (n00b_format_t)99);
    assert_no_carrier(error);
    assert_object_bytes_unchanged(bytes);
}

static void
test_read_unsupported_formats(void)
{
    n00b_buffer_t *macho_bytes = make_object_bytes();
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(macho_bytes, .format = N00B_FMT_MACHO),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);

    assert_format(error, N00B_FMT_MACHO);
    assert_no_carrier(error);
    assert_object_bytes_unchanged(macho_bytes);

    n00b_buffer_t *pe_bytes = make_object_bytes();

    error = require_read_error(
        n00b_obj_bundle_read(pe_bytes, .format = N00B_FMT_PE),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);
    assert_format(error, N00B_FMT_PE);
    assert_no_carrier(error);
    assert_object_bytes_unchanged(pe_bytes);
}

static void
test_write_invalid_arguments(void)
{
    n00b_obj_bundle_t *bundle = make_bundle();
    n00b_buffer_t     *bytes  = make_object_bytes();

    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(nullptr, bundle),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_no_format(error);
    assert_no_carrier(error);

    n00b_buffer_t null_data = {};

    error = require_write_error(n00b_obj_bundle_write(&null_data, bundle),
                                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_no_format(error);
    assert_no_carrier(error);

    error = require_write_error(n00b_obj_bundle_write(bytes, nullptr),
                                N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_no_format(error);
    assert_no_carrier(error);
    assert_object_bytes_unchanged(bytes);

    error = require_write_error(
        n00b_obj_bundle_write(bytes,
                              bundle,
                              .format = (n00b_format_t)99),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_format(error, (n00b_format_t)99);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_AUTO);
    assert_object_bytes_unchanged(bytes);

    error = require_write_error(
        n00b_obj_bundle_write(bytes,
                              bundle,
                              .format = N00B_FMT_ELF,
                              .carrier = (n00b_obj_bundle_carrier_t)99),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_format(error, N00B_FMT_ELF);
    assert_carrier(error, (n00b_obj_bundle_carrier_t)99);
    assert_object_bytes_unchanged(bytes);

    error = require_write_error(
        n00b_obj_bundle_write(
            bytes,
            bundle,
            .format = N00B_FMT_ELF,
            .carrier = N00B_OBJ_BUNDLE_CARRIER_METADATA,
            .replace = (n00b_obj_bundle_replace_policy_t)99),
        N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT);
    assert_format(error, N00B_FMT_ELF);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_METADATA);
    assert_object_bytes_unchanged(bytes);
}

static void
test_write_unsupported_carriers(void)
{
    n00b_obj_bundle_t *bundle = make_bundle();

    n00b_buffer_t *loadable_bytes = make_object_bytes();
    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(loadable_bytes,
                              bundle,
                              .format = N00B_FMT_ELF,
                              .carrier = N00B_OBJ_BUNDLE_CARRIER_LOADABLE),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);

    assert_format(error, N00B_FMT_ELF);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_LOADABLE);
    assert_object_bytes_unchanged(loadable_bytes);

    n00b_buffer_t *split_bytes = make_object_bytes();

    error = require_write_error(
        n00b_obj_bundle_write(split_bytes,
                              bundle,
                              .format = N00B_FMT_ELF,
                              .carrier = N00B_OBJ_BUNDLE_CARRIER_SPLIT),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);
    assert_format(error, N00B_FMT_ELF);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_SPLIT);
    assert_object_bytes_unchanged(split_bytes);

    n00b_buffer_t *macho_bytes = make_object_bytes();

    error = require_write_error(
        n00b_obj_bundle_write(macho_bytes,
                              bundle,
                              .format = N00B_FMT_MACHO,
                              .carrier = N00B_OBJ_BUNDLE_CARRIER_METADATA),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);
    assert_format(error, N00B_FMT_MACHO);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_METADATA);
    assert_object_bytes_unchanged(macho_bytes);

    n00b_buffer_t *pe_bytes = make_object_bytes();

    error = require_write_error(
        n00b_obj_bundle_write(pe_bytes, bundle, .format = N00B_FMT_PE),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);
    assert_format(error, N00B_FMT_PE);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_AUTO);
    assert_object_bytes_unchanged(pe_bytes);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_carrier_error_strings();
    test_read_invalid_arguments();
    test_read_unsupported_formats();
    test_write_invalid_arguments();
    test_write_unsupported_carriers();

    return 0;
}
