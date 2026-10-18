#include <string.h>

#include "private/ir/internal.h"
#include "ir/archive.h"
#include "ir/shape.h"
#include "lib/alloc.h"

#define IR_ARCHIVE_MAGIC   UINT32_C(0x4e30494c)
#define IR_ARCHIVE_VERSION UINT32_C(1)

enum {
    REF_NONE,
    REF_PARAM,
    REF_INSTR,
    REF_LOCAL,
    REF_FUNC,
    REF_GLOBAL,
    REF_FOREIGN,
};

typedef struct {
    core_buffer_t     *out;
    th_type_writer_t  *types;
    ir_func_t         *function;
    bool               failed;
} writer_t;

static void
w_u8(writer_t *w, uint8_t value)
{
    core_buffer_append(w->out, (const char *)&value, 1);
}

static void
w_u32(writer_t *w, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(w->out, (const char *)bytes, 4);
}

static void
w_u64(writer_t *w, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(w->out, (const char *)bytes, 8);
}

static void
w_bytes(writer_t *w, const void *data, size_t length)
{
    w_u64(w, data ? length : UINT64_MAX);
    if (data && length) core_buffer_append(w->out, data, length);
}

static void
w_text(writer_t *w, const char *text)
{
    w_bytes(w, text, text ? strlen(text) : 0);
}

static void
w_type(writer_t *w, th_type *type)
{
    w_u32(w, th_type_writer_add(w->types, type));
}

static void
w_span(writer_t *w, ir_span_t span)
{
    w_u32(w, (uint32_t)span.file);
    w_u32(w, span.start_byte);
    w_u32(w, span.end_byte);
    w_u32(w, span.start_line);
    w_u32(w, span.start_col);
    w_u32(w, span.end_line);
    w_u32(w, span.end_col);
    w_u8(w, span.implied);
}

static void
w_origin(writer_t *w, const ir_origin_t *origin)
{
    uint32_t depth = 0;
    for (const ir_origin_t *at = origin; at; at = at->parent) depth++;
    w_u32(w, depth);
    const ir_origin_t **chain = depth
        ? core_alloc_array(const ir_origin_t *, depth) : nullptr;
    uint32_t i = 0;
    for (const ir_origin_t *at = origin; at; at = at->parent) chain[i++] = at;
    while (i-- > 0) {
        w_u8(w, (uint8_t)chain[i]->kind);
        w_span(w, chain[i]->span);
        w_text(w, chain[i]->agent);
    }
    core_free(chain);
}

static void
w_lit(writer_t *w, ir_lit_t lit)
{
    w_u64(w, (uint64_t)lit.mag);
    w_u64(w, (uint64_t)(lit.mag >> 64));
    w_u8(w, lit.neg);
    w_u8(w, lit.hex);
}

static void
w_foreign(writer_t *w, const ir_value_t *value)
{
    const ir_func_t *owner = value->kind == IR_VAL_INSTR
        ? (value->def && value->def->block ? value->def->block->owner
                                           : nullptr)
        : value->owner;
    if (!owner || !owner->owner) {
        w->failed = true;
        w_u8(w, REF_NONE);
        return;
    }
    w_u8(w, REF_FOREIGN);
    w_text(w, owner->owner->name);
    w_text(w, owner->name);
    w_u8(w, (uint8_t)value->kind);
    switch (value->kind) {
    case IR_VAL_PARAM:
        w_u32(w, value->index);
        break;
    case IR_VAL_INSTR:
        w_u32(w, value->def->block->index);
        w_u32(w, value->def->index);
        break;
    case IR_VAL_LOCAL:
        w_u32(w, value->local ? value->local->index : UINT32_MAX);
        break;
    default:
        w->failed = true;
        break;
    }
}

static void
w_value(writer_t *w, const ir_value_t *value)
{
    if (!value) {
        w_u8(w, REF_NONE);
        return;
    }
    switch (value->kind) {
    case IR_VAL_PARAM:
        if (value->owner != w->function) {
            w_foreign(w, value);
            return;
        }
        w_u8(w, REF_PARAM);
        w_u32(w, value->index);
        return;
    case IR_VAL_INSTR:
        if (!value->def || !value->def->block) break;
        if (value->def->block->owner != w->function) {
            w_foreign(w, value);
            return;
        }
        w_u8(w, REF_INSTR);
        w_u32(w, value->def->block->index);
        w_u32(w, value->def->index);
        return;
    case IR_VAL_LOCAL:
        if (!value->local) break;
        if (value->owner != w->function) {
            w_foreign(w, value);
            return;
        }
        w_u8(w, REF_LOCAL);
        w_u32(w, value->local->index);
        return;
    case IR_VAL_FUNC:
        if (!value->owner || !value->owner->owner) break;
        w_u8(w, REF_FUNC);
        w_text(w, value->owner->owner->name);
        w_text(w, value->owner->name);
        w_type(w, value->type);
        return;
    case IR_VAL_GLOBAL:
        if (!value->global || !value->global->owner) break;
        w_u8(w, REF_GLOBAL);
        w_text(w, value->global->owner->name);
        w_text(w, value->global->name);
        w_type(w, value->type);
        return;
    }
    w->failed = true;
    w_u8(w, REF_NONE);
}

static void
w_annots(writer_t *w, const ir_annots_t *annots)
{
    w_u32(w, annots->n);
    for (uint32_t i = 0; i < annots->n; i++) {
        const ir_annot_t *annot = &annots->a[i];
        w_text(w, annot->key);
        w_u8(w, (uint8_t)annot->kind);
        w_u32(w, (uint32_t)annot->derived_from);
        switch (annot->kind) {
        case IR_ANNOT_BOOL: w_u8(w, annot->b); break;
        case IR_ANNOT_INT: w_lit(w, annot->i); break;
        case IR_ANNOT_STR: w_text(w, annot->s); break;
        case IR_ANNOT_TYPE: w_type(w, annot->t); break;
        case IR_ANNOT_VALUE: w_value(w, annot->v); break;
        }
    }
}

static void
w_producer(writer_t *w, const ir_func_t *producer)
{
    w_text(w, producer && producer->owner ? producer->owner->name : nullptr);
    w_text(w, producer ? producer->name : nullptr);
}

static void
w_measure(writer_t *w, ir_shape_measure_t measure)
{
    w_u8(w, (uint8_t)measure.kind);
    w_u64(w, measure.static_value);
    w_value(w, measure.value);
    w_producer(w, measure.producer);
}

static void
w_shape(writer_t *w, const ir_shape_t *shape)
{
    w_u8(w, shape != nullptr);
    if (!shape) return;
    uint32_t rank = ir_shape_rank(shape);
    w_type(w, ir_shape_element_type(shape));
    w_u32(w, rank);
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent = {0};
        ir_shape_measure_t stride = {0};
        if (!ir_shape_extent(shape, i, &extent)
            || !ir_shape_stride(shape, i, &stride)) w->failed = true;
        w_u8(w, (uint8_t)extent.kind);
        w_u64(w, extent.static_value);
        w_value(w, extent.value);
        w_producer(w, extent.producer);
        w_text(w, extent.producer_identifier);
        w_measure(w, stride);
    }
    ir_shape_measure_t count = {0};
    ir_shape_storage_t storage = {0};
    if (!ir_shape_element_count(shape, &count)
        || !ir_shape_storage(shape, &storage)) w->failed = true;
    w_measure(w, count);
    w_u32(w, storage.flat_prefix_rank);
    w_u8(w, storage.classification_known);
}

static void
w_value_details(writer_t *w, const ir_value_t *value)
{
    w_u8(w, value->is_keyword);
    w_u8(w, value->is_rest);
    w_u8(w, value->is_optional);
    w_shape(w, value->shape);
    w_annots(w, &value->annots);
}

static void
w_init(writer_t *w, const ir_global_init_value_t *value)
{
    w_text(w, value->member);
    w_u8(w, (uint8_t)value->kind);
    switch (value->kind) {
    case IR_GLOBAL_INIT_BOOL: w_u8(w, value->boolean); break;
    case IR_GLOBAL_INIT_INT: w_lit(w, value->integer); break;
    case IR_GLOBAL_INIT_FLOAT: {
        uint64_t bits;
        memcpy(&bits, &value->floating, sizeof(bits));
        w_u64(w, bits);
        break;
    }
    case IR_GLOBAL_INIT_STRING:
        w_bytes(w, value->string.bytes, value->string.length);
        break;
    case IR_GLOBAL_INIT_SYMBOL: w_value(w, value->symbol); break;
    case IR_GLOBAL_INIT_PHASE_RESULT:
        w_text(w, value->phase_result.identifier);
        break;
    case IR_GLOBAL_INIT_TYPE: w_type(w, value->descriptor_type); break;
    case IR_GLOBAL_INIT_SEQUENCE:
    case IR_GLOBAL_INIT_TUPLE:
    case IR_GLOBAL_INIT_RECORD:
    case IR_GLOBAL_INIT_SET:
    case IR_GLOBAL_INIT_DICTIONARY:
    case IR_GLOBAL_INIT_VARIANT:
        w_u64(w, value->aggregate.count);
        for (size_t i = 0; i < value->aggregate.count; i++)
            w_init(w, &value->aggregate.items[i]);
        break;
    case IR_GLOBAL_INIT_NIL:
        break;
    }
}

static void
w_runs(writer_t *w, const ir_layout_bit_run_t *runs, size_t count)
{
    w_u64(w, count);
    for (size_t i = 0; i < count; i++) {
        w_u64(w, runs[i].offset);
        w_u64(w, runs[i].width);
    }
}

static void
w_access(writer_t *w, ir_layout_access_t access)
{
    w_u64(w, access.unit_bit_offset);
    w_u32(w, access.unit_width_bits);
    w_u32(w, access.lsb);
    w_u32(w, access.value_width_bits);
}

static void
w_layout(writer_t *w, const ir_final_layout_t *layout)
{
    w_u64(w, layout->intrinsic_bits);
    w_u32(w, layout->alignment_bytes);
    w_runs(w, layout->occupied, layout->n_occupied);
    w_runs(w, layout->reserved, layout->n_reserved);
    w_u64(w, layout->n_members);
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        w_text(w, member->name);
        w_type(w, member->type);
        w_u32(w, member->ordinal);
        w_u64(w, member->bit_offset);
        w_type(w, member->inline_element_type);
        w_u64(w, member->inline_element_count);
        w_u64(w, member->inline_element_stride_bits);
        w_runs(w, member->occupied, member->n_occupied);
        w_u64(w, member->n_accesses);
        for (size_t j = 0; j < member->n_accesses; j++)
            w_access(w, member->accesses[j]);
    }
    w_u64(w, layout->member_overlap_region.offset);
    w_u64(w, layout->member_overlap_region.width);
    w_u8(w, layout->has_discriminant);
    w_u8(w, layout->discriminant_is_niche);
    w_access(w, layout->discriminant);
}

static void
w_layout_val(writer_t *w, const ir_layout_val_t *value)
{
    w_u8(w, (uint8_t)value->state);
    w_u8(w, value->authored);
    w_u64(w, value->value);
    w_u8(w, (uint8_t)value->sym);
    w_type(w, value->of);
    w_text(w, value->member);
}

static void
w_descriptor(writer_t *w, const ir_descentry_t *entry)
{
    const ir_descriptor_t *d = &entry->d;
    w_text(w, entry->target);
    w_layout_val(w, &d->size);
    w_layout_val(w, &d->align);
    w_layout_val(w, &d->stride);
    w_u32(w, d->n_members);
    for (uint32_t i = 0; i < d->n_members; i++) {
        w_text(w, d->members[i].name);
        w_layout_val(w, &d->members[i].bit_offset);
        w_layout_val(w, &d->members[i].bit_width);
    }
    w_u8(w, d->indirect.fat);
    w_u8(w, d->indirect.authored);
    w_layout_val(w, &d->indirect.words);
    w_layout_val(w, &d->variant.tag_bit_offset);
    w_layout_val(w, &d->variant.tag_bit_width);
    w_u8(w, d->variant.niche);
    w_u8(w, d->variant.authored);
    w_u8(w, d->done);
    w_u8(w, d->final != nullptr);
    if (d->final) w_layout(w, d->final);
}

static void
w_operand(writer_t *w, const ir_operand_t *operand)
{
    w_u8(w, (uint8_t)operand->kind);
    switch (operand->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
    case IR_OPD_FRESH:
        w_value(w, operand->value);
        break;
    case IR_OPD_TYPE: w_type(w, operand->type); break;
    case IR_OPD_NAME: w_text(w, operand->name); break;
    case IR_OPD_BLOCK:
    case IR_OPD_DEFAULT_BLOCK:
        w_u32(w, operand->block ? operand->block->index : UINT32_MAX);
        break;
    case IR_OPD_CONST: {
        w_lit(w, operand->lit.i);
        uint64_t bits;
        memcpy(&bits, &operand->lit.f, sizeof(bits));
        w_u64(w, bits);
        w_text(w, operand->lit.text);
        w_u8(w, operand->lit.is_float);
        break;
    }
    case IR_OPD_PHI_ARM:
        w_u32(w, operand->phi.block ? operand->phi.block->index : UINT32_MAX);
        w_value(w, operand->phi.value);
        break;
    case IR_OPD_SWITCH_ARM:
        w_lit(w, operand->arm.value);
        w_u32(w, operand->arm.block ? operand->arm.block->index : UINT32_MAX);
        break;
    case IR_OPD_NAMED_ARG:
        w_text(w, operand->named.name);
        w_value(w, operand->named.value);
        break;
    default:
        break;
    }
}

static void
w_function_skeleton(writer_t *w, ir_func_t *function)
{
    w->function = function;
    w_span(w, function->span);
    w_origin(w, function->origin);
    w_u32(w, (uint32_t)function->props);
    w_u32(w, function->n_optional_parameters);
    for (uint32_t i = 0; i < function->n_optional_parameters; i++)
        w_u8(w, function->optional_parameters[i]);
    w_u32(w, function->n_params);
    for (uint32_t i = 0; i < function->n_params; i++) {
        ir_value_t *param = function->params[i];
        w_text(w, param->name);
        w_type(w, param->type);
        w_u8(w, ir_param_is_optional(param));
    }
    w_u32(w, function->n_locals);
    for (uint32_t i = 0; i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        w_text(w, local->name);
        w_type(w, local->type);
        w_u8(w, local->value != nullptr);
    }
    w_u32(w, function->n_blocks);
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        w_text(w, block->name);
        w_u32(w, block->n_instrs);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instr = block->instrs[i];
            w_u32(w, (uint32_t)instr->op);
            w_u8(w, instr->dest ? 1 : instr->dest_local ? 2 : 0);
            if (instr->dest) {
                w_text(w, instr->dest->name);
                w_type(w, instr->dest->type);
            } else if (instr->dest_local) {
                w_u32(w, instr->dest_local->index);
            }
        }
    }
    w->function = nullptr;
}

static void
w_function_details(writer_t *w, ir_func_t *function)
{
    w->function = function;
    for (uint32_t i = 0; i < function->n_params; i++)
        w_value_details(w, function->params[i]);
    for (uint32_t i = 0; i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        w_u8(w, (uint8_t)local->mutability);
        w_u8(w, local->address_taken);
        w_u8(w, local->has_init);
        w_lit(w, local->init);
        uint64_t bits;
        memcpy(&bits, &local->init_f, sizeof(bits));
        w_u64(w, bits);
        w_text(w, local->init_text);
        w_u8(w, local->init_is_float);
        w_span(w, local->span);
        w_origin(w, local->origin);
        w_shape(w, local->shape);
        w_annots(w, &local->annots);
        if (local->value) w_value_details(w, local->value);
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        w_span(w, block->span);
        w_origin(w, block->origin);
        w_annots(w, &block->annots);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instr = block->instrs[i];
            w_span(w, instr->span);
            w_origin(w, instr->origin);
            w_shape(w, instr->shape_constraint);
            w_annots(w, &instr->annots);
            if (instr->dest) w_value_details(w, instr->dest);
            w_bytes(w, instr->const_bytes, instr->const_byte_len);
            w_u32(w, instr->n_mods);
            for (uint32_t j = 0; j < instr->n_mods; j++)
                w_text(w, instr->mods[j]);
            w_u32(w, instr->n_operands);
            for (uint32_t j = 0; j < instr->n_operands; j++)
                w_operand(w, &instr->operands[j]);
        }
    }
    w_annots(w, &function->annots);
    w->function = nullptr;
}

bool
ir_modules_save(ir_module_t *const *modules, size_t count,
                core_buffer_t *out)
{
    if (!out || (count && !modules)) return false;
    core_buffer_t body = {0};
    writer_t w = {.out = &body, .types = th_type_writer_new()};
    if (!w.types) return false;
    w_u32(&w, (uint32_t)count);
    for (size_t m = 0; m < count; m++) {
        ir_module_t *module = modules[m];
        w_text(&w, module->name);
        w_text(&w, module->profile ? module->profile->name : nullptr);
        w_text(&w, module->target);
        w_u32(&w, (uint32_t)module->props);
        w_span(&w, module->span);
        w_u64(&w, module->n_files);
        for (size_t i = 0; i < module->n_files; i++)
            w_text(&w, module->files[i]);
        w_u64(&w, module->n_types);
        for (size_t i = 0; i < module->n_types; i++)
            w_type(&w, module->types[i].type);
        w_u64(&w, module->n_funcs);
        for (size_t i = 0; i < module->n_funcs; i++) {
            ir_func_t *function = module->funcs[i];
            w_text(&w, function->name);
            w_type(&w, function->type);
            w_u8(&w, (uint8_t)function->linkage);
        }
        w_u64(&w, module->n_globals);
        for (size_t i = 0; i < module->n_globals; i++) {
            ir_global_t *global = module->globals[i];
            w_text(&w, global->name);
            w_type(&w, global->type);
        }
    }
    for (size_t m = 0; m < count; m++) {
        ir_module_t *module = modules[m];
        for (size_t i = 0; i < module->n_types; i++) {
            ir_typeslot_t *slot = &module->types[i];
            w_type(&w, slot->def);
            w_u64(&w, slot->n_descs);
            for (size_t d = 0; d < slot->n_descs; d++)
                w_descriptor(&w, &slot->descs[d]);
        }
    }
    for (size_t m = 0; m < count; m++)
        for (size_t i = 0; i < modules[m]->n_funcs; i++)
            w_function_skeleton(&w, modules[m]->funcs[i]);
    for (size_t m = 0; m < count; m++) {
        ir_module_t *module = modules[m];
        for (size_t i = 0; i < module->n_globals; i++) {
            ir_global_t *global = module->globals[i];
            w_u8(&w, global->is_static);
            w_u8(&w, global->is_extern);
            w_u8(&w, (uint8_t)global->mutability);
            w_span(&w, global->span);
            w_origin(&w, global->origin);
            w_shape(&w, global->shape);
            w_annots(&w, &global->annots);
            w_u8(&w, global->has_static_initializer
                 && global->static_initializer);
            if (global->has_static_initializer && global->static_initializer)
                w_init(&w, global->static_initializer);
        }
    }
    for (size_t m = 0; m < count; m++) {
        for (size_t i = 0; i < modules[m]->n_funcs; i++)
            w_function_details(&w, modules[m]->funcs[i]);
        w_annots(&w, &modules[m]->annots);
    }
    bool ok = !w.failed;
    core_buffer_t types = {0};
    if (ok) ok = th_type_writer_finish(w.types, &types);
    th_type_writer_free(w.types);
    if (ok) {
        uint8_t header[8];
        for (int i = 0; i < 4; i++)
            header[i] = (uint8_t)(IR_ARCHIVE_MAGIC >> (8 * i));
        for (int i = 0; i < 4; i++)
            header[4 + i] = (uint8_t)(IR_ARCHIVE_VERSION >> (8 * i));
        core_buffer_append(out, (const char *)header, 8);
        core_buffer_append(out, types.data, types.byte_len);
        core_buffer_append(out, body.data, body.byte_len);
    }
    core_buffer_release(&types);
    core_buffer_release(&body);
    return ok;
}

typedef struct {
    ir_ctx_t                 *ctx;
    const uint8_t            *data;
    size_t                    length;
    size_t                    at;
    bool                      failed;
    th_type_reader_t         *types;
    ir_module_t             **modules;
    size_t                    count;
    ir_module_lookup_fn       lookup;
    void                     *lookup_context;
    ir_func_t                *function;
} reader_t;

static uint8_t
r_u8(reader_t *r)
{
    if (r->failed || r->at >= r->length) {
        r->failed = true;
        return 0;
    }
    return r->data[r->at++];
}

static uint32_t
r_u32(reader_t *r)
{
    if (r->failed || r->length - r->at < 4) {
        r->failed = true;
        return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= (uint32_t)r->data[r->at + i] << (8 * i);
    r->at += 4;
    return value;
}

static uint64_t
r_u64(reader_t *r)
{
    if (r->failed || r->length - r->at < 8) {
        r->failed = true;
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)r->data[r->at + i] << (8 * i);
    r->at += 8;
    return value;
}

static const char *
r_bytes(reader_t *r, size_t *length)
{
    uint64_t size = r_u64(r);
    if (length) *length = 0;
    if (r->failed || size == UINT64_MAX) return nullptr;
    if (size > r->length - r->at) {
        r->failed = true;
        return nullptr;
    }
    char *copy = ir_alloc(r->ctx, (size_t)size + 1);
    if (!copy) {
        r->failed = true;
        return nullptr;
    }
    if (size) memcpy(copy, r->data + r->at, (size_t)size);
    copy[size] = '\0';
    r->at += (size_t)size;
    if (length) *length = (size_t)size;
    return copy;
}

static const char *
r_text(reader_t *r)
{
    return r_bytes(r, nullptr);
}

static uint64_t
r_count(reader_t *r)
{
    uint64_t count = r_u64(r);
    if (count > r->length) r->failed = true;
    return r->failed ? 0 : count;
}

static th_type *
r_type(reader_t *r)
{
    uint32_t index = r_u32(r);
    if (!index) return nullptr;
    th_type *type = th_type_reader_get(r->types, index);
    if (!type) r->failed = true;
    return type;
}

static ir_span_t
r_span(reader_t *r)
{
    ir_span_t span;
    span.file = (int32_t)r_u32(r);
    span.start_byte = r_u32(r);
    span.end_byte = r_u32(r);
    span.start_line = r_u32(r);
    span.start_col = r_u32(r);
    span.end_line = r_u32(r);
    span.end_col = r_u32(r);
    span.implied = r_u8(r) != 0;
    return span;
}

static ir_origin_t *
r_origin(reader_t *r)
{
    uint32_t depth = r_u32(r);
    if (depth > r->length) {
        r->failed = true;
        return nullptr;
    }
    ir_origin_t *origin = nullptr;
    for (uint32_t i = 0; !r->failed && i < depth; i++) {
        ir_origin_kind_t kind = (ir_origin_kind_t)r_u8(r);
        ir_span_t span = r_span(r);
        const char *agent = r_text(r);
        origin = ir_origin_new(r->ctx, kind, span, agent, origin);
        if (!origin) r->failed = true;
    }
    return origin;
}

static ir_lit_t
r_lit(reader_t *r)
{
    ir_lit_t lit = {0};
    uint64_t low = r_u64(r);
    uint64_t high = r_u64(r);
    lit.mag = ((ir_uint_t)high << 64) | low;
    lit.neg = r_u8(r) != 0;
    lit.hex = r_u8(r) != 0;
    return lit;
}

static ir_module_t *
r_find_module(reader_t *r, const char *name)
{
    if (!name) return nullptr;
    for (size_t i = 0; i < r->count; i++)
        if (r->modules[i] && !strcmp(r->modules[i]->name, name))
            return r->modules[i];
    return r->lookup ? r->lookup(r->lookup_context, name) : nullptr;
}

static ir_value_t *
r_value(reader_t *r)
{
    uint8_t tag = r_u8(r);
    ir_func_t *function = r->function;
    switch (tag) {
    case REF_NONE:
        return nullptr;
    case REF_PARAM: {
        uint32_t index = r_u32(r);
        if (!function || index >= function->n_params) break;
        return function->params[index];
    }
    case REF_INSTR: {
        uint32_t block = r_u32(r);
        uint32_t instr = r_u32(r);
        if (!function || block >= function->n_blocks
            || instr >= function->blocks[block]->n_instrs
            || !function->blocks[block]->instrs[instr]->dest) break;
        return function->blocks[block]->instrs[instr]->dest;
    }
    case REF_LOCAL: {
        uint32_t index = r_u32(r);
        if (!function || index >= function->n_locals) break;
        ir_local_t *local = function->locals[index];
        return local->value ? local->value : ir_local_value(function, local);
    }
    case REF_FOREIGN: {
        const char *module_name = r_text(r);
        const char *function_name = r_text(r);
        uint8_t kind = r_u8(r);
        ir_module_t *module = r_find_module(r, module_name);
        ir_func_t *owner = module && function_name
            ? ir_func_find(module, function_name) : nullptr;
        if (kind == IR_VAL_PARAM) {
            uint32_t index = r_u32(r);
            if (owner && index < owner->n_params) return owner->params[index];
        } else if (kind == IR_VAL_INSTR) {
            uint32_t block = r_u32(r);
            uint32_t instr = r_u32(r);
            if (owner && block < owner->n_blocks
                && instr < owner->blocks[block]->n_instrs
                && owner->blocks[block]->instrs[instr]->dest)
                return owner->blocks[block]->instrs[instr]->dest;
        } else if (kind == IR_VAL_LOCAL) {
            uint32_t index = r_u32(r);
            if (owner && index < owner->n_locals) {
                ir_local_t *local = owner->locals[index];
                return local->value ? local->value
                                    : ir_local_value(owner, local);
            }
        }
        break;
    }
    case REF_FUNC:
    case REF_GLOBAL: {
        const char *module_name = r_text(r);
        const char *name = r_text(r);
        th_type *type = r_type(r);
        ir_module_t *module = r_find_module(r, module_name);
        if (!module || !name) break;
        ir_value_t *value = ir_alloc(r->ctx, sizeof(*value));
        if (!value) break;
        memset(value, 0, sizeof(*value));
        value->name = name;
        value->type = type;
        if (tag == REF_FUNC) {
            ir_func_t *target = ir_func_find(module, name);
            if (!target) break;
            value->kind = IR_VAL_FUNC;
            value->owner = target;
            value->name = target->name;
        } else {
            ir_global_t *global = ir_global_find(module, name);
            if (!global) break;
            value->kind = IR_VAL_GLOBAL;
            value->global = global;
            value->name = global->name;
        }
        return value;
    }
    default:
        break;
    }
    r->failed = true;
    return nullptr;
}

static void
r_annots(reader_t *r, ir_annots_t *annots)
{
    uint32_t count = r_u32(r);
    if (count > r->length) r->failed = true;
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        ir_annot_t annot = {0};
        annot.key = r_text(r);
        annot.kind = (ir_annot_kind_t)r_u8(r);
        annot.derived_from = (ir_props_t)r_u32(r);
        switch (annot.kind) {
        case IR_ANNOT_BOOL: annot.b = r_u8(r) != 0; break;
        case IR_ANNOT_INT: annot.i = r_lit(r); break;
        case IR_ANNOT_STR: annot.s = r_text(r); break;
        case IR_ANNOT_TYPE: annot.t = r_type(r); break;
        case IR_ANNOT_VALUE: annot.v = r_value(r); break;
        default: r->failed = true; break;
        }
        if (!r->failed && (!annot.key || !ir_annot_set(r->ctx, annots, annot)))
            r->failed = true;
    }
}

static ir_func_t *
r_producer(reader_t *r)
{
    const char *module_name = r_text(r);
    const char *name = r_text(r);
    if (!module_name || !name) return nullptr;
    ir_module_t *module = r_find_module(r, module_name);
    ir_func_t *producer = module ? ir_func_find(module, name) : nullptr;
    if (!producer) r->failed = true;
    return producer;
}

static ir_shape_measure_t
r_measure(reader_t *r)
{
    ir_shape_measure_t measure = {0};
    measure.kind = (ir_shape_measure_kind_t)r_u8(r);
    measure.static_value = r_u64(r);
    measure.value = r_value(r);
    measure.producer = r_producer(r);
    return measure;
}

static ir_shape_t *
r_shape(reader_t *r)
{
    if (!r_u8(r)) return nullptr;
    th_type *element = r_type(r);
    uint32_t rank = r_u32(r);
    ir_shape_t *shape = !r->failed && element && rank
        ? ir_shape_new(r->ctx, element, rank) : nullptr;
    if (!shape) {
        r->failed = true;
        return nullptr;
    }
    for (uint32_t i = 0; !r->failed && i < rank; i++) {
        ir_shape_extent_t extent = {0};
        extent.kind = (ir_shape_extent_kind_t)r_u8(r);
        extent.static_value = r_u64(r);
        extent.value = r_value(r);
        extent.producer = r_producer(r);
        extent.producer_identifier = r_text(r);
        ir_shape_measure_t stride = r_measure(r);
        if (!r->failed && (!ir_shape_set_extent(shape, i, extent)
                           || !ir_shape_set_stride(shape, i, stride)))
            r->failed = true;
    }
    ir_shape_measure_t count = r_measure(r);
    uint32_t flat = r_u32(r);
    bool known = r_u8(r) != 0;
    if (!r->failed && (!ir_shape_set_element_count(shape, count)
                       || !ir_shape_set_storage(shape, flat, known)))
        r->failed = true;
    return shape;
}

static void
r_value_details(reader_t *r, ir_value_t *value)
{
    value->is_keyword = r_u8(r) != 0;
    value->is_rest = r_u8(r) != 0;
    value->is_optional = r_u8(r) != 0;
    value->shape = r_shape(r);
    r_annots(r, &value->annots);
}

static void
r_init(reader_t *r, ir_global_init_value_t *value)
{
    memset(value, 0, sizeof(*value));
    value->member = r_text(r);
    value->kind = (ir_global_init_kind_t)r_u8(r);
    switch (value->kind) {
    case IR_GLOBAL_INIT_BOOL: value->boolean = r_u8(r) != 0; break;
    case IR_GLOBAL_INIT_INT: value->integer = r_lit(r); break;
    case IR_GLOBAL_INIT_FLOAT: {
        uint64_t bits = r_u64(r);
        memcpy(&value->floating, &bits, sizeof(bits));
        break;
    }
    case IR_GLOBAL_INIT_STRING:
        value->string.bytes = r_bytes(r, &value->string.length);
        break;
    case IR_GLOBAL_INIT_SYMBOL: value->symbol = r_value(r); break;
    case IR_GLOBAL_INIT_PHASE_RESULT:
        value->phase_result.identifier = r_text(r);
        break;
    case IR_GLOBAL_INIT_TYPE: value->descriptor_type = r_type(r); break;
    case IR_GLOBAL_INIT_SEQUENCE:
    case IR_GLOBAL_INIT_TUPLE:
    case IR_GLOBAL_INIT_RECORD:
    case IR_GLOBAL_INIT_SET:
    case IR_GLOBAL_INIT_DICTIONARY:
    case IR_GLOBAL_INIT_VARIANT: {
        uint64_t count = r_count(r);
        ir_global_init_value_t *items = count
            ? ir_alloc(r->ctx, (size_t)count * sizeof(*items)) : nullptr;
        if (count && !items) r->failed = true;
        for (uint64_t i = 0; !r->failed && i < count; i++)
            r_init(r, &items[i]);
        value->aggregate.items = items;
        value->aggregate.count = (size_t)count;
        break;
    }
    case IR_GLOBAL_INIT_NIL:
        break;
    default:
        r->failed = true;
        break;
    }
}

static ir_layout_bit_run_t *
r_runs(reader_t *r, size_t *count)
{
    uint64_t n = r_count(r);
    ir_layout_bit_run_t *runs = n
        ? ir_alloc(r->ctx, (size_t)n * sizeof(*runs)) : nullptr;
    if (n && !runs) r->failed = true;
    for (uint64_t i = 0; !r->failed && i < n; i++) {
        runs[i].offset = r_u64(r);
        runs[i].width = r_u64(r);
    }
    *count = (size_t)n;
    return runs;
}

static ir_layout_access_t
r_access(reader_t *r)
{
    ir_layout_access_t access;
    access.unit_bit_offset = r_u64(r);
    access.unit_width_bits = r_u32(r);
    access.lsb = r_u32(r);
    access.value_width_bits = r_u32(r);
    return access;
}

static void
r_layout(reader_t *r, ir_final_layout_t *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->intrinsic_bits = r_u64(r);
    layout->alignment_bytes = r_u32(r);
    layout->occupied = r_runs(r, &layout->n_occupied);
    layout->reserved = r_runs(r, &layout->n_reserved);
    uint64_t count = r_count(r);
    ir_final_member_layout_t *members = count
        ? ir_alloc(r->ctx, (size_t)count * sizeof(*members)) : nullptr;
    if (count && !members) r->failed = true;
    for (uint64_t i = 0; !r->failed && i < count; i++) {
        ir_final_member_layout_t *member = &members[i];
        memset(member, 0, sizeof(*member));
        member->name = r_text(r);
        member->type = r_type(r);
        member->ordinal = r_u32(r);
        member->bit_offset = r_u64(r);
        member->inline_element_type = r_type(r);
        member->inline_element_count = r_u64(r);
        member->inline_element_stride_bits = r_u64(r);
        member->occupied = r_runs(r, &member->n_occupied);
        uint64_t accesses = r_count(r);
        ir_layout_access_t *items = accesses
            ? ir_alloc(r->ctx, (size_t)accesses * sizeof(*items)) : nullptr;
        if (accesses && !items) r->failed = true;
        for (uint64_t j = 0; !r->failed && j < accesses; j++)
            items[j] = r_access(r);
        member->accesses = items;
        member->n_accesses = (size_t)accesses;
    }
    layout->members = members;
    layout->n_members = (size_t)count;
    layout->member_overlap_region.offset = r_u64(r);
    layout->member_overlap_region.width = r_u64(r);
    layout->has_discriminant = r_u8(r) != 0;
    layout->discriminant_is_niche = r_u8(r) != 0;
    layout->discriminant = r_access(r);
}

static void
r_layout_val(reader_t *r, ir_layout_val_t *value)
{
    value->state = (ir_layout_state_t)r_u8(r);
    value->authored = r_u8(r) != 0;
    value->value = r_u64(r);
    value->sym = (ir_symkind_t)r_u8(r);
    value->of = r_type(r);
    value->member = r_text(r);
}

static void
r_descriptor(reader_t *r, ir_module_t *module, th_type *type)
{
    const char *target = r_text(r);
    ir_descriptor_t scratch = {0};
    r_layout_val(r, &scratch.size);
    r_layout_val(r, &scratch.align);
    r_layout_val(r, &scratch.stride);
    uint32_t members = r_u32(r);
    if (members > r->length) r->failed = true;
    ir_member_layout_t *items = !r->failed && members
        ? ir_alloc(r->ctx, members * sizeof(*items)) : nullptr;
    if (members && !items) r->failed = true;
    for (uint32_t i = 0; !r->failed && i < members; i++) {
        items[i].name = r_text(r);
        r_layout_val(r, &items[i].bit_offset);
        r_layout_val(r, &items[i].bit_width);
    }
    scratch.indirect.fat = r_u8(r) != 0;
    scratch.indirect.authored = r_u8(r) != 0;
    r_layout_val(r, &scratch.indirect.words);
    r_layout_val(r, &scratch.variant.tag_bit_offset);
    r_layout_val(r, &scratch.variant.tag_bit_width);
    scratch.variant.niche = r_u8(r) != 0;
    scratch.variant.authored = r_u8(r) != 0;
    bool done = r_u8(r) != 0;
    ir_final_layout_t *final = nullptr;
    if (r_u8(r)) {
        final = ir_alloc(r->ctx, sizeof(*final));
        if (!final) r->failed = true;
        else r_layout(r, final);
    }
    if (r->failed || !type) return;
    ir_descriptor_t *d = target ? ir_layout_ensure(module, type, target)
                                : ir_layout_ensure_targetless(module, type);
    if (!d) {
        r->failed = true;
        return;
    }
    d->size = scratch.size;
    d->align = scratch.align;
    d->stride = scratch.stride;
    for (uint32_t i = 0; i < members; i++)
        if (!ir_layout_descriptor_pin_member(module, d, items[i].name, 0, 0)) {
            r->failed = true;
            return;
        }
    for (uint32_t i = 0; i < d->n_members && i < members; i++)
        for (uint32_t j = 0; j < members; j++)
            if (d->members[i].name && items[j].name
                && !strcmp(d->members[i].name, items[j].name)) {
                d->members[i].bit_offset = items[j].bit_offset;
                d->members[i].bit_width = items[j].bit_width;
            }
    d->indirect = scratch.indirect;
    d->variant = scratch.variant;
    d->done = done;
    if (final && !d->final
        && !ir_layout_publish_final(module, type, target, final))
        r->failed = true;
}

static ir_block_t *
r_block(reader_t *r, ir_func_t *function)
{
    uint32_t index = r_u32(r);
    if (index == UINT32_MAX) return nullptr;
    if (index >= function->n_blocks) {
        r->failed = true;
        return nullptr;
    }
    return function->blocks[index];
}

static void
r_operand(reader_t *r, ir_func_t *function, ir_operand_t *operand)
{
    memset(operand, 0, sizeof(*operand));
    operand->kind = (ir_opdkind_t)r_u8(r);
    switch (operand->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
    case IR_OPD_FRESH:
        operand->value = r_value(r);
        break;
    case IR_OPD_TYPE: operand->type = r_type(r); break;
    case IR_OPD_NAME: operand->name = r_text(r); break;
    case IR_OPD_BLOCK:
    case IR_OPD_DEFAULT_BLOCK:
        operand->block = r_block(r, function);
        break;
    case IR_OPD_CONST: {
        operand->lit.i = r_lit(r);
        uint64_t bits = r_u64(r);
        memcpy(&operand->lit.f, &bits, sizeof(bits));
        operand->lit.text = r_text(r);
        operand->lit.is_float = r_u8(r) != 0;
        break;
    }
    case IR_OPD_PHI_ARM:
        operand->phi.block = r_block(r, function);
        operand->phi.value = r_value(r);
        break;
    case IR_OPD_SWITCH_ARM:
        operand->arm.value = r_lit(r);
        operand->arm.block = r_block(r, function);
        break;
    case IR_OPD_NAMED_ARG:
        operand->named.name = r_text(r);
        operand->named.value = r_value(r);
        break;
    default:
        break;
    }
}

static void
r_function_skeleton(reader_t *r, ir_func_t *function)
{
    r->function = function;
    function->span = r_span(r);
    function->origin = r_origin(r);
    function->props = (ir_props_t)r_u32(r);
    uint32_t optional_count = r_u32(r);
    for (uint32_t i = 0; !r->failed && i < optional_count; i++)
        if (r_u8(r) && !ir_func_set_parameter_optional(function, i, true))
            r->failed = true;
    uint32_t params = r_u32(r);
    if (params > r->length) r->failed = true;
    for (uint32_t i = 0; !r->failed && i < params; i++) {
        const char *name = r_text(r);
        th_type *type = r_type(r);
        bool optional = r_u8(r) != 0;
        ir_value_t *param = ir_param_add(function, name, type);
        if (!param || (optional
                       && !ir_func_set_parameter_optional(function, i, true)))
            r->failed = true;
        else if (optional) param->is_optional = true;
    }
    uint32_t locals = r_u32(r);
    if (locals > r->length) r->failed = true;
    for (uint32_t i = 0; !r->failed && i < locals; i++) {
        const char *name = r_text(r);
        th_type *type = r_type(r);
        bool has_value = r_u8(r) != 0;
        ir_local_t *local = ir_local_add(function, name, type);
        if (!local || (has_value && !ir_local_value(function, local)))
            r->failed = true;
    }
    uint32_t blocks = r_u32(r);
    if (blocks > r->length) r->failed = true;
    for (uint32_t b = 0; !r->failed && b < blocks; b++) {
        ir_block_t *block = ir_block_add(function, r_text(r));
        uint32_t instrs = r_u32(r);
        if (!block || instrs > r->length) {
            r->failed = true;
            break;
        }
        for (uint32_t i = 0; !r->failed && i < instrs; i++) {
            ir_instr_t *instr = ir_instr_add(block, (ir_op_t)r_u32(r));
            uint8_t dest = r_u8(r);
            if (!instr) {
                r->failed = true;
                break;
            }
            if (dest == 1) {
                const char *name = r_text(r);
                th_type *type = r_type(r);
                if (!r->failed && !ir_instr_set_dest(instr, name, type))
                    r->failed = true;
            } else if (dest == 2) {
                uint32_t index = r_u32(r);
                if (index >= function->n_locals
                    || !ir_instr_set_dest_local(instr,
                        function->locals[index])) r->failed = true;
            }
        }
    }
    r->function = nullptr;
}

static void
r_function_details(reader_t *r, ir_func_t *function)
{
    r->function = function;
    for (uint32_t i = 0; !r->failed && i < function->n_params; i++)
        r_value_details(r, function->params[i]);
    for (uint32_t i = 0; !r->failed && i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        local->mutability = (ir_mutability_t)r_u8(r);
        local->address_taken = r_u8(r) != 0;
        local->has_init = r_u8(r) != 0;
        local->init = r_lit(r);
        uint64_t bits = r_u64(r);
        memcpy(&local->init_f, &bits, sizeof(bits));
        local->init_text = r_text(r);
        local->init_is_float = r_u8(r) != 0;
        local->span = r_span(r);
        local->origin = r_origin(r);
        local->shape = r_shape(r);
        r_annots(r, &local->annots);
        if (local->value) r_value_details(r, local->value);
    }
    for (uint32_t b = 0; !r->failed && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        block->span = r_span(r);
        block->origin = r_origin(r);
        r_annots(r, &block->annots);
        for (uint32_t i = 0; !r->failed && i < block->n_instrs; i++) {
            ir_instr_t *instr = block->instrs[i];
            instr->span = r_span(r);
            instr->origin = r_origin(r);
            instr->shape_constraint = r_shape(r);
            r_annots(r, &instr->annots);
            if (instr->dest) r_value_details(r, instr->dest);
            size_t const_length = 0;
            const char *bytes = r_bytes(r, &const_length);
            if (bytes && !ir_instr_set_const_bytes(instr,
                    (const uint8_t *)bytes, const_length)) r->failed = true;
            uint32_t mods = r_u32(r);
            if (mods > r->length) r->failed = true;
            for (uint32_t j = 0; !r->failed && j < mods; j++)
                if (!ir_instr_add_mod(instr, r_text(r))) r->failed = true;
            uint32_t operands = r_u32(r);
            if (operands > r->length) r->failed = true;
            if (!r->failed && operands) {
                instr->operands = ir_alloc(r->ctx,
                    operands * sizeof(*instr->operands));
                if (!instr->operands) r->failed = true;
                for (uint32_t j = 0; !r->failed && j < operands; j++)
                    r_operand(r, function, &instr->operands[j]);
                instr->n_operands = instr->cap_operands = operands;
            }
        }
    }
    r_annots(r, &function->annots);
    r->function = nullptr;
}

static const ir_profile_t *
r_profile(const char *name, const ir_profile_t *const *profiles,
          size_t profile_count)
{
    for (size_t i = 0; name && i < profile_count; i++)
        if (profiles[i] && !strcmp(profiles[i]->name, name))
            return profiles[i];
    return nullptr;
}

bool
ir_modules_load(ir_ctx_t *ctx, const uint8_t *data, size_t length,
                const ir_profile_t *const *profiles, size_t profile_count,
                ir_module_lookup_fn lookup, void *lookup_context,
                ir_module_t **out, size_t capacity, size_t *count)
{
    if (!ctx || !data || length < 8 || !count) return false;
    uint32_t magic = 0, version = 0;
    for (int i = 0; i < 4; i++) magic |= (uint32_t)data[i] << (8 * i);
    for (int i = 0; i < 4; i++) version |= (uint32_t)data[4 + i] << (8 * i);
    if (magic != IR_ARCHIVE_MAGIC || version != IR_ARCHIVE_VERSION)
        return false;
    size_t consumed = 0;
    th_type_reader_t *types = th_type_reader_open(ir_ctx_types(ctx),
        data + 8, length - 8, &consumed);
    if (!types) return false;
    reader_t r = {
        .ctx = ctx, .data = data, .length = length, .at = 8 + consumed,
        .types = types, .lookup = lookup, .lookup_context = lookup_context,
    };
    uint32_t modules = r_u32(&r);
    if (r.failed || modules > capacity || (modules && !out)) {
        th_type_reader_free(types);
        return false;
    }
    r.modules = out;
    size_t *slot_counts = modules ? core_alloc_array(size_t, modules) : nullptr;
    th_type ***slot_types = modules
        ? core_alloc_array(th_type **, modules) : nullptr;
    if (modules && (!slot_counts || !slot_types)) r.failed = true;
    for (uint32_t m = 0; !r.failed && m < modules; m++) {
        const char *name = r_text(&r);
        const ir_profile_t *profile = r_profile(r_text(&r), profiles,
                                                profile_count);
        const char *target = r_text(&r);
        ir_module_t *module = name && profile
            ? ir_module_new(ctx, name, profile, target) : nullptr;
        if (!module) {
            r.failed = true;
            break;
        }
        out[m] = module;
        r.count = m + 1;
        module->props = (ir_props_t)r_u32(&r);
        module->span = r_span(&r);
        uint64_t files = r_count(&r);
        for (uint64_t i = 0; !r.failed && i < files; i++)
            ir_file_add(module, r_text(&r));
        uint64_t slots = r_count(&r);
        slot_types[m] = slots ? core_alloc_array(th_type *, (size_t)slots)
                              : nullptr;
        if (slots && !slot_types[m]) r.failed = true;
        for (uint64_t i = 0; !r.failed && i < slots; i++) {
            th_type *type = r_type(&r);
            slot_types[m][i] = type ? ir_type_intern(module, type) : nullptr;
        }
        slot_counts[m] = r.failed ? 0 : (size_t)slots;
        uint64_t functions = r_count(&r);
        for (uint64_t i = 0; !r.failed && i < functions; i++) {
            const char *function_name = r_text(&r);
            th_type *type = r_type(&r);
            ir_linkage_t linkage = (ir_linkage_t)r_u8(&r);
            if (!r.failed && !ir_func_add(module, function_name, type, linkage))
                r.failed = true;
        }
        uint64_t globals = r_count(&r);
        for (uint64_t i = 0; !r.failed && i < globals; i++) {
            const char *global_name = r_text(&r);
            th_type *type = r_type(&r);
            if (!r.failed && !ir_global_add(module, global_name, type))
                r.failed = true;
        }
    }
    for (uint32_t m = 0; !r.failed && m < modules; m++) {
        ir_module_t *module = out[m];
        size_t slots = slot_counts[m];
        for (size_t i = 0; !r.failed && i < slots; i++) {
            th_type *type = slot_types[m][i];
            th_type *definition = r_type(&r);
            ir_typeslot_t *slot = type ? ir_type_slot(module, type) : nullptr;
            if (type && !slot) r.failed = true;
            if (slot && definition && !ir_type_define(module, type, definition))
                slot->def = th_resolve(definition);
            uint64_t descriptors = r_count(&r);
            for (uint64_t d = 0; !r.failed && d < descriptors; d++)
                r_descriptor(&r, module, type);
        }
    }
    for (uint32_t m = 0; !r.failed && m < modules; m++)
        for (size_t i = 0; !r.failed && i < out[m]->n_funcs; i++)
            r_function_skeleton(&r, out[m]->funcs[i]);
    for (uint32_t m = 0; !r.failed && m < modules; m++) {
        ir_module_t *module = out[m];
        for (size_t i = 0; !r.failed && i < module->n_globals; i++) {
            ir_global_t *global = module->globals[i];
            global->is_static = r_u8(&r) != 0;
            global->is_extern = r_u8(&r) != 0;
            global->mutability = (ir_mutability_t)r_u8(&r);
            global->span = r_span(&r);
            global->origin = r_origin(&r);
            global->shape = r_shape(&r);
            r_annots(&r, &global->annots);
            if (r_u8(&r)) {
                ir_global_init_value_t *init = ir_alloc(ctx, sizeof(*init));
                if (!init) {
                    r.failed = true;
                    break;
                }
                r_init(&r, init);
                global->static_initializer = init;
                global->has_static_initializer = !r.failed;
            }
        }
    }
    for (uint32_t m = 0; !r.failed && m < modules; m++) {
        for (size_t i = 0; !r.failed && i < out[m]->n_funcs; i++)
            r_function_details(&r, out[m]->funcs[i]);
        r_annots(&r, &out[m]->annots);
    }
    th_type_reader_free(types);
    for (uint32_t m = 0; slot_types && m < modules; m++)
        core_free(slot_types[m]);
    core_free(slot_types);
    core_free(slot_counts);
    if (r.failed) return false;
    *count = modules;
    return true;
}
