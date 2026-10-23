#ifndef IR_SUMMARY_H
#define IR_SUMMARY_H

#include "ir/ir.h"
#include "ir/places.h"
#include "ir/sites.h"

typedef enum : uint8_t {
    IR_SPLACE_PARAM,
    IR_SPLACE_GLOBAL,
    IR_SPLACE_THREAD,
    IR_SPLACE_HEAP,
    IR_SPLACE_UNKNOWN,
} ir_splace_kind_t;

typedef struct {
    ir_splace_kind_t   kind;
    uint32_t           param;
    const ir_global_t *global;
    th_type           *type;
} ir_splace_t;

enum {
    IR_SACCESS_READ         = 1u << 0,
    IR_SACCESS_WRITE        = 1u << 1,
    IR_SACCESS_MUST_SUCCESS = 1u << 2,
    IR_SACCESS_MUST_FAILURE = 1u << 3,
    IR_SACCESS_READS_ENTRY  = 1u << 4,
};

typedef struct {
    ir_splace_t place;
    uint8_t     flags;
} ir_saccess_t;

enum {
    IR_SUMMARY_MAY_FAIL         = 1u << 0,
    IR_SUMMARY_MAY_TRAP         = 1u << 1,
    IR_SUMMARY_MAY_ALLOCATE     = 1u << 2,
    IR_SUMMARY_MAY_BLOCK        = 1u << 3,
    IR_SUMMARY_DIVERGES         = 1u << 4,
    IR_SUMMARY_CALLS_UNKNOWN    = 1u << 5,
    IR_SUMMARY_MAY_IO           = 1u << 6,
    IR_SUMMARY_NO_RETURN        = 1u << 7,
    IR_SUMMARY_MAY_COLLECT      = 1u << 8,
    IR_SUMMARY_ALLOCATES_DIRECT = 1u << 9,
    IR_SUMMARY_UNBOUNDED_RECURSION = 1u << 10,
    IR_SUMMARY_CALLS_FINALIZERS = 1u << 11,
    IR_SUMMARY_STARTS_THREADS   = 1u << 12,
    IR_SUMMARY_CALLS_CALLBACKS  = 1u << 13,
    IR_SUMMARY_REINTERPRETS     = 1u << 14,
};

enum {
    IR_STRAP_EXPLICIT   = 1u << 0,
    IR_STRAP_BOUNDS     = 1u << 1,
    IR_STRAP_UNWRAP     = 1u << 2,
    IR_STRAP_ARITHMETIC = 1u << 3,
    IR_STRAP_ALL        = 0xf,
};

typedef struct {
    uint8_t  kind;
    uint32_t line;
    uint32_t column;
} ir_strap_site_t;

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
} ir_sreturn_kind_t;

typedef struct {
    ir_sreturn_kind_t  kind;
    uint32_t           param;
    const ir_global_t *global;
} ir_sreturn_t;

enum { IR_SFLOW_SELF, IR_SFLOW_CONTENTS, IR_SFLOW_KINDS };

typedef struct {
    uint64_t from[IR_SFLOW_KINDS];
    bool     nonlocal;
    bool     fresh;
    bool     shared;
} ir_scarried_t;

typedef struct {
    uint64_t       writes[IR_SFLOW_KINDS];
    uint64_t       mutates[IR_SFLOW_KINDS];
    uint64_t       escapes[IR_SFLOW_KINDS];
    uint64_t       leaks[IR_SFLOW_KINDS];
    uint64_t       receives;
    bool           receives_fresh;
    bool           receives_nonlocal;
    bool           writes_nonlocal;
    bool           opaque;
    uint32_t       n_carried;
    ir_scarried_t *carried;
} ir_sflow_t;

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
    bool (*flow)(void *context, ir_summaries_t *summaries,
                 ir_func_t *function, const ir_sflow_t *previous,
                 ir_sflow_t *out);
    const ir_summary_t *(*entries)(void *context, ir_summaries_t *summaries,
                                   uint32_t flag);
    bool (*global_reachable)(void *context, const ir_global_t *global);
    bool defer_trap_proofs;
} ir_summary_hooks_t;

const ir_sflow_t *ir_summary_flow(const ir_summary_t *summary);
void              ir_summary_set_flow(ir_summary_t *summary,
                                      const ir_sflow_t *flow);
void              ir_sflow_release(ir_sflow_t *flow);
bool              ir_sflow_eq(const ir_sflow_t *a, const ir_sflow_t *b);
ir_sflow_t        ir_sflow_opaque(void);

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
void          ir_summary_add_access(ir_summary_t *summary, ir_splace_t place,
                                    uint8_t flags);
ir_summary_t *ir_summary_conservative(ir_summaries_t *summaries,
                                      const ir_func_t *function);

const ir_func_t     *ir_summary_function(const ir_summary_t *summary);
uint32_t             ir_summary_flags(const ir_summary_t *summary);
uint8_t              ir_summary_trap_kinds(const ir_summary_t *summary);
void                 ir_summary_set_trap_kinds(ir_summary_t *summary,
                                               uint8_t kinds);
uint32_t             ir_summary_trap_site_count(const ir_summary_t *summary);
const ir_strap_site_t *ir_summary_trap_site_at(const ir_summary_t *summary,
                                               uint32_t index);
uint32_t             ir_summary_access_count(const ir_summary_t *summary);
const ir_saccess_t  *ir_summary_access_at(const ir_summary_t *summary,
                                          uint32_t index);
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
void         ir_summary_set_return(ir_summary_t *summary,
                                   ir_sreturn_t returned);
uint8_t      ir_summary_call_escape(ir_summaries_t *summaries,
                                    const ir_instr_t *call, uint32_t param);
typedef const ir_global_t *(*ir_summary_global_fn)(void *context,
                                                    core_string_t module,
                                                    core_string_t name);
bool          ir_summary_encode(const ir_summary_t *summary,
                                core_buffer_t *out);
ir_summary_t *ir_summary_decode(ir_summaries_t *summaries,
                                const ir_func_t *function,
                                const uint8_t *data, size_t size,
                                ir_summary_global_fn resolve, void *context);
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
    IR_SEFFECT_TRAPS_SHIFT       = 16,
    IR_SEFFECT_READS_PARAM_SHIFT  = 32,
    IR_SEFFECT_WRITES_PARAM_SHIFT = 48,
};
#define IR_SEFFECT_READS_NONLOCAL  (UINT64_C(1) << 24)
#define IR_SEFFECT_WRITES_NONLOCAL (UINT64_C(1) << 25)

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

#endif
