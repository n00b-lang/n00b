#include "internal.h"

// 2: callee storage named by an origin refined by its instantiated type.
// 3: an open heap type is written with the function's signature.
enum { SUMMARY_CODEC_VERSION = 3 };

// A heap type with type variables names the function's own: a caller
// instantiates it by binding the function's signature at the call
// (heap_instance). Text names a variable only within one printed type, so
// such a type is written in one type with the signature,
// `(signature) -> heap`, after this prefix, and read back with its
// variables bound to the signature's (heap_type_text, heap_type_from_text).
static const core_string_t heap_signature_prefix =
    CORE_STRING_STATIC("with-signature ");

static core_string_t
heap_type_text(th_ctx *types, const ir_func_t *function, th_type *type)
{
    if (!types || !type) return (core_string_t){0};
    if (th_is_closed(types, type) || !function || !function->type
        || !function->owner)
        return ir_types_text(types, type);
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param signature = {.mode = TH_IN, .type = function->type};
    th_type *pair = nil_type ? th_func_with_error(types, type, nil_type,
        &signature, 1, nullptr, nullptr, 0) : nullptr;
    if (!pair) return ir_types_text(types, type);
    core_buffer_t text = {0};
    core_buffer_append_string(&text, heap_signature_prefix);
    core_buffer_append_string(&text, ir_types_text(types, pair));
    core_string_t kept = ir_strdup(function->owner->ctx,
                                   core_buffer_string(&text));
    core_buffer_release(&text);
    return kept;
}

typedef struct {
    core_list_t(summary_type_ptr_t) vars;
    core_list_t(summary_type_ptr_t) values;
} signature_binding_t;

// Binds each variable of `pattern` to what stands in its place in
// `actual`, which has the same shape.
static bool
bind_signature(signature_binding_t *b, th_type *pattern, th_type *actual,
               uint32_t depth)
{
    pattern = th_resolve(pattern);
    actual  = th_resolve(actual);
    if (!pattern || !actual) return pattern == actual;
    if (depth > 64) return false;
    if (th_kind_of(pattern) == TH_VAR) {
        for (size_t v = 0; v < b->vars.len; v++)
            if (b->vars.data[v] == pattern)
                return b->values.data[v] == actual;
        core_list_push(b->vars, pattern);
        core_list_push(b->values, actual);
        return true;
    }
    if (pattern == actual) return true;
    if (!th_same_shape(pattern, actual)
        || th_child_count(pattern) != th_child_count(actual))
        return false;
    for (size_t c = 0; c < th_child_count(pattern); c++)
        if (!bind_signature(b, th_child(pattern, c), th_child(actual, c),
                            depth + 1))
            return false;
    return true;
}

static th_type *
heap_type_from_text(th_ctx *types, const ir_func_t *function,
                    core_string_t text)
{
    if (!core_string_starts_with(text, heap_signature_prefix))
        return ir_type_from_text(types, text);
    th_type *pair = function && function->type ? ir_type_from_text(types,
        core_string_slice(text, heap_signature_prefix.u8_bytes,
                          text.u8_bytes - heap_signature_prefix.u8_bytes))
        : nullptr;
    pair = pair ? th_resolve(pair) : nullptr;
    const th_param *signature = pair && th_kind_of(pair) == TH_FUNCTION
            && th_func_nparams(pair) == 1
        ? th_func_param(pair, 0) : nullptr;
    th_type *heap = signature ? th_func_ret(pair) : nullptr;
    if (!heap) return nullptr;
    signature_binding_t binding = {core_list_new(summary_type_ptr_t),
                                   core_list_new(summary_type_ptr_t)};
    th_type *type = bind_signature(&binding, signature->type,
                                   function->type, 0)
        ? th_substitute(types, heap, binding.vars.data, binding.values.data,
                        binding.vars.len)
        : nullptr;
    core_list_free(binding.vars);
    core_list_free(binding.values);
    return type;
}

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

// An item as encoded, and the key it's sorted by: the same encoding with
// its texts spelled out. Encoded through an interner, a text is a number
// in the order the module's texts were first met, which depends on what
// else the module's table holds (the functions a program summarized), so
// sorting by those bytes put the same summary's flows in an order that
// depended on the program.
typedef struct {
    core_buffer_t bytes;
    core_buffer_t key;
    size_t        index;
} encoded_item_t;

static int
compare_encoded(const void *left, const void *right)
{
    const encoded_item_t *a = left, *b = right;
    size_t shorter = a->key.byte_len < b->key.byte_len
        ? a->key.byte_len : b->key.byte_len;
    int order = shorter ? memcmp(a->key.data, b->key.data, shorter) : 0;
    if (order) return order;
    return (a->key.byte_len > b->key.byte_len)
        - (a->key.byte_len < b->key.byte_len);
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
put_flows(core_buffer_t *out, const ir_summary_texts_t *texts, th_ctx *types,
          const flow_table_t *flows)
{
    size_t          count   = flows->sources.len;
    encoded_item_t *sources = core_alloc_array(encoded_item_t, count + 1);
    uint32_t       *rank    = core_alloc_array(uint32_t, count + 1);
    size_t          used    = 0;
    // The sources some destination draws on: every kind of flow into every
    // destination, in one set.
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
        put_path(&sources[used].bytes, texts, types, flows->sources.data[s]);
        put_path(&sources[used].key, nullptr, types, flows->sources.data[s]);
        used++;
    }
    if (used > 1)
        qsort(sources, used, sizeof(encoded_item_t), compare_encoded);
    put_u32(out, (uint32_t)used);
    for (size_t k = 0; k < used; k++) {
        rank[sources[k].index] = (uint32_t)k;
        core_buffer_append(out, sources[k].bytes.data,
                           sources[k].bytes.byte_len);
    }
    encoded_item_t *dests = core_alloc_array(encoded_item_t,
                                             flows->dests.len + 1);
    size_t          kept  = 0;
    for (size_t d = 0; d < flows->dests.len; d++) {
        const flow_dest_t *dest = &flows->dests.data[d];
        if (!dest_flows(dest)) continue;
        core_buffer_t *bytes = &dests[kept].bytes;
        core_buffer_t *key   = &dests[kept].key;
        put_path(bytes, texts, types, dest->to);
        put_path(key, nullptr, types, dest->to);
        const flow_bits_t *kinds[] = {&dest->data, &dest->control,
                                      &dest->alias, &dest->copy};
        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            put_ranked_bits(bytes, kinds[k], rank, count);
            put_ranked_bits(key, kinds[k], rank, count);
        }
        dests[kept++].index = d;
    }
    if (kept > 1) qsort(dests, kept, sizeof(encoded_item_t), compare_encoded);
    put_u32(out, (uint32_t)kept);
    for (size_t k = 0; k < kept; k++)
        core_buffer_append(out, dests[k].bytes.data, dests[k].bytes.byte_len);
    for (size_t k = 0; k < used; k++) {
        core_buffer_release(&sources[k].bytes);
        core_buffer_release(&sources[k].key);
    }
    for (size_t k = 0; k < kept; k++) {
        core_buffer_release(&dests[k].bytes);
        core_buffer_release(&dests[k].key);
    }
    core_list_free(referenced);
    core_free(sources);
    core_free(dests);
    core_free(rank);
}

static int
compare_heap_origins(const void *left, const void *right)
{
    uint32_t a = (*(const ir_sheap_t *const *)left)->origin;
    uint32_t b = (*(const ir_sheap_t *const *)right)->origin;
    return (a > b) - (a < b);
}

static int
compare_witness_flags(const void *left, const void *right)
{
    uint32_t a = (*(const ir_switness_t *const *)left)->flag;
    uint32_t b = (*(const ir_switness_t *const *)right)->flag;
    return (a > b) - (a < b);
}

// A summary's heaps (and witnesses) in a fixed order: by origin (by flag),
// each of which names one. Their order in the summary is the order its
// paths and flows were met in, which depends on whether its callees'
// summaries were computed or decoded from the cache (decoded ones come in
// the codec's sorted order), so encoding them in it made the same summary
// encode differently warm and cold.
static const ir_sheap_t **
sorted_heaps(const ir_summary_t *summary)
{
    const ir_sheap_t **heaps = core_alloc_array(const ir_sheap_t *,
                                                summary->heaps.len + 1);
    for (size_t i = 0; i < summary->heaps.len; i++)
        heaps[i] = &summary->heaps.data[i];
    if (summary->heaps.len > 1)
        qsort(heaps, summary->heaps.len, sizeof(*heaps),
              compare_heap_origins);
    return heaps;
}

static const ir_switness_t **
sorted_witnesses(const ir_summary_t *summary)
{
    const ir_switness_t **witnesses = core_alloc_array(
        const ir_switness_t *, summary->witnesses.len + 1);
    for (size_t i = 0; i < summary->witnesses.len; i++)
        witnesses[i] = &summary->witnesses.data[i];
    if (summary->witnesses.len > 1)
        qsort(witnesses, summary->witnesses.len, sizeof(*witnesses),
              compare_witness_flags);
    return witnesses;
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
    bool callbacks = summary->callback_params != 0;
    put_u32(out, (uint32_t)summary->effects.len + (callbacks ? 1 : 0));
    for (size_t i = 0; i < summary->effects.len; i++)
        put_text(out, texts, summary->effects.data[i]);
    if (callbacks) {
        core_buffer_t tag = {0};
        callbacks_tag_text(summary, &tag);
        put_text(out, texts, core_buffer_string(&tag));
        core_buffer_release(&tag);
    }
    put_paths(out, texts, types, &summary->paths);
    put_u8(out, (uint8_t)(summary->results_known
                          | summary->returns_external << 1));
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_RESULTS]);
    put_flows(out, texts, types, &summary->flows[IR_SFLOW_STORES]);
    put_u32(out, positions ? (uint32_t)summary->witnesses.len : 0);
    const ir_switness_t **witnesses = sorted_witnesses(summary);
    for (size_t i = 0; positions && i < summary->witnesses.len; i++) {
        const ir_switness_t *witness = witnesses[i];
        put_u32(out, witness->flag);
        put_text(out, texts, witness->module);
        put_text(out, texts, witness->name);
        put_u32(out, witness->line);
        put_u32(out, witness->column);
    }
    core_free(witnesses);
    put_u32(out, (uint32_t)summary->heaps.len);
    const ir_sheap_t **heaps = sorted_heaps(summary);
    for (size_t i = 0; i < summary->heaps.len; i++) {
        const ir_sheap_t *heap = heaps[i];
        put_u32(out, heap->origin);
        put_text(out, texts, heap_type_text(types, summary->function,
                                            heap->type));
        put_u8(out, heap->elements);
        put_text(out, texts, positions ? heap->module : (core_string_t){0});
        put_text(out, texts, positions ? heap->name : (core_string_t){0});
        put_u32(out, positions ? heap->line : 0);
        put_u32(out, positions ? heap->column : 0);
    }
    core_free(heaps);
    return true;
}

static uint64_t
points_to_content(const ir_summary_t *summary)
{
    if (summary->stored) return summary->stable;
    th_ctx *types = summary->function && summary->function->owner
        ? ir_module_types(summary->function->owner) : nullptr;
    core_buffer_t out = {0};
    put_u32(&out, summary->flags
        & (IR_SUMMARY_DIVERGES | IR_SUMMARY_NO_RETURN | IR_SUMMARY_MAY_FAIL
           | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS));
    put_paths(&out, nullptr, types, &summary->paths);
    put_u8(&out, (uint8_t)(summary->results_known
                           | summary->returns_external << 1));
    put_flows(&out, nullptr, types, &summary->flows[IR_SFLOW_RESULTS]);
    put_flows(&out, nullptr, types, &summary->flows[IR_SFLOW_STORES]);
    put_u32(&out, (uint32_t)summary->heaps.len);
    const ir_sheap_t **heaps = sorted_heaps(summary);
    for (size_t i = 0; i < summary->heaps.len; i++) {
        const ir_sheap_t *heap = heaps[i];
        put_u32(&out, heap->origin);
        put_text(&out, nullptr, types && heap->type
            ? ir_types_text(types, heap->type) : (core_string_t){0});
        put_u8(&out, heap->elements);
        put_text(&out, nullptr, heap->module);
        put_text(&out, nullptr, heap->name);
        put_u32(&out, heap->line);
        put_u32(&out, heap->column);
    }
    core_free(heaps);
    uint64_t hash = core_xxh3_64bits_raw(out.data ? out.data : "",
                                         out.byte_len) | 1;
    core_buffer_release(&out);
    return hash;
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
get_bits(summary_reader_t *r, flow_table_t *flows, flow_dest_t **dest,
         const ir_spath_t *to, const path_list_t *sources, uint32_t *mapped,
         ir_sflow_kind_t kind)
{
    // As flow_put for each flow, in the same order, but each source and the
    // destination are looked up once.
    uint32_t words = get_u32(r);
    for (uint32_t w = 0; !r->failed && w < words; w++) {
        uint64_t low  = get_u32(r);
        uint64_t word = low | (uint64_t)get_u32(r) << 32;
        for (; !r->failed && word; word &= word - 1) {
            size_t source = (size_t)w * 64 + (size_t)__builtin_ctzll(word);
            if (source >= sources->len) {
                r->failed = true;
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
    uint32_t    count   = get_u32(r);
    for (uint32_t s = 0; !r->failed && s < count; s++) {
        ir_spath_t path = get_path(r);
        core_list_push(sources, path);
    }
    uint32_t *mapped = core_alloc_array(uint32_t, sources.len + 1);
    for (size_t s = 0; s < sources.len; s++) mapped[s] = UINT32_MAX;
    uint32_t dests = get_u32(r);
    for (uint32_t d = 0; !r->failed && d < dests; d++) {
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
        if (callbacks_tag(name)) {
            if (!read_callbacks_tag(name, summary)) r.failed = true;
            continue;
        }
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
            heap.type = heap_type_from_text(r.types, function, type);
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
