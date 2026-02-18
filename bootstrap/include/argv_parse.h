/**
 * @file argv_parse.h
 * @brief Minimal, conservative argv scanning for ncc.
 *
 * Extracts only the information needed to ensure proper passthrough to the
 * underlying compiler. Does not modify the array in place.
 *
 * This approach avoids encoding knowledge of every flag for every compiler.
 * Source files are identified heuristically based on file extension.
 * By default, matches ".c" and ".nc" extensions (configurable via meson).
 */
#pragma once

/**
 * @brief Parsed command-line arguments for ncc.
 */
typedef struct {
    /** @name Preprocessing flags */
    /** @{ */
    int  flag_E_index;           /**< Index of first -E flag in argv (0 if absent) */
    int  flag_c_index;           /**< Index of -c flag in argv (0 if absent) */
    int  e_count;                /**< Number of -E flags (bootstrap only) */
    bool has_E;                  /**< True if -E (preprocess only) specified */
    bool has_c;                  /**< True if -c (compile only) specified */
    bool has_raw_cpp;            /**< True if --ncc-raw-cpp (stop after CPP) specified */
    bool has_no_ncc;             /**< True if --no-ncc (disable ncc processing) specified */
    bool has_help;               /**< True if --ncc-help (show ncc help) specified */
    bool has_modernize;          /**< True if --modernize (C23 modernization) specified */
    bool has_modernize_overflow; /**< True if --modernize-overflow (comment-only) */
    bool has_dump_tokens;        /**< True if --dump-tokens specified */
    bool passthrough_only;       /**< True if flags like -dM/-dD require passthrough */
    bool implicit_cmd;           /**< True if -c/-E was inferred, not in original argv */
    /** @} */

    /** @name Output specification */
    /** @{ */
    int  flag_o_index;         /**< Index of -o flag in argv (-1 if absent) */
    bool filename_in_same_arg; /**< True if filename attached to -o (e.g., -ofile) */
    /** @} */

    /** @name Language standard */
    /** @{ */
    bool has_c23; /**< True if -std=c23/c2y/gnu23/gnu2y specified */
    /** @} */

    /** @name Dependency generation flags */
    /** @{ */
    bool  has_dep_flags; /**< True if -MD or -MMD present (dep generation requested) */
    char *dep_file;      /**< Dependency output file from -MF (nullptr if absent) */
    char *dep_target_q;  /**< Dependency target from -MQ, make-quoted (nullptr if absent) */
    char *dep_target;    /**< Dependency target from -MT, unquoted (nullptr if absent) */
    /** @} */

    /** @name Input sources */
    /** @{ */
    bool   has_stdin;      /**< True if "-" found as source (stdin input) */
    char  *language;       /**< Language specified by -x flag (may be nullptr) */
    char **sources;        /**< Array of source file paths */
    int   *source_indices; /**< Indices of source files in original argv */
    int    num_sources;    /**< Number of source files found */
    /** @} */

    /** @name Constexpr headers */
    /** @{ */
    const char **constexpr_headers;     /**< Headers for constexpr compilation */
    int          num_constexpr_headers; /**< Count of constexpr headers */
    /** @} */

    /** @name Original arguments */
    /** @{ */
    char **argv; /**< Original argument vector */
    int    argc; /**< Original argument count */
    /** @} */
} ncc_argv_t;

/**
 * @brief Parse command-line arguments.
 *
 * Scans argv for compiler flags, output specifications, and source files.
 * Populates the context structure without modifying the original argv.
 *
 * @param ctx Context structure to populate
 * @param argc Argument count
 * @param argv Argument vector
 */
extern void ncc_argv_parse(ncc_argv_t *ctx, int argc, char **argv);
