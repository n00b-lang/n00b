#ifndef IR_SUMMARY_H
#define IR_SUMMARY_H

#include "ir/ir.h"
#include "ir/places.h"
#include "ir/sites.h"
#include "lib/list.h"

typedef enum : uint8_t {
    IR_SPLACE_PARAM,
    IR_SPLACE_GLOBAL,
    IR_SPLACE_THREAD,
    IR_SPLACE_HEAP,
    IR_SPLACE_UNKNOWN,
    IR_SPLACE_SOURCE,
} ir_splace_kind_t;

typedef struct {
    ir_splace_kind_t   kind;
    uint32_t           param;
    const ir_global_t *global;
    th_type           *type;
} ir_splace_t;

enum {
#define IR_EFFECT_FLAG(id, name, bit) IR_SUMMARY_##id = 1u << bit,
#include "ir/summary_effects.def"
};

#define IR_SUMMARY_OBSERVES (IR_SUMMARY_MAY_INPUT | IR_SUMMARY_MAY_OUTPUT \
                             | IR_SUMMARY_READS_ENV | IR_SUMMARY_READS_CLOCK \
                             | IR_SUMMARY_READS_RANDOM)

enum {
#define IR_EFFECT_TRAP(id, name, bit) IR_STRAP_##id = 1u << bit,
#include "ir/summary_effects.def"
};

enum {
    IR_STRAP_ALL = 0
#define IR_EFFECT_TRAP(id, name, bit) | IR_STRAP_##id
#include "ir/summary_effects.def"
};

typedef struct {
    uint8_t  kind;
    uint32_t line;
    uint32_t column;
} ir_strap_site_t;

typedef struct {
    uint32_t      flag;
    ir_func_t    *callee;
    core_string_t module;
    core_string_t name;
    uint32_t      line;
    uint32_t      column;
} ir_switness_t;

typedef enum : uint8_t {
    IR_SINPUT_VALUE,
    IR_SINPUT_REFERENT,
    IR_SINPUT_PLACE,
    IR_SINPUT_UNKNOWN,
    IR_SINPUT_CALL,
} ir_sinput_kind_t;

typedef struct {
    ir_sinput_kind_t kind;
    uint32_t         param;
    ir_splace_t      place;
    core_string_t    callee;
} ir_sinput_t;

typedef enum : uint8_t {
    IR_SOUTPUT_RESULT,
    IR_SOUTPUT_FAILURE,
    IR_SOUTPUT_PLACE,
} ir_soutput_kind_t;

typedef struct {
    ir_soutput_kind_t kind;
    ir_splace_t       place;
    uint32_t          data_first;
    uint32_t          data_count;
    uint32_t          control_first;
    uint32_t          control_count;
} ir_soutput_t;

enum {
    IR_SESCAPE_ADDRESS  = 1u << 0,
    IR_SESCAPE_REFERENT = 1u << 1,
};

typedef enum : uint8_t {
    IR_SRETURN_NONE,
    IR_SRETURN_PARAM,
    IR_SRETURN_GLOBAL,
    IR_SRETURN_FRESH_EXTERNAL,
    IR_SRETURN_UNKNOWN,
    IR_SRETURN_FRESH,
} ir_sreturn_kind_t;

typedef struct {
    ir_sreturn_kind_t  kind;
    uint32_t           param;
    const ir_global_t *global;
} ir_sreturn_t;

typedef struct ir_summary_t  ir_summary_t;
typedef struct ir_summaries_t ir_summaries_t;

typedef void (*ir_summary_target_fn)(void *sink, ir_func_t *target);

typedef struct {
    void *context;
    bool (*call_targets)(void *context, const ir_instr_t *call,
                         ir_summary_target_fn add, void *sink);
    ir_summary_t *(*axiom)(void *context, ir_summaries_t *summaries,
                           const ir_func_t *function);
    uint32_t (*adjust)(void *context, const ir_func_t *function,
                       uint32_t flags);
    const ir_summary_t *(*entries)(void *context, ir_summaries_t *summaries,
                                   uint32_t flag);
    bool (*global_reachable)(void *context, const ir_global_t *global);
    bool defer_trap_proofs;
} ir_summary_hooks_t;

ir_summaries_t *ir_summaries_compute(ir_func_t *const *functions,
                                     size_t count,
                                     const ir_summary_hooks_t *hooks);
ir_summaries_t *ir_escapes_compute(ir_func_t *const *functions, size_t count,
                                   const ir_summary_hooks_t *hooks);
void            ir_summaries_free(ir_summaries_t *summaries);
ir_summary_t   *ir_summary_of(ir_summaries_t *summaries,
                              const ir_func_t *function);

ir_summary_t *ir_summary_new(ir_summaries_t *summaries,
                             const ir_func_t *function);
void          ir_summary_set_flags(ir_summary_t *summary, uint32_t flags);
bool          ir_summary_add_effect(ir_summary_t *summary, core_string_t name);
uint32_t      ir_summary_effect_count(const ir_summary_t *summary);
core_string_t ir_summary_effect_at(const ir_summary_t *summary,
                                   uint32_t index);
bool          ir_summary_has_effect(const ir_summary_t *summary,
                                    core_string_t name);
ir_summary_t *ir_summary_conservative(ir_summaries_t *summaries,
                                      const ir_func_t *function);

const ir_func_t     *ir_summary_function(const ir_summary_t *summary);
bool                 ir_summary_results_known(const ir_summary_t *summary);
uint32_t             ir_summary_flags(const ir_summary_t *summary);
uint8_t              ir_summary_trap_kinds(const ir_summary_t *summary);
void                 ir_summary_set_trap_kinds(ir_summary_t *summary,
                                               uint8_t kinds);
uint32_t             ir_summary_trap_site_count(const ir_summary_t *summary);
const ir_strap_site_t *ir_summary_trap_site_at(const ir_summary_t *summary,
                                               uint32_t index);
const ir_switness_t  *ir_summary_witness_for(const ir_summary_t *summary,
                                             uint32_t flag);
void                 ir_switness_callee_text(const ir_switness_t *witness,
                                             core_buffer_t *out);
uint32_t             ir_summary_input_count(const ir_summary_t *summary);
const ir_sinput_t   *ir_summary_input_at(const ir_summary_t *summary,
                                         uint32_t index);
uint32_t             ir_summary_output_count(const ir_summary_t *summary);
const ir_soutput_t  *ir_summary_output_at(const ir_summary_t *summary,
                                          uint32_t index);
const uint32_t      *ir_summary_dependences(const ir_summary_t *summary);

uint8_t      ir_summary_param_escape(const ir_summary_t *summary,
                                     uint32_t param);
void         ir_summary_set_param_escape(ir_summary_t *summary,
                                         uint32_t param, uint8_t flags);
ir_sreturn_t ir_summary_return(const ir_summary_t *summary);
bool         ir_summary_returns_fresh(const ir_summary_t *summary);
void         ir_summary_set_return(ir_summary_t *summary,
                                   ir_sreturn_t returned);
typedef struct {
    const ir_global_t *(*global)(void *context, core_string_t module,
                                 core_string_t name);
    ir_func_t *(*function)(void *context, core_string_t module,
                           core_string_t name);
    void *context;
} ir_summary_resolver_t;
bool          ir_summary_encode(const ir_summary_t *summary,
                                core_buffer_t *out);
bool          ir_summary_encode_identity(const ir_summary_t *summary,
                                         core_buffer_t *out);
ir_summary_t *ir_summary_decode(ir_summaries_t *summaries,
                                const ir_func_t *function,
                                const uint8_t *data, size_t size,
                                const ir_summary_resolver_t *resolver);
typedef struct {
    uint32_t (*intern)(void *context, core_string_t text);
    core_string_t (*lookup)(void *context, uint32_t reference);
    void *context;
} ir_summary_texts_t;
bool          ir_summary_encode_with(const ir_summary_t *summary,
                                     core_buffer_t *out,
                                     const ir_summary_texts_t *texts);
ir_summary_t *ir_summary_decode_with(ir_summaries_t *summaries,
                                     const ir_func_t *function,
                                     const uint8_t *data, size_t size,
                                     const ir_summary_resolver_t *resolver,
                                     const ir_summary_texts_t *texts);
bool         ir_summary_call_diverges(ir_summaries_t *summaries,
                                      const ir_instr_t *call);
bool         ir_summary_place_unseen(ir_summaries_t *summaries,
                                     const ir_splace_t *place);
typedef void (*ir_summary_callee_fn)(void *context, const ir_func_t *target,
                                     const ir_summary_t *summary);
bool         ir_summary_call_callees(ir_summaries_t *summaries,
                                     const ir_instr_t *call,
                                     ir_summary_callee_fn visit,
                                     void *context);
bool         ir_summary_call_pure(ir_summaries_t *summaries,
                                  const ir_instr_t *call);
bool         ir_summary_call_writes(ir_summaries_t *summaries,
                                    const ir_func_t *f, ir_places_t *places,
                                    const ir_instr_t *call, uint32_t place);

enum {
    IR_SEFFECT_FLAG_BITS = 0
#define IR_EFFECT_FLAG(id, name, bit) + 1
#include "ir/summary_effects.def"
};

enum {
    IR_SEFFECT_TRAP_BITS = 0
#define IR_EFFECT_TRAP(id, name, bit) + 1
#include "ir/summary_effects.def"
};

enum {
    IR_SEFFECT_FLAG_MASK   = (1u << IR_SEFFECT_FLAG_BITS) - 1,
    IR_SEFFECT_TRAPS_SHIFT = IR_SEFFECT_FLAG_BITS,
#define IR_EFFECT_NONLOCAL(id, name, bit) IR_SEFFECT_##id##_BIT = bit,
#define IR_EFFECT_PARAMETER(id, name, shift) IR_SEFFECT_##id##_SHIFT = shift,
#include "ir/summary_effects.def"
};
#define IR_SEFFECT_READS_NONLOCAL  (UINT64_C(1) << IR_SEFFECT_READS_NONLOCAL_BIT)
#define IR_SEFFECT_WRITES_NONLOCAL (UINT64_C(1) << IR_SEFFECT_WRITES_NONLOCAL_BIT)

core_string_t ir_summary_flag_name(uint32_t bit);
core_string_t ir_summary_trap_name(uint32_t bit);
bool          ir_summary_effect_lookup(core_string_t name, uint64_t *bits);

ir_summary_t *ir_summary_for(ir_summaries_t *summaries, ir_func_t *function);
const ir_summary_t *ir_summary_entries(ir_summaries_t *summaries,
                                       uint32_t flag);
uint64_t      ir_summary_effect_bits(const ir_summary_t *summary);

enum {
    IR_SFIELD_READ  = 1u << 0,
    IR_SFIELD_WRITE = 1u << 1,
    IR_SFIELD_WHOLE = 1u << 2,
};

typedef struct {
    th_type       *record;
    core_string_t  name;
    uint8_t        flags;
} ir_sfield_t;

enum {
    IR_SPATH_READ         = 1u << 0,
    IR_SPATH_WRITE        = 1u << 1,
    IR_SPATH_DEEP         = 1u << 2,
    IR_SPATH_MUST_SUCCESS = 1u << 3,
    IR_SPATH_MUST_FAILURE = 1u << 4,
    IR_SPATH_READS_ENTRY  = 1u << 5,
    IR_SPATH_TYPED        = 1u << 6,
    IR_SPATH_ROOT         = 1u << 7,
};

#define IR_SPATH_ACCESS (IR_SPATH_READ | IR_SPATH_WRITE | IR_SPATH_READS_ENTRY)
#define IR_SPATH_MUST   (IR_SPATH_MUST_SUCCESS | IR_SPATH_MUST_FAILURE)

enum { IR_SPATH_STEPS = 4 };

typedef struct {
    ir_splace_kind_t   root;
    uint32_t           param;
    const ir_global_t *global;
    th_type           *type;
    uint8_t            depth;
    uint8_t            flags;
    uint8_t            output;
    core_string_t      steps[IR_SPATH_STEPS];
} ir_spath_t;

core_list_decl(ir_spath_t);
typedef core_list_t(ir_spath_t) ir_spath_list_t;

typedef struct {
    ir_spath_t to;
    ir_spath_t from;
} ir_sstore_t;

typedef enum : uint8_t {
    IR_SFLOW_DATA,
    IR_SFLOW_CONTROL,
} ir_sflow_kind_t;

typedef enum : uint8_t {
    IR_SFLOW_RESULTS,
    IR_SFLOW_STORES,
} ir_sflow_table_t;

uint32_t          ir_summary_path_count(const ir_summary_t *summary);
const ir_spath_t *ir_summary_path_at(const ir_summary_t *summary,
                                     uint32_t index);
void              ir_summary_add_path(ir_summary_t *summary,
                                      const ir_spath_t *path);
uint32_t          ir_summary_path_places(const ir_func_t *function,
                                         const ir_spath_t *path,
                                         ir_splace_t places[2]);
ir_spath_t        ir_spath_of_place(const ir_splace_t *place, uint8_t flags);
th_type          *ir_summary_path_type(const ir_func_t *function,
                                       const ir_spath_t *path, uint8_t depth);
bool              ir_summary_path_inline(const ir_func_t *function,
                                         const ir_spath_t *path);
void              ir_summary_merged_paths(const ir_summary_t *summary,
                                          ir_spath_list_t *into);
void              ir_spath_list_release(ir_spath_list_t *paths);
uint32_t           ir_summary_result_count(const ir_summary_t *summary);
const ir_sstore_t *ir_summary_result_at(const ir_summary_t *summary,
                                        uint32_t index);
void               ir_summary_add_result(ir_summary_t *summary,
                                         const ir_spath_t *to,
                                         const ir_spath_t *from);
uint32_t           ir_summary_store_count(const ir_summary_t *summary);
const ir_sstore_t *ir_summary_store_at(const ir_summary_t *summary,
                                       uint32_t index);
int                ir_spath_compare(const ir_spath_t *a, const ir_spath_t *b);

uint32_t          ir_summary_flow_count(const ir_summary_t *summary,
                                        ir_sflow_table_t table);
const ir_spath_t *ir_summary_flow_to(const ir_summary_t *summary,
                                     ir_sflow_table_t table, uint32_t flow);
uint32_t          ir_summary_flow_source_count(const ir_summary_t *summary,
                                               ir_sflow_table_t table);
const ir_spath_t *ir_summary_flow_source(const ir_summary_t *summary,
                                         ir_sflow_table_t table,
                                         uint32_t source);
bool              ir_summary_flow_has(const ir_summary_t *summary,
                                      ir_sflow_table_t table, uint32_t flow,
                                      uint32_t source, ir_sflow_kind_t kind);
void              ir_summary_add_flow(ir_summary_t *summary,
                                      ir_sflow_table_t table,
                                      const ir_spath_t *to,
                                      const ir_spath_t *from,
                                      ir_sflow_kind_t kind);
bool ir_summary_may_access(const ir_summary_t *summary,
                           const ir_func_t *function, core_string_t query,
                           bool write);

uint32_t           ir_summary_field_count(const ir_summary_t *summary);
const ir_sfield_t *ir_summary_field_at(const ir_summary_t *summary,
                                       uint32_t index);
bool         ir_summary_argument_addressable(const ir_func_t *function,
                                             const ir_value_t *argument);
bool         ir_summary_place_of(const ir_func_t *function,
                                 const ir_place_t *place, ir_splace_t *out);
bool         ir_address_escapes(ir_summaries_t *summaries,
                                const ir_func_t *function,
                                const ir_value_t *address);

bool        ir_splace_eq(const ir_splace_t *a, const ir_splace_t *b);
ir_splace_t ir_splace_global(const ir_global_t *global);

uint8_t ir_summary_effect(const ir_summary_t *summary,
                          const ir_splace_t *place,
                          ir_path_outcome_t outcome);
uint8_t ir_summary_state_before(ir_summaries_t *summaries, ir_func_t *f,
                                const ir_splace_t *place, uint8_t entry,
                                const ir_instr_t *in);

typedef enum {
    IR_PT_FRESH_NONE,
    IR_PT_FRESH_EMPTY,
    IR_PT_FRESH_HOLDING,
} ir_pt_fresh_t;

typedef struct {
    void          *context;
    ir_pt_fresh_t (*fresh)(void *context, const ir_instr_t *in);
    bool           callee_globals_unknown;
} ir_points_to_hooks_t;

typedef enum {
    IR_PTO_STORAGE,
    IR_PTO_PARAM,
    IR_PTO_CONTENTS,
    IR_PTO_GLOBAL,
    IR_PTO_UNKNOWN,
} ir_pto_kind_t;

typedef struct {
    ir_pto_kind_t      kind;
    uint32_t           param;
    const ir_instr_t  *instr;
    const ir_value_t  *value;
    const ir_local_t  *local;
    const ir_global_t *global;
} ir_pto_t;

core_list_decl(ir_pto_t);
typedef core_list_t(ir_pto_t) ir_pto_list_t;

typedef struct ir_points_to_t ir_points_to_t;

typedef void (*ir_points_to_holder_fn)(void *context, const ir_value_t *value,
                                       const ir_local_t *local);

ir_points_to_t   *ir_points_to_new(ir_summaries_t *summaries,
                                   const ir_func_t *function,
                                   const ir_points_to_hooks_t *hooks);
void              ir_points_to_free(ir_points_to_t *points_to);
void              ir_points_to_value(ir_points_to_t *points_to,
                                     const ir_instr_t *at,
                                     const ir_value_t *value,
                                     ir_pto_list_t *into);
void              ir_points_to_loaded(ir_points_to_t *points_to,
                                      const ir_instr_t *at,
                                      const ir_value_t *value,
                                      ir_pto_list_t *into);
void              ir_points_to_reach(ir_points_to_t *points_to,
                                     ir_pto_list_t *objects, bool contents);
bool              ir_points_to_call_writes(ir_points_to_t *points_to,
                                           const ir_instr_t *call,
                                           uint64_t trusted,
                                           ir_pto_list_t *into);
bool              ir_points_to_escapes(ir_points_to_t *points_to,
                                       const ir_pto_t *object,
                                       const ir_instr_t **where);
void              ir_points_to_holders(ir_points_to_t *points_to,
                                       const ir_pto_t *object,
                                       ir_points_to_holder_fn holder,
                                       void *context);
bool              ir_pto_eq(const ir_pto_t *a, const ir_pto_t *b);
bool              ir_pto_add(ir_pto_list_t *objects, ir_pto_t object);
bool              ir_pto_has(const ir_pto_list_t *objects,
                             const ir_pto_t *object);

#endif
