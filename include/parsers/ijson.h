/**
 * @file ijson.h
 * @brief Incremental streaming JSON parser (SAX-style).
 *
 * Table-driven, zero-allocation, incremental JSON parser.  Validates
 * UTF-8 and reports parse events via a callback with byte offsets.
 *
 * ### Grammar
 *
 * ```
 * json:           element;
 * ws:             (' ' | '\n' | '\r' | '\t')*;
 * value:          object | array | string | number | true | false | null;
 * object:         '{' ws members ws '}';
 * members:        (member more_members)?;
 * more_members:   (',' member)*;
 * member:         ws string ws ':' element;
 * array:          '[' ws elements ws ']';
 * elements:       (element more_elements)?;
 * more_elements:  (',' element)*;
 * element:        ws value ws;
 * string:         '"' characters '"';
 * characters:     character*;
 * character:      esc_seq | (U+0020 .. U+10FFFF) except ('"' && '\');
 * esc_seq:        '\' escape;
 * escape:         simple_escape | codepoint;
 * simple_escape:  '"' | '\' | '/' | 'b' | 'f' | 'n' | 'r' | 't';
 * codepoint:      'u' hex hex hex hex;
 * hex:            digit | base16_digit;
 * base16_digit:   ('A' .. 'F') | ('a' .. 'f');
 * number:         integer fraction exponent;
 * integer:        minus (digit | onenine digits);
 * digits:         digit+;
 * digit:          ('0' .. '9');
 * onenine:        ('1' .. '9');
 * fraction:       ('.' digits)?;
 * exponent:       (exp_indicator sign digits)?;
 * exp_indicator:  ('E' | 'e');
 * true:           't' 'r' 'u' 'e';
 * false:          'f' 'a' 'l' 's' 'e';
 * null:           'n' 'u' 'l' 'l';
 * minus:          '-'?;
 * sign:           ('+'|'-')?;
 * ```
 */
#pragma once

#include "n00b.h"

/**
 * @brief Parse events emitted during incremental JSON parsing.
 */
typedef enum {
    N00B_JSON_START,       /**< Beginning of a top-level JSON value. */
    N00B_JSON_END,         /**< End of a top-level JSON value. */
    N00B_JSTRING_START,    /**< Opening quote of a string. */
    N00B_JSTRING_END,      /**< Closing quote of a string. */
    N00B_JINT_START,       /**< Start of an integer part. */
    N00B_JNEGINT_START,    /**< Start of a negative integer. */
    N00B_JFRACT_START,     /**< Start of a fractional part. */
    N00B_JNEG_EXP_START,   /**< Start of a negative exponent. */
    N00B_JEXP_START,       /**< Start of an exponent part. */
    N00B_JINT_END,         /**< End of the integer part. */
    N00B_JFRACT_END,       /**< End of the fractional part. */
    N00B_JEXP_END,         /**< End of the exponent part. */
    N00B_JNUMBER_END,      /**< End of an entire number. */
    N00B_JOBJECT_START,    /**< Opening brace of an object. */
    N00B_JOBJECT_END,      /**< Closing brace of an object. */
    N00B_JARRAY_START,     /**< Opening bracket of an array. */
    N00B_JARRAY_END,       /**< Closing bracket of an array. */
    N00B_JTRUE,            /**< The literal `true`. */
    N00B_JFALSE,           /**< The literal `false`. */
    N00B_Jnullptr,         /**< The literal `null`. */
    N00B_JERROR,           /**< Parse error. */
} n00b_jparse_event_t;

/**
 * @brief Callback signature for parse event delivery.
 *
 * @param event    The parse event type.
 * @param offset   Byte offset from the start of the stream.
 * @param user     User-supplied context pointer.
 */
typedef void (*n00b_jparse_cb)(n00b_jparse_event_t, uint64_t, void *);

/**
 * @brief Incremental JSON parser context.
 *
 * Holds the production stack (mmap'd), current state, stream offset,
 * and callback.  No GC allocation is performed during parsing.
 */
typedef struct {
    uint64_t      *stack;      /**< Production stack (mmap'd). */
    uint64_t      *sp;         /**< Current stack pointer. */
    uint64_t       raw_offset; /**< Current byte offset in the stream. */
    uint64_t       stack_size; /**< Size of the mmap'd stack in bytes. */
    uint8_t        cur_state;  /**< Current state machine state. */
    n00b_jparse_cb callback;   /**< Event callback. */
    void          *user_param; /**< User context for callback. */
} n00b_ijson_ctx_t;

/**
 * @brief State identifiers for the parser state machine.
 */
typedef enum : uint8_t {
    N00B_JST_FAIL,
    N00B_JST_POP,
    N00B_JST_SPOP,
    N00B_JST_NPOP,
    N00B_JST_OPOP,
    N00B_JST_APOP,
    N00B_JST_TPOP,
    N00B_JST_FPOP,
    N00B_JST_0POP,
    N00B_JST_READY,
    N00B_JST_JSON,
    N00B_JST_SUCCESS,
    N00B_JST_ELEMENTS_0,
    N00B_JST_ELEMENTS_1,
    N00B_JST_ELEMENTS_2,
    N00B_JST_ELEMENT_0,
    N00B_JST_ELEMENT_1,
    N00B_JST_ELEMENT_2,
    N00B_JST_MAYBE_ELEMENTS,
    N00B_JST_WS,
    N00B_JST_VALUE,
    N00B_JST_OBJECT_0,
    N00B_JST_OBJECT_1,
    N00B_JST_OBJECT_2,
    N00B_JST_OBJECT_3,
    N00B_JST_OBJECT_4,
    N00B_JST_OBJECT_POSSIBLE_EMPTY,
    N00B_JST_MEMBERS_0,
    N00B_JST_MEMBERS_1,
    N00B_JST_MEMBER_0,
    N00B_JST_MEMBER_1,
    N00B_JST_MEMBER_2,
    N00B_JST_MEMBER_3,
    N00B_JST_MEMBER_4,
    N00B_JST_MAYBE_MEMBERS,
    N00B_JST_ARRAY_0,
    N00B_JST_ARRAY_1,
    N00B_JST_ARRAY_POSSIBLE_EMPTY,
    N00B_JST_ARRAY_2,
    N00B_JST_ARRAY_3,
    N00B_JST_ARRAY_4,
    N00B_JST_STRING_0,
    N00B_JST_STRING_1,
    N00B_JST_STRING_2,
    N00B_JST_ESC,
    N00B_JST_HEX_4,
    N00B_JST_HEX_3,
    N00B_JST_HEX_2,
    N00B_JST_HEX_1,
    N00B_JST_U3,
    N00B_JST_U2,
    N00B_JST_U1,
    N00B_JST_NEG_START,
    N00B_JST_NEG_FIRST,
    N00B_JST_NUM_START,
    N00B_JST_ZNUM_START,
    N00B_JST_ZNUM_END,
    N00B_JST_NUMBER,
    N00B_JST_END_INT,
    N00B_JST_MAYBE_FRACT,
    N00B_JST_REPORT_FRACT,
    N00B_JST_FRACT,
    N00B_JST_FRACT_OPT,
    N00B_JST_END_FRACT,
    N00B_JST_MAYBE_EXPONENT,
    N00B_JST_MAYBE_EXPONENT_1,
    N00B_JST_EXPONENT_SIGN_0,
    N00B_JST_EXPONENT_SIGN_1,
    N00B_JST_EXPONENT_REPORT,
    N00B_JST_NEG_EXPONENT_REPORT,
    N00B_JST_EXPONENT,
    N00B_JST_EXPONENT_OPT,
    N00B_JST_END_EXPONENT,
    N00B_JST_TRUE_0,
    N00B_JST_TRUE_1,
    N00B_JST_TRUE_2,
    N00B_JST_TRUE_3,
    N00B_JST_FALSE_0,
    N00B_JST_FALSE_1,
    N00B_JST_FALSE_2,
    N00B_JST_FALSE_3,
    N00B_JST_FALSE_4,
    N00B_JST_nullptr_0,
    N00B_JST_nullptr_1,
    N00B_JST_nullptr_2,
    N00B_JST_nullptr_3,
    N00B_NUM_JSTATES,
} n00b_jstate_id_t;

/**
 * @brief Action types for state table entries.
 */
typedef enum : uint8_t {
    N00B_JACTION_FAIL,
    N00B_JACTION_START,
    N00B_JACTION_SUCCESS,
    N00B_JACTION_POP,
    N00B_JACTION_POP_REPORT,
    N00B_JACTION_ENTER_NT,
    N00B_JACTION_ENTER_REPORT,
    N00B_JACTION_ADVANCE,
    N00B_JACTION_WS,
    N00B_JACTION_SELECT,
    N00B_JACTION_BOOL_SELECT,
    N00B_JACTION_U8_SELECT,
    N00B_NUM_JACTIONS,
} n00b_jaction_t;

/**
 * @brief State table entry.
 */
typedef struct {
    uint8_t            *list_address;
    n00b_jaction_t      action;
    n00b_jstate_id_t    next_state;
    n00b_jstate_id_t    alt_state;
    n00b_jstate_id_t    end_transition;
    char                match_char;
    n00b_jparse_event_t report;
    int                 offset;
#if defined(N00B_IJSON_DEBUG)
    char *name;
#endif
} n00b_jentry_t;

/** @brief Default production stack size (4KB / 1 page). */
#define N00B_JSTACK_DEFAULT (1 << 12)

/**
 * @brief Feed a chunk of data to the incremental parser.
 *
 * @param ctx  Parser context.
 * @param data Input bytes.
 * @param len  Number of bytes in @p data.
 */
extern void n00b_ijson_incremental_parse(n00b_ijson_ctx_t *ctx,
                                         uint8_t *data,
                                         uint32_t len);

/**
 * @brief Initialize a parser context.
 *
 * @param stream  Context to initialize.
 * @kw stack_size Production stack size in bytes (default 4KB).
 * @kw user_param User context pointer passed to callback.
 * @kw callback   Event callback (default: stdout printer).
 */
extern void _n00b_ijson_init(n00b_ijson_ctx_t *stream) _kargs {
    int64_t        stack_size = N00B_JSTACK_DEFAULT;
    void          *user_param = nullptr;
    n00b_jparse_cb callback   = nullptr;
};

/**
 * @brief Reset parser state for reuse.
 *
 * Keeps the allocated stack but resets the offset and stack pointer.
 */
extern void n00b_ijson_reset(n00b_ijson_ctx_t *ctx);

/**
 * @brief Release parser resources.
 *
 * Unmaps the production stack.
 */
extern void n00b_ijson_delete(n00b_ijson_ctx_t *ctx);

/**
 * @brief Signal end of input to the parser.
 *
 * Forces final state transitions and reports `N00B_JSON_END` or
 * `N00B_JERROR` depending on whether the stream was complete.
 */
extern void n00b_ijson_end_of_input(n00b_ijson_ctx_t *ctx);

#define n00b_ijson_init(x, ...) _n00b_ijson_init(x __VA_OPT__(,) __VA_ARGS__)
