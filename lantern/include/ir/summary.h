#ifndef IR_SUMMARY_H
#define IR_SUMMARY_H

#include "ir/ir.h"

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
    IR_SUMMARY_MAY_FAIL      = 1u << 0,
    IR_SUMMARY_MAY_TRAP      = 1u << 1,
    IR_SUMMARY_MAY_ALLOCATE  = 1u << 2,
    IR_SUMMARY_MAY_BLOCK     = 1u << 3,
    IR_SUMMARY_DIVERGES      = 1u << 4,
    IR_SUMMARY_CALLS_UNKNOWN = 1u << 5,
    IR_SUMMARY_MAY_IO        = 1u << 6,
};

typedef enum : uint8_t {
    IR_SINPUT_VALUE,
    IR_SINPUT_REFERENT,
    IR_SINPUT_PLACE,
    IR_SINPUT_UNKNOWN,
} ir_sinput_kind_t;

typedef struct {
    ir_sinput_kind_t kind;
    uint32_t         param;
    ir_splace_t      place;
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

typedef struct ir_summary_t  ir_summary_t;
typedef struct ir_summaries_t ir_summaries_t;

typedef void (*ir_summary_target_fn)(void *sink, ir_func_t *target);

typedef struct {
    void *context;
    bool (*call_targets)(void *context, const ir_instr_t *call,
                         ir_summary_target_fn add, void *sink);
    ir_summary_t *(*axiom)(void *context, ir_summaries_t *summaries,
                           const ir_func_t *function);
} ir_summary_hooks_t;

ir_summaries_t *ir_summaries_compute(ir_func_t *const *functions,
                                     size_t count,
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

bool ir_splace_eq(const ir_splace_t *a, const ir_splace_t *b);

#endif
