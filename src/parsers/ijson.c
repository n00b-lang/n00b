/*
 * ijson.c — Incremental streaming JSON parser.
 *
 * Table-driven, zero-allocation state machine. The only conditional
 * jumps in the main parse loop are for the iteration over the input.
 * Computed goto dispatch makes the inner loop branchless.
 */

#include "n00b.h"
#include "parsers/ijson.h"
#include "core/mmaps.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(N00B_IJSON_DEBUG)
#define IDBUG(...)  printf(__VA_ARGS__)
#define INAME(x, y) .name = x, .y
#else
#define IDBUG(...)
#define INAME(x, y) .y
#endif

// ============================================================================
// Lookup tables
// ============================================================================

static const uint8_t value_table[256] = {
    ['{'] = N00B_JST_OBJECT_0,
    ['['] = N00B_JST_ARRAY_0,
    ['"'] = N00B_JST_STRING_1,
    ['t'] = N00B_JST_TRUE_0,
    ['f'] = N00B_JST_FALSE_0,
    ['n'] = N00B_JST_nullptr_0,
    ['-'] = N00B_JST_NEG_START,
    ['0'] = N00B_JST_ZNUM_START,
    ['1'] = N00B_JST_NUM_START,
    ['2'] = N00B_JST_NUM_START,
    ['3'] = N00B_JST_NUM_START,
    ['4'] = N00B_JST_NUM_START,
    ['5'] = N00B_JST_NUM_START,
    ['6'] = N00B_JST_NUM_START,
    ['7'] = N00B_JST_NUM_START,
    ['8'] = N00B_JST_NUM_START,
    ['9'] = N00B_JST_NUM_START,
};

static const uint8_t neg_first_digit[256] = {
    ['0'] = N00B_JST_ZNUM_END,
    ['1'] = N00B_JST_NUMBER,
    ['2'] = N00B_JST_NUMBER,
    ['3'] = N00B_JST_NUMBER,
    ['4'] = N00B_JST_NUMBER,
    ['5'] = N00B_JST_NUMBER,
    ['6'] = N00B_JST_NUMBER,
    ['7'] = N00B_JST_NUMBER,
    ['8'] = N00B_JST_NUMBER,
    ['9'] = N00B_JST_NUMBER,
};

static const uint8_t strchr_table[256] = {
    [0x20] = N00B_JST_STRING_2, [0x21] = N00B_JST_STRING_2,
    ['"']  = N00B_JST_SPOP, // 0x22
    [0x23] = N00B_JST_STRING_2, [0x24] = N00B_JST_STRING_2, [0x25] = N00B_JST_STRING_2,
    [0x26] = N00B_JST_STRING_2, [0x27] = N00B_JST_STRING_2, [0x28] = N00B_JST_STRING_2,
    [0x29] = N00B_JST_STRING_2, [0x2a] = N00B_JST_STRING_2, [0x2b] = N00B_JST_STRING_2,
    [0x2c] = N00B_JST_STRING_2, [0x2d] = N00B_JST_STRING_2, [0x2e] = N00B_JST_STRING_2,
    [0x2f] = N00B_JST_STRING_2, [0x30] = N00B_JST_STRING_2, [0x31] = N00B_JST_STRING_2,
    [0x32] = N00B_JST_STRING_2, [0x33] = N00B_JST_STRING_2, [0x34] = N00B_JST_STRING_2,
    [0x35] = N00B_JST_STRING_2, [0x36] = N00B_JST_STRING_2, [0x37] = N00B_JST_STRING_2,
    [0x38] = N00B_JST_STRING_2, [0x39] = N00B_JST_STRING_2, [0x3a] = N00B_JST_STRING_2,
    [0x3b] = N00B_JST_STRING_2, [0x3c] = N00B_JST_STRING_2, [0x3d] = N00B_JST_STRING_2,
    [0x3e] = N00B_JST_STRING_2, [0x3f] = N00B_JST_STRING_2, [0x40] = N00B_JST_STRING_2,
    [0x41] = N00B_JST_STRING_2, [0x42] = N00B_JST_STRING_2, [0x43] = N00B_JST_STRING_2,
    [0x44] = N00B_JST_STRING_2, [0x45] = N00B_JST_STRING_2, [0x46] = N00B_JST_STRING_2,
    [0x47] = N00B_JST_STRING_2, [0x48] = N00B_JST_STRING_2, [0x49] = N00B_JST_STRING_2,
    [0x4a] = N00B_JST_STRING_2, [0x4b] = N00B_JST_STRING_2, [0x4c] = N00B_JST_STRING_2,
    [0x4d] = N00B_JST_STRING_2, [0x4e] = N00B_JST_STRING_2, [0x4f] = N00B_JST_STRING_2,
    [0x50] = N00B_JST_STRING_2, [0x51] = N00B_JST_STRING_2, [0x52] = N00B_JST_STRING_2,
    [0x53] = N00B_JST_STRING_2, [0x54] = N00B_JST_STRING_2, [0x55] = N00B_JST_STRING_2,
    [0x56] = N00B_JST_STRING_2, [0x57] = N00B_JST_STRING_2, [0x58] = N00B_JST_STRING_2,
    [0x59] = N00B_JST_STRING_2, [0x5a] = N00B_JST_STRING_2, [0x5b] = N00B_JST_STRING_2,
    ['\\'] = N00B_JST_ESC, // 0x5c
    [0x5d] = N00B_JST_STRING_2, [0x5e] = N00B_JST_STRING_2, [0x5f] = N00B_JST_STRING_2,
    [0x60] = N00B_JST_STRING_2, [0x61] = N00B_JST_STRING_2, [0x62] = N00B_JST_STRING_2,
    [0x63] = N00B_JST_STRING_2, [0x64] = N00B_JST_STRING_2, [0x65] = N00B_JST_STRING_2,
    [0x66] = N00B_JST_STRING_2, [0x67] = N00B_JST_STRING_2, [0x68] = N00B_JST_STRING_2,
    [0x69] = N00B_JST_STRING_2, [0x6a] = N00B_JST_STRING_2, [0x6b] = N00B_JST_STRING_2,
    [0x6c] = N00B_JST_STRING_2, [0x6d] = N00B_JST_STRING_2, [0x6e] = N00B_JST_STRING_2,
    [0x6f] = N00B_JST_STRING_2, [0x70] = N00B_JST_STRING_2, [0x71] = N00B_JST_STRING_2,
    [0x72] = N00B_JST_STRING_2, [0x73] = N00B_JST_STRING_2, [0x74] = N00B_JST_STRING_2,
    [0x75] = N00B_JST_STRING_2, [0x76] = N00B_JST_STRING_2, [0x77] = N00B_JST_STRING_2,
    [0x78] = N00B_JST_STRING_2, [0x79] = N00B_JST_STRING_2, [0x7a] = N00B_JST_STRING_2,
    [0x7b] = N00B_JST_STRING_2, [0x7c] = N00B_JST_STRING_2, [0x7d] = N00B_JST_STRING_2,
    [0x7e] = N00B_JST_STRING_2, [0x7f] = N00B_JST_STRING_2, [0xc0] = N00B_JST_U1,
    [0xc1] = N00B_JST_U1,       [0xc2] = N00B_JST_U1,       [0xc3] = N00B_JST_U1,
    [0xc4] = N00B_JST_U1,       [0xc5] = N00B_JST_U1,       [0xc6] = N00B_JST_U1,
    [0xc7] = N00B_JST_U1,       [0xc8] = N00B_JST_U1,       [0xc9] = N00B_JST_U1,
    [0xca] = N00B_JST_U1,       [0xcb] = N00B_JST_U1,       [0xcc] = N00B_JST_U1,
    [0xcd] = N00B_JST_U1,       [0xce] = N00B_JST_U1,       [0xcf] = N00B_JST_U1,
    [0xd0] = N00B_JST_U1,       [0xd1] = N00B_JST_U1,       [0xd2] = N00B_JST_U1,
    [0xd3] = N00B_JST_U1,       [0xd4] = N00B_JST_U1,       [0xd5] = N00B_JST_U1,
    [0xd6] = N00B_JST_U1,       [0xd7] = N00B_JST_U1,       [0xd8] = N00B_JST_U1,
    [0xd9] = N00B_JST_U1,       [0xda] = N00B_JST_U1,       [0xdb] = N00B_JST_U1,
    [0xdc] = N00B_JST_U1,       [0xdd] = N00B_JST_U1,       [0xde] = N00B_JST_U1,
    [0xdf] = N00B_JST_U1,       [0xe0] = N00B_JST_U2,       [0xe1] = N00B_JST_U2,
    [0xe2] = N00B_JST_U2,       [0xe3] = N00B_JST_U2,       [0xe4] = N00B_JST_U2,
    [0xe5] = N00B_JST_U2,       [0xe6] = N00B_JST_U2,       [0xe7] = N00B_JST_U2,
    [0xe8] = N00B_JST_U2,       [0xe9] = N00B_JST_U2,       [0xea] = N00B_JST_U2,
    [0xeb] = N00B_JST_U2,       [0xec] = N00B_JST_U2,       [0xed] = N00B_JST_U2,
    [0xee] = N00B_JST_U2,       [0xef] = N00B_JST_U2,       [0xf0] = N00B_JST_U3,
    [0xf1] = N00B_JST_U3,       [0xf2] = N00B_JST_U3,       [0xf3] = N00B_JST_U3,
    [0xf4] = N00B_JST_U3,       [0xf5] = N00B_JST_U3,       [0xf6] = N00B_JST_U3,
    [0xf7] = N00B_JST_U3,
};

static uint8_t esc_table[256] = {
    ['"']  = N00B_JST_STRING_2,
    ['\\'] = N00B_JST_STRING_2,
    ['/']  = N00B_JST_STRING_2,
    ['b']  = N00B_JST_STRING_2,
    ['f']  = N00B_JST_STRING_2,
    ['n']  = N00B_JST_STRING_2,
    ['r']  = N00B_JST_STRING_2,
    ['t']  = N00B_JST_STRING_2,
    ['u']  = N00B_JST_HEX_4,
};

static uint8_t u8_continue[4] = {
    0,
    0,
    1,
    0,
};

static uint8_t hex_table[256] = {
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1,
    ['8'] = 1, ['9'] = 1, ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1,
};

static const uint8_t digit_table[256] = {
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
};

// ============================================================================
// State table
// ============================================================================

static const n00b_jentry_t n00b_jstate_table[N00B_NUM_JSTATES] = {
    [N00B_JST_FAIL] = {
        INAME("@fail", action) = N00B_JACTION_FAIL,
    },
    [N00B_JST_SUCCESS] = {
        INAME("@success", action) = N00B_JACTION_SUCCESS,
    },
    [N00B_JST_POP] = {
        INAME("@pop", action) = N00B_JACTION_POP,
    },
    [N00B_JST_SPOP] = {
        INAME("@spop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JSTRING_END,
        .offset                = 0,
    },
    [N00B_JST_NPOP] = {
        INAME("@npop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JNUMBER_END,
        .offset                = 0,
    },
    [N00B_JST_OPOP] = {
        INAME("@opop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JOBJECT_END,
        .offset                = 0,
    },
    [N00B_JST_APOP] = {
        INAME("@apop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JARRAY_END,
        .offset                = 0,
    },
    [N00B_JST_TPOP] = {
        INAME("@tpop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JTRUE,
        .offset                = -4,
    },
    [N00B_JST_FPOP] = {
        INAME("@fpop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_JFALSE,
        .offset                = -5,
    },
    [N00B_JST_0POP] = {
        INAME("@0pop", action) = N00B_JACTION_POP_REPORT,
        .report                = N00B_Jnullptr,
        .offset                = -4,
    },
    [N00B_JST_READY] = {
        INAME("@ready", action) = N00B_JACTION_START,
    },
    [N00B_JST_JSON] = {
        INAME("@json", action) = N00B_JACTION_ENTER_NT,
        .next_state            = N00B_JST_ELEMENT_0,
        .alt_state             = N00B_JST_SUCCESS,
    },
    [N00B_JST_ELEMENTS_0] = {
        INAME("@elements_0", action) = N00B_JACTION_ENTER_NT,
        .next_state                  = N00B_JST_WS,
        .alt_state                   = N00B_JST_ELEMENTS_1,
    },
    [N00B_JST_ELEMENTS_1] = {
        INAME("@elements_1", action) = N00B_JACTION_ENTER_NT,
        .next_state                  = N00B_JST_ELEMENT_0,
        .alt_state                   = N00B_JST_ELEMENTS_2,
    },
    [N00B_JST_ELEMENTS_2] = {
        INAME("@elements_2", action) = N00B_JACTION_ENTER_NT,
        .next_state                  = N00B_JST_WS,
        .alt_state                   = N00B_JST_MAYBE_ELEMENTS,
    },
    [N00B_JST_ELEMENT_0] = {
        INAME("@element_0", action) = N00B_JACTION_ENTER_NT,
        .next_state                 = N00B_JST_WS,
        .alt_state                  = N00B_JST_ELEMENT_1,
    },
    [N00B_JST_ELEMENT_1] = {
        INAME("@element_1", action) = N00B_JACTION_ENTER_NT,
        .next_state                 = N00B_JST_VALUE,
        .alt_state                  = N00B_JST_ELEMENT_2,
    },
    [N00B_JST_ELEMENT_2] = {
        INAME("@element_2", action) = N00B_JACTION_ENTER_NT,
        .next_state                 = N00B_JST_WS,
        .alt_state                  = N00B_JST_POP,
    },
    [N00B_JST_MAYBE_ELEMENTS] = {
        INAME("@element_3", action) = N00B_JACTION_ADVANCE,
        .match_char                 = ',',
        .next_state                 = N00B_JST_ELEMENTS_0,
        .alt_state                  = N00B_JST_POP,
        .end_transition             = N00B_JST_FAIL,
    },
    [N00B_JST_WS] = {
        INAME("@ws", action) = N00B_JACTION_WS,
        .end_transition      = N00B_JST_POP,
    },
    [N00B_JST_VALUE] = {
        INAME("@value", action) = N00B_JACTION_SELECT,
        .list_address           = (uint8_t *)value_table,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_OBJECT_0] = {
        INAME("@object_0", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                = N00B_JST_OBJECT_1,
        .report                    = N00B_JOBJECT_START,
        .offset                    = -1,
    },
    [N00B_JST_OBJECT_1] = {
        INAME("@object_1", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_WS,
        .alt_state                 = N00B_JST_OBJECT_POSSIBLE_EMPTY,
    },
    [N00B_JST_OBJECT_POSSIBLE_EMPTY] = {
        INAME("@object_possible_empty", action) = N00B_JACTION_ADVANCE,
        .match_char                             = '}',
        .next_state                             = N00B_JST_OPOP,
        .alt_state                              = N00B_JST_OBJECT_2,
    },
    [N00B_JST_OBJECT_2] = {
        INAME("@object_2", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_MEMBERS_0,
        .alt_state                 = N00B_JST_OBJECT_3,
    },
    [N00B_JST_OBJECT_3] = {
        INAME("@object_3", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_WS,
        .alt_state                 = N00B_JST_OBJECT_4,
    },
    [N00B_JST_OBJECT_4] = {
        INAME("@object_4", action) = N00B_JACTION_ADVANCE,
        .match_char                = '}',
        .next_state                = N00B_JST_OPOP,
        .alt_state                 = N00B_JST_FAIL,
        .end_transition            = N00B_JST_FAIL,
    },
    [N00B_JST_MEMBERS_0] = {
        INAME("@members_0", action) = N00B_JACTION_ENTER_NT,
        .next_state                 = N00B_JST_MEMBER_0,
        .alt_state                  = N00B_JST_MEMBERS_1,
    },
    [N00B_JST_MEMBERS_1] = {
        INAME("@members_1", action) = N00B_JACTION_ENTER_NT,
        .next_state                 = N00B_JST_MAYBE_MEMBERS,
        .alt_state                  = N00B_JST_POP,
    },
    [N00B_JST_MEMBER_0] = {
        INAME("@member_0", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_WS,
        .alt_state                 = N00B_JST_MEMBER_1,
    },
    [N00B_JST_MEMBER_1] = {
        INAME("@member_1", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_STRING_0,
        .alt_state                 = N00B_JST_MEMBER_2,
    },
    [N00B_JST_MEMBER_2] = {
        INAME("@member_2", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_WS,
        .alt_state                 = N00B_JST_MEMBER_3,
    },
    [N00B_JST_MEMBER_3] = {
        INAME("@member_3", action) = N00B_JACTION_ADVANCE,
        .match_char                = ':',
        .next_state                = N00B_JST_MEMBER_4,
        .alt_state                 = N00B_JST_FAIL,
        .end_transition            = N00B_JST_FAIL,
    },
    [N00B_JST_MEMBER_4] = {
        INAME("@member_4", action) = N00B_JACTION_ENTER_NT,
        .next_state                = N00B_JST_ELEMENT_0,
        .alt_state                 = N00B_JST_POP,
    },
    [N00B_JST_MAYBE_MEMBERS] = {
        INAME("@maybe_members", action) = N00B_JACTION_ADVANCE,
        .match_char                     = ',',
        .next_state                     = N00B_JST_MEMBERS_0,
        .alt_state                      = N00B_JST_POP,
        .end_transition                 = N00B_JST_FAIL,
    },
    [N00B_JST_ARRAY_0] = {
        INAME("@array_0", action) = N00B_JACTION_ENTER_REPORT,
        .next_state               = N00B_JST_ARRAY_1,
        .report                   = N00B_JARRAY_START,
        .offset                   = -1,
    },
    [N00B_JST_ARRAY_1] = {
        INAME("@array_1", action) = N00B_JACTION_ENTER_NT,
        .next_state               = N00B_JST_WS,
        .alt_state                = N00B_JST_ARRAY_POSSIBLE_EMPTY,
    },
    [N00B_JST_ARRAY_POSSIBLE_EMPTY] = {
        INAME("@array_possible_empty", action) = N00B_JACTION_ADVANCE,
        .match_char                            = ']',
        .next_state                            = N00B_JST_APOP,
        .alt_state                             = N00B_JST_ARRAY_2,
    },
    [N00B_JST_ARRAY_2] = {
        INAME("@array_2", action) = N00B_JACTION_ENTER_NT,
        .next_state               = N00B_JST_ELEMENTS_0,
        .alt_state                = N00B_JST_ARRAY_3,
    },
    [N00B_JST_ARRAY_3] = {
        INAME("@array_3", action) = N00B_JACTION_ENTER_NT,
        .next_state               = N00B_JST_WS,
        .alt_state                = N00B_JST_ARRAY_4,
    },
    [N00B_JST_ARRAY_4] = {
        INAME("@array_4", action) = N00B_JACTION_ADVANCE,
        .match_char               = ']',
        .next_state               = N00B_JST_APOP,
        .alt_state                = N00B_JST_FAIL,
        .end_transition           = N00B_JST_FAIL,
    },
    [N00B_JST_STRING_0] = {
        INAME("@string_0", action) = N00B_JACTION_ADVANCE,
        .match_char                = '"',
        .next_state                = N00B_JST_STRING_1,
        .alt_state                 = N00B_JST_FAIL,
        .end_transition            = N00B_JST_FAIL,
    },
    [N00B_JST_STRING_1] = {
        INAME("@string_1", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                = N00B_JST_STRING_2,
        .report                    = N00B_JSTRING_START,
        .offset                    = -1,
    },
    [N00B_JST_STRING_2] = {
        INAME("@string_2", action) = N00B_JACTION_SELECT,
        .list_address              = (uint8_t *)strchr_table,
        .end_transition            = N00B_JST_FAIL,
    },
    [N00B_JST_ESC] = {
        INAME("@esc", action) = N00B_JACTION_SELECT,
        .list_address         = (uint8_t *)esc_table,
        .end_transition       = N00B_JST_FAIL,
    },
    [N00B_JST_HEX_4] = {
        INAME("@hex_4", action) = N00B_JACTION_BOOL_SELECT,
        .list_address           = (uint8_t *)hex_table,
        .next_state             = N00B_JST_HEX_3,
        .alt_state              = N00B_JST_FAIL,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_HEX_3] = {
        INAME("@hex_3", action) = N00B_JACTION_BOOL_SELECT,
        .list_address           = (uint8_t *)hex_table,
        .next_state             = N00B_JST_HEX_2,
        .alt_state              = N00B_JST_FAIL,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_HEX_2] = {
        INAME("@hex_2", action) = N00B_JACTION_BOOL_SELECT,
        .list_address           = (uint8_t *)hex_table,
        .next_state             = N00B_JST_HEX_1,
        .alt_state              = N00B_JST_FAIL,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_HEX_1] = {
        INAME("@hex_1", action) = N00B_JACTION_BOOL_SELECT,
        .list_address           = (uint8_t *)hex_table,
        .next_state             = N00B_JST_STRING_2,
        .alt_state              = N00B_JST_FAIL,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_U3] = {
        INAME("@u3", action) = N00B_JACTION_U8_SELECT,
        .next_state          = N00B_JST_U2,
        .alt_state           = N00B_JST_FAIL,
        .end_transition      = N00B_JST_FAIL,
    },
    [N00B_JST_U2] = {
        INAME("@u2", action) = N00B_JACTION_U8_SELECT,
        .next_state          = N00B_JST_U1,
        .alt_state           = N00B_JST_FAIL,
        .end_transition      = N00B_JST_FAIL,
    },
    [N00B_JST_U1] = {
        INAME("@u1", action) = N00B_JACTION_U8_SELECT,
        .next_state          = N00B_JST_STRING_2,
        .alt_state           = N00B_JST_FAIL,
        .end_transition      = N00B_JST_FAIL,
    },
    [N00B_JST_NEG_START] = {
        INAME("@neg_start", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                 = N00B_JST_NEG_FIRST,
        .report                     = N00B_JNEGINT_START,
        .offset                     = 0,
    },
    [N00B_JST_NEG_FIRST] = {
        INAME("@neg_first", action) = N00B_JACTION_SELECT,
        .list_address               = (uint8_t *)neg_first_digit,
        .end_transition             = N00B_JST_FAIL,
    },
    [N00B_JST_NUM_START] = {
        INAME("@num_start", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                 = N00B_JST_NUMBER,
        .report                     = N00B_JINT_START,
        .offset                     = -1,
    },
    [N00B_JST_ZNUM_START] = {
        INAME("@znum_start", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                  = N00B_JST_ZNUM_END,
        .report                      = N00B_JINT_START,
        .offset                      = -1,
    },
    [N00B_JST_ZNUM_END] = {
        INAME("@znum_end", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                = N00B_JST_MAYBE_FRACT,
        .report                    = N00B_JINT_END,
        .offset                    = 0,
    },
    [N00B_JST_NUMBER] = {
        INAME("@number", action) = N00B_JACTION_BOOL_SELECT,
        .list_address            = (uint8_t *)digit_table,
        .next_state              = N00B_JST_NUMBER,
        .alt_state               = N00B_JST_END_INT,
        .end_transition          = N00B_JST_END_INT,
    },
    [N00B_JST_END_INT] = {
        INAME("@end_int", action) = N00B_JACTION_ENTER_REPORT,
        .next_state               = N00B_JST_MAYBE_FRACT,
        .report                   = N00B_JINT_END,
        .offset                   = 0,
    },
    [N00B_JST_MAYBE_FRACT] = {
        INAME("@maybe_fract", action) = N00B_JACTION_ADVANCE,
        .match_char                   = '.',
        .next_state                   = N00B_JST_REPORT_FRACT,
        .alt_state                    = N00B_JST_MAYBE_EXPONENT,
        .end_transition               = N00B_JST_MAYBE_EXPONENT,
    },
    [N00B_JST_REPORT_FRACT] = {
        INAME("@report_fract", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                    = N00B_JST_FRACT,
        .report                        = N00B_JFRACT_START,
        .offset                        = 0,
    },
    [N00B_JST_FRACT] = {
        INAME("@fract", action) = N00B_JACTION_BOOL_SELECT,
        .list_address           = (uint8_t *)digit_table,
        .next_state             = N00B_JST_FRACT_OPT,
        .alt_state              = N00B_JST_FAIL,
        .end_transition         = N00B_JST_FAIL,
    },
    [N00B_JST_FRACT_OPT] = {
        INAME("@fract_opt", action) = N00B_JACTION_BOOL_SELECT,
        .list_address               = (uint8_t *)digit_table,
        .next_state                 = N00B_JST_FRACT_OPT,
        .alt_state                  = N00B_JST_END_FRACT,
        .end_transition             = N00B_JST_END_FRACT,
    },
    [N00B_JST_END_FRACT] = {
        INAME("@end_fract", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                 = N00B_JST_MAYBE_EXPONENT,
        .report                     = N00B_JFRACT_END,
        .offset                     = 0,
    },
    [N00B_JST_MAYBE_EXPONENT] = {
        INAME("@maybe_exponent", action) = N00B_JACTION_ADVANCE,
        .match_char                      = 'e',
        .next_state                      = N00B_JST_EXPONENT_SIGN_0,
        .alt_state                       = N00B_JST_MAYBE_EXPONENT_1,
        .end_transition                  = N00B_JST_NPOP,
    },
    [N00B_JST_MAYBE_EXPONENT_1] = {
        INAME("@maybe_exponent_1", action) = N00B_JACTION_ADVANCE,
        .match_char                        = 'E',
        .next_state                        = N00B_JST_EXPONENT_SIGN_0,
        .alt_state                         = N00B_JST_NPOP,
        .end_transition                    = N00B_JST_NPOP,
    },
    [N00B_JST_EXPONENT_SIGN_0] = {
        INAME("@exponent_sign_0", action) = N00B_JACTION_ADVANCE,
        .match_char                       = '+',
        .next_state                       = N00B_JST_EXPONENT_REPORT,
        .alt_state                        = N00B_JST_EXPONENT_SIGN_1,
        .end_transition                   = N00B_JST_NPOP,
    },
    [N00B_JST_EXPONENT_SIGN_1] = {
        INAME("@exponent_sign_1", action) = N00B_JACTION_ADVANCE,
        .match_char                       = '-',
        .next_state                       = N00B_JST_NEG_EXPONENT_REPORT,
        .alt_state                        = N00B_JST_EXPONENT_REPORT,
        .end_transition                   = N00B_JST_NPOP,
    },
    [N00B_JST_EXPONENT_REPORT] = {
        INAME("@exponent_report", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                       = N00B_JST_EXPONENT,
        .report                           = N00B_JEXP_START,
        .offset                           = 0,
    },
    [N00B_JST_NEG_EXPONENT_REPORT] = {
        INAME("@neg_exponent_report", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                           = N00B_JST_EXPONENT,
        .report                               = N00B_JNEG_EXP_START,
        .offset                               = 0,
    },
    [N00B_JST_EXPONENT] = {
        INAME("@exponent", action) = N00B_JACTION_BOOL_SELECT,
        .list_address              = (uint8_t *)digit_table,
        .next_state                = N00B_JST_EXPONENT_OPT,
        .alt_state                 = N00B_JST_FAIL,
        .end_transition            = N00B_JST_FAIL,
    },
    [N00B_JST_EXPONENT_OPT] = {
        INAME("@exponent_opt", action) = N00B_JACTION_BOOL_SELECT,
        .list_address                  = (uint8_t *)digit_table,
        .next_state                    = N00B_JST_EXPONENT_OPT,
        .alt_state                     = N00B_JST_END_EXPONENT,
        .end_transition                = N00B_JST_END_EXPONENT,
    },
    [N00B_JST_END_EXPONENT] = {
        INAME("@end_exponent", action) = N00B_JACTION_ENTER_REPORT,
        .next_state                    = N00B_JST_NPOP,
        .report                        = N00B_JEXP_END,
        .offset                        = 0,
    },
    [N00B_JST_TRUE_0] = {
        INAME("@true_0", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'r',
        .next_state              = N00B_JST_TRUE_1,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
    [N00B_JST_TRUE_1] = {
        INAME("@true_1", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'u',
        .next_state              = N00B_JST_TRUE_2,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
    [N00B_JST_TRUE_2] = {
        INAME("@true_2", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'e',
        .next_state              = N00B_JST_TPOP,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
    [N00B_JST_FALSE_0] = {
        INAME("@false_0", action) = N00B_JACTION_ADVANCE,
        .match_char               = 'a',
        .next_state               = N00B_JST_FALSE_1,
        .alt_state                = N00B_JST_FAIL,
        .end_transition           = N00B_JST_FAIL,
    },
    [N00B_JST_FALSE_1] = {
        INAME("@false_1", action) = N00B_JACTION_ADVANCE,
        .match_char               = 'l',
        .next_state               = N00B_JST_FALSE_2,
        .alt_state                = N00B_JST_FAIL,
        .end_transition           = N00B_JST_FAIL,
    },
    [N00B_JST_FALSE_2] = {
        INAME("@false_2", action) = N00B_JACTION_ADVANCE,
        .match_char               = 's',
        .next_state               = N00B_JST_FALSE_3,
        .alt_state                = N00B_JST_FAIL,
        .end_transition           = N00B_JST_FAIL,
    },
    [N00B_JST_FALSE_3] = {
        INAME("@false_3", action) = N00B_JACTION_ADVANCE,
        .match_char               = 'e',
        .next_state               = N00B_JST_FPOP,
        .alt_state                = N00B_JST_FAIL,
        .end_transition           = N00B_JST_FAIL,
    },
    [N00B_JST_nullptr_0] = {
        INAME("@null_0", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'u',
        .next_state              = N00B_JST_nullptr_1,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
    [N00B_JST_nullptr_1] = {
        INAME("@null_1", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'l',
        .next_state              = N00B_JST_nullptr_2,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
    [N00B_JST_nullptr_2] = {
        INAME("@null_2", action) = N00B_JACTION_ADVANCE,
        .match_char              = 'l',
        .next_state              = N00B_JST_0POP,
        .alt_state               = N00B_JST_FAIL,
        .end_transition          = N00B_JST_FAIL,
    },
};

// ============================================================================
// End-of-input handler
// ============================================================================

void
n00b_ijson_end_of_input(n00b_ijson_ctx_t *s)
{
    static void *eom_dispatch[N00B_NUM_JACTIONS] = {
        [N00B_JACTION_FAIL]         = &&action_fail,
        [N00B_JACTION_SUCCESS]      = &&action_success,
        [N00B_JACTION_POP]          = &&action_pop,
        [N00B_JACTION_POP_REPORT]   = &&action_pop_report,
        [N00B_JACTION_ENTER_NT]     = &&action_enter_nt,
        [N00B_JACTION_ENTER_REPORT] = &&action_enter_report,
        [N00B_JACTION_ADVANCE]      = &&use_end_transition,
        [N00B_JACTION_WS]           = &&use_end_transition,
        [N00B_JACTION_SELECT]       = &&use_end_transition,
        [N00B_JACTION_BOOL_SELECT]  = &&use_end_transition,
        [N00B_JACTION_U8_SELECT]    = &&use_end_transition,
        [N00B_JACTION_START]        = &&use_end_transition,
    };

    n00b_jentry_t state_info;

    while (true) {
        state_info = n00b_jstate_table[s->cur_state];
        IDBUG("!cur state: %s (at end)\n", state_info.name);
        goto *eom_dispatch[state_info.action];
use_end_transition:
        IDBUG("A!: use_end_transition\n");
        s->cur_state = state_info.end_transition;
        continue;
action_fail:
        IDBUG("A!: fail\n");
        (*s->callback)(N00B_JERROR, s->raw_offset, s->user_param);
        s->sp        = s->stack;
        s->cur_state = N00B_JST_READY;
        return;
action_success:
        IDBUG("A!: success\n");
        (*s->callback)(N00B_JSON_END, s->raw_offset, s->user_param);
        s->cur_state = N00B_JST_READY;
        return;
action_pop:
        IDBUG("A!: pop\n");
        s->cur_state = (uint8_t)*s->sp--;
        continue;
action_pop_report:
        IDBUG("A!: pop_report\n");
        (*s->callback)(state_info.report, s->raw_offset + state_info.offset, s->user_param);
        s->cur_state = (uint8_t)*s->sp--;
        continue;
action_enter_report:
        IDBUG("A!: enter_report\n");
        (*s->callback)(state_info.report, s->raw_offset + state_info.offset, s->user_param);
        s->cur_state = state_info.next_state;
        continue;
action_enter_nt:
        IDBUG("A!: enter_nt\n");
        *++s->sp     = (uint64_t)state_info.alt_state;
        s->cur_state = state_info.next_state;
        continue;
    }
}

// ============================================================================
// Incremental parse
// ============================================================================

void
n00b_ijson_incremental_parse(n00b_ijson_ctx_t *s, uint8_t *p, uint32_t n)
{
    static void *action_dispatch[N00B_NUM_JACTIONS] = {
        [N00B_JACTION_START]        = &&action_start,
        [N00B_JACTION_FAIL]         = &&action_fail,
        [N00B_JACTION_SUCCESS]      = &&action_success,
        [N00B_JACTION_POP]          = &&action_pop,
        [N00B_JACTION_POP_REPORT]   = &&action_pop_report,
        [N00B_JACTION_ENTER_NT]     = &&action_enter_nt,
        [N00B_JACTION_ENTER_REPORT] = &&action_enter_report,
        [N00B_JACTION_ADVANCE]      = &&action_advance,
        [N00B_JACTION_WS]           = &&action_white_space,
        [N00B_JACTION_SELECT]       = &&action_select,
        [N00B_JACTION_BOOL_SELECT]  = &&action_bool_select,
        [N00B_JACTION_U8_SELECT]    = &&action_u8_select,
    };

    static void *post_loop_dispatch[N00B_NUM_JACTIONS] = {
        [N00B_JACTION_FAIL]         = &&action_fail,
        [N00B_JACTION_SUCCESS]      = &&action_success,
        [N00B_JACTION_POP]          = &&action_pop,
        [N00B_JACTION_POP_REPORT]   = &&action_pop_report,
        [N00B_JACTION_ENTER_NT]     = &&action_enter_nt,
        [N00B_JACTION_ENTER_REPORT] = &&action_enter_report,
        [N00B_JACTION_ADVANCE]      = &&need_input,
        [N00B_JACTION_WS]           = &&need_input,
        [N00B_JACTION_SELECT]       = &&need_input,
        [N00B_JACTION_BOOL_SELECT]  = &&need_input,
        [N00B_JACTION_U8_SELECT]    = &&need_input,
        [N00B_JACTION_START]        = &&need_input,
    };

    static void *match_check_targets[2] = {
        &&item_doesnt_match,
        &&item_matches,
    };

    static void *space_check_targets[2] = {
        &&nl_check,
        &&found_space,
    };

    static void *nl_check_targets[2] = {
        &&cr_check,
        &&found_space,
    };

    static void *cr_check_targets[2] = {
        &&tab_check,
        &&found_space,
    };

    static void *tab_check_targets[2] = {
        &&action_pop,
        &&found_space,
    };

    n00b_jentry_t state_info;
    uint8_t       chr;

    for (uint32_t i = 0; i < n;) {
        state_info = n00b_jstate_table[s->cur_state];

        IDBUG("+cur state: %s (i = %d / %d)\n", state_info.name, i, n);
        goto *action_dispatch[state_info.action];

action_start:
        IDBUG("A: start\n");
        (*s->callback)(N00B_JSON_START, s->raw_offset, s->user_param);
        s->cur_state = N00B_JST_JSON;
        continue;
action_fail:
        IDBUG("A: fail\n");
        (*s->callback)(N00B_JERROR, s->raw_offset, s->user_param);
        s->cur_state = N00B_JST_READY;
        s->sp        = s->stack;
        continue;
action_success:
        IDBUG("A: success\n");
        (*s->callback)(N00B_JSON_END, s->raw_offset, s->user_param);
        s->cur_state = N00B_JST_READY;
        continue;
action_pop:
        IDBUG("A: pop\n");
        s->cur_state = (uint8_t)*s->sp--;
        continue;
action_pop_report:
        IDBUG("A: pop_report\n");
        (*s->callback)(state_info.report, s->raw_offset + state_info.offset, s->user_param);
        s->cur_state = (uint8_t)*s->sp--;
        continue;
action_enter_report:
        IDBUG("A: enter_report\n");
        (*s->callback)(state_info.report, s->raw_offset + state_info.offset, s->user_param);
        s->cur_state = state_info.next_state;
        continue;
action_enter_nt:
        IDBUG("A: enter_nt\n");
        *++s->sp     = (uint64_t)state_info.alt_state;
        s->cur_state = state_info.next_state;
        continue;
action_advance:
        IDBUG("A: advance (cur: '%c'(%d); expect: '%c'(%d))\n",
              *p, *p, state_info.match_char, state_info.match_char);
        goto *match_check_targets[!(*p - state_info.match_char)];
action_bool_select:
        IDBUG("A: bool_select\n");
        chr = state_info.list_address[(int)*p];
        goto *match_check_targets[(int)chr];
action_u8_select:
        IDBUG("A: u8_select\n");
        goto *match_check_targets[u8_continue[(int)((*p) >> 6)]];
item_doesnt_match:
        IDBUG("A: no_match\n");
        s->cur_state = state_info.alt_state;
        continue;
item_matches:
        IDBUG("it matches\n");
        s->cur_state = state_info.next_state;
        i++;
        p++;
        s->raw_offset++;
        continue;
action_white_space:
        IDBUG("A: ws\n");
        chr = *p;
        goto *space_check_targets[(int)(!(chr - ' '))];
nl_check:
        goto *nl_check_targets[(int)(!(chr - '\n'))];
cr_check:
        goto *cr_check_targets[(int)(!(chr - '\r'))];
tab_check:
        goto *tab_check_targets[(int)(!(chr - '\t'))];
found_space:
        IDBUG("-- space found\n");
        i++;
        p++;
        s->raw_offset++;
        continue;
action_select:
        IDBUG("A: select; chr = '%c'(%x)\n", (unsigned int)*p, (unsigned int)*p);
        s->cur_state = state_info.list_address[(int)*p++];
        i++;
        s->raw_offset++;
        continue;
    }

    state_info = n00b_jstate_table[s->cur_state];
    IDBUG("+cur state: %s (i = %d / %d)\n", state_info.name, n, n);
    goto *post_loop_dispatch[state_info.action];

need_input:
    return;
}

// ============================================================================
// Default stdout callback
// ============================================================================

static void
n00b_ijson_stdout(n00b_jparse_event_t event, uint64_t offset, void *ignore)
{
    (void)ignore;

    static const char *arr[] = {
        [N00B_JSON_START]      = "[json]",
        [N00B_JSON_END]        = "[/json]",
        [N00B_JSTRING_START]   = "[string]",
        [N00B_JSTRING_END]     = "[/string]",
        [N00B_JINT_START]      = "[number][int]",
        [N00B_JNEGINT_START]   = "[number][-int]",
        [N00B_JFRACT_START]    = "[fract]",
        [N00B_JEXP_START]      = "[exp]",
        [N00B_JNEG_EXP_START]  = "[-exp]",
        [N00B_JNUMBER_END]     = "[/number]",
        [N00B_JINT_END]        = "[/int]",
        [N00B_JFRACT_END]      = "[/fract]",
        [N00B_JEXP_END]        = "[/exp]",
        [N00B_JOBJECT_START]   = "[object]",
        [N00B_JOBJECT_END]     = "[/object]",
        [N00B_JARRAY_START]    = "[array]",
        [N00B_JARRAY_END]      = "[/array]",
        [N00B_JTRUE]           = "[true]",
        [N00B_JFALSE]          = "[false]",
        [N00B_Jnullptr]        = "[null]",
        [N00B_JERROR]          = "[error]",
    };

    printf("%s @%lld\n", arr[event], (long long)offset);

    if (event == N00B_JERROR) {
        abort();
    }
}

// ============================================================================
// Init / reset / delete
// ============================================================================

void
_n00b_ijson_init(n00b_ijson_ctx_t *stream) _kargs
{
    int64_t        stack_size = N00B_JSTACK_DEFAULT;
    void          *user_param = nullptr;
    n00b_jparse_cb callback   = nullptr;
}
{
    if (!callback) {
        callback = n00b_ijson_stdout;
    }

    auto r = n00b_mmap(stack_size);
    void *stack = n00b_result_get(r);

    *stream = (n00b_ijson_ctx_t){
        .stack      = stack,
        .sp         = stack,
        .raw_offset = 0,
        .stack_size = stack_size,
        .cur_state  = N00B_JST_READY,
        .callback   = callback,
        .user_param = user_param,
    };
}

void
n00b_ijson_reset(n00b_ijson_ctx_t *stream)
{
    stream->raw_offset = 0;
    stream->sp         = stream->stack;
    stream->cur_state  = N00B_JST_READY;
}

void
n00b_ijson_delete(n00b_ijson_ctx_t *stream)
{
    n00b_munmap(stream->stack);
}
