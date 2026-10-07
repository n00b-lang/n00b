#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi_container.h"
#include "n00b-abi-v1.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

static const n00bb_abi_directory_entry_t *
entry_for(const n00bb_abi_directory_entry_t *entries, size_t count,
          uint32_t kind)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].kind == kind) return &entries[i];
    return nullptr;
}

static const void *
row_at(const core_object_section_t *section,
       const n00bb_abi_directory_entry_t *entry, uint32_t index)
{
    assert(entry && index < entry->record_count);
    return section->bytes + entry->file_offset
        + (size_t)index * entry->record_bytes;
}

static bool
short_name_is(const n00b_abi_string_v1 *value, const char *name)
{
    size_t length = strlen(name);
    return length <= 14
        && (value->short_value.selector & 15u) == length
        && !(value->short_value.selector & 0xc0u)
        && !memcmp(value->short_value.bytes, name, length);
}

int
main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0);
    assert(!fseek(file, 0, SEEK_SET));
    uint8_t *object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    assert(!fclose(file));

    struct {
        core_target_object_format_t format;
        const char *segment;
        const char *name;
    } locations[] = {
        {CORE_TARGET_OBJECT_ELF, nullptr, ".data.rel.ro.n00b_meta"},
        {CORE_TARGET_OBJECT_MACH_O, "__DATA_CONST", "__n00b_meta"},
        {CORE_TARGET_OBJECT_COFF, nullptr, ".n00b$M"},
    };
    core_object_section_t metadata = {0};
    bool found = false;
    for (size_t i = 0; i < sizeof(locations) / sizeof(*locations); i++)
        if (core_object_section_find(object, (size_t)length,
            locations[i].format, locations[i].segment, locations[i].name,
            &metadata)) {
            found = true;
            break;
        }
    assert(found && metadata.size >= N00BB_ABI_HEADER_BYTES);
    n00bb_abi_image_key_t key = {
        .target_id = core_le_load_u32(metadata.bytes + 16),
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .cpu_profile = core_le_load_u64(metadata.bytes + 40),
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    n00bb_abi_directory_entry_t entries[16];
    n00bb_abi_image_header_t header;
    assert(n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
        schemas, schema_count, false, &header, entries,
        sizeof(entries) / sizeof(*entries)));
    const n00bb_abi_directory_entry_t *types = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_TYPE);
    const n00bb_abi_directory_entry_t *fields = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_FIELD);
    const n00bb_abi_directory_entry_t *symbols = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_SYMBOL);
    const n00bb_abi_directory_entry_t *layouts = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_LAYOUT);
    const n00bb_abi_directory_entry_t *runs = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_PHYSICAL_RUN);
    const n00bb_abi_directory_entry_t *locations_table = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_SOURCE_LOCATION);
    const n00bb_abi_directory_entry_t *stream = entry_for(entries,
        header.directory_count, N00B_ABI_TABLE_U32_STREAM);
    assert(types && fields && symbols && layouts && runs && locations_table
        && stream);

    uint32_t record_index = 0;
    for (uint32_t i = 1; i < types->record_count; i++) {
        const n00b_abi_type_v1 *type = row_at(&metadata, types, i);
        assert(type->kind != N00B_ABI_TYPE_RECORD
            || type->owner_module_index == 1);
        if (type->kind == N00B_ABI_TYPE_RECORD
            && type->owner_module_index == 1
            && type->fields_or_arms.count == 1) {
            assert(!record_index);
            record_index = i;
        }
    }
    assert(record_index);
    const n00b_abi_type_v1 *record = row_at(&metadata, types, record_index);
    assert(record->layout_index && record->layout_index < layouts->record_count);
    const n00b_abi_layout_v1 *layout = row_at(&metadata, layouts,
                                               record->layout_index);
    assert(layout->type_index == record_index);
    assert(record->fields_or_arms.first < stream->record_count);
    uint32_t field_index = *(const uint32_t *)row_at(&metadata, stream,
                                                     record->fields_or_arms.first);
    assert(field_index && field_index < fields->record_count);
    const n00b_abi_field_v1 *field = row_at(&metadata, fields, field_index);
    assert(short_name_is(&field->name, "reading"));
    assert(field->parent_type_index == record_index);
    assert(field->child_type_index < types->record_count);
    assert(field->physical_runs.count);
    assert(field->physical_runs.first < stream->record_count);
    uint32_t run_index = *(const uint32_t *)row_at(&metadata, stream,
                                                   field->physical_runs.first);
    assert(run_index && run_index < runs->record_count);
    const n00b_abi_physical_run_v1 *run = row_at(&metadata, runs, run_index);
    assert(run->width_bits > 0);
    assert(field->symbol_index && field->symbol_index < symbols->record_count);
    const n00b_abi_symbol_v1 *field_symbol = row_at(&metadata, symbols,
                                                      field->symbol_index);
    assert(short_name_is(&field_symbol->name, "reading"));
    assert(field_symbol->type_index == field->child_type_index);
    assert(field_symbol->declaration_location_index
        && field_symbol->declaration_location_index
            < locations_table->record_count);
    const n00b_abi_source_location_v1 *location = row_at(&metadata,
        locations_table, field_symbol->declaration_location_index);
    assert(location->file_index == 1 && location->start_line > 0);

    free(object);
    return 0;
}
