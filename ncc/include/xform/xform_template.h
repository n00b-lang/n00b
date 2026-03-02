#pragma once

/**
 * @file xform_template.h
 * @brief Parameterized code template engine for ncc transforms.
 *
 * Templates are C code strings with `$N` substitution slots.  At
 * registration time, the fixed portions are pre-lexed into token arrays.
 * At instantiation time, only the dynamic `$N` argument strings are lexed,
 * then spliced into the pre-lexed token runs and parsed as a single stream.
 *
 * ### Usage
 *
 * ```c
 * n00b_template_registry_t reg;
 * n00b_template_registry_init(&reg, grammar, tokenize_cb);
 * n00b_template_register(&reg, "my_tmpl", "primary_expression",
 *                        "($0 + $1)");
 *
 * const char *args[] = { "42", "x" };
 * n00b_result_t(n00b_parse_tree_ptr_t) r =
 *     n00b_template_instantiate(&reg, "my_tmpl", args, 2);
 * // ...
 * n00b_template_registry_free(&reg);
 * ```
 */

#include "xform/transform.h"
#include "core/dict.h"

// ============================================================================
// Error codes
// ============================================================================

/** @brief Template name not found in registry. */
#define N00B_TMPL_ERR_NOT_FOUND     10
/** @brief Argument count does not match template slot count. */
#define N00B_TMPL_ERR_ARG_COUNT     11
/** @brief Lexing a `$N` argument string failed. */
#define N00B_TMPL_ERR_ARG_LEX       12
/** @brief Parsing the assembled token stream failed. */
#define N00B_TMPL_ERR_PARSE_FAILED  13

// ============================================================================
// Types
// ============================================================================

/**
 * @brief One segment of a compiled template.
 *
 * Contains pre-lexed fixed tokens followed by a `$N` substitution
 * slot (or `slot_index == -1` for trailing text with no substitution).
 */
typedef struct {
    n00b_token_info_t **tokens;
    int32_t             count;
    int32_t             slot_index; /**< Slot number, or -1 if none. */
} n00b_template_segment_t;

/** @brief A compiled template ready for instantiation. */
typedef struct {
    char                    *name;
    char                    *start_symbol;
    n00b_template_segment_t *segments;
    int32_t                  num_segments;
    int32_t                  num_slots; /**< max($N) + 1 */
} n00b_compiled_template_t;

/** @brief Registry caching compiled templates by name. */
typedef struct {
    n00b_dict_t     templates; /**< name -> n00b_compiled_template_t* */
    n00b_grammar_t *grammar;
    n00b_scan_cb_t  tokenize;
} n00b_template_registry_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize a template registry.
 * @param reg       Registry to initialize.
 * @param grammar   Grammar for parsing (must be finalized).
 * @param tokenize  Tokenizer callback for lexing template fragments.
 */
void n00b_template_registry_init(n00b_template_registry_t *reg,
                                 n00b_grammar_t           *grammar,
                                 n00b_scan_cb_t            tokenize);

/** @brief Free all resources held by the registry. */
void n00b_template_registry_free(n00b_template_registry_t *reg);

/**
 * @brief Compile and cache a template.
 *
 * Scans @p template_text for `$N` substitution slots, pre-lexes the
 * fixed text between slots, and stores the compiled result keyed by
 * @p name.
 *
 * @param reg            Registry to add the template to.
 * @param name           Unique template name.
 * @param start_symbol   Grammar non-terminal to parse as.
 * @param template_text  Template string with `$N` placeholders.
 * @return True on success, false if fixed-segment lexing fails.
 */
bool n00b_template_register(n00b_template_registry_t *reg,
                            const char               *name,
                            const char               *start_symbol,
                            const char               *template_text);

/**
 * @brief Instantiate a registered template with argument strings.
 *
 * Each `$N` slot is replaced by the corresponding entry in @p args.
 * The assembled token stream is parsed against the template's start
 * symbol and the resulting subtree is returned.
 *
 * @param reg    Registry containing the template.
 * @param name   Template name (must have been registered).
 * @param args   Array of C strings, one per slot.
 * @param nargs  Number of arguments (must equal template's num_slots).
 * @return Ok with cloned parse tree, or err with error code.
 */
n00b_result_t(n00b_parse_tree_ptr_t)
n00b_template_instantiate(n00b_template_registry_t *reg,
                          const char               *name,
                          const char              **args,
                          int                       nargs);

/**
 * @brief Query the number of substitution slots in a registered template.
 *
 * @param reg   Registry containing the template.
 * @param name  Template name.
 * @return Slot count (>= 0), or -1 if not found.
 */
int n00b_template_slot_count(n00b_template_registry_t *reg,
                             const char               *name);
