#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/storage_origin.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    const ir_value_t *raw;
    const ir_value_t *unmanaged_parameter;
    bool closed;
} fixture_t;

static bool raw_source(const ir_value_t *value, void *context)
{
    return value == ((fixture_t *)context)->raw;
}

static bool unmanaged_parameter(const ir_value_t *value, void *context)
{
    return value == ((fixture_t *)context)->unmanaged_parameter;
}

static bool closed_callers(const ir_func_t *function, void *context)
{
    (void)function;
    return ((fixture_t *)context)->closed;
}

int main(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("origin"), &ir_profile_bce,
                                         S("test"));
    th_type *reference = ir_type_parse(module, S("ref[byte]"));
    th_type *object = ir_type_parse(module, S("ref[(next: ref[byte])]"));
    th_type *field_reference = ir_type_parse(module, S("ref[ref[byte]]"));
    th_type *array_reference = ir_type_parse(module,
        S("ref[array[ref[byte]]]"));
    th_type *inline_record = ir_type_parse(module,
        S("(items: array[ref[byte]])"));
    th_type *inline_reference = ir_type_parse(module,
        S("ref[(items: array[ref[byte]])]"));
    ir_layout_bit_run_t inline_bits = {.offset = 0, .width = 256};
    ir_final_member_layout_t inline_member = {
        .name = S("items"), .type = th_ind_referent(array_reference),
        .inline_element_type = reference,
        .inline_element_count = 4, .inline_element_stride_bits = 64,
        .occupied = &inline_bits, .n_occupied = 1,
    };
    ir_final_layout_t inline_layout = {
        .intrinsic_bits = 256, .alignment_bytes = 8,
        .occupied = &inline_bits, .n_occupied = 1,
        .members = &inline_member, .n_members = 1,
    };
    bool inline_layout_ready = ir_layout_publish_final(module,
        inline_record, ir_module_target(module), &inline_layout);
    th_type *optional = ir_type_parse(module, S("Option[ref[byte]]"));
    ir_func_t *map = ir_func_add(module, S("map"),
        ir_type_parse(module, S("() -> ref[byte]")), IR_LINK_EXTERN);
    ir_func_t *make = ir_func_add(module, S("make"),
        ir_type_parse(module, S("() -> ref[(next: ref[byte])]")),
        IR_LINK_INTERNAL);
    ir_block_t *entry = ir_block_add(make, S("entry"));
    ir_local_t *mapped = ir_local_add(make, S("mapped"), reference);
    ir_instr_t *raw = ir_instr_add(entry, IR_OP_CALL);
    ir_instr_set_dest(raw, S("raw"), reference);
    ir_instr_operand_value(raw, ir_func_address(module, map));
    ir_instr_t *bind = ir_instr_add(entry, IR_OP_COPY);
    ir_instr_set_dest_local(bind, mapped);
    ir_instr_operand_value(bind, raw->dest);
    ir_instr_t *some = ir_instr_add(entry, IR_OP_OPTION_SOME);
    ir_instr_set_dest(some, S("some"), optional);
    ir_instr_operand_value(some, ir_local_value(make, mapped));
    ir_instr_t *get = ir_instr_add(entry, IR_OP_OPTION_GET);
    ir_instr_set_dest(get, S("get"), reference);
    ir_instr_operand_value(get, some->dest);
    ir_instr_t *typed = ir_instr_add(entry, IR_OP_BITCAST);
    ir_instr_set_dest(typed, S("typed"), object);
    ir_instr_operand_value(typed, get->dest);
    ir_instr_t *field = ir_instr_add(entry, IR_OP_FIELD);
    ir_instr_set_dest(field, S("field"), field_reference);
    ir_instr_operand_value(field, typed->dest);
    ir_instr_operand_name(field, S("next"));
    ir_instr_t *zero = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(zero, S("zero"), ir_type_parse(module, S("uint")));
    ir_instr_operand_uint(zero, 0);
    ir_instr_t *typed_array = ir_instr_add(entry, IR_OP_BITCAST);
    ir_instr_set_dest(typed_array, S("typed_array"), array_reference);
    ir_instr_operand_value(typed_array, raw->dest);
    ir_instr_t *indexed = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(indexed, S("indexed"), field_reference);
    ir_instr_operand_value(indexed, typed_array->dest);
    ir_instr_operand_value(indexed, zero->dest);
    ir_instr_t *inline_object = ir_instr_add(entry, IR_OP_BITCAST);
    ir_instr_set_dest(inline_object, S("inline_object"), inline_reference);
    ir_instr_operand_value(inline_object, raw->dest);
    ir_instr_t *inline_field = ir_instr_add(entry, IR_OP_FIELD);
    ir_instr_set_dest(inline_field, S("inline_field"), array_reference);
    ir_instr_operand_value(inline_field, inline_object->dest);
    ir_instr_operand_name(inline_field, S("items"));
    ir_instr_t *inline_index = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(inline_index, S("inline_index"), field_reference);
    ir_instr_operand_value(inline_index, inline_field->dest);
    ir_instr_operand_value(inline_index, zero->dest);
    ir_instr_t *offset = ir_instr_add(entry, IR_OP_OFFSET);
    ir_instr_set_dest(offset, S("offset"), object);
    ir_instr_operand_value(offset, typed->dest);
    ir_instr_operand_value(offset, zero->dest);
    ir_instr_t *raw_offset = ir_instr_add(entry, IR_OP_OFFSET);
    ir_instr_set_dest(raw_offset, S("raw_offset"), reference);
    ir_instr_operand_value(raw_offset, raw->dest);
    ir_instr_operand_value(raw_offset, zero->dest);
    ir_instr_t *cast_offset = ir_instr_add(entry, IR_OP_BITCAST);
    ir_instr_set_dest(cast_offset, S("cast_offset"), object);
    ir_instr_operand_value(cast_offset, raw_offset->dest);
    ir_instr_t *managed = ir_instr_add(entry, IR_OP_ALLOC);
    ir_instr_set_dest(managed, S("managed"), object);
    ir_instr_operand_type(managed, th_ind_referent(object));
    ir_instr_t *managed_inline = ir_instr_add(entry, IR_OP_ALLOC);
    ir_instr_set_dest(managed_inline, S("managed_inline"), inline_reference);
    ir_instr_operand_type(managed_inline, inline_record);
    ir_instr_t *managed_array = ir_instr_add(entry, IR_OP_FIELD);
    ir_instr_set_dest(managed_array, S("managed_array"), array_reference);
    ir_instr_operand_value(managed_array, managed_inline->dest);
    ir_instr_operand_name(managed_array, S("items"));
    ir_instr_t *managed_index = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(managed_index, S("managed_index"), field_reference);
    ir_instr_operand_value(managed_index, managed_array->dest);
    ir_instr_operand_value(managed_index, zero->dest);
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, typed->dest);

    ir_func_t *use = ir_func_add(module, S("use"),
        ir_type_parse(module, S("(ref[(next: ref[byte])]) -> nil")),
        IR_LINK_EXTERNAL);
    ir_value_t *argument = ir_param_add(use, S("object"), object);
    ir_block_t *use_block = ir_block_add(use, S("entry"));
    ir_instr_t *parameter_field = ir_instr_add(use_block, IR_OP_FIELD);
    ir_instr_set_dest(parameter_field, S("field"), field_reference);
    ir_instr_operand_value(parameter_field, argument);
    ir_instr_operand_name(parameter_field, S("next"));
    ir_instr_add(use_block, IR_OP_RETURN);

    ir_func_t *caller = ir_func_add(module, S("caller"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *caller_block = ir_block_add(caller, S("entry"));
    ir_instr_t *invoke_make = ir_instr_add(caller_block, IR_OP_CALL);
    ir_instr_set_dest(invoke_make, S("made"), object);
    ir_instr_operand_value(invoke_make, ir_func_address(module, make));
    ir_instr_t *invoke_use = ir_instr_add(caller_block, IR_OP_CALL);
    ir_instr_set_dest(invoke_use, S("used"), ir_type_parse(module, S("nil")));
    ir_instr_operand_value(invoke_use, ir_func_address(module, use));
    ir_instr_operand_value(invoke_use, invoke_make->dest);
    ir_instr_add(caller_block, IR_OP_RETURN);

    ir_func_t *functions[] = {make, use, caller};
    fixture_t fixture = {.raw = raw->dest, .closed = true};
    ir_storage_origin_t *origin = ir_storage_origin_new(functions, 3,
        raw_source, unmanaged_parameter, closed_callers, true, &fixture);
    check("raw mapping is not a managed-heap exclusion",
          origin && !ir_storage_origin_unmanaged(origin, raw->dest));
    check("typed object at mapping base is outside the heap",
          origin && ir_storage_origin_unmanaged(origin, typed->dest));
    check("field storage follows the typed object",
          origin && ir_storage_origin_unmanaged(origin, field->dest));
    check("inline array layout is published", inline_layout_ready);
    check("indexed inline storage follows the typed unmanaged object",
          origin && ir_storage_origin_unmanaged(origin, inline_index->dest));
    check("dynamic array indexing can enter managed backing storage",
          origin && !ir_storage_origin_unmanaged(origin, indexed->dest));
    check("an offset can enter a managed payload",
          origin && !ir_storage_origin_unmanaged(origin, offset->dest));
    check("offsetting before the cast cannot prove a base object",
          origin && !ir_storage_origin_unmanaged(origin, cast_offset->dest));
    check("managed allocation is not an unmanaged origin",
          origin && !ir_storage_origin_unmanaged(origin, managed->dest));
    check("indexed managed storage keeps its barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 managed_index->dest));
    check("closed callsites carry provenance through parameters",
          origin && ir_storage_origin_unmanaged(origin,
                                                parameter_field->dest));
    ir_storage_origin_free(origin);

    fixture.closed = false;
    origin = ir_storage_origin_new(functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("unknown callers keep the barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 parameter_field->dest));
    ir_storage_origin_free(origin);

    fixture.unmanaged_parameter = argument;
    origin = ir_storage_origin_new(functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("externally supplied unmanaged parameters retain provenance",
          origin && ir_storage_origin_unmanaged(origin,
                                               parameter_field->dest));
    ir_storage_origin_free(origin);
    fixture.unmanaged_parameter = nullptr;

    fixture.closed = true;
    ir_func_t *second_caller = ir_func_add(module, S("second_caller"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *second_block = ir_block_add(second_caller, S("entry"));
    ir_instr_t *second_alloc = ir_instr_add(second_block, IR_OP_ALLOC);
    ir_instr_set_dest(second_alloc, S("managed"), object);
    ir_instr_operand_type(second_alloc, th_ind_referent(object));
    ir_instr_t *second_call = ir_instr_add(second_block, IR_OP_CALL);
    ir_instr_set_dest(second_call, S("used"), ir_type_parse(module, S("nil")));
    ir_instr_operand_value(second_call, ir_func_address(module, use));
    ir_instr_operand_value(second_call, second_alloc->dest);
    ir_instr_add(second_block, IR_OP_RETURN);
    ir_func_t *with_managed_caller[] = {make, use, caller, second_caller};
    origin = ir_storage_origin_new(with_managed_caller, 4, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("a managed callsite keeps the barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 parameter_field->dest));
    ir_storage_origin_free(origin);

    ir_func_t *escaper = ir_func_add(module, S("escaper"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *escape_block = ir_block_add(escaper, S("entry"));
    ir_value_t *use_address = ir_func_address(module, use);
    ir_instr_t *escape = ir_instr_add(escape_block, IR_OP_COPY);
    ir_instr_set_dest(escape, S("escaped"), use_address->type);
    ir_instr_operand_value(escape, use_address);
    ir_instr_add(escape_block, IR_OP_RETURN);
    ir_func_t *with_escaper[] = {make, use, caller, escaper};
    origin = ir_storage_origin_new(with_escaper, 4, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("an indirect caller keeps the barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 parameter_field->dest));
    ir_storage_origin_free(origin);

    th_type *optional_object = ir_type_parse(module,
        S("Option[ref[(next: ref[byte])]]"));
    th_type *global_address_type = ir_type_parse(module,
        S("ref[Option[ref[(next: ref[byte])]]]"));
    ir_global_t *global = ir_global_add(module, S("mapped_holder"),
                                       optional_object);
    ir_global_init_value_t nil_init = {.kind = IR_GLOBAL_INIT_NIL};
    bool initialized = global && ir_global_set_static_initializer(module,
        global, &nil_init);
    ir_func_t *global_user = ir_func_add(module, S("global_user"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *global_block = ir_block_add(global_user, S("entry"));
    ir_instr_t *global_address = ir_instr_add(global_block,
                                             IR_OP_ADDRESS_OF);
    ir_instr_set_dest(global_address, S("holder_address"),
                      global_address_type);
    ir_instr_operand_value(global_address, ir_global_address(module, global));
    ir_instr_t *mapped_call = ir_instr_add(global_block, IR_OP_CALL);
    ir_instr_set_dest(mapped_call, S("mapped"), object);
    ir_instr_operand_value(mapped_call, ir_func_address(module, make));
    ir_instr_t *mapped_some = ir_instr_add(global_block, IR_OP_OPTION_SOME);
    ir_instr_set_dest(mapped_some, S("mapped_some"), optional_object);
    ir_instr_operand_value(mapped_some, mapped_call->dest);
    ir_instr_t *global_store = ir_instr_add(global_block, IR_OP_STORE);
    ir_instr_add_mod(global_store, S("atomic"));
    ir_instr_operand_value(global_store, global_address->dest);
    ir_instr_operand_value(global_store, mapped_some->dest);
    ir_instr_t *global_load = ir_instr_add(global_block, IR_OP_LOAD);
    ir_instr_add_mod(global_load, S("atomic"));
    ir_instr_set_dest(global_load, S("loaded"), optional_object);
    ir_instr_operand_value(global_load, global_address->dest);
    ir_instr_t *self_store = ir_instr_add(global_block, IR_OP_STORE);
    ir_instr_add_mod(self_store, S("atomic"));
    ir_instr_operand_value(self_store, global_address->dest);
    ir_instr_operand_value(self_store, global_load->dest);
    ir_instr_t *global_get = ir_instr_add(global_block, IR_OP_OPTION_GET);
    ir_instr_set_dest(global_get, S("object"), object);
    ir_instr_operand_value(global_get, global_load->dest);
    ir_instr_t *global_field = ir_instr_add(global_block, IR_OP_FIELD);
    ir_instr_set_dest(global_field, S("field"), field_reference);
    ir_instr_operand_value(global_field, global_get->dest);
    ir_instr_operand_name(global_field, S("next"));
    ir_instr_add(global_block, IR_OP_RETURN);
    ir_func_t *global_functions[] = {make, global_user};
    origin = ir_storage_origin_new(global_functions, 2, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("nil-initialized atomic global retains mapped origin across self-copy",
          initialized && origin
              && ir_storage_origin_unmanaged(origin, global_field->dest));
    ir_storage_origin_free(origin);

    ir_func_t *managed_writer = ir_func_add(module, S("managed_writer"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *managed_block = ir_block_add(managed_writer, S("entry"));
    ir_instr_t *managed_address = ir_instr_add(managed_block,
                                              IR_OP_ADDRESS_OF);
    ir_instr_set_dest(managed_address, S("holder_address"),
                      global_address_type);
    ir_instr_operand_value(managed_address,
                           ir_global_address(module, global));
    ir_instr_t *managed_object = ir_instr_add(managed_block, IR_OP_ALLOC);
    ir_instr_set_dest(managed_object, S("object"), object);
    ir_instr_operand_type(managed_object, th_ind_referent(object));
    ir_instr_t *managed_some = ir_instr_add(managed_block, IR_OP_OPTION_SOME);
    ir_instr_set_dest(managed_some, S("some"), optional_object);
    ir_instr_operand_value(managed_some, managed_object->dest);
    ir_instr_t *managed_store = ir_instr_add(managed_block, IR_OP_STORE);
    ir_instr_operand_value(managed_store, managed_address->dest);
    ir_instr_operand_value(managed_store, managed_some->dest);
    ir_instr_add(managed_block, IR_OP_RETURN);
    ir_func_t *managed_global_functions[] = {make, global_user,
                                             managed_writer};
    origin = ir_storage_origin_new(managed_global_functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("managed store into global retains barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 global_field->dest));
    ir_storage_origin_free(origin);

    ir_func_t *address_escaper = ir_func_add(module, S("address_escaper"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *escape_global_block = ir_block_add(address_escaper,
                                                   S("entry"));
    ir_instr_t *escaping_address = ir_instr_add(escape_global_block,
                                               IR_OP_ADDRESS_OF);
    ir_instr_set_dest(escaping_address, S("holder_address"),
                      global_address_type);
    ir_instr_operand_value(escaping_address,
                           ir_global_address(module, global));
    ir_instr_t *address_copy = ir_instr_add(escape_global_block, IR_OP_COPY);
    ir_instr_set_dest(address_copy, S("escaped_address"),
                      global_address_type);
    ir_instr_operand_value(address_copy, escaping_address->dest);
    ir_instr_add(escape_global_block, IR_OP_RETURN);
    ir_func_t *escaped_global_functions[] = {make, global_user,
                                             address_escaper};
    origin = ir_storage_origin_new(escaped_global_functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("escaped global address retains barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 global_field->dest));
    ir_storage_origin_free(origin);

    ir_func_t *field_reader = ir_func_add(module, S("field_reader"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *field_block = ir_block_add(field_reader, S("entry"));
    ir_instr_t *field_holder = ir_instr_add(field_block, IR_OP_CALL);
    ir_instr_set_dest(field_holder, S("holder"), object);
    ir_instr_operand_value(field_holder, ir_func_address(module, make));
    ir_instr_t *field_source = ir_instr_add(field_block, IR_OP_BITCAST);
    ir_instr_set_dest(field_source, S("source"), reference);
    ir_instr_operand_value(field_source, field_holder->dest);
    ir_instr_t *content_address = ir_instr_add(field_block, IR_OP_FIELD);
    ir_instr_set_dest(content_address, S("content_address"), field_reference);
    ir_instr_operand_value(content_address, field_holder->dest);
    ir_instr_operand_name(content_address, S("next"));
    ir_instr_t *field_store = ir_instr_add(field_block, IR_OP_STORE);
    ir_instr_operand_value(field_store, content_address->dest);
    ir_instr_operand_value(field_store, field_source->dest);
    ir_instr_t *field_load = ir_instr_add(field_block, IR_OP_LOAD);
    ir_instr_set_dest(field_load, S("loaded_content"), reference);
    ir_instr_operand_value(field_load, content_address->dest);
    ir_instr_add(field_block, IR_OP_RETURN);
    ir_func_t *field_functions[] = {make, field_reader};
    origin = ir_storage_origin_new(field_functions, 2, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("mapped field content follows its only visible writer",
          origin && ir_storage_origin_unmanaged(origin, field_load->dest));
    ir_storage_origin_free(origin);
    origin = ir_storage_origin_new(field_functions, 2, raw_source,
                                   unmanaged_parameter, closed_callers, false,
                                   &fixture);
    check("unknown native field writers retain barrier",
          origin && !ir_storage_origin_unmanaged(origin, field_load->dest));
    ir_storage_origin_free(origin);

    ir_func_t *managed_field_writer = ir_func_add(module,
        S("managed_field_writer"), ir_type_parse(module, S("() -> nil")),
        IR_LINK_INTERNAL);
    ir_block_t *managed_field_block = ir_block_add(managed_field_writer,
                                                   S("entry"));
    ir_instr_t *managed_holder = ir_instr_add(managed_field_block,
                                             IR_OP_ALLOC);
    ir_instr_set_dest(managed_holder, S("holder"), object);
    ir_instr_operand_type(managed_holder, th_ind_referent(object));
    ir_instr_t *managed_content = ir_instr_add(managed_field_block,
                                              IR_OP_ALLOC);
    ir_instr_set_dest(managed_content, S("content"), reference);
    ir_instr_operand_type(managed_content, th_ind_referent(reference));
    ir_instr_t *managed_field_address = ir_instr_add(managed_field_block,
                                                    IR_OP_FIELD);
    ir_instr_set_dest(managed_field_address, S("content_address"),
                      field_reference);
    ir_instr_operand_value(managed_field_address, managed_holder->dest);
    ir_instr_operand_name(managed_field_address, S("next"));
    ir_instr_t *managed_field_store = ir_instr_add(managed_field_block,
                                                  IR_OP_STORE);
    ir_instr_operand_value(managed_field_store, managed_field_address->dest);
    ir_instr_operand_value(managed_field_store, managed_content->dest);
    ir_instr_add(managed_field_block, IR_OP_RETURN);
    ir_func_t *mixed_field_functions[] = {make, field_reader,
                                          managed_field_writer};
    origin = ir_storage_origin_new(mixed_field_functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("managed field writer retains barrier",
          origin && !ir_storage_origin_unmanaged(origin, field_load->dest));
    ir_storage_origin_free(origin);

    ir_func_t *field_escaper = ir_func_add(module, S("field_escaper"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *field_escape_block = ir_block_add(field_escaper, S("entry"));
    ir_instr_t *escaped_holder = ir_instr_add(field_escape_block, IR_OP_CALL);
    ir_instr_set_dest(escaped_holder, S("holder"), object);
    ir_instr_operand_value(escaped_holder, ir_func_address(module, make));
    ir_instr_t *escaped_field = ir_instr_add(field_escape_block, IR_OP_FIELD);
    ir_instr_set_dest(escaped_field, S("field_address"), field_reference);
    ir_instr_operand_value(escaped_field, escaped_holder->dest);
    ir_instr_operand_name(escaped_field, S("next"));
    ir_instr_t *field_address_copy = ir_instr_add(field_escape_block,
                                                 IR_OP_COPY);
    ir_instr_set_dest(field_address_copy, S("escaped"), field_reference);
    ir_instr_operand_value(field_address_copy, escaped_field->dest);
    ir_instr_add(field_escape_block, IR_OP_RETURN);
    ir_func_t *escaped_field_functions[] = {make, field_reader, field_escaper};
    origin = ir_storage_origin_new(escaped_field_functions, 3, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("escaped field address retains barrier",
          origin && !ir_storage_origin_unmanaged(origin, field_load->dest));
    ir_storage_origin_free(origin);

    ir_func_t *recursive_a = ir_func_add(module, S("recursive_a"),
        ir_type_parse(module, S("(ref[(next: ref[byte])]) -> nil")),
        IR_LINK_INTERNAL);
    ir_func_t *recursive_b = ir_func_add(module, S("recursive_b"),
        ir_type_parse(module, S("(ref[(next: ref[byte])]) -> nil")),
        IR_LINK_INTERNAL);
    ir_value_t *recursive_a_arg = ir_param_add(recursive_a, S("object"),
                                               object);
    ir_value_t *recursive_b_arg = ir_param_add(recursive_b, S("object"),
                                               object);
    ir_block_t *recursive_a_block = ir_block_add(recursive_a, S("entry"));
    ir_instr_t *recursive_field = ir_instr_add(recursive_a_block,
                                              IR_OP_FIELD);
    ir_instr_set_dest(recursive_field, S("field"), field_reference);
    ir_instr_operand_value(recursive_field, recursive_a_arg);
    ir_instr_operand_name(recursive_field, S("next"));
    ir_instr_t *call_b = ir_instr_add(recursive_a_block, IR_OP_CALL);
    ir_instr_set_dest(call_b, S("called"), ir_type_parse(module, S("nil")));
    ir_instr_operand_value(call_b, ir_func_address(module, recursive_b));
    ir_instr_operand_value(call_b, recursive_a_arg);
    ir_instr_add(recursive_a_block, IR_OP_RETURN);
    ir_block_t *recursive_b_block = ir_block_add(recursive_b, S("entry"));
    ir_instr_t *call_a = ir_instr_add(recursive_b_block, IR_OP_CALL);
    ir_instr_set_dest(call_a, S("called"), ir_type_parse(module, S("nil")));
    ir_instr_operand_value(call_a, ir_func_address(module, recursive_a));
    ir_instr_operand_value(call_a, recursive_b_arg);
    ir_instr_add(recursive_b_block, IR_OP_RETURN);
    ir_func_t *mapped_root = ir_func_add(module, S("mapped_root"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *mapped_root_block = ir_block_add(mapped_root, S("entry"));
    ir_instr_t *mapped_root_make = ir_instr_add(mapped_root_block,
                                               IR_OP_CALL);
    ir_instr_set_dest(mapped_root_make, S("mapped"), object);
    ir_instr_operand_value(mapped_root_make, ir_func_address(module, make));
    ir_instr_t *mapped_root_call = ir_instr_add(mapped_root_block, IR_OP_CALL);
    ir_instr_set_dest(mapped_root_call, S("called"),
                      ir_type_parse(module, S("nil")));
    ir_instr_operand_value(mapped_root_call,
                           ir_func_address(module, recursive_a));
    ir_instr_operand_value(mapped_root_call, mapped_root_make->dest);
    ir_instr_add(mapped_root_block, IR_OP_RETURN);
    ir_func_t *recursive_functions[] = {make, recursive_a, recursive_b,
                                        mapped_root};
    origin = ir_storage_origin_new(recursive_functions, 4, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("closed recursive callers retain mapped parameter origin",
          origin && ir_storage_origin_unmanaged(origin,
                                                recursive_field->dest));
    ir_storage_origin_free(origin);

    ir_func_t *managed_root = ir_func_add(module, S("managed_root"),
        ir_type_parse(module, S("() -> nil")), IR_LINK_INTERNAL);
    ir_block_t *managed_root_block = ir_block_add(managed_root, S("entry"));
    ir_instr_t *managed_root_alloc = ir_instr_add(managed_root_block,
                                                 IR_OP_ALLOC);
    ir_instr_set_dest(managed_root_alloc, S("managed"), object);
    ir_instr_operand_type(managed_root_alloc, th_ind_referent(object));
    ir_instr_t *managed_root_call = ir_instr_add(managed_root_block,
                                                IR_OP_CALL);
    ir_instr_set_dest(managed_root_call, S("called"),
                      ir_type_parse(module, S("nil")));
    ir_instr_operand_value(managed_root_call,
                           ir_func_address(module, recursive_a));
    ir_instr_operand_value(managed_root_call, managed_root_alloc->dest);
    ir_instr_add(managed_root_block, IR_OP_RETURN);
    ir_func_t *mixed_recursive_functions[] = {make, recursive_a, recursive_b,
                                              mapped_root, managed_root};
    origin = ir_storage_origin_new(mixed_recursive_functions, 5, raw_source,
                                   unmanaged_parameter, closed_callers, true,
                                   &fixture);
    check("managed caller in a recursive cycle retains barrier",
          origin && !ir_storage_origin_unmanaged(origin,
                                                 recursive_field->dest));
    ir_storage_origin_free(origin);
    ir_ctx_free(ctx);
    return report("IR storage origin");
}
