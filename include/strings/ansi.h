#pragma once
/** @file ansi.h
 *  @brief Incremental ANSI/VT escape sequence parser and convenience macros.
 *
 *  Provides a state-machine parser that classifies raw terminal output into
 *  text spans, C0/C1 control codes, CSI sequences, nF/Fp/Fe/Fs sequences,
 *  and control strings (DCS, OSC, PM, APC, SOS).  Designed for incremental
 *  use: call `n00b_ansi_parse()` with successive buffers and retrieve
 *  completed nodes via `n00b_ansi_parser_results()`.
 *
 *  ### Related modules
 *
 *  - `core/buffer.h` -- input buffer type
 *  - `unicode/encoding.h` -- UTF-8 decode for codepoint iteration
 *  - `unicode/properties.h` -- general category for control detection
 *  - `strings/string_convert.h` -- hex encoding for repr output
 */

#include "unicode/types_ext.h"
#include "core/list.h"
#include "core/buffer.h"

// ===================================================================
// ANSI node classification
// ===================================================================

/** @brief Classification of a parsed ANSI node. */
typedef enum {
    N00B_ANSI_TEXT,             /**< Printable text run */
    N00B_ANSI_C0_CODE,         /**< C0 control code (0x00-0x1F) */
    N00B_ANSI_C1_CODE,         /**< C1 control code (0x80-0x9F) */
    N00B_ANSI_NF_SEQUENCE,     /**< nF escape sequence (ESC 0x20-0x2F ... 0x30-0x7E) */
    N00B_ANSI_FP_SEQUENCE,     /**< Fp escape sequence (ESC 0x30-0x3F) */
    N00B_ANSI_FE_SEQUENCE,     /**< Fe escape sequence (ESC 0x40-0x5F) */
    N00B_ANSI_FS_SEQUENCE,     /**< Fs escape sequence (ESC 0x60-0x7E) */
    N00B_ANSI_CONTROL_SEQUENCE,/**< CSI control sequence */
    N00B_ANSI_PRIVATE_CONTROL, /**< CSI with private parameter indicator */
    N00B_ANSI_CTL_STR_CHAR,    /**< Character-oriented control string (SOS) */
    N00B_ANSI_CRL_STR_CMD,     /**< Command control string (DCS/OSC/PM/APC) */
    N00B_ANSI_PARTIAL,         /**< Incomplete sequence (awaiting more data) */
    N00B_ANSI_INVALID,         /**< Malformed sequence */
} n00b_ansi_kind;

// ===================================================================
// ANSI control detail
// ===================================================================

/** @brief Detail fields for CSI and private control sequences. */
typedef struct {
    char   *pstart;    /**< Start of parameter bytes */
    char   *istart;    /**< Start of intermediate bytes */
    int     plen;      /**< Length of parameter bytes */
    int     ilen;      /**< Length of intermediate bytes */
    uint8_t ppi;       /**< Private parameter indicator (only for private) */
    uint8_t ctrl_byte; /**< Final byte / C0 control byte */
} n00b_ansi_ctrl_t;

// ===================================================================
// ANSI node
// ===================================================================

/** @brief A single parsed ANSI element (text run or escape sequence). */
typedef struct {
    char            *start;  /**< Start of this node's raw bytes */
    char            *end;    /**< One past the last byte */
    n00b_ansi_ctrl_t ctrl;   /**< Control details (valid for control nodes) */
    n00b_ansi_kind   kind;   /**< Classification of this node */
} n00b_ansi_node_t;

// Declare the typed list for ANSI node pointers.
n00b_list_decl(n00b_ansi_node_t *);

// ===================================================================
// Parser context
// ===================================================================

/** @brief Incremental ANSI parser state.
 *
 *  Created with `n00b_ansi_parser_create()`, fed with `n00b_ansi_parse()`.
 */
typedef struct {
    char                              *cur;     /**< Current parse position */
    char                              *end;     /**< End of current buffer */
    n00b_list_t(n00b_ansi_node_t *)    results; /**< Parsed node list */
} n00b_ansi_ctx;

// ===================================================================
// Public API
// ===================================================================

/** @brief Create a new ANSI parser context.
 *  @kw allocator  Optional allocator.
 *  @return A heap-allocated parser context.
 *  @post Caller must free with `n00b_free()`.
 */
n00b_ansi_ctx *n00b_ansi_parser_create()
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Feed a buffer of bytes to the parser.
 *
 *  Parses all complete sequences and appends nodes to the context's
 *  results list.  Partial sequences are retained for combining with
 *  the next call.
 *
 *  @param ctx  Parser context.
 *  @param buf  Input buffer (not modified; data must remain valid).
 *
 *  @pre  @p ctx was created by `n00b_ansi_parser_create()`.
 *  @pre  @p buf data remains valid until `n00b_ansi_parser_results()`.
 */
void n00b_ansi_parse(n00b_ansi_ctx *ctx, n00b_buffer_t *buf);

/** @brief Retrieve and reset the parsed results.
 *
 *  Returns the current results list (by value) and installs a fresh
 *  empty list in the context.  If the last node is PARTIAL, it is
 *  carried over to the new list for the next parse call.
 *
 *  @param ctx  Parser context.
 *  @return A typed list of parsed node pointers.
 *
 *  @post The returned list is owned by the caller.
 */
n00b_list_t(n00b_ansi_node_t *) n00b_ansi_parser_results(n00b_ansi_ctx *ctx);

/** @brief Concatenate ANSI nodes back into a string.
 *
 *  @param nodes         Typed list of ANSI node pointers.
 *  @param keep_control  If false, strip non-text control sequences
 *                       (newlines and tabs are always kept).
 *  @kw allocator  Optional allocator.
 *  @return The reconstituted string.
 */
n00b_string_t n00b_ansi_nodes_to_string(
    n00b_list_t(n00b_ansi_node_t *) nodes, bool keep_control)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// ANSI escape string macros
// ===================================================================

#define N00B_ANSI_ENABLE_WINDOW_MARGINS "\e[?69h"

#define n00b_ansi_dim                  "\e[2m"
#define n00b_ansi_reset                "\e[0m"
#define n00b_ansi_inverse              "\e[7m"
#define n00b_ansi_reset_inverse        "\e[27m"
#define n00b_erase_below_cursor        "\e[0J"
#define n00b_erase_above_cursor        "\e[1J"
#define n00b_erase_screen              "\e[2J"
#define n00b_erase_backscroll          "\e[3J"
#define n00b_erase_line_to_right       "\e[0K"
#define n00b_erase_line_to_left        "\e[1K"
#define n00b_erase_full_line           "\e[2K"
#define n00b_ansi_save_cursor          "\e[s"
#define n00b_ansi_restore_cursor       "\e[u"
#define n00b_cursor_up_fmt             "\e[%dA"
#define n00b_cursor_down_fmt           "\e[%dB"
#define n00b_cursor_right_fmt          "\e[%dC"
#define n00b_cursor_left_fmt           "\e[%dD"
#define n00b_cursor_down_cr_fmt        "\e[%dE"
#define n00b_cursor_up_cr_fmt          "\e[%dF"
#define n00b_cursor_set_col_fmt        "\e[%dG"
#define n00b_cursor_setpos_fmt         "\e[%d;%dH"
#define n00b_scroll_up_fmt             "\e[%dS"
#define n00b_scroll_down_fmt           "\e[%dT"
#define n00b_jump_page_fmt             "\e[%dU"
#define n00b_hstop_this_col            "\e[0W"
#define n00b_vstop_this_row            "\e[1W"
#define n00b_hstop_clear_this_col      "\e[2W"
#define n00b_vstop_clear_this_row      "\e[3W"
#define n00b_clear_hstops_line         "\e[4W"
#define n00b_clear_all_hstops          "\e[5W"
#define n00b_clear_all_vstops          "\e[6W"
#define n00b_cursor_tab_fwd_fmt        "\e[%dI"
#define n00b_cursor_tab_back_fmt       "\e[%dZ"
#define n00b_cursor_tab_down_fmt       "\e[%dX"
#define n00b_cursor_tab_up_fmt         "\e[%dY"
#define n00b_reset_line_full           "\e[1K\e[1F\e[1E"
#define n00b_vmargin_fmt               "\e[%d;%dr"
#define n00b_hmargin_fmt               "\e[%d;%ds"
#define n00b_alt_screen_disable        "\e[?1046h"
#define n00b_alt_screen                "\e[?47"
#define n00b_alt_screen_clear_on_exit  "\e[?1047h"
#define n00b_alt_screen_clear_on_enter "\e[?1049h"
#define n00b_main_screen               "\e[?1049l"
