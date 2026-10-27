#include "llvm_backend.h"
#include "llvm_stack_maps.h"
#include "native_frame_maps.h"
#include "frame_roots.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/pass.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)
#define S(x) CORE_STRING_STATIC(x)

static core_string_t
symbol_text(core_object_symbol_name_t name)
{
    return core_string_borrow((const char *)name.bytes, name.length);
}

static bool
call_collector(ir_module_t *runtime, ir_func_t *collector, ir_block_t *block)
{
    ir_instr_t *call = block ? ir_instr_insert(block, IR_OP_CALL, 0) : nullptr;
    return call
        && ir_instr_operand_value(call, ir_func_address(runtime, collector))
        && ir_instr_set_call_signature(call, collector->type);
}

enum { PIN_ROOT, PIN_INLINED, PIN_OPTIONAL, PIN_DISTINCT, PIN_RECORD,
       PIN_KINDS };

typedef struct {
    unsigned roots[PIN_KINDS];
    unsigned maps[PIN_KINDS];
} map_counts_t;

static int
pin_kind(core_object_symbol_name_t name)
{
    const core_string_t suffixes[PIN_KINDS] = {
        S("pin_root"), S("pin_inlined"), S("pin_optional"),
        S("pin_distinct"), S("pin_record_caller"),
    };
    core_string_t function = symbol_text(name);
    for (int kind = 0; kind < PIN_KINDS; kind++)
        if (core_string_ends_with(function, suffixes[kind])) return kind;
    return -1;
}

static bool
count_map(void *context, const n00bb_llvm_stack_function_t *function)
{
    map_counts_t *counts = context;
    int kind = pin_kind(function->name);
    if (kind >= 0) counts->maps[kind]++;
    return true;
}

static bool
check_root(void *context, const n00bb_llvm_stack_location_t *location)
{
    map_counts_t *counts = context;
    int kind = pin_kind(location->function->name);
    if (kind < 0) return true;
    counts->roots[kind]++;
    CHECK(location->kind == N00BB_LLVM_STACKMAP_DIRECT);
    CHECK(location->size_bytes == sizeof(void *));
#if !defined(_WIN32)
    CHECK(location->frame_offset < 0 || location->frame_offset >= 16);
#endif
#if defined(__aarch64__)
    CHECK(location->dwarf_register == 29);
#elif defined(__x86_64__)
    CHECK(location->dwarf_register == 6);
#endif
    return true;
}

static bool
no_mapping_source(const ir_value_t *value, void *context)
{
    (void)value;
    (void)context;
    return false;
}

static bool
unmanaged_parameter(const ir_value_t *value, void *context)
{
    return value == context;
}

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    const ir_scalar_t scalars[] = {
        {.type_text = S("nil"), .class = IR_CLASS_VOID},
        {.type_text = S("bool"), .class = IR_CLASS_BOOL, .width = 1},
        {.type_text = S("uint"), .class = IR_CLASS_UINT, .width = 64},
    };
    const ir_profile_t profile = {
        .name = S("frame-root-test"), .scalars = scalars, .n_scalars = 3,
    };
    core_target_desc_t host;
    CHECK(core_target_host(&host));
    core_string_t target = host.triple;
    ir_module_t *runtime = ir_module_new(context, S("probe"), &profile,
                                         target);
    th_type *point = ir_type_parse(runtime, S("Point"));
    th_type *point_definition = ir_type_parse(runtime,
                                              S("(x: uint, y: uint)"));
    th_type *number = ir_type_parse(runtime, S("uint"));
    const ir_final_layout_t number_layout = {
        .intrinsic_bits = 64, .alignment_bytes = 8,
    };
    const ir_layout_bit_run_t first_run[] = {{.offset = 0, .width = 64}};
    const ir_layout_bit_run_t second_run[] = {{.offset = 64, .width = 64}};
    const ir_layout_bit_run_t occupied[] = {{.offset = 0, .width = 128}};
    const ir_final_member_layout_t members[] = {
        {.name = S("x"), .type = number, .ordinal = 0,
         .bit_offset = 0, .occupied = first_run, .n_occupied = 1},
        {.name = S("y"), .type = number, .ordinal = 1,
         .bit_offset = 64, .occupied = second_run, .n_occupied = 1},
    };
    const ir_final_layout_t point_layout = {
        .intrinsic_bits = 128, .alignment_bytes = 8,
        .occupied = occupied, .n_occupied = 1,
        .members = members, .n_members = 2,
    };
    CHECK(point && point_definition && number
        && ir_layout_publish_final(runtime, number, target, &number_layout)
        && ir_type_define(runtime, point, point_definition)
        && ir_layout_publish_final(runtime, point, target, &point_layout));
    th_type *with_reference = ir_type_parse(runtime,
        S("(value: ref[uint])"));
    CHECK(with_reference && n00bb_frame_pointer_free_value(runtime, point)
        && !n00bb_frame_pointer_free_value(runtime, with_reference));
    ir_func_t *plain_record = ir_func_add(runtime, S("plain_record"),
        ir_type_parse(runtime, S("(Point) -> uint")), IR_LINK_INTERNAL);
    ir_func_t *referenced_record = ir_func_add(runtime,
        S("referenced_record"),
        ir_type_parse(runtime, S("((value: ref[uint])) -> uint")),
        IR_LINK_INTERNAL);
    size_t plain_roots = 0, referenced_roots = 0;
    CHECK(plain_record && referenced_record
        && ir_param_add(plain_record, S("value"), point)
        && ir_local_add(plain_record, S("local"), point)
        && ir_param_add(referenced_record, S("value"), with_reference)
        && ir_local_add(referenced_record, S("local"), with_reference)
        && n00bb_frame_roots(plain_record, nullptr, nullptr, &plain_roots)
        && n00bb_frame_roots(referenced_record, nullptr, nullptr,
                             &referenced_roots)
        && plain_roots == 0 && referenced_roots == 2);
    ir_func_t *function = ir_func_add(runtime, S("pin_root"),
        ir_type_parse(runtime, S("(ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *parameter = function
        ? ir_param_add(function, S("root"),
                       ir_type_parse(runtime, S("ref[Point]")))
        : nullptr;
    ir_local_t *scalar_local = function
        ? ir_local_add(function, S("scalar"), number) : nullptr;
    ir_block_t *entry = function ? ir_block_add(function, S("entry"))
                                 : nullptr;
    ir_instr_t *copy = entry ? ir_instr_add(entry, IR_OP_COPY) : nullptr;
    ir_instr_t *bitcast = entry ? ir_instr_add(entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *copy_chain = entry ? ir_instr_add(entry, IR_OP_COPY) : nullptr;
    ir_instr_t *field = entry ? ir_instr_add(entry, IR_OP_FIELD) : nullptr;
    ir_instr_t *field_cast = entry
        ? ir_instr_add(entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *result = entry ? ir_instr_add(entry, IR_OP_RETURN) : nullptr;
    CHECK(runtime && parameter && scalar_local
        && copy && bitcast && copy_chain
        && field && field_cast && result
        && ir_instr_set_dest(copy, S("copied"), parameter->type)
        && ir_instr_operand_value(copy, parameter)
        && ir_instr_set_dest(bitcast, S("casted"), parameter->type)
        && ir_instr_operand_value(bitcast, parameter)
        && ir_instr_set_dest(copy_chain, S("chain"), parameter->type)
        && ir_instr_operand_value(copy_chain, bitcast->dest)
        && ir_instr_set_dest(field, S("field"),
                             ir_type_parse(runtime, S("ref[uint]")))
        && ir_instr_operand_value(field, parameter)
        && ir_instr_operand_name(field, S("x"))
        && ir_instr_set_dest(field_cast, S("field.cast"), field->dest->type)
        && ir_instr_operand_value(field_cast, field->dest)
        && ir_instr_operand_value(result, copy->dest));
    ir_func_t *unmanaged = ir_func_add(runtime, S("pin_unmanaged"),
        ir_type_parse(runtime, S("(ref[uint]) -> ref[uint]")),
        IR_LINK_INTERNAL);
    ir_value_t *unmanaged_param = unmanaged
        ? ir_param_add(unmanaged, S("root"),
                       ir_type_parse(runtime, S("ref[uint]")))
        : nullptr;
    ir_block_t *unmanaged_entry = unmanaged
        ? ir_block_add(unmanaged, S("entry")) : nullptr;
    ir_instr_t *unmanaged_copy = unmanaged_entry
        ? ir_instr_add(unmanaged_entry, IR_OP_COPY) : nullptr;
    ir_instr_t *unmanaged_result = unmanaged_entry
        ? ir_instr_add(unmanaged_entry, IR_OP_RETURN) : nullptr;
    CHECK(unmanaged_param && unmanaged_copy && unmanaged_result
        && ir_instr_set_dest(unmanaged_copy, S("copied"),
                             unmanaged_param->type)
        && ir_instr_operand_value(unmanaged_copy, unmanaged_param)
        && ir_instr_operand_value(unmanaged_result, unmanaged_copy->dest));
    ir_func_t *pointer_local = ir_func_add(runtime, S("pin_pointer_local"),
        ir_type_parse(runtime, S("(ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *local_param = pointer_local
        ? ir_param_add(pointer_local, S("root"), parameter->type) : nullptr;
    ir_local_t *local_root = pointer_local
        ? ir_local_add(pointer_local, S("local.root"), parameter->type)
        : nullptr;
    ir_block_t *local_entry = pointer_local
        ? ir_block_add(pointer_local, S("entry")) : nullptr;
    ir_instr_t *local_return = local_entry
        ? ir_instr_add(local_entry, IR_OP_RETURN) : nullptr;
    CHECK(local_param && local_root && local_return
        && ir_instr_operand_value(local_return, local_param));
    ir_func_t *opaque_local = ir_func_add(runtime, S("pin_opaque_local"),
        ir_type_parse(runtime, S("(ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *opaque_param = opaque_local
        ? ir_param_add(opaque_local, S("root"), parameter->type) : nullptr;
    ir_local_t *opaque_root = opaque_local
        ? ir_local_add(opaque_local, S("local.option"),
            ir_type_parse(runtime, S("Option[ref[Point]]"))) : nullptr;
    ir_block_t *opaque_entry = opaque_local
        ? ir_block_add(opaque_local, S("entry")) : nullptr;
    ir_instr_t *opaque_return = opaque_entry
        ? ir_instr_add(opaque_entry, IR_OP_RETURN) : nullptr;
    CHECK(opaque_param && opaque_root && opaque_return
        && ir_instr_operand_value(opaque_return, opaque_param));
    ir_func_t *inline_helper = ir_func_add(runtime, S("pin_inline_helper"),
        ir_type_parse(runtime, S("(ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *helper_root = inline_helper
        ? ir_param_add(inline_helper, S("root"), parameter->type) : nullptr;
    ir_block_t *helper_entry = inline_helper
        ? ir_block_add(inline_helper, S("entry")) : nullptr;
    ir_instr_t *helper_return = helper_entry
        ? ir_instr_add(helper_entry, IR_OP_RETURN) : nullptr;
    CHECK(helper_root && helper_return
        && ir_instr_operand_value(helper_return, helper_root)
        && ir_func_set_always_inline(context, inline_helper, true));
    ir_func_t *inlined = ir_func_add(runtime, S("pin_inlined"),
        ir_type_parse(runtime, S("(bool, ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *condition = inlined
        ? ir_param_add(inlined, S("condition"),
                       ir_type_parse(runtime, S("bool")))
        : nullptr;
    ir_value_t *caller_root = inlined
        ? ir_param_add(inlined, S("root"), parameter->type) : nullptr;
    ir_block_t *caller_entry = inlined
        ? ir_block_add(inlined, S("entry")) : nullptr;
    ir_block_t *then_block = inlined
        ? ir_block_add(inlined, S("then")) : nullptr;
    ir_block_t *else_block = inlined
        ? ir_block_add(inlined, S("else")) : nullptr;
    ir_instr_t *branch = caller_entry
        ? ir_instr_add(caller_entry, IR_OP_BRANCH) : nullptr;
    ir_instr_t *inline_call = then_block
        ? ir_instr_add(then_block, IR_OP_CALL) : nullptr;
    ir_instr_t *then_return = then_block
        ? ir_instr_add(then_block, IR_OP_RETURN) : nullptr;
    ir_instr_t *else_return = else_block
        ? ir_instr_add(else_block, IR_OP_RETURN) : nullptr;
    CHECK(condition && caller_root && branch && inline_call
        && then_return && else_return
        && ir_instr_operand_value(branch, condition)
        && ir_instr_operand_block(branch, then_block)
        && ir_instr_operand_block(branch, else_block)
        && ir_instr_set_dest(inline_call, S("called"), parameter->type)
        && ir_instr_operand_value(inline_call,
            ir_func_address(runtime, inline_helper))
        && ir_instr_operand_value(inline_call, caller_root)
        && ir_instr_set_call_signature(inline_call, inline_helper->type)
        && ir_instr_operand_value(then_return, inline_call->dest)
        && ir_instr_operand_value(else_return, caller_root));
    th_type *optional_type = ir_type_parse(runtime,
                                           S("Option[ref[Point]]"));
    ir_func_t *optional = ir_func_add(runtime, S("pin_optional"),
        ir_type_parse(runtime,
            S("(Option[ref[Point]]) -> Option[ref[Point]]")),
        IR_LINK_INTERNAL);
    ir_value_t *optional_param = optional
        ? ir_param_add(optional, S("root"), optional_type) : nullptr;
    ir_block_t *optional_entry = optional
        ? ir_block_add(optional, S("entry")) : nullptr;
    ir_instr_t *optional_return = optional_entry
        ? ir_instr_add(optional_entry, IR_OP_RETURN) : nullptr;
    CHECK(optional_param && optional_return
        && ir_instr_operand_value(optional_return, optional_param));
    ir_func_t *distinct = ir_func_add(runtime, S("pin_distinct"),
        ir_type_parse(runtime, S("(ref[Point], ref[Point]) -> ref[Point]")),
        IR_LINK_INTERNAL);
    ir_value_t *distinct_first = distinct
        ? ir_param_add(distinct, S("first"), parameter->type) : nullptr;
    ir_value_t *distinct_second = distinct
        ? ir_param_add(distinct, S("second"), parameter->type) : nullptr;
    ir_block_t *distinct_entry = distinct
        ? ir_block_add(distinct, S("entry")) : nullptr;
    ir_instr_t *distinct_return = distinct_entry
        ? ir_instr_add(distinct_entry, IR_OP_RETURN) : nullptr;
    CHECK(distinct_first && distinct_second && distinct_return
        && ir_instr_operand_value(distinct_return, distinct_first));
    ir_func_t *record_helper = ir_func_add(runtime,
        S("pin_record_helper"), inline_helper->type, IR_LINK_INTERNAL);
    ir_value_t *record_input = record_helper
        ? ir_param_add(record_helper, S("root"), parameter->type) : nullptr;
    ir_local_t *record_local = record_helper
        ? ir_local_add(record_helper, S("aggregate"), point) : nullptr;
    ir_block_t *record_entry = record_helper
        ? ir_block_add(record_helper, S("entry")) : nullptr;
    ir_instr_t *record_return = record_entry
        ? ir_instr_add(record_entry, IR_OP_RETURN) : nullptr;
    CHECK(record_input && record_local && record_return
        && ir_instr_operand_value(record_return, record_input)
        && ir_func_set_always_inline(context, record_helper, true));
    ir_func_t *record_caller = ir_func_add(runtime,
        S("pin_record_caller"), inline_helper->type, IR_LINK_INTERNAL);
    ir_value_t *caller_input = record_caller
        ? ir_param_add(record_caller, S("root"), parameter->type) : nullptr;
    ir_block_t *record_caller_entry = record_caller
        ? ir_block_add(record_caller, S("entry")) : nullptr;
    ir_instr_t *record_call = record_caller_entry
        ? ir_instr_add(record_caller_entry, IR_OP_CALL) : nullptr;
    ir_instr_t *record_caller_return = record_caller_entry
        ? ir_instr_add(record_caller_entry, IR_OP_RETURN) : nullptr;
    CHECK(caller_input && record_call && record_caller_return
        && ir_instr_set_dest(record_call, S("result"), parameter->type)
        && ir_instr_operand_value(record_call,
            ir_func_address(runtime, record_helper))
        && ir_instr_operand_value(record_call, caller_input)
        && ir_instr_set_call_signature(record_call,
            record_helper->type)
        && ir_instr_operand_value(record_caller_return,
            record_call->dest));
    ir_func_t *reachable[] = {function, unmanaged, pointer_local, opaque_local,
                              inline_helper, inlined, optional, distinct,
                              record_helper, record_caller};
    ir_storage_origin_t *origin = ir_storage_origin_new(reachable, 10,
        no_mapping_source, unmanaged_parameter, nullptr, false,
        unmanaged_param);
    CHECK(origin);
    ir_func_t *collector = ir_func_add(runtime, S("pin_collect"),
        ir_type_parse(runtime, S("() -> nil")), IR_LINK_EXTERN);
    ir_block_t *collecting[] = {entry, unmanaged_entry, local_entry,
                                opaque_entry, helper_entry, caller_entry,
                                optional_entry, distinct_entry, record_entry,
                                record_caller_entry};
    for (size_t i = 0; i < sizeof collecting / sizeof *collecting; i++)
        CHECK(collector && call_collector(runtime, collector, collecting[i]));
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
        runtime, true, false, diagnostics);
    CHECK(backend && n00bb_llvm_backend_bind_storage_origin(backend, origin)
        && n00bb_llvm_backend_enable_frame_maps(backend));
    core_buffer_t object = {0};
    CHECK(n00bb_llvm_backend_emit_object(backend, nullptr, 0, 0, &object,
        diagnostics));
#if defined(__APPLE__)
    core_target_object_format_t format = CORE_TARGET_OBJECT_MACH_O;
#elif defined(_WIN32)
    core_target_object_format_t format = CORE_TARGET_OBJECT_COFF;
#else
    core_target_object_format_t format = CORE_TARGET_OBJECT_ELF;
#endif
    map_counts_t counts = {0};
    bool found = false;
    n00bb_llvm_stack_visitor_t visitor = {
        .function = count_map, .location = check_root, .context = &counts,
    };
    CHECK(n00bb_llvm_stack_maps_visit(object.data, object.byte_len, format,
                                      &visitor, &found));
    CHECK(found && counts.roots[PIN_ROOT] == 1 && counts.maps[PIN_ROOT] == 1
        && counts.roots[PIN_INLINED] >= 2 && counts.maps[PIN_INLINED] == 1
        && counts.roots[PIN_OPTIONAL] == 1 && counts.maps[PIN_OPTIONAL] == 1
        && counts.roots[PIN_DISTINCT] == 2 && counts.maps[PIN_DISTINCT] == 1
        && counts.roots[PIN_RECORD] == 2 && counts.maps[PIN_RECORD] == 1);
    core_target_desc_t native_target;
    CHECK(core_target_host(&native_target));
    n00bb_native_frames_t *frames = n00bb_native_frames_new(format);
    CHECK(frames && n00bb_native_frames_add(frames, object.data,
                                            object.byte_len));
    uint8_t *descriptor = NULL;
    size_t descriptor_bytes = 0;
    CHECK(n00bb_native_frames_emit(frames, &native_target, &descriptor,
                                   &descriptor_bytes));
    core_object_section_t section;
    CHECK(core_object_section_find(descriptor, descriptor_bytes, format,
        format == CORE_TARGET_OBJECT_MACH_O
            ? S("__DATA_CONST") : (core_string_t){},
        format == CORE_TARGET_OBJECT_MACH_O ? S("__n00b_frames")
            : format == CORE_TARGET_OBJECT_COFF ? S(".n00b$F")
                                            : S(".data.rel.ro.n00b_frames"),
        &section));
    CHECK(core_le_load_u32(section.bytes) >= 3);
    CHECK(core_le_load_u32(section.bytes + 4) >= 3);
    CHECK(section.relocation_count == core_le_load_u32(section.bytes));
    CHECK(core_le_load_u32(section.bytes + 16) > 0);
    CHECK(core_le_load_u32(section.bytes + 20) >= 1);
    core_object_absolute64_relocation_t relocation;
    CHECK(core_object_absolute64_relocation_at(&section, format, 0,
                                               &relocation));
    CHECK(relocation.section_offset == 8);
    core_object_symbol_name_t symbol;
    CHECK(core_object_symbol_name_at(descriptor, descriptor_bytes, format,
        &section, relocation.native_symbol_index, &symbol));
    CHECK(core_string_ends_with(symbol_text(symbol), S("pin_root")));
    bool found_inlined = false;
    for (uint32_t i = 0; i < section.relocation_count; i++) {
        CHECK(core_object_absolute64_relocation_at(&section, format, i,
                                                   &relocation));
        CHECK(core_object_symbol_name_at(descriptor, descriptor_bytes,
            format, &section, relocation.native_symbol_index, &symbol));
        if (!core_string_ends_with(symbol_text(symbol), S("pin_inlined")))
            continue;
        CHECK(core_le_load_u32(section.bytes + relocation.section_offset + 12)
              >= 1);
        found_inlined = true;
    }
    CHECK(found_inlined);
    core_free(descriptor);
    n00bb_native_frames_free(frames);
    core_buffer_release(&object);
    n00bb_llvm_backend_free(backend);
    ir_storage_origin_free(origin);
    core_diagnostics_free(diagnostics);
    ir_ctx_free(context);
    return 0;
}
