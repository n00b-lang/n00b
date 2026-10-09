#include "llvm_backend.h"
#include "llvm_stack_maps.h"
#include "native_frame_maps.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

typedef struct {
    unsigned roots;
    unsigned sentinels;
    unsigned inlined_roots;
    unsigned inlined_sentinels;
    unsigned optional_roots;
    unsigned optional_sentinels;
    unsigned distinct_roots;
    unsigned distinct_sentinels;
    unsigned conservative_roots;
    unsigned conservative_sentinels;
} map_counts_t;

static bool
check_root(void *context, const n00bb_llvm_stack_location_t *location)
{
    map_counts_t *counts = context;
    const char *name = "pin_root";
    size_t length = strlen(name);
    bool pin_root = location->function.length >= length
        && !memcmp(location->function.bytes
            + location->function.length - length, name, length);
    name = "pin_inlined";
    length = strlen(name);
    bool pin_inlined = location->function.length >= length
        && !memcmp(location->function.bytes
            + location->function.length - length, name, length);
    name = "pin_optional";
    length = strlen(name);
    bool pin_optional = location->function.length >= length
        && !memcmp(location->function.bytes
            + location->function.length - length, name, length);
    name = "pin_distinct";
    length = strlen(name);
    bool pin_distinct = location->function.length >= length
        && !memcmp(location->function.bytes
            + location->function.length - length, name, length);
    name = "pin_conservative_caller";
    length = strlen(name);
    bool pin_conservative = location->function.length >= length
        && !memcmp(location->function.bytes
            + location->function.length - length, name, length);
    if (!pin_root && !pin_inlined && !pin_optional && !pin_distinct
        && !pin_conservative)
        return true;
    if (pin_root) {
        if (location->id == N00BB_LLVM_FRAME_SENTINEL_ID)
            counts->sentinels++;
        else counts->roots++;
    } else if (pin_inlined) {
        if (location->id == N00BB_LLVM_FRAME_SENTINEL_ID)
            counts->inlined_sentinels++;
        else counts->inlined_roots++;
    } else if (pin_optional && location->id == N00BB_LLVM_FRAME_SENTINEL_ID)
        counts->optional_sentinels++;
    else if (pin_optional) counts->optional_roots++;
    else if (pin_distinct) {
        if (location->id == N00BB_LLVM_FRAME_SENTINEL_ID)
            counts->distinct_sentinels++;
        else counts->distinct_roots++;
    } else if (pin_conservative) {
        if (location->id == N00BB_LLVM_FRAME_SENTINEL_ID)
            counts->conservative_sentinels++;
        else counts->conservative_roots++;
    }
    CHECK(location->size_bytes == sizeof(void *));
    CHECK(location->frame_offset < 0);
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
        {.type_text = "nil", .class = IR_CLASS_VOID},
        {.type_text = "bool", .class = IR_CLASS_BOOL, .width = 1},
        {.type_text = "uint", .class = IR_CLASS_UINT, .width = 64},
    };
    const ir_profile_t profile = {
        .name = "frame-root-test", .scalars = scalars, .n_scalars = 3,
    };
    char *target = LLVMGetDefaultTargetTriple();
    ir_module_t *comptime = ir_module_new(context, "probe$comptime",
                                         &profile, target);
    ir_module_t *runtime = ir_module_new(context, "probe", &profile, target);
    th_type *point = ir_type_parse(runtime, "Point");
    th_type *point_definition = ir_type_parse(runtime, "(x: uint, y: uint)");
    th_type *number = ir_type_parse(runtime, "uint");
    const ir_final_layout_t number_layout = {
        .intrinsic_bits = 64, .alignment_bytes = 8,
    };
    const ir_layout_bit_run_t first_run[] = {{.offset = 0, .width = 64}};
    const ir_layout_bit_run_t second_run[] = {{.offset = 64, .width = 64}};
    const ir_layout_bit_run_t occupied[] = {{.offset = 0, .width = 128}};
    const ir_final_member_layout_t members[] = {
        {.name = "x", .type = number, .ordinal = 0,
         .bit_offset = 0, .occupied = first_run, .n_occupied = 1},
        {.name = "y", .type = number, .ordinal = 1,
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
    ir_func_t *function = ir_func_add(runtime, "pin_root",
        ir_type_parse(runtime, "(ref[Point]) -> ref[Point]"), IR_LINK_INTERNAL);
    ir_value_t *parameter = function
        ? ir_param_add(function, "root", ir_type_parse(runtime, "ref[Point]"))
        : nullptr;
    ir_local_t *scalar_local = function
        ? ir_local_add(function, "scalar", number) : nullptr;
    ir_block_t *entry = function ? ir_block_add(function, "entry") : nullptr;
    ir_instr_t *copy = entry ? ir_instr_add(entry, IR_OP_COPY) : nullptr;
    ir_instr_t *bitcast = entry ? ir_instr_add(entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *copy_chain = entry ? ir_instr_add(entry, IR_OP_COPY) : nullptr;
    ir_instr_t *field = entry ? ir_instr_add(entry, IR_OP_FIELD) : nullptr;
    ir_instr_t *field_cast = entry
        ? ir_instr_add(entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *result = entry ? ir_instr_add(entry, IR_OP_RETURN) : nullptr;
    CHECK(comptime && runtime && parameter && scalar_local
        && copy && bitcast && copy_chain
        && field && field_cast && result
        && ir_instr_set_dest(copy, "copied", parameter->type)
        && ir_instr_operand_value(copy, parameter)
        && ir_instr_set_dest(bitcast, "casted", parameter->type)
        && ir_instr_operand_value(bitcast, parameter)
        && ir_instr_set_dest(copy_chain, "chain", parameter->type)
        && ir_instr_operand_value(copy_chain, bitcast->dest)
        && ir_instr_set_dest(field, "field", ir_type_parse(runtime, "ref[uint]"))
        && ir_instr_operand_value(field, parameter)
        && ir_instr_operand_name(field, "x")
        && ir_instr_set_dest(field_cast, "field.cast", field->dest->type)
        && ir_instr_operand_value(field_cast, field->dest)
        && ir_instr_operand_value(result, copy->dest));
    ir_func_t *unmanaged = ir_func_add(runtime, "pin_unmanaged",
        ir_type_parse(runtime, "(ref[uint]) -> ref[uint]"), IR_LINK_INTERNAL);
    ir_value_t *unmanaged_param = unmanaged
        ? ir_param_add(unmanaged, "root", ir_type_parse(runtime, "ref[uint]"))
        : nullptr;
    ir_block_t *unmanaged_entry = unmanaged
        ? ir_block_add(unmanaged, "entry") : nullptr;
    ir_instr_t *unmanaged_copy = unmanaged_entry
        ? ir_instr_add(unmanaged_entry, IR_OP_COPY) : nullptr;
    ir_instr_t *unmanaged_result = unmanaged_entry
        ? ir_instr_add(unmanaged_entry, IR_OP_RETURN) : nullptr;
    CHECK(unmanaged_param && unmanaged_copy && unmanaged_result
        && ir_instr_set_dest(unmanaged_copy, "copied", unmanaged_param->type)
        && ir_instr_operand_value(unmanaged_copy, unmanaged_param)
        && ir_instr_operand_value(unmanaged_result, unmanaged_copy->dest));
    ir_func_t *pointer_local = ir_func_add(runtime, "pin_pointer_local",
        ir_type_parse(runtime, "(ref[Point]) -> ref[Point]"), IR_LINK_INTERNAL);
    ir_value_t *local_param = pointer_local
        ? ir_param_add(pointer_local, "root", parameter->type) : nullptr;
    ir_local_t *local_root = pointer_local
        ? ir_local_add(pointer_local, "local.root", parameter->type) : nullptr;
    ir_block_t *local_entry = pointer_local
        ? ir_block_add(pointer_local, "entry") : nullptr;
    ir_instr_t *local_return = local_entry
        ? ir_instr_add(local_entry, IR_OP_RETURN) : nullptr;
    CHECK(local_param && local_root && local_return
        && ir_instr_operand_value(local_return, local_param));
    ir_func_t *opaque_local = ir_func_add(runtime, "pin_opaque_local",
        ir_type_parse(runtime, "(ref[Point]) -> ref[Point]"), IR_LINK_INTERNAL);
    ir_value_t *opaque_param = opaque_local
        ? ir_param_add(opaque_local, "root", parameter->type) : nullptr;
    ir_local_t *opaque_root = opaque_local
        ? ir_local_add(opaque_local, "local.option",
            ir_type_parse(runtime, "option[ref[Point]]")) : nullptr;
    ir_block_t *opaque_entry = opaque_local
        ? ir_block_add(opaque_local, "entry") : nullptr;
    ir_instr_t *opaque_return = opaque_entry
        ? ir_instr_add(opaque_entry, IR_OP_RETURN) : nullptr;
    CHECK(opaque_param && opaque_root && opaque_return
        && ir_instr_operand_value(opaque_return, opaque_param));
    ir_func_t *inline_helper = ir_func_add(runtime, "pin_inline_helper",
        ir_type_parse(runtime, "(ref[Point]) -> ref[Point]"), IR_LINK_INTERNAL);
    ir_value_t *helper_root = inline_helper
        ? ir_param_add(inline_helper, "root", parameter->type) : nullptr;
    ir_block_t *helper_entry = inline_helper
        ? ir_block_add(inline_helper, "entry") : nullptr;
    ir_instr_t *helper_return = helper_entry
        ? ir_instr_add(helper_entry, IR_OP_RETURN) : nullptr;
    CHECK(helper_root && helper_return
        && ir_instr_operand_value(helper_return, helper_root)
        && ir_func_set_always_inline(context, inline_helper, true));
    ir_func_t *inlined = ir_func_add(runtime, "pin_inlined",
        ir_type_parse(runtime, "(bool, ref[Point]) -> ref[Point]"),
        IR_LINK_INTERNAL);
    ir_value_t *condition = inlined
        ? ir_param_add(inlined, "condition", ir_type_parse(runtime, "bool"))
        : nullptr;
    ir_value_t *caller_root = inlined
        ? ir_param_add(inlined, "root", parameter->type) : nullptr;
    ir_block_t *caller_entry = inlined
        ? ir_block_add(inlined, "entry") : nullptr;
    ir_block_t *then_block = inlined
        ? ir_block_add(inlined, "then") : nullptr;
    ir_block_t *else_block = inlined
        ? ir_block_add(inlined, "else") : nullptr;
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
        && ir_instr_set_dest(inline_call, "called", parameter->type)
        && ir_instr_operand_value(inline_call,
            ir_func_address(runtime, inline_helper))
        && ir_instr_operand_value(inline_call, caller_root)
        && ir_instr_set_call_signature(inline_call, inline_helper->type)
        && ir_instr_operand_value(then_return, inline_call->dest)
        && ir_instr_operand_value(else_return, caller_root));
    th_type *optional_type = ir_type_parse(runtime, "option[ref[Point]]");
    ir_func_t *optional = ir_func_add(runtime, "pin_optional",
        ir_type_parse(runtime,
            "(option[ref[Point]]) -> option[ref[Point]]"),
        IR_LINK_INTERNAL);
    ir_value_t *optional_param = optional
        ? ir_param_add(optional, "root", optional_type) : nullptr;
    ir_block_t *optional_entry = optional
        ? ir_block_add(optional, "entry") : nullptr;
    ir_instr_t *optional_return = optional_entry
        ? ir_instr_add(optional_entry, IR_OP_RETURN) : nullptr;
    CHECK(optional_param && optional_return
        && ir_instr_operand_value(optional_return, optional_param));
    ir_func_t *distinct = ir_func_add(runtime, "pin_distinct",
        ir_type_parse(runtime, "(ref[Point], ref[Point]) -> ref[Point]"),
        IR_LINK_INTERNAL);
    ir_value_t *distinct_first = distinct
        ? ir_param_add(distinct, "first", parameter->type) : nullptr;
    ir_value_t *distinct_second = distinct
        ? ir_param_add(distinct, "second", parameter->type) : nullptr;
    ir_block_t *distinct_entry = distinct
        ? ir_block_add(distinct, "entry") : nullptr;
    ir_instr_t *distinct_return = distinct_entry
        ? ir_instr_add(distinct_entry, IR_OP_RETURN) : nullptr;
    CHECK(distinct_first && distinct_second && distinct_return
        && ir_instr_operand_value(distinct_return, distinct_first));
    ir_func_t *conservative_helper = ir_func_add(runtime,
        "pin_conservative_helper", inline_helper->type, IR_LINK_INTERNAL);
    ir_value_t *conservative_input = conservative_helper
        ? ir_param_add(conservative_helper, "root", parameter->type) : nullptr;
    ir_local_t *conservative_local = conservative_helper
        ? ir_local_add(conservative_helper, "aggregate", point) : nullptr;
    ir_block_t *conservative_entry = conservative_helper
        ? ir_block_add(conservative_helper, "entry") : nullptr;
    ir_instr_t *conservative_return = conservative_entry
        ? ir_instr_add(conservative_entry, IR_OP_RETURN) : nullptr;
    CHECK(conservative_input && conservative_local && conservative_return
        && ir_instr_operand_value(conservative_return, conservative_input)
        && ir_func_set_always_inline(context, conservative_helper, true));
    ir_func_t *conservative_caller = ir_func_add(runtime,
        "pin_conservative_caller", inline_helper->type, IR_LINK_INTERNAL);
    ir_value_t *caller_input = conservative_caller
        ? ir_param_add(conservative_caller, "root", parameter->type) : nullptr;
    ir_block_t *conservative_caller_entry = conservative_caller
        ? ir_block_add(conservative_caller, "entry") : nullptr;
    ir_instr_t *conservative_call = conservative_caller_entry
        ? ir_instr_add(conservative_caller_entry, IR_OP_CALL) : nullptr;
    ir_instr_t *conservative_caller_return = conservative_caller_entry
        ? ir_instr_add(conservative_caller_entry, IR_OP_RETURN) : nullptr;
    CHECK(caller_input && conservative_call && conservative_caller_return
        && ir_instr_set_dest(conservative_call, "result", parameter->type)
        && ir_instr_operand_value(conservative_call,
            ir_func_address(runtime, conservative_helper))
        && ir_instr_operand_value(conservative_call, caller_input)
        && ir_instr_set_call_signature(conservative_call,
            conservative_helper->type)
        && ir_instr_operand_value(conservative_caller_return,
            conservative_call->dest));
    ir_func_t *reachable[] = {function, unmanaged, pointer_local, opaque_local,
                              inline_helper, inlined, optional, distinct,
                              conservative_helper, conservative_caller};
    ir_storage_origin_t *origin = ir_storage_origin_new(reachable, 10,
        no_mapping_source, unmanaged_parameter, nullptr, false,
        unmanaged_param);
    CHECK(origin);
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
        comptime, runtime, true, false, diagnostics);
    CHECK(backend && n00bb_llvm_backend_bind_storage_origin(backend, origin)
        && n00bb_llvm_backend_enable_frame_maps(backend));
    core_buffer_t object = {0};
    CHECK(n00bb_llvm_backend_emit_object(backend, false, nullptr, 0,
        nullptr, 0, 0, &object, diagnostics));
#if defined(__APPLE__)
    core_target_object_format_t format = CORE_TARGET_OBJECT_MACH_O;
#elif defined(_WIN32)
    core_target_object_format_t format = CORE_TARGET_OBJECT_COFF;
#else
    core_target_object_format_t format = CORE_TARGET_OBJECT_ELF;
#endif
    map_counts_t counts = {0};
    bool found = false;
    CHECK(n00bb_llvm_stack_maps_visit(object.data, object.byte_len, format,
                                      check_root, &counts, &found));
    CHECK(found && counts.roots == 1 && counts.sentinels == 1
        && counts.inlined_roots >= 2 && counts.inlined_sentinels == 1
        && counts.optional_roots == 1 && counts.optional_sentinels == 1
        && counts.distinct_roots == 2 && counts.distinct_sentinels == 1
        && counts.conservative_roots == 0
        && counts.conservative_sentinels == 0);
    core_target_desc_t native_target;
#if defined(__APPLE__) && defined(__aarch64__)
    CHECK(core_target_profile("arm64-macos", 0, &native_target));
#elif defined(_WIN32)
    CHECK(core_target_profile("x86_64-windows", 0, &native_target));
#else
    CHECK(core_target_profile("x86_64-linux", 0, &native_target));
#endif
    n00bb_native_frames_t *frames = n00bb_native_frames_new(format);
    CHECK(frames && n00bb_native_frames_add(frames, object.data,
                                            object.byte_len));
    uint8_t *descriptor = NULL;
    size_t descriptor_bytes = 0;
    CHECK(n00bb_native_frames_emit(frames, &native_target, &descriptor,
                                   &descriptor_bytes));
    core_object_section_t section;
    CHECK(core_object_section_find(descriptor, descriptor_bytes, format,
        format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : NULL,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_frames"
            : format == CORE_TARGET_OBJECT_COFF ? ".n00b$F"
                                            : ".data.rel.ro.n00b_frames",
        &section));
    CHECK(core_le_load_u32(section.bytes) >= 3);
    CHECK(core_le_load_u32(section.bytes + 4) >= 3);
    CHECK(section.relocation_count == core_le_load_u32(section.bytes));
    CHECK((int32_t)core_le_load_u32(section.bytes + 16) < 0);
    CHECK(core_le_load_u32(section.bytes + 20) == 1);
    core_object_absolute64_relocation_t relocation;
    CHECK(core_object_absolute64_relocation_at(&section, format, 0,
                                               &relocation));
    CHECK(relocation.section_offset == 8);
    core_object_symbol_name_t symbol;
    CHECK(core_object_symbol_name_at(descriptor, descriptor_bytes, format,
        &section, relocation.native_symbol_index, &symbol));
    CHECK(symbol.length >= strlen("pin_root"));
    CHECK(!memcmp(symbol.bytes + symbol.length - strlen("pin_root"),
                  "pin_root", strlen("pin_root")));
    bool found_inlined = false;
    for (uint32_t i = 0; i < section.relocation_count; i++) {
        CHECK(core_object_absolute64_relocation_at(&section, format, i,
                                                   &relocation));
        CHECK(core_object_symbol_name_at(descriptor, descriptor_bytes,
            format, &section, relocation.native_symbol_index, &symbol));
        const char name[] = "pin_inlined";
        if (symbol.length < sizeof(name) - 1
            || memcmp(symbol.bytes + symbol.length - sizeof(name) + 1,
                      name, sizeof(name) - 1)) continue;
        CHECK(core_le_load_u32(section.bytes + relocation.section_offset + 12)
              == 1);
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
    LLVMDisposeMessage(target);
    return 0;
}
