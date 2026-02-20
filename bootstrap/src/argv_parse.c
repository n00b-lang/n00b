/**
 * @file argv_parse.c
 * @brief Command-line argument parser for NCC.
 *
 * Classifies compiler flags (`-c`, `-E`, `-o`, `-M*`, etc.),
 * identifies source files by extension, detects NCC-specific
 * flags (`--no-ncc`, `--dump-tokens`, `--modernize`), and
 * populates `ncc_argv_t` for downstream pipeline decisions.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "base_alloc_shim.h"
#include "argv_parse.h"
#include "macros.h"

#if !defined(NCC_EXT_ENV_VAR)
#define NCC_EXT_ENV_VAR "NCC_EXTENSIONS"
#endif

static char **default_extensions = nullptr;
static int    num_extensions     = 0;

static void
extract_extensions(char *str)
{
    if (!str) {
        return;
    }

    // Go through and count the periods.
    char *p = str;

    if (*p != '.') {
err:
        fprintf(stderr, "Extension lists must be dot-separated alpha-numeric values.\n");
        exit(-1);
    }

    // First count array size.
    while (*p) {
        if (*p++ == '.') {
            num_extensions++;
        }
    }

    char *end          = p;
    default_extensions = base_calloc(num_extensions, sizeof(char *));

    p = str;

    for (int i = 0; i < num_extensions; i++) {
        char *start = p;
        p++; // Skip past the period.

        while (*p != '.' && p < end) {
            if (!isalnum(*p)) {
                goto err;
            }
            p++;
        }
        char *s = base_calloc((p - start) + 1, sizeof(char));

        memcpy(s, start, p - start);
        default_extensions[i] = s;
    }
}

[[gnu::constructor]] static void
setup_extensions(void)
{
    char *alt_list = getenv(NCC_EXT_ENV_VAR);

    if (alt_list) {
        extract_extensions(alt_list);
    }
    else {
        extract_extensions(NCC_TO_STRING(NCC_EXTENSIONS));
    }
}

bool
cstr_ends_with(const char *str, const char *sub)
{
    int str_len = strlen(str);
    int sub_len = strlen(sub);

    if (str_len < sub_len) {
        return false;
    }

    const char *p = str + str_len;
    const char *q = sub + sub_len;

    for (int i = 0; i < sub_len; i++) {
        if (*--p != *--q) {
            return false;
        }
    }

    return true;
}

// Check if path has a source file extension we want to operate on,
// based on the 'extensions' option we got when we are built, or the
// NCC_EXTENSIONS environment variable (which overrides).
static bool
ncc_should_process(const char *path)
{
    if (!path) {
        return false;
    }

    for (int i = 0; i < num_extensions; i++) {
        if (cstr_ends_with(path, default_extensions[i])) {
            return true;
        }
    }

    return false;
}

static inline void
add_source_file(ncc_argv_t *ctx, char *file, int arg_index)
{
    ctx->sources[ctx->num_sources]          = file;
    ctx->source_indices[ctx->num_sources++] = arg_index;
}

/**
 * @brief Parse a flag that may have its value in the same arg or the next.
 * @param arg Current argument string
 * @param flag Flag prefix (e.g., "-o", "-x")
 * @param argc Total argument count
 * @param i Current index (updated if value is in next arg)
 * @param argv Argument vector
 * @param matched Output: Set to true if flag matched (even with no value)
 * @return Pointer to value string, or nullptr if no value available
 */
static char *
parse_flag_value(const char *arg,
                 const char *flag,
                 int         argc,
                 int        *i,
                 char      **argv,
                 bool       *matched)
{
    size_t flag_len = strlen(flag);
    *matched        = false;

    if (strcmp(arg, flag) == 0) {
        // Flag and value are separate arguments
        *matched = true;
        if (*i + 1 < argc) {
            (*i)++;
            return argv[*i];
        }
        return nullptr;
    }

    if (strncmp(arg, flag, flag_len) == 0 && strlen(arg) > flag_len) {
        // Value is in the same argument
        *matched = true;
        return (char *)(arg + flag_len);
    }

    return nullptr;
}

void
ncc_argv_parse(ncc_argv_t *ctx, int argc, char **argv)
{
    *ctx = (ncc_argv_t){
        .flag_E_index         = 0,
        .flag_c_index         = 0,
        .flag_o_index         = 0,
        .has_E                = false,
        .has_c                = false,
        .has_no_ncc           = false,
        .has_help             = false,
        .has_c23              = false,
        .has_dep_flags        = false,
        .dep_file             = nullptr,
        .dep_target_q         = nullptr,
        .dep_target           = nullptr,
        .passthrough_only     = false,
        .has_stdin            = false,
        .filename_in_same_arg = false,
        .language             = nullptr,
        .sources              = base_calloc(argc, sizeof(char *)),
        .source_indices       = base_calloc(argc, sizeof(int)),
        .num_sources          = 0,
        .argc                 = argc,
        .argv                 = argv,
    };

    if (!ctx->sources || !ctx->source_indices) {
        abort();
    }

    bool no_flags = false;
    int  last_arg = 0;

    // argv0 is always the program name.
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (no_flags) {
            // After --, only check for source files and stdin
            if (arg[0] == '-' && strlen(arg) == 1) {
                ctx->has_stdin = true;
            }
            else {
                if (ncc_should_process(arg)) {
                    add_source_file(ctx, arg, i);
                }
                last_arg = i;
            }
            continue;
        }
        if (arg[0] != '-') {
            last_arg = i;

            if (ncc_should_process(arg)) {
                add_source_file(ctx, arg, i);
            }
            continue;
        }

        if (!strcmp(arg, "--")) {
            no_flags = true;
            continue;
        }

        if (!strcmp(arg, "--no-ncc")) {
            ctx->has_no_ncc = true;
            // Remove --no-ncc from argv by shifting remaining args
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--; // Re-process this position since we shifted
            continue;
        }

        if (!strcmp(arg, "--ncc-help")) {
            ctx->has_help = true;
            // Remove --ncc-help from argv by shifting remaining args
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--;
            continue;
        }

        if (!strcmp(arg, "--modernize")) {
            ctx->has_modernize = true;
            // Remove --modernize from argv by shifting remaining args
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--;
            continue;
        }

        if (!strcmp(arg, "--modernize-overflow")) {
            ctx->has_modernize_overflow = true;
            ctx->has_modernize          = true; // implies --modernize
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--;
            continue;
        }

        if (strncmp(arg, "--ncc-constexpr-include=", 24) == 0) {
            const char *header = arg + 24;
            ctx->constexpr_headers
                = base_realloc(ctx->constexpr_headers,
                               (ctx->num_constexpr_headers + 1) * sizeof(const char *));
            ctx->constexpr_headers[ctx->num_constexpr_headers++] = header;
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--;
            continue;
        }

        if (!strcmp(arg, "-E")) {
            ctx->e_count++;
            if (!ctx->has_E) {
                ctx->has_E        = true;
                ctx->flag_E_index = i;
            }
            continue;
        }

        if (!strcmp(arg, "--dump-tokens")) {
            ctx->has_dump_tokens = true;
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argv[argc - 1] = nullptr;
            argc--;
            ctx->argc = argc;
            i--;
            continue;
        }

        // Flags that produce non-C output requiring passthrough
        if (!strcmp(arg, "-dM") || !strcmp(arg, "-dD") || !strcmp(arg, "-dN")
            || !strcmp(arg, "-dI")) {
            ctx->passthrough_only = true;
            continue;
        }

        if (!strcmp(arg, "-c")) {
            ctx->has_c        = true;
            ctx->flag_c_index = i;
            continue;
        }

        // Dependency generation flags (-MD/-MMD/-MF/-MQ/-MT)
        if (!strcmp(arg, "-MD") || !strcmp(arg, "-MMD")) {
            ctx->has_dep_flags = true;
            continue;
        }

        {
            bool  matched;
            char *value = parse_flag_value(arg, "-MF", argc, &i, argv, &matched);
            if (matched) {
                ctx->has_dep_flags = true;
                ctx->dep_file      = value;
                continue;
            }
        }

        {
            bool  matched;
            char *value = parse_flag_value(arg, "-MQ", argc, &i, argv, &matched);
            if (matched) {
                ctx->has_dep_flags = true;
                ctx->dep_target_q  = value;
                continue;
            }
        }

        {
            bool  matched;
            char *value = parse_flag_value(arg, "-MT", argc, &i, argv, &matched);
            if (matched) {
                ctx->has_dep_flags = true;
                ctx->dep_target    = value;
                continue;
            }
        }

        // Handle -o flag (output file)
        {
            bool  matched;
            int   saved_i = i;
            char *value   = parse_flag_value(arg, "-o", argc, &i, argv, &matched);
            if (matched) {
                ctx->flag_o_index         = saved_i;
                ctx->filename_in_same_arg = (i == saved_i) && value;
                continue;
            }
        }

        // Handle -x flag (language)
        {
            bool  matched;
            char *value = parse_flag_value(arg, "-x", argc, &i, argv, &matched);
            if (matched) {
                ctx->language = value;
                continue;
            }
        }

        // Handle -std flag (C standard version)
        {
            bool  matched;
            char *value = parse_flag_value(arg, "-std=", argc, &i, argv, &matched);
            if (!matched) {
                value = parse_flag_value(arg, "-std", argc, &i, argv, &matched);
            }
            if (matched && value) {
                if (!strcmp(value, "c23") || !strcmp(value, "gnu23") || !strcmp(value, "c2x")
                    || !strcmp(value, "gnu2x") || !strcmp(value, "c2y")
                    || !strcmp(value, "gnu2y")) {
                    ctx->has_c23 = true;
                }
                continue;
            }
        }

        if (!strcmp(arg, "-")) {
            ctx->has_stdin = true;
            continue;
        }
    }

    // If we didn't find something with a valid extension, but the
    // language is "c", then we assume the last argument wins. It's
    // a heuristic for sure (aka a bit janky).
    //
    // If the language isn't "c", we ignore any argument.
    if (ctx->language) {
        if (!strcmp(ctx->language, "c")) {
            if (!ctx->num_sources && last_arg) {
                add_source_file(ctx, argv[last_arg], last_arg);
            }
        }
        else {
            ctx->num_sources = 0;
        }
    }
}
