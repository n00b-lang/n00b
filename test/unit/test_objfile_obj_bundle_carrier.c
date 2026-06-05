#include "n00b.h"
#include "adt/list.h"
#include "adt/option.h"
#include "compiler/objfile/sink.h"
#include "core/file.h"
#include "core/runtime.h"
#include "util/assert.h"
#include "util/path.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_rewrite.h"
#include "compiler/objfile/elf_types.h"
#include "compiler/objfile/obj_bundle.h"
#include "compiler/objfile/writer.h"
#include "text/strings/string_ops.h"

#include "objfile_elf_casegen.h"

#include <errno.h>

#define N00B_TEST_REQUIRE(expr) n00b_require((expr), #expr)
#define TEST_ELF64_EHDR_SIZE 64u
#define TEST_ELF64_SHDR_SIZE 64u
#define TEST_SH_NAME 0u
#define TEST_ELF_GUARD_SECTION_TYPE 0xc001u

typedef struct test_elf_section {
    n00b_string_t *name;
    uint32_t       type;
    uint64_t       flags;
    n00b_buffer_t *content;
    uint32_t       name_off;
    uint64_t       offset;
    uint64_t       size;
} test_elf_section_t;

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

static n00b_obj_bundle_t *
make_populated_bundle_a(void)
{
    n00b_obj_bundle_t *bundle  = make_bundle();
    n00b_buffer_t     *payload = n00b_buffer_from_cstr("payload-a");

    auto add = n00b_obj_bundle_add_artifact(bundle,
                                            r"bin/tool",
                                            payload,
                                            .mode = 0755);

    N00B_TEST_REQUIRE(n00b_result_is_ok(add));

    auto set_exec = n00b_obj_bundle_set_default_exec(bundle, r"bin/tool");

    N00B_TEST_REQUIRE(n00b_result_is_ok(set_exec));
    return bundle;
}

static n00b_obj_bundle_t *
make_populated_bundle_b(void)
{
    n00b_obj_bundle_t *bundle  = make_bundle();
    n00b_buffer_t     *payload = n00b_buffer_from_cstr("payload-b-longer");

    auto add = n00b_obj_bundle_add_artifact(bundle,
                                            r"bin/tool",
                                            payload,
                                            .mode = 0755);

    N00B_TEST_REQUIRE(n00b_result_is_ok(add));

    auto set_exec = n00b_obj_bundle_set_default_exec(bundle, r"bin/tool");

    N00B_TEST_REQUIRE(n00b_result_is_ok(set_exec));
    return bundle;
}

static n00b_obj_bundle_t *
make_execution_plan_bundle(void)
{
    n00b_obj_bundle_t *bundle = make_bundle();
    n00b_buffer_t     *default_payload =
        n00b_buffer_from_cstr("default-exec-payload");
    n00b_buffer_t     *tool_payload =
        n00b_buffer_from_cstr("selector-exec-payload");

    auto add_default = n00b_obj_bundle_add_artifact(
        bundle,
        r"bin/default",
        default_payload,
        .kind = N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE);

    N00B_TEST_REQUIRE(n00b_result_is_ok(add_default));

    auto add_tool = n00b_obj_bundle_add_artifact(
        bundle,
        r"bin/tool",
        tool_payload,
        .kind = N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE);

    N00B_TEST_REQUIRE(n00b_result_is_ok(add_tool));

    auto set_exec = n00b_obj_bundle_set_default_exec(bundle,
                                                     r"bin/default");

    N00B_TEST_REQUIRE(n00b_result_is_ok(set_exec));

    auto map = n00b_obj_bundle_add_exec_mapping(bundle,
                                                r"tool",
                                                r"bin/tool");

    N00B_TEST_REQUIRE(n00b_result_is_ok(map));
    return bundle;
}

static void
assert_buffer_eq(n00b_buffer_t *actual, n00b_buffer_t *expected)
{
    N00B_TEST_REQUIRE(actual != nullptr);
    N00B_TEST_REQUIRE(expected != nullptr);
    N00B_TEST_REQUIRE(actual->byte_len == expected->byte_len);

    if (actual->byte_len != 0) {
        N00B_TEST_REQUIRE(memcmp(actual->data,
                                 expected->data,
                                 actual->byte_len) == 0);
    }
}

static void
assert_buffer_unchanged(n00b_buffer_t *bytes, n00b_buffer_t *snapshot)
{
    assert_buffer_eq(bytes, snapshot);
}

static n00b_buffer_t *
encode_bundle_or_die(n00b_obj_bundle_t *bundle)
{
    auto encoded = n00b_obj_bundle_encode(bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(encoded));
    return n00b_result_get(encoded);
}

static void
assert_bundle_unchanged(n00b_obj_bundle_t *bundle,
                        n00b_buffer_t     *encoded_snapshot)
{
    assert_buffer_eq(encode_bundle_or_die(bundle), encoded_snapshot);
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

static n00b_obj_bundle_exec_plan_t *
require_exec_plan_ok(n00b_result_t(n00b_obj_bundle_exec_plan_t *) result)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));

    n00b_obj_bundle_exec_plan_t *plan = n00b_result_get(result);

    N00B_TEST_REQUIRE(plan != nullptr);
    return plan;
}

static n00b_obj_bundle_exec_argv_t *
require_exec_plan_argv(n00b_obj_bundle_exec_plan_t *plan)
{
    auto argv = n00b_obj_bundle_exec_plan_argv(plan);

    N00B_TEST_REQUIRE(n00b_option_is_set(argv));
    return n00b_option_get(argv);
}

static void
assert_exec_argv_entry(n00b_obj_bundle_exec_argv_t *argv,
                       size_t                       index,
                       n00b_string_t               *expected)
{
    N00B_TEST_REQUIRE(index < n00b_list_len(*argv));
    assert_string_eq(n00b_list_get(*argv, index), expected);
}

static void
assert_exec_plan_tool_selection(n00b_obj_bundle_exec_plan_t *plan)
{
    auto artifact_id = n00b_obj_bundle_exec_plan_selected_artifact_id(plan);
    auto logical_path =
        n00b_obj_bundle_exec_plan_selected_logical_path(plan);
    auto policy_kind = n00b_obj_bundle_exec_plan_policy_kind(plan);
    auto policy_scope = n00b_obj_bundle_exec_plan_policy_scope(plan);
    n00b_obj_bundle_exec_argv_t *argv = require_exec_plan_argv(plan);

    N00B_TEST_REQUIRE(n00b_obj_bundle_exec_plan_selection_source(plan)
                      == N00B_OBJ_BUNDLE_EXEC_SELECTION_SELECTOR_MAPPING);
    N00B_TEST_REQUIRE(n00b_option_is_set(artifact_id));
    N00B_TEST_REQUIRE(n00b_option_get(artifact_id) == 1);
    N00B_TEST_REQUIRE(n00b_option_is_set(logical_path));
    assert_string_eq(n00b_option_get(logical_path), r"bin/tool");
    N00B_TEST_REQUIRE(n00b_obj_bundle_exec_plan_requested_mode(plan)
                      == N00B_OBJ_BUNDLE_EXEC_EXTRACTED);
    N00B_TEST_REQUIRE(n00b_obj_bundle_exec_plan_resolved_mode(plan)
                      == N00B_OBJ_BUNDLE_EXEC_EXTRACTED);
    N00B_TEST_REQUIRE(n00b_obj_bundle_exec_plan_platform_support(plan)
                      == N00B_OBJ_BUNDLE_EXEC_PLATFORM_SUPPORTED);
    N00B_TEST_REQUIRE(n00b_obj_bundle_exec_plan_requires_extraction(plan));
    N00B_TEST_REQUIRE(n00b_option_is_set(policy_kind));
    N00B_TEST_REQUIRE(n00b_option_get(policy_kind)
                      == N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT);
    N00B_TEST_REQUIRE(n00b_option_is_set(policy_scope));
    N00B_TEST_REQUIRE(n00b_option_get(policy_scope)
                      == N00B_OBJ_BUNDLE_POLICY_SCOPE_EXECUTION);
    N00B_TEST_REQUIRE(!n00b_obj_bundle_exec_plan_fallback_used(plan));
    N00B_TEST_REQUIRE(n00b_list_len(*argv) == 1);
    assert_exec_argv_entry(argv, 0, r"bin/tool");
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
assert_elf_metadata_carrier_error(n00b_obj_bundle_error_t *error)
{
    assert_format(error, N00B_FMT_ELF);
    assert_carrier(error, N00B_OBJ_BUNDLE_CARRIER_METADATA);
}

static void
assert_detail(n00b_obj_bundle_error_t *error, int64_t expected)
{
    auto detail = n00b_obj_bundle_error_detail(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(detail));
    N00B_TEST_REQUIRE(n00b_option_get(detail) == expected);
}

static n00b_objfile_sink_result_t *
require_write_file_ok(n00b_result_t(n00b_objfile_sink_result_t *) result)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));
    n00b_objfile_sink_result_t *facts = n00b_result_get(result);
    N00B_TEST_REQUIRE(facts != nullptr);
    return facts;
}

static n00b_obj_bundle_extract_result_t *
require_extract_ok(n00b_result_t(n00b_obj_bundle_extract_result_t *) result)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));
    n00b_obj_bundle_extract_result_t *facts = n00b_result_get(result);
    N00B_TEST_REQUIRE(facts != nullptr);
    return facts;
}

static n00b_obj_bundle_error_t *
require_write_file_bundle_error(
    n00b_result_t(n00b_objfile_sink_result_t *) result,
    n00b_obj_bundle_error_code_t                expected)
{
    N00B_TEST_REQUIRE(n00b_result_is_err(result));
    N00B_TEST_REQUIRE(
        n00b_result_is_err_payload(n00b_obj_bundle_error_t *, result));

    n00b_obj_bundle_error_t *error =
        n00b_result_get_err_payload(n00b_obj_bundle_error_t *, result);

    N00B_TEST_REQUIRE(n00b_obj_bundle_error_code(error) == expected);
    return error;
}

static n00b_objfile_sink_error_t *
require_write_file_sink_collision(
    n00b_result_t(n00b_objfile_sink_result_t *) result)
{
    N00B_TEST_REQUIRE(n00b_result_is_err(result));
    N00B_TEST_REQUIRE(
        n00b_result_is_err_payload(n00b_objfile_sink_error_t *, result));

    n00b_objfile_sink_error_t *error =
        n00b_result_get_err_payload(n00b_objfile_sink_error_t *, result);
    n00b_objfile_sink_error_code_t code =
        n00b_objfile_sink_error_code(error);

    N00B_TEST_REQUIRE(code == N00B_OBJFILE_SINK_ERR_COMMIT_FAILED
                      || code == N00B_OBJFILE_SINK_ERR_UNSUPPORTED);

    auto detail = n00b_objfile_sink_error_detail(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(detail));
    if (code == N00B_OBJFILE_SINK_ERR_COMMIT_FAILED) {
        N00B_TEST_REQUIRE(n00b_option_get(detail) == EEXIST);
    }
    else {
        N00B_TEST_REQUIRE(n00b_option_get(detail) == ENOSYS);
    }

    return error;
}

static void
assert_sink_destination(n00b_objfile_sink_error_t *error,
                        n00b_string_t             *expected)
{
    auto destination = n00b_objfile_sink_error_destination_path(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(destination));
    assert_string_eq(n00b_option_get(destination), expected);
}

static n00b_objfile_sink_result_t *
require_sink_error_facts(n00b_objfile_sink_error_t *error)
{
    auto facts = n00b_objfile_sink_error_result_facts(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(facts));
    return n00b_option_get(facts);
}

static n00b_string_t *
fixture_dir(void)
{
    auto dir_r = n00b_new_temp_dir(r"n00b_bundle_file_", r"_dir");

    N00B_TEST_REQUIRE(n00b_result_is_ok(dir_r));
    return n00b_result_get(dir_r);
}

static n00b_string_t *
fixture_child(n00b_string_t *dir, n00b_string_t *name)
{
    return n00b_path_simple_join(dir, name);
}

static void
fixture_write_buffer(n00b_string_t *path, n00b_buffer_t *buffer)
{
    auto open_r = n00b_file_open(path, .mode = N00B_FILE_W);

    N00B_TEST_REQUIRE(n00b_result_is_ok(open_r));
    n00b_file_t *file = n00b_result_get(open_r);

    auto write_r = n00b_file_write_all(file, buffer);

    N00B_TEST_REQUIRE(n00b_result_is_ok(write_r));
    N00B_TEST_REQUIRE(n00b_result_get(write_r) == buffer->byte_len);

    auto close_r = n00b_file_close_result(file);

    N00B_TEST_REQUIRE(n00b_result_is_ok(close_r));
}

static void
fixture_write_string(n00b_string_t *path, n00b_string_t *contents)
{
    fixture_write_buffer(path,
                         n00b_buffer_from_bytes(contents->data,
                                                (int64_t)contents->u8_bytes));
}

static n00b_buffer_t *
fixture_read_buffer(n00b_string_t *path)
{
    auto open_r = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);

    N00B_TEST_REQUIRE(n00b_result_is_ok(open_r));
    n00b_file_t *file = n00b_result_get(open_r);

    auto buffer_r = n00b_file_as_buffer(file);

    N00B_TEST_REQUIRE(n00b_result_is_ok(buffer_r));
    n00b_buffer_t *copy = n00b_buffer_copy(n00b_result_get(buffer_r));

    auto close_r = n00b_file_close_result(file);

    N00B_TEST_REQUIRE(n00b_result_is_ok(close_r));
    return copy;
}

static void
fixture_assert_file_bytes(n00b_string_t *path, n00b_buffer_t *expected)
{
    assert_buffer_eq(fixture_read_buffer(path), expected);
}

static void
fixture_assert_file_string(n00b_string_t *path, n00b_string_t *expected)
{
    fixture_assert_file_bytes(
        path,
        n00b_buffer_from_bytes(expected->data, (int64_t)expected->u8_bytes));
}

static void
fixture_unlink(n00b_string_t *path)
{
    auto unlink_r = n00b_file_unlink(path, .ignore_missing = true);

    N00B_TEST_REQUIRE(n00b_result_is_ok(unlink_r));
}

static void
assert_write_file_facts(n00b_objfile_sink_result_t *facts,
                        n00b_string_t              *destination_path,
                        n00b_buffer_t              *expected_bytes,
                        n00b_objfile_sink_overwrite_t expected_overwrite)
{
    assert_string_eq(n00b_objfile_sink_result_destination_path(facts),
                     destination_path);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_bytes_requested(facts)
                      == expected_bytes->byte_len);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_bytes_written(facts)
                      == expected_bytes->byte_len);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_mode_requested(facts)
                      == N00B_OBJFILE_SINK_MODE_ATOMIC);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_mode_used(facts)
                      == N00B_OBJFILE_SINK_MODE_ATOMIC);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_overwrite(facts)
                      == expected_overwrite);
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_commit_attempted(facts));
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_commit_completed(facts));
}

static void
assert_sink_collision_facts(n00b_objfile_sink_error_t *error,
                            n00b_string_t             *destination_path)
{
    assert_sink_destination(error, destination_path);

    auto mode = n00b_objfile_sink_error_mode(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(mode));
    N00B_TEST_REQUIRE(n00b_option_get(mode) == N00B_OBJFILE_SINK_MODE_ATOMIC);

    auto overwrite = n00b_objfile_sink_error_overwrite(error);

    N00B_TEST_REQUIRE(n00b_option_is_set(overwrite));
    N00B_TEST_REQUIRE(n00b_option_get(overwrite)
                      == N00B_OBJFILE_SINK_REJECT_EXISTING);

    n00b_objfile_sink_result_t *facts = require_sink_error_facts(error);

    N00B_TEST_REQUIRE(n00b_objfile_sink_result_commit_attempted(facts));
    N00B_TEST_REQUIRE(!n00b_objfile_sink_result_commit_completed(facts));
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_cleanup_attempted(facts));
    N00B_TEST_REQUIRE(n00b_objfile_sink_result_cleanup_succeeded(facts));
}

static uint32_t
test_get32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static n00b_buffer_t *
make_rewrite_target(void)
{
    n00b_buffer_t *buf = n00b_test_elf_minimal_exec(0x400080,
                                                    0,
                                                    0x400000,
                                                    256,
                                                    512,
                                                    true,
                                                    false,
                                                    true,
                                                    false);
    buf->byte_len = 395;
    return buf;
}

static n00b_elf_binary_t *
parse_elf_or_die(n00b_buffer_t *bytes)
{
    n00b_bstream_t *stream = n00b_bstream_new(bytes);
    auto            parsed = n00b_elf_parse(stream);

    N00B_TEST_REQUIRE(n00b_result_is_ok(parsed));
    return n00b_result_get(parsed);
}

static uint64_t
section_header_offset(n00b_elf_binary_t *bin, uint32_t section_index)
{
    uint64_t offset = bin->header.shoff
                    + (uint64_t)section_index * bin->header.shentsize;

    N00B_TEST_REQUIRE(section_index < bin->num_sections);
    N00B_TEST_REQUIRE(offset + TEST_ELF64_SHDR_SIZE
                      <= bin->stream->buf->byte_len);
    return offset;
}

static n00b_elf_section_t *
section_named(n00b_elf_binary_t *bin, n00b_string_t *name)
{
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (bin->sections[i].name != nullptr
            && n00b_unicode_str_eq(bin->sections[i].name, name)) {
            return &bin->sections[i];
        }
    }

    return nullptr;
}

static uint32_t
section_index_named(n00b_elf_binary_t *bin, n00b_string_t *name)
{
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (bin->sections[i].name != nullptr
            && n00b_unicode_str_eq(bin->sections[i].name, name)) {
            return i;
        }
    }

    N00B_TEST_REQUIRE(false);
    return 0;
}

static n00b_elf_section_t *
require_section_named(n00b_elf_binary_t *bin, n00b_string_t *name)
{
    n00b_elf_section_t *section = section_named(bin, name);

    N00B_TEST_REQUIRE(section != nullptr);
    return section;
}

static uint64_t
count_sections_named(n00b_elf_binary_t *bin, n00b_string_t *name)
{
    uint64_t count = 0;

    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (bin->sections[i].name != nullptr
            && n00b_unicode_str_eq(bin->sections[i].name, name)) {
            count++;
        }
    }

    return count;
}

static void
assert_range_zeroed(n00b_buffer_t *bytes, uint64_t start, uint64_t end)
{
    N00B_TEST_REQUIRE(start <= end);
    N00B_TEST_REQUIRE(end <= bytes->byte_len);

    for (uint64_t i = start; i < end; i++) {
        N00B_TEST_REQUIRE(bytes->data[i] == 0);
    }
}

static void
assert_section_content(n00b_elf_section_t *section, n00b_buffer_t *expected)
{
    N00B_TEST_REQUIRE(section != nullptr);
    N00B_TEST_REQUIRE(section->content != nullptr);
    assert_buffer_eq(section->content, expected);
}

static n00b_buffer_t *
filled_payload(uint8_t fill, size_t len)
{
    n00b_buffer_t *payload = n00b_buffer_new((int64_t)len);

    memset(payload->data, fill, len);
    payload->byte_len = len;
    return payload;
}

static n00b_elf_rewrite_metadata_request_t
test_rewrite_request(n00b_string_t *name, n00b_buffer_t *payload)
{
    return (n00b_elf_rewrite_metadata_request_t){
        .section_name   = name,
        .payload        = payload,
        .file_alignment = 8,
        .section_type   = SHT_PROGBITS,
        .section_flags  = 0,
        .policy         = {
            .flags = N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION
                   | N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY,
        },
    };
}

static n00b_buffer_t *
make_target_with_chalk_mark(n00b_buffer_t **mark_payload_out)
{
    n00b_buffer_t *target = make_rewrite_target();
    n00b_elf_binary_t *elf = parse_elf_or_die(target);
    n00b_buffer_t *payload = filled_payload(0x5a, 17);
    n00b_elf_rewrite_metadata_request_t request =
        test_rewrite_request(r".chalk.mark", payload);

    auto plan_result = n00b_elf_rewrite_plan_chalk_mark_insert(elf, &request);

    N00B_TEST_REQUIRE(n00b_result_is_ok(plan_result));

    auto written = n00b_elf_rewrite_apply_metadata_insert_plan(
        elf,
        n00b_result_get(plan_result));

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));

    if (mark_payload_out != nullptr) {
        *mark_payload_out = payload;
    }

    return n00b_result_get(written);
}

static n00b_buffer_t *
make_target_with_chalk_free(n00b_buffer_t **free_payload_out)
{
    n00b_buffer_t *target = make_rewrite_target();
    n00b_elf_binary_t *elf = parse_elf_or_die(target);
    n00b_buffer_t *payload = filled_payload(0x6b, 19);
    n00b_string_t *scratch_name = r".n00b.freee";
    n00b_string_t *chalk_name   = r".chalk.free";
    n00b_elf_rewrite_metadata_request_t request =
        test_rewrite_request(scratch_name, payload);

    N00B_TEST_REQUIRE(scratch_name->u8_bytes == chalk_name->u8_bytes);

    auto written = n00b_elf_rewrite_apply_metadata_insert(elf, &request);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));

    n00b_buffer_t *bytes = n00b_result_get(written);
    n00b_elf_binary_t *with_free_slot = parse_elf_or_die(bytes);
    uint32_t index = section_index_named(with_free_slot, scratch_name);
    uint64_t shdr = section_header_offset(with_free_slot, index);
    uint32_t name_off = test_get32_le((const uint8_t *)bytes->data
                                      + shdr
                                      + TEST_SH_NAME);
    n00b_elf_section_t *shstrtab =
        &with_free_slot->sections[with_free_slot->header.shstrndx];
    uint64_t name_pos = shstrtab->offset + name_off;

    N00B_TEST_REQUIRE(name_pos + chalk_name->u8_bytes <= bytes->byte_len);
    memcpy(bytes->data + name_pos, chalk_name->data, chalk_name->u8_bytes);
    N00B_TEST_REQUIRE(section_named(parse_elf_or_die(bytes), chalk_name)
                      != nullptr);

    if (free_payload_out != nullptr) {
        *free_payload_out = payload;
    }

    return bytes;
}

static void
write_elf64_header(n00b_writer_t *writer, uint64_t shoff, uint16_t shnum)
{
    n00b_writer_setpos(writer, 0);
    n00b_writer_write_u8(writer, 0x7f);
    n00b_writer_write_u8(writer, 'E');
    n00b_writer_write_u8(writer, 'L');
    n00b_writer_write_u8(writer, 'F');
    n00b_writer_write_u8(writer, ELFCLASS64);
    n00b_writer_write_u8(writer, ELFDATA2LSB);
    n00b_writer_write_u8(writer, EV_CURRENT);
    n00b_writer_write_u8(writer, ELFOSABI_NONE);
    n00b_writer_write_u8(writer, 0);
    n00b_writer_write_zeros(writer, EI_NIDENT - EI_PAD);
    n00b_writer_write_u16(writer, ET_REL);
    n00b_writer_write_u16(writer, EM_X86_64);
    n00b_writer_write_u32(writer, EV_CURRENT);
    n00b_writer_write_u64(writer, 0);
    n00b_writer_write_u64(writer, 0);
    n00b_writer_write_u64(writer, shoff);
    n00b_writer_write_u32(writer, 0);
    n00b_writer_write_u16(writer, TEST_ELF64_EHDR_SIZE);
    n00b_writer_write_u16(writer, 0);
    n00b_writer_write_u16(writer, 0);
    n00b_writer_write_u16(writer, TEST_ELF64_SHDR_SIZE);
    n00b_writer_write_u16(writer, shnum);
    n00b_writer_write_u16(writer, 1);
}

static void
write_elf64_shdr(n00b_writer_t *writer,
                 uint32_t       name,
                 uint32_t       type,
                 uint64_t       flags,
                 uint64_t       offset,
                 uint64_t       size)
{
    n00b_writer_write_u32(writer, name);
    n00b_writer_write_u32(writer, type);
    n00b_writer_write_u64(writer, flags);
    n00b_writer_write_u64(writer, 0);
    n00b_writer_write_u64(writer, offset);
    n00b_writer_write_u64(writer, size);
    n00b_writer_write_u32(writer, 0);
    n00b_writer_write_u32(writer, 0);
    n00b_writer_write_u64(writer, 1);
    n00b_writer_write_u64(writer, 0);
}

static uint32_t
write_strtab_name(n00b_writer_t *writer, n00b_string_t *name)
{
    size_t off = n00b_writer_pos(writer);

    N00B_TEST_REQUIRE(off <= UINT32_MAX);
    N00B_TEST_REQUIRE(name != nullptr);
    N00B_TEST_REQUIRE(name->u8_bytes <= UINT32_MAX);

    if (name->u8_bytes != 0) {
        n00b_writer_write_bytes(writer, name->data, name->u8_bytes);
    }

    n00b_writer_write_u8(writer, 0);
    return (uint32_t)off;
}

static n00b_buffer_t *
make_elf_with_sections(test_elf_section_t *sections, size_t section_count)
{
    N00B_TEST_REQUIRE(section_count + 2 <= UINT16_MAX);

    n00b_writer_t *strtab = n00b_writer_new(128);

    n00b_writer_write_u8(strtab, 0);
    uint32_t shstr_name_off = write_strtab_name(strtab, r".shstrtab");

    for (size_t i = 0; i < section_count; i++) {
        sections[i].name_off = write_strtab_name(strtab, sections[i].name);
    }

    N00B_TEST_REQUIRE(!n00b_writer_has_error(strtab));
    n00b_buffer_t *shstrtab = n00b_writer_finalize(strtab);

    n00b_writer_t *writer = n00b_writer_new(1024);

    n00b_writer_set_endian(writer, N00B_ENDIAN_LITTLE);
    n00b_writer_write_zeros(writer, TEST_ELF64_EHDR_SIZE);

    for (size_t i = 0; i < section_count; i++) {
        n00b_buffer_t *content = sections[i].content;

        sections[i].offset = 0;
        sections[i].size   = content == nullptr ? 0 : content->byte_len;

        if (sections[i].type == SHT_NOBITS
            || content == nullptr
            || content->byte_len == 0) {
            continue;
        }

        n00b_writer_align(writer, 8);
        sections[i].offset = n00b_writer_pos(writer);
        n00b_writer_write_bytes(writer, content->data, content->byte_len);
    }

    n00b_writer_align(writer, 8);
    uint64_t shstrtab_off = n00b_writer_pos(writer);

    n00b_writer_write_bytes(writer, shstrtab->data, shstrtab->byte_len);
    n00b_writer_align(writer, 8);

    uint64_t shoff = n00b_writer_pos(writer);

    write_elf64_shdr(writer, 0, SHT_NULL, 0, 0, 0);
    write_elf64_shdr(writer,
                     shstr_name_off,
                     SHT_STRTAB,
                     0,
                     shstrtab_off,
                     shstrtab->byte_len);

    for (size_t i = 0; i < section_count; i++) {
        write_elf64_shdr(writer,
                         sections[i].name_off,
                         sections[i].type,
                         sections[i].flags,
                         sections[i].offset,
                         sections[i].size);
    }

    size_t end = n00b_writer_pos(writer);

    write_elf64_header(writer, shoff, (uint16_t)(section_count + 2));
    n00b_writer_setpos(writer, end);

    N00B_TEST_REQUIRE(!n00b_writer_has_error(writer));
    return n00b_writer_finalize(writer);
}

static n00b_buffer_t *
make_bundle_bytes(void)
{
    n00b_obj_bundle_t *bundle  = make_bundle();
    n00b_buffer_t     *payload = n00b_buffer_from_cstr("payload");

    auto add = n00b_obj_bundle_add_artifact(bundle,
                                            r"bin/tool",
                                            payload,
                                            .mode = 0755);

    N00B_TEST_REQUIRE(n00b_result_is_ok(add));

    auto set_exec = n00b_obj_bundle_set_default_exec(bundle, r"bin/tool");

    N00B_TEST_REQUIRE(n00b_result_is_ok(set_exec));

    auto encoded = n00b_obj_bundle_encode(bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(encoded));
    return n00b_result_get(encoded);
}

static n00b_buffer_t *
make_elf_with_bundle_section(n00b_buffer_t *bundle_bytes)
{
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
    };

    return make_elf_with_sections(sections, 1);
}

static n00b_obj_bundle_t *
require_read_success(n00b_result_t(n00b_obj_bundle_t *) result,
                     n00b_buffer_t                    *expected_encoded)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));

    n00b_obj_bundle_t *bundle = n00b_result_get(result);
    auto              encoded = n00b_obj_bundle_encode(bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(encoded));
    assert_buffer_eq(n00b_result_get(encoded), expected_encoded);

    return bundle;
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
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    n00b_buffer_t *macho_bytes  = make_elf_with_bundle_section(bundle_bytes);
    n00b_buffer_t *macho_copy   = n00b_buffer_copy(macho_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(macho_bytes, .format = N00B_FMT_MACHO),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);

    assert_format(error, N00B_FMT_MACHO);
    assert_no_carrier(error);
    assert_buffer_unchanged(macho_bytes, macho_copy);

    n00b_buffer_t *pe_bytes = make_elf_with_bundle_section(bundle_bytes);
    n00b_buffer_t *pe_copy  = n00b_buffer_copy(pe_bytes);

    error = require_read_error(
        n00b_obj_bundle_read(pe_bytes, .format = N00B_FMT_PE),
        N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_CARRIER);
    assert_format(error, N00B_FMT_PE);
    assert_no_carrier(error);
    assert_buffer_unchanged(pe_bytes, pe_copy);
}

static void
test_read_valid_elf_carrier(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    n00b_buffer_t *object_bytes = make_elf_with_bundle_section(bundle_bytes);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);

    require_read_success(n00b_obj_bundle_read(object_bytes), bundle_bytes);
    assert_buffer_unchanged(object_bytes, snapshot);

    snapshot = n00b_buffer_copy(object_bytes);

    require_read_success(
        n00b_obj_bundle_read(object_bytes, .format = N00B_FMT_ELF),
        bundle_bytes);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_missing_elf_carrier(void)
{
    n00b_buffer_t *payload = n00b_buffer_from_cstr("text");
    test_elf_section_t sections[] = {
        {
            .name    = r".text",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = payload,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 1);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(object_bytes),
        N00B_OBJ_BUNDLE_ERR_BUNDLE_NOT_FOUND);

    assert_elf_metadata_carrier_error(error);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_duplicate_elf_carrier(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 2);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(object_bytes),
        N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_detail(error, 2);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_malformed_elf_carrier_payload(void)
{
    n00b_buffer_t *bad_payload = n00b_buffer_from_cstr("not-a-bundle");
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bad_payload,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 1);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(object_bytes),
        N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_detail(error, N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_malformed_elf_carrier_shape(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    test_elf_section_t wrong_type[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_NOTE,
            .flags   = 0,
            .content = bundle_bytes,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(wrong_type, 1);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(object_bytes),
        N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_detail(error, SHT_NOTE);
    assert_buffer_unchanged(object_bytes, snapshot);

    test_elf_section_t alloc_carrier[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = SHF_ALLOC,
            .content = bundle_bytes,
        },
    };

    object_bytes = make_elf_with_sections(alloc_carrier, 1);
    snapshot     = n00b_buffer_copy(object_bytes);
    error = require_read_error(n00b_obj_bundle_read(object_bytes),
                               N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_detail(error, SHF_ALLOC);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_elf_carrier_with_foreign_sections(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    n00b_buffer_t *foreign      = n00b_buffer_from_cstr("foreign");
    n00b_buffer_t *chalk        = n00b_buffer_from_cstr("chalk");
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
        {
            .name    = r".0c001.file",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = foreign,
        },
        {
            .name    = r".0c001.wrap",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = foreign,
        },
        {
            .name    = r".0c001.code",
            .type    = SHT_PROGBITS,
            .flags   = SHF_ALLOC,
            .content = foreign,
        },
        {
            .name    = r".0c001.extra",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = foreign,
        },
        {
            .name    = r".chalk.mark",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = chalk,
        },
        {
            .name    = r".chalk.free",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = chalk,
        },
        {
            .name    = r".guard",
            .type    = 0xc001u,
            .flags   = 0,
            .content = foreign,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 8);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);

    require_read_success(n00b_obj_bundle_read(object_bytes), bundle_bytes);
    assert_buffer_unchanged(object_bytes, snapshot);
}

static void
test_read_file_carrier_alone_not_imported(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.file",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 1);
    n00b_buffer_t *snapshot     = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_error_t *error = require_read_error(
        n00b_obj_bundle_read(object_bytes),
        N00B_OBJ_BUNDLE_ERR_BUNDLE_NOT_FOUND);

    assert_elf_metadata_carrier_error(error);
    assert_buffer_unchanged(object_bytes, snapshot);
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

static void
assert_written_bundle_reads_back(n00b_buffer_t *object_bytes,
                                 n00b_buffer_t *expected_encoded)
{
    n00b_elf_binary_t *elf = parse_elf_or_die(object_bytes);
    n00b_elf_section_t *carrier =
        require_section_named(elf, r".0c001.bundle");

    N00B_TEST_REQUIRE(count_sections_named(elf, r".0c001.bundle") == 1);
    N00B_TEST_REQUIRE(carrier->type == SHT_PROGBITS);
    N00B_TEST_REQUIRE((carrier->flags & SHF_ALLOC) == 0);
    assert_section_content(carrier, expected_encoded);
    require_read_success(n00b_obj_bundle_read(object_bytes), expected_encoded);
}

static void
test_write_insert_readback_and_immutability(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_buffer_t     *object_snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t     *bundle_snapshot = encode_bundle_or_die(bundle);

    auto written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));
    assert_buffer_unchanged(object_bytes, object_snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
    assert_written_bundle_reads_back(n00b_result_get(written),
                                     bundle_snapshot);
}

static n00b_buffer_t *
write_bundle_or_die(n00b_buffer_t     *object_bytes,
                    n00b_obj_bundle_t *bundle)
{
    auto written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));
    return n00b_result_get(written);
}

static void
test_write_file_atomic_readback_and_byte_equality(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t     *bundle_snapshot = encode_bundle_or_die(bundle);
    auto               expected_r = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(expected_r));
    n00b_buffer_t *expected = n00b_result_get(expected_r);
    n00b_string_t *path = fixture_child(fixture_dir(), r"bundle-output.o");

    n00b_objfile_sink_result_t *facts = require_write_file_ok(
        n00b_obj_bundle_write_file(object_bytes, bundle, path));

    assert_write_file_facts(facts,
                            path,
                            expected,
                            N00B_OBJFILE_SINK_REJECT_EXISTING);
    fixture_assert_file_bytes(path, expected);

    n00b_buffer_t *file_bytes = fixture_read_buffer(path);

    require_read_success(n00b_obj_bundle_read(file_bytes), bundle_snapshot);
    fixture_unlink(path);
}

static void
test_read_elf_metadata_carrier_then_extract(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_obj_bundle_t *bundle = make_bundle();
    n00b_buffer_t     *payload =
        n00b_buffer_from_cstr("carrier extraction payload");

    auto add = n00b_obj_bundle_add_artifact(bundle,
                                            r"share/data.txt",
                                            payload);
    N00B_TEST_REQUIRE(n00b_result_is_ok(add));

    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);
    auto           written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));
    n00b_buffer_t *carrier_object = n00b_result_get(written);
    n00b_obj_bundle_t *read_bundle = require_read_success(
        n00b_obj_bundle_read(carrier_object),
        bundle_snapshot);

    n00b_string_t *root =
        n00b_new_temp_path(r"n00b_carrier_extract_", r"_root");
    n00b_obj_bundle_extract_result_t *facts = require_extract_ok(
        n00b_obj_bundle_extract(read_bundle, root));

    assert_string_eq(n00b_obj_bundle_extract_result_destination_root(facts),
                     root);
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_atomic_requested(facts));
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_atomic_used(facts));
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_commit_attempted(facts));
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_commit_completed(facts));
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_files_planned(facts) == 1);
    N00B_TEST_REQUIRE(n00b_obj_bundle_extract_result_files_written(facts) == 1);
    N00B_TEST_REQUIRE(
        n00b_obj_bundle_extract_result_directories_planned(facts) == 0);
    N00B_TEST_REQUIRE(
        n00b_obj_bundle_extract_result_directories_written(facts) == 2);

    auto policy_kind = n00b_obj_bundle_extract_result_policy_kind(facts);
    N00B_TEST_REQUIRE(n00b_option_is_set(policy_kind));
    N00B_TEST_REQUIRE(n00b_option_get(policy_kind)
                      == N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT);

    auto temp_root = n00b_obj_bundle_extract_result_temp_root(facts);
    N00B_TEST_REQUIRE(n00b_option_is_set(temp_root));
    N00B_TEST_REQUIRE(!n00b_path_exists(n00b_option_get(temp_root)));

    n00b_string_t *share = fixture_child(root, r"share");
    n00b_string_t *data = fixture_child(share, r"data.txt");

    fixture_assert_file_bytes(data, payload);

    auto cleanup_r = n00b_path_remove_tree(root, .ignore_missing = true);
    N00B_TEST_REQUIRE(n00b_result_is_ok(cleanup_r));
}

static void
test_read_elf_metadata_carrier_then_plan_execution(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_buffer_t     *object_snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_execution_plan_bundle();
    n00b_buffer_t     *bundle_snapshot = encode_bundle_or_die(bundle);
    n00b_obj_bundle_exec_plan_t *original_plan = require_exec_plan_ok(
        n00b_obj_bundle_exec_plan(bundle,
                                  .selector = r"tool",
                                  .mode = N00B_OBJ_BUNDLE_EXEC_EXTRACTED));

    auto written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));
    assert_buffer_unchanged(object_bytes, object_snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);

    n00b_buffer_t *carrier_object = n00b_result_get(written);
    n00b_buffer_t *carrier_snapshot = n00b_buffer_copy(carrier_object);
    n00b_obj_bundle_t *read_bundle = require_read_success(
        n00b_obj_bundle_read(carrier_object),
        bundle_snapshot);
    n00b_obj_bundle_exec_plan_t *read_plan = require_exec_plan_ok(
        n00b_obj_bundle_exec_plan(read_bundle,
                                  .selector = r"tool",
                                  .mode = N00B_OBJ_BUNDLE_EXEC_EXTRACTED));

    assert_exec_plan_tool_selection(original_plan);
    assert_exec_plan_tool_selection(read_plan);
    assert_buffer_unchanged(carrier_object, carrier_snapshot);
    assert_bundle_unchanged(read_bundle, bundle_snapshot);
}

static void
test_write_file_default_overwrite_rejects_existing_destination(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_string_t     *path = fixture_child(fixture_dir(), r"reject-output.o");

    fixture_write_string(path, r"original destination");

    n00b_objfile_sink_error_t *error = require_write_file_sink_collision(
        n00b_obj_bundle_write_file(object_bytes, bundle, path));

    assert_sink_collision_facts(error, path);
    fixture_assert_file_string(path, r"original destination");
    fixture_unlink(path);
}

static void
test_write_file_explicit_overwrite_replaces_destination(void)
{
    n00b_buffer_t     *object_bytes = make_rewrite_target();
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    auto               expected_r = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(expected_r));
    n00b_buffer_t *expected = n00b_result_get(expected_r);
    n00b_string_t *path = fixture_child(fixture_dir(), r"replace-output.o");

    fixture_write_string(path, r"old object bytes");

    n00b_objfile_sink_result_t *facts = require_write_file_ok(
        n00b_obj_bundle_write_file(
            object_bytes,
            bundle,
            path,
            .overwrite = N00B_OBJFILE_SINK_REPLACE_EXISTING));

    assert_write_file_facts(facts,
                            path,
                            expected,
                            N00B_OBJFILE_SINK_REPLACE_EXISTING);
    fixture_assert_file_bytes(path, expected);
    fixture_unlink(path);
}

static void
test_write_file_carrier_replace_rejects_before_sink_replace(void)
{
    n00b_obj_bundle_t *original_bundle = make_populated_bundle_a();
    n00b_buffer_t *bundled = write_bundle_or_die(make_rewrite_target(),
                                                 original_bundle);
    n00b_obj_bundle_t *new_bundle = make_populated_bundle_b();
    n00b_string_t *path =
        fixture_child(fixture_dir(), r"carrier-policy-output.o");

    fixture_write_string(path, r"carrier failure sentinel");

    n00b_obj_bundle_error_t *error = require_write_file_bundle_error(
        n00b_obj_bundle_write_file(
            bundled,
            new_bundle,
            path,
            .overwrite = N00B_OBJFILE_SINK_REPLACE_EXISTING),
        N00B_OBJ_BUNDLE_ERR_REPLACE_REQUIRED);

    assert_elf_metadata_carrier_error(error);
    fixture_assert_file_string(path, r"carrier failure sentinel");
    fixture_unlink(path);
}

static void
test_write_file_sink_rejects_after_carrier_replace_allowed(void)
{
    n00b_obj_bundle_t *original_bundle = make_populated_bundle_a();
    n00b_buffer_t *bundled = write_bundle_or_die(make_rewrite_target(),
                                                 original_bundle);
    n00b_obj_bundle_t *new_bundle = make_populated_bundle_b();
    n00b_string_t *path =
        fixture_child(fixture_dir(), r"sink-policy-output.o");

    fixture_write_string(path, r"sink failure sentinel");

    n00b_objfile_sink_error_t *error = require_write_file_sink_collision(
        n00b_obj_bundle_write_file(
            bundled,
            new_bundle,
            path,
            .replace = N00B_OBJ_BUNDLE_REPLACE_EXISTING));

    assert_sink_collision_facts(error, path);
    fixture_assert_file_string(path, r"sink failure sentinel");
    fixture_unlink(path);
}

static void
test_write_existing_valid_bundle_requires_replace(void)
{
    n00b_obj_bundle_t *original_bundle = make_populated_bundle_a();
    n00b_buffer_t *bundled = write_bundle_or_die(make_rewrite_target(),
                                                 original_bundle);
    n00b_buffer_t *bundled_snapshot = n00b_buffer_copy(bundled);
    n00b_obj_bundle_t *new_bundle = make_populated_bundle_b();
    n00b_buffer_t *new_bundle_snapshot = encode_bundle_or_die(new_bundle);

    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(bundled, new_bundle),
        N00B_OBJ_BUNDLE_ERR_REPLACE_REQUIRED);

    assert_elf_metadata_carrier_error(error);
    assert_buffer_unchanged(bundled, bundled_snapshot);
    assert_bundle_unchanged(new_bundle, new_bundle_snapshot);
}

static void
test_write_existing_valid_bundle_replaces_explicitly(void)
{
    n00b_obj_bundle_t *original_bundle = make_populated_bundle_a();
    n00b_buffer_t *bundled = write_bundle_or_die(make_rewrite_target(),
                                                 original_bundle);
    n00b_buffer_t *bundled_snapshot = n00b_buffer_copy(bundled);
    n00b_elf_binary_t *old_elf = parse_elf_or_die(bundled);
    n00b_elf_section_t *old_carrier =
        require_section_named(old_elf, r".0c001.bundle");
    uint64_t old_payload_start = old_carrier->offset;
    uint64_t old_payload_end = old_carrier->offset + old_carrier->size;
    n00b_obj_bundle_t *new_bundle = make_populated_bundle_b();
    n00b_buffer_t *new_bundle_snapshot = encode_bundle_or_die(new_bundle);

    auto replaced = n00b_obj_bundle_write(
        bundled,
        new_bundle,
        .replace = N00B_OBJ_BUNDLE_REPLACE_EXISTING);

    N00B_TEST_REQUIRE(n00b_result_is_ok(replaced));

    n00b_buffer_t *out = n00b_result_get(replaced);

    assert_buffer_unchanged(bundled, bundled_snapshot);
    assert_bundle_unchanged(new_bundle, new_bundle_snapshot);
    assert_written_bundle_reads_back(out, new_bundle_snapshot);
    assert_range_zeroed(out, old_payload_start, old_payload_end);
}

static void
test_write_malformed_existing_bundle_rejects_even_with_replace(void)
{
    n00b_buffer_t *bad_payload = n00b_buffer_from_cstr("not-a-bundle");
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bad_payload,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 1);
    n00b_buffer_t *snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);
    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(
            object_bytes,
            bundle,
            .replace = N00B_OBJ_BUNDLE_REPLACE_EXISTING),
        N00B_OBJ_BUNDLE_ERR_MALFORMED_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_buffer_unchanged(object_bytes, snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
}

static void
test_write_duplicate_existing_bundle_rejects(void)
{
    n00b_buffer_t *bundle_bytes = make_bundle_bytes();
    test_elf_section_t sections[] = {
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
        {
            .name    = r".0c001.bundle",
            .type    = SHT_PROGBITS,
            .flags   = 0,
            .content = bundle_bytes,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 2);
    n00b_buffer_t *snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);
    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(
            object_bytes,
            bundle,
            .replace = N00B_OBJ_BUNDLE_REPLACE_EXISTING),
        N00B_OBJ_BUNDLE_ERR_DUPLICATE_BUNDLE_CARRIER);

    assert_elf_metadata_carrier_error(error);
    assert_detail(error, 2);
    assert_buffer_unchanged(object_bytes, snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
}

static void
assert_reserved_write_case(n00b_string_t                *name,
                           uint32_t                      type,
                           uint64_t                      flags,
                           n00b_obj_bundle_error_code_t  expected)
{
    n00b_buffer_t *payload = n00b_buffer_from_cstr("reserved");
    test_elf_section_t sections[] = {
        {
            .name    = name,
            .type    = type,
            .flags   = flags,
            .content = payload,
        },
    };
    n00b_buffer_t *object_bytes = make_elf_with_sections(sections, 1);
    n00b_buffer_t *snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);
    n00b_obj_bundle_error_t *error = require_write_error(
        n00b_obj_bundle_write(object_bytes, bundle),
        expected);

    assert_elf_metadata_carrier_error(error);
    assert_buffer_unchanged(object_bytes, snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
}

static void
test_write_reserved_and_wrapped_inputs_reject(void)
{
    assert_reserved_write_case(r".0c001.file",
                               SHT_PROGBITS,
                               0,
                               N00B_OBJ_BUNDLE_ERR_FOREIGN_LEGACY_BUNDLE);
    assert_reserved_write_case(
        r".0c001.wrap",
        SHT_PROGBITS,
        0,
        N00B_OBJ_BUNDLE_ERR_ALREADY_WRAPPED_OR_RESERVED);
    assert_reserved_write_case(
        r".0c001.code",
        SHT_PROGBITS,
        SHF_ALLOC,
        N00B_OBJ_BUNDLE_ERR_ALREADY_WRAPPED_OR_RESERVED);
    assert_reserved_write_case(
        r".0c001.extra",
        SHT_PROGBITS,
        0,
        N00B_OBJ_BUNDLE_ERR_RESERVED_NAMESPACE_OCCUPIED);
    assert_reserved_write_case(r".guard",
                               TEST_ELF_GUARD_SECTION_TYPE,
                               0,
                               N00B_OBJ_BUNDLE_ERR_GUARD_SECTION_PRESENT);
}

static void
test_write_preserves_chalk_mark(void)
{
    n00b_buffer_t *mark_payload = nullptr;
    n00b_buffer_t *object_bytes = make_target_with_chalk_mark(&mark_payload);
    n00b_buffer_t *snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);

    auto written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));

    n00b_buffer_t *out = n00b_result_get(written);
    n00b_elf_binary_t *elf = parse_elf_or_die(out);
    n00b_elf_section_t *mark = require_section_named(elf, r".chalk.mark");

    assert_buffer_unchanged(object_bytes, snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
    assert_written_bundle_reads_back(out, bundle_snapshot);
    N00B_TEST_REQUIRE(count_sections_named(elf, r".chalk.mark") == 1);
    assert_section_content(mark, mark_payload);
}

static void
test_write_preserves_chalk_free_without_reuse(void)
{
    n00b_buffer_t *free_payload = nullptr;
    n00b_buffer_t *object_bytes = make_target_with_chalk_free(&free_payload);
    n00b_buffer_t *snapshot = n00b_buffer_copy(object_bytes);
    n00b_obj_bundle_t *bundle = make_populated_bundle_a();
    n00b_buffer_t *bundle_snapshot = encode_bundle_or_die(bundle);

    auto written = n00b_obj_bundle_write(object_bytes, bundle);

    N00B_TEST_REQUIRE(n00b_result_is_ok(written));

    n00b_buffer_t *out = n00b_result_get(written);
    n00b_elf_binary_t *elf = parse_elf_or_die(out);
    n00b_elf_section_t *free_section =
        require_section_named(elf, r".chalk.free");
    n00b_elf_section_t *carrier =
        require_section_named(elf, r".0c001.bundle");

    assert_buffer_unchanged(object_bytes, snapshot);
    assert_bundle_unchanged(bundle, bundle_snapshot);
    assert_written_bundle_reads_back(out, bundle_snapshot);
    N00B_TEST_REQUIRE(count_sections_named(elf, r".chalk.free") == 1);
    assert_section_content(free_section, free_payload);
    N00B_TEST_REQUIRE(free_section->offset != carrier->offset);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_carrier_error_strings();
    test_read_invalid_arguments();
    test_read_unsupported_formats();
    test_read_valid_elf_carrier();
    test_read_missing_elf_carrier();
    test_read_duplicate_elf_carrier();
    test_read_malformed_elf_carrier_payload();
    test_read_malformed_elf_carrier_shape();
    test_read_elf_carrier_with_foreign_sections();
    test_read_file_carrier_alone_not_imported();
    test_write_invalid_arguments();
    test_write_unsupported_carriers();
    test_write_insert_readback_and_immutability();
    test_write_file_atomic_readback_and_byte_equality();
    test_read_elf_metadata_carrier_then_extract();
    test_read_elf_metadata_carrier_then_plan_execution();
    test_write_file_default_overwrite_rejects_existing_destination();
    test_write_file_explicit_overwrite_replaces_destination();
    test_write_file_carrier_replace_rejects_before_sink_replace();
    test_write_file_sink_rejects_after_carrier_replace_allowed();
    test_write_existing_valid_bundle_requires_replace();
    test_write_existing_valid_bundle_replaces_explicitly();
    test_write_malformed_existing_bundle_rejects_even_with_replace();
    test_write_duplicate_existing_bundle_rejects();
    test_write_reserved_and_wrapped_inputs_reject();
    test_write_preserves_chalk_mark();
    test_write_preserves_chalk_free_without_reuse();

    return 0;
}
