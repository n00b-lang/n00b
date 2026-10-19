#include "private/th/internal.h"
#include "lib/hash.h"

static const th_type *as(const th_type *t, th_kind k)
{
    t = th_resolve_c(t);

    return t && t->kind == k ? t : nullptr;
}

bool th_named_info(const th_type *t, core_string_t *name, bool *is_variant,
                   size_t *nargs)
{
    const th_type *n = as(t, TH_NAMED);

    if (!n) {
        return false;
    }

    if (name) {
        *name = n->named.name;
    }

    if (is_variant) {
        *is_variant = n->named.is_variant;
    }

    if (nargs) {
        *nargs = n->named.nargs;
    }

    return true;
}

size_t th_named_nargs(const th_type *t)
{
    const th_type *n = as(t, TH_NAMED);

    return n ? n->named.nargs : 0;
}

const th_arg *th_named_arg(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_NAMED);

    return n && i < n->named.nargs ? &n->named.args[i] : nullptr;
}

bool th_bitint_info(const th_type *t, uint64_t *width,
                    th_bitint_flavor *flavor)
{
    const th_type *n = as(t, TH_BITINT);

    if (!n) {
        return false;
    }

    if (width) {
        *width = n->bitint.width;
    }

    if (flavor) {
        *flavor = n->bitint.flavor;
    }

    return true;
}

th_type *th_ind_referent(const th_type *t)
{
    const th_type *n = as(t, TH_INDIRECTION);

    return n ? n->ind.referent : nullptr;
}

bool th_ind_attrs(const th_type *t, uint8_t *attrs)
{
    const th_type *n = as(t, TH_INDIRECTION);

    if (!n) {
        return false;
    }

    if (attrs) {
        *attrs = n->ind.attrs;
    }

    return true;
}

th_type *th_seq_elem(const th_type *t)
{
    const th_type *n = as(t, TH_SEQUENCE);

    return n ? n->seq.elem : nullptr;
}

bool th_seq_extent(const th_type *t, uint64_t *extent)
{
    const th_type *n = as(t, TH_SEQUENCE);

    if (!n || !n->seq.has_extent) {
        return false;
    }

    if (extent) {
        *extent = n->seq.extent;
    }

    return true;
}

size_t th_record_len(const th_type *t)
{
    const th_type *n = as(t, TH_RECORD);

    return n ? n->record.n : 0;
}

const th_member *th_record_member(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_RECORD);

    return n && i < n->record.n ? &n->record.members[i] : nullptr;
}

th_type *th_record_lookup(const th_type *t, core_string_t name)
{
    const th_type *n = as(t, TH_RECORD);

    if (!n || !name.data) {
        return nullptr;
    }

    for (size_t i = 0; i < n->record.n; i++) {
        if (core_string_eq(n->record.members[i].name, name)) {
            return n->record.members[i].type;
        }
    }

    return nullptr;
}

bool th_trait_name(const th_type *t, core_string_t *name)
{
    const th_type *trait = as(t, TH_TRAIT);
    if (!trait) return false;
    if (name) *name = trait->trait.name;
    return true;
}

size_t th_trait_len(const th_type *t)
{
    const th_type *trait = as(t, TH_TRAIT);
    return trait ? trait->trait.n : 0;
}

const th_member *th_trait_method(const th_type *t, size_t i)
{
    const th_type *trait = as(t, TH_TRAIT);
    return trait && i < trait->trait.n ? &trait->trait.methods[i] : nullptr;
}

size_t th_trait_requirement_count(const th_type *t)
{
    const th_type *trait = as(t, TH_TRAIT);
    return trait ? trait->trait.nr : 0;
}

bool th_trait_requirement_at(const th_type *t, size_t i,
                             th_trait_requirement *out)
{
    const th_type *trait = as(t, TH_TRAIT);
    if (!trait || i >= trait->trait.nr || !out) return false;
    *out = trait->trait.requirements[i];
    return true;
}

static core_dict_t *record_index(core_dict_t **slot)
{
    if (!*slot) {
        *slot = core_alloc(core_dict_t);
        core_dict_init(*slot, core_hash_word, nullptr);
    }
    return *slot;
}

static uint64_t head_hash(core_string_t name, uint64_t arity)
{
    uint64_t key[] = {core_xxh3_64bits_raw(name.data, name.u8_bytes), arity};
    return core_xxh3_64bits_raw(key, sizeof(key));
}

static uint64_t generic_head_hash(const th_type *type, size_t *nargs)
{
    core_string_t name = {};
    if (!th_named_info(type, &name, nullptr, nargs)) return 0;
    return head_hash(name, (uint64_t)*nargs);
}

static uint64_t builtin_head_key(core_string_t name, int8_t arity)
{
    return head_hash(name,
                     arity == TH_BUILTIN_VARIADIC ? UINT64_MAX
                                                  : (uint64_t)arity);
}

static void index_builtin_heads(core_dict_t **slot,
                                const th_builtin_type_t *types, size_t count)
{
    if (*slot) {
        core_dict_free(*slot);
        core_free(*slot);
        *slot = nullptr;
    }
    core_dict_t *heads = record_index(slot);
    for (size_t i = 0; i < count; i++) {
        uint64_t key = builtin_head_key(types[i].name, types[i].arity);
        core_dict_put(heads, (const void *)(uintptr_t)key,
                      (void *)(uintptr_t)(i + 1));
    }
}

void th_ctx_set_builtin_types(th_ctx *ctx, const th_builtin_type_t *types,
                              size_t count)
{
    if (!ctx) return;
    ctx->builtin_types = types;
    ctx->builtin_count = count;
    index_builtin_heads(&ctx->builtin_heads, types, count);
}

static const th_builtin_type_t *default_builtin_types;
static size_t default_builtin_count;
static core_dict_t *default_builtin_heads;

void th_set_default_builtin_types(const th_builtin_type_t *types,
                                  size_t count)
{
    default_builtin_types = types;
    default_builtin_count = count;
    index_builtin_heads(&default_builtin_heads, types, count);
}

void th_builtin_defaults_apply(th_ctx *ctx)
{
    if (ctx && default_builtin_types && !ctx->builtin_types)
        th_ctx_set_builtin_types(ctx, default_builtin_types,
                                 default_builtin_count);
}

bool th_ctx_has_builtin_types(const th_ctx *ctx)
{
    return ctx && ctx->builtin_types;
}

static const th_builtin_type_t *builtin_for_head(
    core_dict_t *heads, const th_builtin_type_t *types, uint64_t key)
{
    bool found = false;
    size_t at = heads
        ? (size_t)(uintptr_t)core_dict_get(heads,
                                           (const void *)(uintptr_t)key,
                                           &found)
        : 0;
    return found ? &types[at - 1] : nullptr;
}

static const th_builtin_type_t TH_INTRINSIC_TUPLE = {
    .name = CORE_STRING_STATIC(TH_TUPLE),
    .kind = TH_BUILTIN_TUPLE,
    .arity = TH_BUILTIN_VARIADIC,
};

static const th_builtin_type_t TH_INTRINSIC_OPTION = {
    .name = CORE_STRING_STATIC(TH_OPTION),
    .kind = TH_BUILTIN_OPTION,
    .arity = 1,
};

static const th_builtin_type_t TH_INTRINSIC_NIL = {
    .name = CORE_STRING_STATIC(TH_NIL),
    .kind = TH_BUILTIN_NIL,
    .arity = 0,
};

static const th_builtin_type_t *intrinsic_builtin(const th_type *named)
{
    if (core_string_eq(named->named.name, TH_INTRINSIC_TUPLE.name))
        return &TH_INTRINSIC_TUPLE;
    if (core_string_eq(named->named.name, TH_INTRINSIC_OPTION.name)
        && !named->named.is_variant
        && named->named.nargs == 1
        && named->named.args[0].kind == TH_ARG_TYPE)
        return &TH_INTRINSIC_OPTION;
    if (core_string_eq(named->named.name, TH_INTRINSIC_NIL.name)
        && !named->named.is_variant
        && named->named.nargs == 0)
        return &TH_INTRINSIC_NIL;
    return nullptr;
}

th_type *th_nil(th_ctx *ctx)
{
    return th_name(ctx, CORE_STRING_STATIC(TH_NIL));
}

bool th_is_nil(const th_type *type)
{
    th_type *named = type ? (th_type *)th_resolve_c(type) : nullptr;
    return named && named->kind == TH_NAMED
        && intrinsic_builtin(named) == &TH_INTRINSIC_NIL;
}

th_type *th_option(th_ctx *ctx, th_type *item)
{
    th_arg argument = {.kind = TH_ARG_TYPE, .type = item};
    return th_apply(ctx, CORE_STRING_STATIC(TH_OPTION), &argument, 1);
}

th_type *th_option_item(const th_type *type)
{
    th_type *option = type ? (th_type *)th_resolve_c(type) : nullptr;
    if (!option || option->kind != TH_NAMED
        || intrinsic_builtin(option) != &TH_INTRINSIC_OPTION)
        return nullptr;
    return option->named.args[0].type;
}

static const th_builtin_type_t *classify_named(th_type *named,
                                               core_dict_t *heads,
                                               const th_builtin_type_t *types)
{
    if (named->named.builtin_known) return named->named.builtin;
    const th_builtin_type_t *builtin = builtin_for_head(heads, types,
        head_hash(named->named.name, (uint64_t)named->named.nargs));
    if (!builtin)
        builtin = builtin_for_head(heads, types,
            head_hash(named->named.name, UINT64_MAX));
    if (!builtin) builtin = intrinsic_builtin(named);
    if (builtin || types) {
        named->named.builtin = builtin;
        named->named.builtin_known = true;
    }
    return builtin;
}

const th_builtin_type_t *th_builtin_type(th_ctx *ctx, const th_type *type)
{
    if (!ctx || !type) return nullptr;
    th_type *named = (th_type *)th_resolve_c(type);
    if (!named || named->kind != TH_NAMED) return nullptr;
    return classify_named(named, ctx->builtin_heads, ctx->builtin_types);
}

const th_builtin_type_t *th_builtin_of(const th_type *type)
{
    th_type *named = type ? (th_type *)th_resolve_c(type) : nullptr;
    if (!named || named->kind != TH_NAMED) return nullptr;
    return classify_named(named, default_builtin_heads,
                          default_builtin_types);
}

th_builtin_kind_t th_builtin_kind_of(const th_type *type)
{
    const th_builtin_type_t *builtin = th_builtin_of(type);
    return builtin ? builtin->kind : TH_BUILTIN_NONE;
}

th_type *th_builtin_applied(th_type *type, th_builtin_kind_t kind,
                            size_t count)
{
    type = type ? th_resolve(type) : nullptr;
    return type && th_builtin_kind_of(type) == kind
        && th_named_nargs(type) == count ? type : nullptr;
}

th_builtin_kind_t th_builtin_kind(th_ctx *ctx, const th_type *type)
{
    const th_builtin_type_t *builtin = th_builtin_type(ctx, type);
    return builtin ? builtin->kind : TH_BUILTIN_NONE;
}

th_type *th_builtin_item(th_ctx *ctx, const th_type *type,
                         th_builtin_kind_t kind, size_t index)
{
    if (th_builtin_kind(ctx, type) != kind) return nullptr;
    const th_arg *argument = th_named_arg(th_resolve_c(type), index);
    return argument && argument->kind == TH_ARG_TYPE ? argument->type
                                                     : nullptr;
}

static bool generic_parameters(const th_type *type, size_t nargs)
{
    for (size_t i = 0; i < nargs; i++) {
        const th_arg *argument = th_named_arg(type, i);
        if (!argument || argument->kind != TH_ARG_TYPE
            || th_kind_of(th_resolve(argument->type)) != TH_VAR)
            return false;
    }
    return nargs > 0;
}

static th_type_record_t *record_at(th_ctx *ctx, core_dict_t *index,
                                   const void *key)
{
    bool found = false;
    size_t at = index
        ? (size_t)(uintptr_t)core_dict_get(index, key, &found) : 0;
    return found ? &ctx->type_records.data[at - 1] : nullptr;
}

static th_type_record_t *record_lookup(th_ctx *ctx, const th_type *type)
{
    th_type_record_t *same_node = record_at(ctx, ctx->open_type_records,
                                            type);
    if (same_node || !th_is_closed(ctx, (th_type *)type)) return same_node;
    uint64_t hash = th_hash(ctx, (th_type *)type);
    return record_at(ctx, ctx->closed_type_records,
                     (const void *)(uintptr_t)hash);
}

static th_type_record_t *record_insert(th_ctx *ctx, th_type *type,
                                       th_type *definition, void *owner)
{
    bool closed = th_is_closed(ctx, type);
    uint64_t hash = closed ? th_hash(ctx, type) : 0;
    core_list_push(ctx->type_records, ((th_type_record_t){
        .hash = hash,
        .type = type,
        .definition = definition,
        .owner = owner,
    }));
    void *slot = (void *)(uintptr_t)ctx->type_records.len;
    if (closed)
        core_dict_put(record_index(&ctx->closed_type_records),
                      (const void *)(uintptr_t)hash, slot);
    else
        core_dict_put(record_index(&ctx->open_type_records), type, slot);
    size_t nargs = 0;
    uint64_t head = generic_head_hash(type, &nargs);
    if (!closed && generic_parameters(type, nargs)
        && !record_at(ctx, ctx->generic_type_heads,
                      (const void *)(uintptr_t)head))
        core_dict_put(record_index(&ctx->generic_type_heads),
                      (const void *)(uintptr_t)head, slot);
    return &ctx->type_records.data[ctx->type_records.len - 1];
}

bool th_type_define_owned(th_ctx *ctx, th_type *type, th_type *definition,
                          void *owner)
{
    type = type ? th_resolve(type) : nullptr;
    if (!ctx || !type || !definition || th_kind_of(type) != TH_NAMED)
        return false;
    th_type_record_t *record = record_lookup(ctx, type);
    if (!record) return record_insert(ctx, type, definition, owner) != nullptr;
    if (record->definition != definition
        && th_hash(ctx, record->definition) != th_hash(ctx, definition))
        return false;
    if (!record->owner) record->owner = owner;
    return true;
}

bool th_type_define(th_ctx *ctx, th_type *type, th_type *definition)
{
    return th_type_define_owned(ctx, type, definition, nullptr);
}

void *th_type_definition_owner(th_ctx *ctx, const th_type *type)
{
    if (!ctx || !type) return nullptr;
    th_type_record_t *record = record_lookup(ctx, th_resolve_c(type));
    return record ? record->owner : nullptr;
}

th_type *th_auto_deref(th_ctx *ctx, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    while (type) {
        th_type *definition = th_kind_of(type) == TH_NAMED
            ? th_type_definition(ctx, type) : nullptr;
        if (definition && th_kind_of(th_resolve(definition)) == TH_INDIRECTION)
            type = th_resolve(definition);
        th_type *option_item = th_option_item(type);
        if (option_item) {
            type = th_resolve(option_item);
            continue;
        }
        if (th_kind_of(type) != TH_INDIRECTION) break;
        type = th_resolve(th_ind_referent(type));
    }
    return type;
}

th_type *th_type_definition(th_ctx *ctx, const th_type *type)
{
    if (!ctx || !type) return nullptr;
    type = th_resolve_c(type);
    if (th_kind_of((th_type *)type) != TH_NAMED) return nullptr;
    th_type_record_t *record = record_lookup(ctx, type);
    if (record) return record->definition;
    size_t nargs = 0;
    uint64_t head = generic_head_hash(type, &nargs);
    if (!nargs) return nullptr;
    th_type_record_t *generic = record_at(ctx, ctx->generic_type_heads,
                                          (const void *)(uintptr_t)head);
    if (!generic) return nullptr;
    const th_type **from = core_alloc_array(const th_type *, nargs);
    th_type **to = core_alloc_array(th_type *, nargs);
    bool valid = from && to;
    for (size_t j = 0; valid && j < nargs; j++) {
        const th_arg *argument = th_named_arg(type, j);
        valid = argument && argument->kind == TH_ARG_TYPE;
        if (valid) {
            from[j] = th_named_arg(generic->type, j)->type;
            to[j] = argument->type;
        }
    }
    th_type *generic_definition = generic->definition;
    th_type *definition = valid
        ? th_type_clone_replacing(ctx, generic_definition, from, to, nargs)
        : nullptr;
    core_free(to);
    core_free(from);
    if (definition && th_is_closed(ctx, (th_type *)type))
        record_insert(ctx, (th_type *)type, definition, nullptr);
    return definition;
}

th_type *th_record_lookup_in(th_ctx *ctx, const th_type *type,
                             core_string_t name)
{
    th_type *member = th_record_lookup(type, name);
    if (member || !ctx) return member;
    th_type *definition = th_type_definition(ctx, type);
    return definition ? th_record_lookup(definition, name) : nullptr;
}

th_type *th_func_ret(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.ret : nullptr;
}

th_type *th_func_error(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);
    return n ? n->fn.error : nullptr;
}

size_t th_func_nparams(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.np : 0;
}

const th_param *th_func_param(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n && i < n->fn.np ? &n->fn.params[i] : nullptr;
}

const th_param *th_func_rest(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.rest : nullptr;
}

size_t th_func_nkw(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.nkw : 0;
}

const th_kwparam *th_func_kw(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n && i < n->fn.nkw ? &n->fn.kw[i] : nullptr;
}

const th_kwparam *th_func_kw_lookup(const th_type *t, core_string_t name)
{
    const th_type *n = as(t, TH_FUNCTION);

    if (!n || !name.data) {
        return nullptr;
    }

    for (size_t i = 0; i < n->fn.nkw; i++) {
        if (core_string_eq(n->fn.kw[i].name, name)) {
            return &n->fn.kw[i];
        }
    }

    return nullptr;
}

bool th_binder_arity(const th_type *t, uint64_t *arity)
{
    const th_type *n = as(t, TH_BINDER);

    if (!n) {
        return false;
    }

    if (arity) {
        *arity = n->binder.arity;
    }

    return true;
}

th_type *th_binder_body(const th_type *t)
{
    const th_type *n = as(t, TH_BINDER);

    return n ? n->binder.body : nullptr;
}

bool th_bound_info(const th_type *t, uint64_t *distance, uint64_t *position)
{
    const th_type *n = as(t, TH_BOUND);

    if (!n) {
        return false;
    }

    if (distance) {
        *distance = n->bound.distance;
    }

    if (position) {
        *position = n->bound.position;
    }

    return true;
}

bool th_sum_comb(const th_type *t, th_comb *comb)
{
    const th_type *n = as(t, TH_SUM);

    if (!n) {
        return false;
    }

    if (comb) {
        *comb = n->sum.comb;
    }

    return true;
}

size_t th_sum_len(const th_type *t)
{
    const th_type *n = as(t, TH_SUM);

    return n ? n->sum.n : 0;
}

th_type *th_sum_arm(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_SUM);

    return n && i < n->sum.n ? n->sum.arms[i] : nullptr;
}

bool th_var_index(const th_type *t, uint64_t *index)
{
    const th_type *n = as(t, TH_VAR);

    if (!n || !n->var.has_index) {
        return false;
    }

    if (index) {
        *index = n->var.index;
    }

    return true;
}

core_string_t th_var_name(const th_type *t)
{
    const th_type *n = as(t, TH_VAR);

    return n ? n->var.name : (core_string_t){};
}

static bool named_is(const th_type *t, core_string_t name)
{
    const th_type *v = as(t, TH_NAMED);

    return v && v->named.nargs == 0 && core_string_eq(v->named.name, name);
}

bool th_is_erased(const th_type *t)
{
    return named_is(t, CORE_STRING_STATIC(TH_ERASED));
}

bool th_is_bottom(const th_type *t)
{
    return named_is(t, CORE_STRING_STATIC(TH_BOTTOM));
}
