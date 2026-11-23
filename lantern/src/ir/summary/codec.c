#include "internal.h"

static const core_string_t callbacks_prefix =
    CORE_STRING_STATIC("ir.callbacks:");

enum { CALLBACKS_TAG_DIGITS = 8 + 8 + 2 };

static bool
callbacks_tag(core_string_t name)
{
    return core_string_starts_with(name, callbacks_prefix);
}

static void
put_hex(core_buffer_t *text, uint64_t value, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int d = digits - 1; d >= 0; d--)
        core_buffer_putc(text, hex[(value >> (4 * d)) & 0xf]);
}

static void
callbacks_tag_text(const ir_summary_t *summary, core_buffer_t *text)
{
    core_buffer_append_string(text, callbacks_prefix);
    put_hex(text, summary->callback_params, 8);
    put_hex(text, summary->own_flags, 8);
    put_hex(text, summary->own_traps, 2);
}

static bool
read_callbacks_tag(core_string_t name, ir_summary_t *summary)
{
    size_t at = callbacks_prefix.u8_bytes;
    if (name.u8_bytes != at + CALLBACKS_TAG_DIGITS) return false;
    uint64_t fields[3] = {0};
    const int widths[3] = {8, 8, 2};
    for (int f = 0; f < 3; f++)
        for (int d = 0; d < widths[f]; d++, at++) {
            char c = name.data[at];
            int digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return false;
            fields[f] = fields[f] << 4 | (uint64_t)digit;
        }
    summary->callback_params = (uint32_t)fields[0];
    summary->own_flags = (uint32_t)fields[1];
    summary->own_traps = (uint8_t)fields[2] & IR_STRAP_ALL;
    return true;
}

typedef struct {
    core_byte_cursor_t           in;
    const ir_summary_texts_t    *texts;
    const ir_summary_resolver_t *resolver;
} summary_reader_t;

typedef struct {
    th_ctx           *ctx;
    th_type_writer_t *writer;
} type_sink_t;

static void
put_type(core_buffer_t *out, const type_sink_t *sink, th_type *type)
{
    if (sink && sink->writer)
        core_put_u32(out, type ? th_type_writer_add(sink->writer,
            th_is_closed(sink->ctx, type) ? type
                : th_type_clone_fresh(sink->ctx, type)) : 0);
    else
        core_put_u64(out, type && sink && sink->ctx ? th_hash(sink->ctx, type) : 0);
}

static void
put_text(core_buffer_t *out, const ir_summary_texts_t *texts,
         core_string_t text)
{
    if (texts)
        core_put_u32(out, text.data && text.u8_bytes
            ? texts->intern(texts->context, text) : 0);
    else core_put_text(out, text);
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
put_path(core_buffer_t *out, const ir_summary_texts_t *texts,
         const type_sink_t *types, const ir_spath_t *path)
{
    core_put_u8(out, path->root);
    core_put_u32(out, path->param);
    put_global(out, texts, named_root(path->root) ? path->global : nullptr);
    put_type(out, types, path->type);
    core_put_u8(out, path->flags);
    core_put_u8(out, path->output);
    core_put_u32(out, path->depth);
    for (uint32_t s = 0; s < path->depth; s++)
        put_text(out, texts, path->steps[s]);
}

static void
put_paths(core_buffer_t *out, const ir_summary_texts_t *texts,
          const type_sink_t *types, const path_list_t *paths)
{
    core_put_u32(out, (uint32_t)paths->len);
    for (size_t i = 0; i < paths->len; i++)
        put_path(out, texts, types, &paths->data[i]);
}

static void
put_bits(core_buffer_t *out, const flow_bits_t *bits)
{
    core_put_u32(out, (uint32_t)bits->len);
    for (size_t w = 0; w < bits->len; w++) {
        core_put_u32(out, (uint32_t)bits->data[w]);
        core_put_u32(out, (uint32_t)(bits->data[w] >> 32));
    }
}

typedef struct {
    core_buffer_t bytes;
    size_t        index;
} encoded_item_t;

static int
compare_encoded(const void *left, const void *right)
{
    const encoded_item_t *a = left, *b = right;
    size_t shorter = a->bytes.byte_len < b->bytes.byte_len
        ? a->bytes.byte_len : b->bytes.byte_len;
    int order = shorter ? memcmp(a->bytes.data, b->bytes.data, shorter) : 0;
    if (order) return order;
    return (a->bytes.byte_len > b->bytes.byte_len)
        - (a->bytes.byte_len < b->bytes.byte_len);
}

static void
put_ranked_bits(core_buffer_t *out, const flow_bits_t *bits,
                const uint32_t *rank, size_t count)
{
    flow_bits_t ranked = core_list_new(uint64_t);
    for (size_t w = 0; w < bits->len && w * 64 < count; w++)
        for (uint64_t word = bits->data[w]; word; word &= word - 1) {
            size_t s = w * 64 + (size_t)__builtin_ctzll(word);
            if (s < count) (void)bit_set(&ranked, rank[s]);
        }
    put_bits(out, &ranked);
    core_list_free(ranked);
}

static bool
dest_flows(const flow_dest_t *dest)
{
    return bit_count(&dest->data) || bit_count(&dest->control)
        || bit_count(&dest->alias) || bit_count(&dest->copy);
}

static void
put_dest_bits(core_buffer_t *out, const flow_dest_t *dest,
              const uint32_t *rank, size_t count)
{
    put_ranked_bits(out, &dest->data, rank, count);
    put_ranked_bits(out, &dest->control, rank, count);
    put_ranked_bits(out, &dest->alias, rank, count);
    put_ranked_bits(out, &dest->copy, rank, count);
}

static void
put_flows(core_buffer_t *out, const ir_summary_texts_t *texts,
          const type_sink_t *types, const flow_table_t *flows)
{
    type_sink_t     key     = {types ? types->ctx : nullptr, nullptr};
    size_t          count   = flows->sources.len;
    encoded_item_t *sources = core_alloc_array(encoded_item_t, count + 1);
    uint32_t       *rank    = core_alloc_array(uint32_t, count + 1);
    size_t          used    = 0;
    flow_bits_t referenced = core_list_new(uint64_t);
    for (size_t d = 0; d < flows->dests.len; d++) {
        const flow_dest_t *dest  = &flows->dests.data[d];
        const flow_bits_t *kinds[] = {&dest->data, &dest->control,
                                      &dest->alias, &dest->copy};
        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            while (referenced.len < kinds[k]->len)
                core_list_push(referenced, UINT64_C(0));
            for (size_t w = 0; w < kinds[k]->len; w++)
                referenced.data[w] |= kinds[k]->data[w];
        }
    }
    for (size_t s = 0; s < count; s++) {
        if (!bit_has(&referenced, (uint32_t)s)) continue;
        sources[used].index = s;
        put_path(&sources[used].bytes, nullptr, &key, flows->sources.data[s]);
        used++;
    }
    if (used > 1)
        qsort(sources, used, sizeof(encoded_item_t), compare_encoded);
    core_put_u32(out, (uint32_t)used);
    for (size_t k = 0; k < used; k++) {
        rank[sources[k].index] = (uint32_t)k;
        put_path(out, texts, types, flows->sources.data[sources[k].index]);
    }
    encoded_item_t *dests = core_alloc_array(encoded_item_t,
                                             flows->dests.len + 1);
    size_t          kept  = 0;
    for (size_t d = 0; d < flows->dests.len; d++) {
        const flow_dest_t *dest = &flows->dests.data[d];
        if (!dest_flows(dest)) continue;
        put_path(&dests[kept].bytes, nullptr, &key, dest->to);
        put_dest_bits(&dests[kept].bytes, dest, rank, count);
        dests[kept++].index = d;
    }
    if (kept > 1) qsort(dests, kept, sizeof(encoded_item_t), compare_encoded);
    core_put_u32(out, (uint32_t)kept);
    for (size_t k = 0; k < kept; k++) {
        const flow_dest_t *dest = &flows->dests.data[dests[k].index];
        put_path(out, texts, types, dest->to);
        put_dest_bits(out, dest, rank, count);
    }
    for (size_t k = 0; k < used; k++) core_buffer_release(&sources[k].bytes);
    for (size_t k = 0; k < kept; k++) core_buffer_release(&dests[k].bytes);
    core_list_free(referenced);
    core_free(sources);
    core_free(dests);
    core_free(rank);
}

static int
heap_order(const void *left, const void *right)
{
    const ir_sheap_t *a = left, *b = right;
    return (a->origin > b->origin) - (a->origin < b->origin);
}

static bool
encode_summary(const ir_summary_t *summary, core_buffer_t *out,
               const ir_summary_texts_t *texts)
{
    if (!summary || !out || (texts && !texts->intern)) return false;
    type_sink_t sink = {summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr,
        texts ? texts->types : nullptr};
    const type_sink_t *types = &sink;
    core_put_u32(out, summary->flags);
    core_put_u8(out, summary->traps);
    core_put_u32(out, (uint32_t)summary->trap_sites.len);
    for (size_t i = 0; i < summary->trap_sites.len; i++) {
        const ir_strap_site_t *site = &summary->trap_sites.data[i];
        core_put_u8(out, site->kind);
        core_put_u32(out, site->line);
        core_put_u32(out, site->column);
    }
    bool callbacks = summary->callback_params != 0;
    core_put_u32(out, (uint32_t)summary->effects.len + (callbacks ? 1 : 0));
    for (size_t i = 0; i < summary->effects.len; i++)
        put_text(out, texts, summary->effects.data[i]);
    if (callbacks) {
        core_buffer_t tag = {0};
        callbacks_tag_text(summary, &tag);
        put_text(out, texts, core_buffer_string(&tag));
        core_buffer_release(&tag);
    }
    put_paths(out, texts, types, &summary->paths);
    core_put_u8(out, (uint8_t)(summary->results_known
                          | summary->returns_external << 1));
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_RESULTS]);
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_STORES]);
    core_put_u32(out, (uint32_t)summary->witnesses.len);
    for (size_t i = 0; i < summary->witnesses.len; i++) {
        const ir_switness_t *witness = &summary->witnesses.data[i];
        core_put_u32(out, witness->flag);
        put_text(out, texts, witness->module);
        put_text(out, texts, witness->name);
        core_put_u32(out, witness->line);
        core_put_u32(out, witness->column);
    }
    size_t heap_count = summary->heaps.len;
    ir_sheap_t *heaps = core_alloc_array(ir_sheap_t, heap_count + 1);
    if (heap_count)
        memcpy(heaps, summary->heaps.data, heap_count * sizeof(*heaps));
    qsort(heaps, heap_count, sizeof(*heaps), heap_order);
    core_put_u32(out, (uint32_t)heap_count);
    for (size_t i = 0; i < heap_count; i++) {
        const ir_sheap_t *heap = &heaps[i];
        core_put_u32(out, heap->origin);
        put_type(out, types, heap->type);
        core_put_u8(out, heap->elements);
        put_text(out, texts, heap->module);
        put_text(out, texts, heap->name);
        core_put_u32(out, heap->line);
        core_put_u32(out, heap->column);
    }
    core_free(heaps);
    core_put_u32(out, (uint32_t)summary->fields.len);
    for (size_t i = 0; i < summary->fields.len; i++) {
        const ir_sfield_t *field = &summary->fields.data[i];
        put_type(out, types, field->record);
        put_text(out, texts, field->name);
        core_put_u8(out, field->flags);
    }
    return true;
}

static uint64_t
points_to_content(const ir_summary_t *summary)
{
    if (summary->stored) return summary->stable;
    type_sink_t sink = {summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr, nullptr};
    const type_sink_t *types = &sink;
    core_buffer_t out = {0};
    core_put_u32(&out, summary->flags
        & (IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN | IR_SUMMARY_MAY_FAIL
           | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS));
    put_paths(&out, nullptr, types, &summary->paths);
    core_put_u8(&out, (uint8_t)(summary->results_known
                           | summary->returns_external << 1));
    put_flows(&out, nullptr, types, &summary->flows[IR_SFLOW_RESULTS]);
    put_flows(&out, nullptr, types, &summary->flows[IR_SFLOW_STORES]);
    core_put_u32(&out, (uint32_t)summary->heaps.len);
    for (size_t i = 0; i < summary->heaps.len; i++) {
        const ir_sheap_t *heap = &summary->heaps.data[i];
        core_put_u32(&out, heap->origin);
        put_type(&out, types, heap->type);
        core_put_u8(&out, heap->elements);
        put_text(&out, nullptr, heap->module);
        put_text(&out, nullptr, heap->name);
        core_put_u32(&out, heap->line);
        core_put_u32(&out, heap->column);
    }
    uint64_t hash = core_xxh3_64bits_raw(out.data ? out.data : "",
                                         out.byte_len) | 1;
    core_buffer_release(&out);
    return hash;
}

bool
ir_summary_encode(const ir_summary_t *summary, core_buffer_t *out)
{
    return encode_summary(summary, out, nullptr);
}

bool
ir_summary_encode_with(const ir_summary_t *summary, core_buffer_t *out,
                       const ir_summary_texts_t *texts)
{
    return texts && encode_summary(summary, out, texts);
}

static th_type *
get_type(summary_reader_t *r)
{
    uint32_t index = core_cursor_u32(&r->in);
    th_type *type = index ? th_type_reader_get(r->texts->reader, index)
                          : nullptr;
    if (index && !type) r->in.failed = true;
    return type;
}

static core_string_t
get_text(summary_reader_t *r)
{
    uint32_t reference = core_cursor_u32(&r->in);
    return reference && !r->in.failed
        ? r->texts->lookup(r->texts->context, reference)
        : (core_string_t){0};
}

static const ir_global_t *
get_global(summary_reader_t *r)
{
    core_string_t module = get_text(r);
    core_string_t name   = get_text(r);
    return !r->in.failed && module.data && name.data && r->resolver
        && r->resolver->global
        ? r->resolver->global(r->resolver->context, module, name) : nullptr;
}


static ir_spath_t
get_path(summary_reader_t *r)
{
    ir_spath_t path = {.root = (ir_splace_kind_t)core_cursor_u8(&r->in)};
    path.param  = core_cursor_u32(&r->in);
    path.global = get_global(r);
    path.type   = get_type(r);
    path.flags  = core_cursor_u8(&r->in);
    path.output = core_cursor_u8(&r->in);
    uint32_t depth = core_cursor_u32(&r->in);
    if (path.root > IR_SPLACE_SOURCE) r->in.failed = true;
    core_list_t(core_string_t) steps = core_list_new(core_string_t);
    for (uint32_t s = 0; !r->in.failed && s < depth; s++) {
        core_string_t step = get_text(r);
        if (!step.u8_bytes) r->in.failed = true;
        else core_list_push(steps, step);
    }
    if (!r->in.failed) {
        path.depth = depth;
        path.steps = steps_make(steps.data, depth);
    }
    core_list_free(steps);
    return path;
}

static void
get_paths(summary_reader_t *r, path_list_t *paths)
{
    uint32_t count = core_cursor_u32(&r->in);
    for (uint32_t i = 0; !r->in.failed && i < count; i++) {
        ir_spath_t path = get_path(r);
        if (!r->in.failed) add_path_to(paths, &path);
    }
}

static void
get_bits(summary_reader_t *r, flow_table_t *flows, flow_dest_t **dest,
         const ir_spath_t *to, const path_list_t *sources, uint32_t *mapped,
         ir_sflow_kind_t kind)
{
    uint32_t words = core_cursor_u32(&r->in);
    for (uint32_t w = 0; !r->in.failed && w < words; w++) {
        uint64_t low  = core_cursor_u32(&r->in);
        uint64_t word = low | (uint64_t)core_cursor_u32(&r->in) << 32;
        for (; !r->in.failed && word; word &= word - 1) {
            size_t source = (size_t)w * 64 + (size_t)__builtin_ctzll(word);
            if (source >= sources->len) {
                r->in.failed = true;
                break;
            }
            if (mapped[source] == UINT32_MAX)
                mapped[source] = flow_source(flows, &sources->data[source]);
            if (!*dest) {
                ir_spath_t key = canonical_path(to);
                *dest = flow_dest(flows, &key);
            }
            bit_set(kind_bits(*dest, kind), mapped[source]);
        }
    }
}

static void
get_flows(summary_reader_t *r, flow_table_t *flows)
{
    path_list_t sources = core_list_new(ir_spath_t);
    uint32_t    count   = core_cursor_u32(&r->in);
    for (uint32_t s = 0; !r->in.failed && s < count; s++) {
        ir_spath_t path = get_path(r);
        core_list_push(sources, path);
    }
    uint32_t *mapped = core_alloc_array(uint32_t, sources.len + 1);
    for (size_t s = 0; s < sources.len; s++) mapped[s] = UINT32_MAX;
    uint32_t dests = core_cursor_u32(&r->in);
    for (uint32_t d = 0; !r->in.failed && d < dests; d++) {
        ir_spath_t   to   = get_path(r);
        flow_dest_t *dest = nullptr;
        get_bits(r, flows, &dest, &to, &sources, mapped, IR_SFLOW_DATA);
        get_bits(r, flows, &dest, &to, &sources, mapped, IR_SFLOW_CONTROL);
        get_bits(r, flows, &dest, &to, &sources, mapped, IR_SFLOW_ALIAS);
        get_bits(r, flows, &dest, &to, &sources, mapped, IR_SFLOW_COPY);
    }
    core_free(mapped);
    core_list_free(sources);
}

static core_string_t
get_owned_text(summary_reader_t *r, const ir_func_t *function)
{
    core_string_t text = get_text(r);
    return text.u8_bytes && function && !r->in.failed
        ? ir_strdup(function->owner->ctx, text) : (core_string_t){0};
}

static ir_switness_t
get_witness(summary_reader_t *r, const ir_func_t *function)
{
    ir_switness_t witness = {.flag = core_cursor_u32(&r->in)};
    witness.module = get_owned_text(r, function);
    witness.name   = get_owned_text(r, function);
    witness.line   = core_cursor_u32(&r->in);
    witness.column = core_cursor_u32(&r->in);
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
    summary_reader_t r = {.in = core_cursor(data, size), .texts = texts,
                          .resolver = resolver};
    if (!data || !texts || !texts->lookup) return nullptr;
    ir_summary_t *summary = ir_summary_new(nullptr, function);
    summary->flags      = core_cursor_u32(&r.in);
    summary->traps      = core_cursor_u8(&r.in) & IR_STRAP_ALL;
    uint32_t trap_sites = core_cursor_u32(&r.in);
    for (uint32_t i = 0; !r.in.failed && i < trap_sites; i++) {
        ir_strap_site_t site = {.kind = core_cursor_u8(&r.in) & IR_STRAP_ALL};
        site.line   = core_cursor_u32(&r.in);
        site.column = core_cursor_u32(&r.in);
        core_list_push(summary->trap_sites, site);
    }
    uint32_t effects = core_cursor_u32(&r.in);
    for (uint32_t i = 0; !r.in.failed && i < effects; i++) {
        core_string_t name = get_text(&r);
        if (callbacks_tag(name)) {
            if (!read_callbacks_tag(name, summary)) r.in.failed = true;
            continue;
        }
        if (!name.u8_bytes || !ir_summary_add_effect(summary, name))
            r.in.failed = true;
    }
    get_paths(&r, &summary->paths);
    uint8_t results = core_cursor_u8(&r.in);
    summary->results_known    = results & 1;
    summary->returns_external = results >> 1 & 1;
    get_flows(&r, &summary->flows[IR_SFLOW_RESULTS]);
    get_flows(&r, &summary->flows[IR_SFLOW_STORES]);
    uint32_t witnesses = core_cursor_u32(&r.in);
    for (uint32_t i = 0; !r.in.failed && i < witnesses; i++) {
        ir_switness_t witness = get_witness(&r, function);
        if (!r.in.failed) core_list_push(summary->witnesses, witness);
    }
    uint32_t heaps = core_cursor_u32(&r.in);
    for (uint32_t i = 0; !r.in.failed && i < heaps; i++) {
        ir_sheap_t heap = {.origin = core_cursor_u32(&r.in)};
        heap.type     = get_type(&r);
        heap.elements = core_cursor_u8(&r.in) != 0;
        heap.module   = get_owned_text(&r, function);
        heap.name   = get_owned_text(&r, function);
        heap.line   = core_cursor_u32(&r.in);
        heap.column = core_cursor_u32(&r.in);
        if (!r.in.failed) core_list_push(summary->heaps, heap);
    }
    uint32_t fields = core_cursor_u32(&r.in);
    for (uint32_t i = 0; !r.in.failed && i < fields; i++) {
        ir_sfield_t field = {.record = get_type(&r)};
        field.name  = get_owned_text(&r, function);
        field.flags = core_cursor_u8(&r.in);
        if (!r.in.failed) core_list_push(summary->fields, field);
    }
    if (r.in.failed || r.in.at != r.in.end) {
        summary_free(summary);
        return nullptr;
    }
    if (summaries) core_list_push(summaries->owned, summary);
    return summary;
}

ir_summary_t *
ir_summary_decode_with(ir_summaries_t *summaries, const ir_func_t *function,
                       const uint8_t *data, size_t size,
                       const ir_summary_resolver_t *resolver,
                       const ir_summary_texts_t *texts)
{
    return decode_summary(summaries, function, data, size, resolver, texts);
}
