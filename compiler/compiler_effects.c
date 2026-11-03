#include "compiler_effects.h"

static const n00bb_effect_name_t effect_names[] = {
#define IR_EFFECT_NAME(name, bits) \
    {CORE_STRING_STATIC(#name), (uint64_t)(bits), false},
#define IR_EFFECT_GROUP(name, bits) \
    {CORE_STRING_STATIC(#name), (uint64_t)(bits), true},
#include "ir/effect_names.def"
};

size_t
n00bb_effect_name_count(void)
{
    return sizeof(effect_names) / sizeof(effect_names[0]);
}

const n00bb_effect_name_t *
n00bb_effect_name_at(size_t index)
{
    return index < n00bb_effect_name_count() ? &effect_names[index]
                                              : nullptr;
}

const n00bb_effect_name_t *
n00bb_effect_named(core_string_t name)
{
    for (size_t i = 0; i < n00bb_effect_name_count(); i++)
        if (core_string_eq(effect_names[i].name, name))
            return &effect_names[i];
    return nullptr;
}

uint64_t
n00bb_named_effect_bits(void)
{
    uint64_t bits = 0;
    for (size_t i = 0; i < n00bb_effect_name_count(); i++)
        if (!effect_names[i].group) bits |= effect_names[i].bits;
    return bits;
}

static bool
reaches_element(const ir_func_t *function, const ir_spath_t *path,
                uint32_t below)
{
    for (uint32_t depth = 1; depth < below; depth++) {
        th_type *type = ir_summary_path_type(function, path, depth);
        if (type && th_kind_of(th_resolve(type)) == TH_VAR) return true;
    }
    return false;
}

static uint8_t
container_access(const ir_func_t *function, const ir_spath_t *path)
{
    if (!reaches_element(function, path, path->depth)) return path->flags;
    return path->flags & IR_SPATH_ACCESS ? IR_SPATH_READ : 0;
}

static bool
lends_through(const ir_func_t *function, const ir_summary_t *summary,
              ir_sflow_table_t table, uint32_t param)
{
    for (uint32_t s = 0; s < ir_summary_flow_source_count(summary, table);
         s++) {
        const ir_spath_t *source = ir_summary_flow_source(summary, table, s);
        if (source->root != IR_SPLACE_PARAM || source->param != param
            || reaches_element(function, source, source->depth + 1))
            continue;
        for (uint32_t f = 0; f < ir_summary_flow_count(summary, table); f++)
            if (ir_summary_flow_has(summary, table, f, s, IR_SFLOW_ALIAS))
                return true;
    }
    return false;
}

uint8_t
n00bb_parameter_role(const ir_func_t *function, const ir_summary_t *summary,
                     uint32_t param)
{
    if (!summary) return N00BB_ROLE_READS | N00BB_ROLE_WRITES
                       | N00BB_ROLE_LENDS;
    uint8_t role = 0;
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        bool rooted = path->root == IR_SPLACE_PARAM && path->param == param;
        bool unknown = path->root == IR_SPLACE_UNKNOWN
            && !(path->flags & IR_SPATH_TYPED);
        if (!rooted && !unknown) continue;
        uint8_t flags = rooted ? container_access(function, path)
                               : path->flags;
        if (flags & (IR_SPATH_READ | IR_SPATH_READS_ENTRY))
            role |= N00BB_ROLE_READS;
        if (flags & IR_SPATH_WRITE) role |= N00BB_ROLE_WRITES;
    }
    if ((ir_summary_param_escape(summary, param) & IR_SESCAPE_ADDRESS)
        || lends_through(function, summary, IR_SFLOW_RESULTS, param)
        || lends_through(function, summary, IR_SFLOW_STORES, param))
        role |= N00BB_ROLE_LENDS;
    return role;
}
