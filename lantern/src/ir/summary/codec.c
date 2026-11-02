#include "internal.h"

enum { SUMMARY_CODEC_VERSION = 1 };

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         at;
    bool           failed;
    const ir_summary_texts_t    *texts;
    const ir_summary_resolver_t *resolver;
    th_ctx        *types;
} summary_reader_t;

static void
put_u8(core_buffer_t *out, uint8_t value)
{
    core_buffer_append(out, (const char *)&value, 1);
}

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static void
put_text(core_buffer_t *out, const ir_summary_texts_t *texts,
         core_string_t text)
{
    if (texts) {
        put_u32(out, text.data && text.u8_bytes
            ? texts->intern(texts->context, text) : 0);
        return;
    }
    put_u32(out, text.data ? (uint32_t)text.u8_bytes : 0);
    if (text.data && text.u8_bytes)
        core_buffer_append(out, text.data, text.u8_bytes);
}

static void
put_global(core_buffer_t *out, const ir_summary_texts_t *texts,
           const ir_global_t *global)
{
    const ir_global_t *identity = global ? ir_global_identity(global)
                                         : nullptr;
    put_text(out, texts, identity && identity->owner
        ? ir_module_name(identity->owner) : (core_string_t){0});
    put_text(out, texts, identity ? identity->name : (core_string_t){0});
}


static void
put_path(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
         const ir_spath_t *path)
{
    put_u8(out, path->root);
    put_u32(out, path->param);
    put_global(out, texts, named_root(path->root) ? path->global : nullptr);
    put_text(out, texts, types && path->type
        ? ir_types_text(types, path->type) : (core_string_t){0});
    put_u8(out, path->flags);
    put_u8(out, path->output);
    put_u32(out, path->depth);
    for (uint32_t s = 0; s < path->depth; s++)
        put_text(out, texts, path->steps[s]);
}

static void
put_paths(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
          const path_list_t *paths)
{
    put_u32(out, (uint32_t)paths->len);
    for (size_t i = 0; i < paths->len; i++)
        put_path(out, texts, types, &paths->data[i]);
}

static void
put_bits(core_buffer_t *out, const flow_bits_t *bits)
{
    put_u32(out, (uint32_t)bits->len);
    for (size_t w = 0; w < bits->len; w++) {
        put_u32(out, (uint32_t)bits->data[w]);
        put_u32(out, (uint32_t)(bits->data[w] >> 32));
    }
}

static void
put_flows(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
          const flow_table_t *flows)
{
    put_u32(out, (uint32_t)flows->sources.len);
    for (size_t s = 0; s < flows->sources.len; s++)
        put_path(out, texts, types, flows->sources.data[s]);
    put_u32(out, (uint32_t)flows->dests.len);
    for (size_t d = 0; d < flows->dests.len; d++) {
        put_path(out, texts, types, flows->dests.data[d].to);
        put_bits(out, &flows->dests.data[d].data);
        put_bits(out, &flows->dests.data[d].control);
        put_bits(out, &flows->dests.data[d].alias);
        put_bits(out, &flows->dests.data[d].copy);
    }
}

static bool
encode_summary(const ir_summary_t *summary, core_buffer_t *out,
               const ir_summary_texts_t *texts, bool positions)
{
    if (!summary || !out || (texts && !texts->intern)) return false;
    th_ctx *types = summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr;
    core_buffer_append(out, !positions ? "IRSI" : texts ? "IRSN" : "IRSM",
                       4);
    put_u8(out, SUMMARY_CODEC_VERSION);
    put_u32(out, summary->flags);
    put_u8(out, summary->traps);
    put_u32(out, positions ? (uint32_t)summary->trap_sites.len : 0);
    for (size_t i = 0; positions && i < summary->trap_sites.len; i++) {
        const ir_strap_site_t *site = &summary->trap_sites.data[i];
        put_u8(out, site->kind);
        put_u32(out, site->line);
        put_u32(out, site->column);
    }
    put_u32(out, (uint32_t)summary->effects.len);
    for (size_t i = 0; i < summary->effects.len; i++)
        put_text(out, texts, summary->effects.data[i]);
    put_paths(out, texts, types, &summary->paths);
    put_u8(out, (uint8_t)(summary->results_known
                          | summary->returns_external << 1));
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_RESULTS]);
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_STORES]);
    put_u32(out, positions ? (uint32_t)summary->witnesses.len : 0);
    for (size_t i = 0; positions && i < summary->witnesses.len; i++) {
        const ir_switness_t *witness = &summary->witnesses.data[i];
        put_u32(out, witness->flag);
        put_text(out, texts, witness->module);
        put_text(out, texts, witness->name);
        put_u32(out, witness->line);
        put_u32(out, witness->column);
    }
    put_u32(out, (uint32_t)summary->heaps.len);
    for (size_t i = 0; i < summary->heaps.len; i++) {
        const ir_sheap_t *heap = &summary->heaps.data[i];
        put_u32(out, heap->origin);
        put_text(out, texts, types && heap->type
            ? ir_types_text(types, heap->type) : (core_string_t){0});
        put_u8(out, heap->elements);
        put_text(out, texts, positions ? heap->module : (core_string_t){0});
        put_text(out, texts, positions ? heap->name : (core_string_t){0});
        put_u32(out, positions ? heap->line : 0);
        put_u32(out, positions ? heap->column : 0);
    }
    return true;
}

bool
ir_summary_encode(const ir_summary_t *summary, core_buffer_t *out)
{
    return encode_summary(summary, out, nullptr, true);
}

bool
ir_summary_encode_identity(const ir_summary_t *summary, core_buffer_t *out)
{
    return encode_summary(summary, out, nullptr, false);
}

bool
ir_summary_encode_with(const ir_summary_t *summary, core_buffer_t *out,
                       const ir_summary_texts_t *texts)
{
    return texts && encode_summary(summary, out, texts, true);
}

static uint8_t
get_u8(summary_reader_t *r)
{
    if (r->failed || r->at >= r->size) {
        r->failed = true;
        return 0;
    }
    return r->data[r->at++];
}

static uint32_t
get_u32(summary_reader_t *r)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= (uint32_t)get_u8(r) << (8 * i);
    return value;
}

static core_string_t
get_text(summary_reader_t *r)
{
    if (r->texts) {
        uint32_t reference = get_u32(r);
        return reference && !r->failed
            ? r->texts->lookup(r->texts->context, reference)
            : (core_string_t){0};
    }
    uint32_t length = get_u32(r);
    if (r->failed || length > r->size - r->at) {
        r->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = length
        ? core_string_borrow((const char *)r->data + r->at, length)
        : (core_string_t){0};
    r->at += length;
    return text;
}

static const ir_global_t *
get_global(summary_reader_t *r)
{
    core_string_t module = get_text(r);
    core_string_t name   = get_text(r);
    return !r->failed && module.data && name.data && r->resolver
        && r->resolver->global
        ? r->resolver->global(r->resolver->context, module, name) : nullptr;
}


static ir_spath_t
get_path(summary_reader_t *r)
{
    ir_spath_t path = {.root = (ir_splace_kind_t)get_u8(r)};
    path.param  = get_u32(r);
    path.global = get_global(r);
    core_string_t type = get_text(r);
    if (r->types && type.u8_bytes)
        path.type = ir_type_from_text(r->types, type);
    path.flags  = get_u8(r);
    path.output = get_u8(r);
    uint32_t depth = get_u32(r);
    if (path.root > IR_SPLACE_SOURCE) r->failed = true;
    core_list_t(core_string_t) steps = core_list_new(core_string_t);
    for (uint32_t s = 0; !r->failed && s < depth; s++) {
        core_string_t step = get_text(r);
        if (!step.u8_bytes) r->failed = true;
        else core_list_push(steps, step);
    }
    if (!r->failed) {
        path.depth = depth;
        path.steps = steps_make(steps.data, depth);
    }
    core_list_free(steps);
    return path;
}

static void
get_paths(summary_reader_t *r, path_list_t *paths)
{
    uint32_t count = get_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        ir_spath_t path = get_path(r);
        if (!r->failed) add_path_to(paths, &path);
    }
}

static void
get_bits(summary_reader_t *r, flow_table_t *flows, const ir_spath_t *to,
         const path_list_t *sources, ir_sflow_kind_t kind)
{
    uint32_t words = get_u32(r);
    for (uint32_t w = 0; !r->failed && w < words; w++) {
        uint64_t low  = get_u32(r);
        uint64_t word = low | (uint64_t)get_u32(r) << 32;
        for (uint32_t b = 0; !r->failed && b < 64; b++) {
            if (!(word >> b & 1)) continue;
            size_t source = (size_t)w * 64 + b;
            if (source >= sources->len) r->failed = true;
            else flow_put(flows, to, &sources->data[source], kind);
        }
    }
}

static void
get_flows(summary_reader_t *r, flow_table_t *flows)
{
    path_list_t sources = core_list_new(ir_spath_t);
    uint32_t    count   = get_u32(r);
    for (uint32_t s = 0; !r->failed && s < count; s++) {
        ir_spath_t path = get_path(r);
        core_list_push(sources, path);
    }
    uint32_t dests = get_u32(r);
    for (uint32_t d = 0; !r->failed && d < dests; d++) {
        ir_spath_t to = get_path(r);
        get_bits(r, flows, &to, &sources, IR_SFLOW_DATA);
        get_bits(r, flows, &to, &sources, IR_SFLOW_CONTROL);
        get_bits(r, flows, &to, &sources, IR_SFLOW_ALIAS);
        get_bits(r, flows, &to, &sources, IR_SFLOW_COPY);
    }
    core_list_free(sources);
}

static core_string_t
get_owned_text(summary_reader_t *r, const ir_func_t *function)
{
    core_string_t text = get_text(r);
    return text.u8_bytes && function && !r->failed
        ? ir_strdup(function->owner->ctx, text) : (core_string_t){0};
}

static ir_switness_t
get_witness(summary_reader_t *r, const ir_func_t *function)
{
    ir_switness_t witness = {.flag = get_u32(r)};
    witness.module = get_owned_text(r, function);
    witness.name   = get_owned_text(r, function);
    witness.line   = get_u32(r);
    witness.column = get_u32(r);
    const ir_summary_resolver_t *resolver = r->resolver;
    if (witness.module.data && witness.name.data && resolver
        && resolver->function)
        witness.callee = resolver->function(resolver->context,
                                            witness.module, witness.name);
    return witness;
}

static ir_summary_t *
decode_summary(ir_summaries_t *summaries, const ir_func_t *function,
               const uint8_t *data, size_t size,
               const ir_summary_resolver_t *resolver,
               const ir_summary_texts_t *texts)
{
    summary_reader_t r = {.data = data, .size = size, .texts = texts,
                          .resolver = resolver};
    if (!data || size < 5 || (texts && !texts->lookup)
        || memcmp(data, texts ? "IRSN" : "IRSM", 4)
        || data[4] != SUMMARY_CODEC_VERSION) return nullptr;
    r.at = 5;
    th_ctx *types = function && function->owner
        ? ir_module_types(function->owner) : nullptr;
    r.types = types;
    ir_summary_t *summary = ir_summary_new(nullptr, function);
    summary->flags      = get_u32(&r);
    summary->traps      = get_u8(&r) & IR_STRAP_ALL;
    uint32_t trap_sites = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < trap_sites; i++) {
        ir_strap_site_t site = {.kind = get_u8(&r) & IR_STRAP_ALL};
        site.line   = get_u32(&r);
        site.column = get_u32(&r);
        core_list_push(summary->trap_sites, site);
    }
    uint32_t effects = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < effects; i++) {
        core_string_t name = get_text(&r);
        if (!name.u8_bytes || !ir_summary_add_effect(summary, name))
            r.failed = true;
    }
    get_paths(&r, &summary->paths);
    uint8_t results = get_u8(&r);
    summary->results_known    = results & 1;
    summary->returns_external = results >> 1 & 1;
    get_flows(&r, &summary->flows[IR_SFLOW_RESULTS]);
    get_flows(&r, &summary->flows[IR_SFLOW_STORES]);
    uint32_t witnesses = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < witnesses; i++) {
        ir_switness_t witness = get_witness(&r, function);
        if (!r.failed) core_list_push(summary->witnesses, witness);
    }
    uint32_t heaps = get_u32(&r);
    for (uint32_t i = 0; !r.failed && i < heaps; i++) {
        ir_sheap_t    heap = {.origin = get_u32(&r)};
        core_string_t type = get_text(&r);
        if (r.types && type.u8_bytes)
            heap.type = ir_type_from_text(r.types, type);
        heap.elements = get_u8(&r) != 0;
        heap.module   = get_owned_text(&r, function);
        heap.name   = get_owned_text(&r, function);
        heap.line   = get_u32(&r);
        heap.column = get_u32(&r);
        if (!r.failed) core_list_push(summary->heaps, heap);
    }
    if (r.failed || r.at != r.size) {
        summary_free(summary);
        return nullptr;
    }
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

ir_summary_t *
ir_summary_decode(ir_summaries_t *summaries, const ir_func_t *function,
                  const uint8_t *data, size_t size,
                  const ir_summary_resolver_t *resolver)
{
    return decode_summary(summaries, function, data, size, resolver,
                          nullptr);
}

ir_summary_t *
ir_summary_decode_with(ir_summaries_t *summaries, const ir_func_t *function,
                       const uint8_t *data, size_t size,
                       const ir_summary_resolver_t *resolver,
                       const ir_summary_texts_t *texts)
{
    return texts ? decode_summary(summaries, function, data, size, resolver,
                                  texts)
                 : nullptr;
}
