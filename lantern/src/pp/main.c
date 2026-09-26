#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/commander.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_print.h"
#include "scanner/lex.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "private/parse/grammar.h"

#define PP_OK    0
#define PP_ERROR 1
#define PP_USAGE 2

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    const char *name;
    const char *grammar;
    const char *start;
    const char *exts;
} language_t;

static const language_t LANGUAGES[] = {
    {"cel",        "cel.bnf",        "expr",                  ".cel"          },
    {"csharp",     "csharp.bnf",     "compilation-unit",      ".cs"           },
    {"go",         "go.bnf",         "source-file",           ".go"           },
    {"java",       "java.bnf",       "compilation-unit",      ".java"         },
    {"javascript", "javascript.bnf", "module",                ".js.mjs.cjs.jsx"},
    {"json",       "json.bnf",       "json",                  ".json"         },
    {"kotlin",     "kotlin.bnf",     "kotlin-file",           ".kt.kts"       },
    {"lua",        "lua.bnf",        "chunk",                 ".lua"          },
    {"php",        "php.bnf",        "top-statement-list",    ".php"          },
    {"python",     "python.bnf",     "file",                  ".py.pyi"       },
    {"ruby",       "ruby.bnf",       "program",               ".rb"           },
    {"rust",       "rust.bnf",       "crate",                 ".rs"           },
    {"swift",      "swift.bnf",      "top-level-declaration", ".swift"        },
    {"typescript", "typescript.bnf", "source-file",           ".ts.tsx.mts"   },
    {"c",          "c_ncc.bnf",      "translation_unit",      ".c.h"          },
    {"bce",        "bce.bnf",        "translation_unit",      ".bce"          },
    {"n00b",       "n00b.bnf",       "module",                ".n00b"         },
};

#define N_LANGUAGES (sizeof(LANGUAGES) / sizeof(LANGUAGES[0]))

static const parse_cmdr_flag_t FLAGS[] = {
    {S("--language"), S("-l"), PARSE_CMDR_TYPE_WORD, true,
     S("override the language instead of guessing from the extension")},
    {S("--grammar"), S("-g"), PARSE_CMDR_TYPE_WORD, true,
     S("read the grammar from this .bnf file instead of the built-in one")},
    {S("--start"), {0}, PARSE_CMDR_TYPE_WORD, true,
     S("start symbol, when it is not the grammar's first production")},
    {S("--width"), S("-w"), PARSE_CMDR_TYPE_INT, true,
     S("column to wrap at (default 80)")},
    {S("--indent"), S("-i"), PARSE_CMDR_TYPE_INT, true,
     S("spaces per indent level (default 4)")},
    {S("--tokens"), S("-t"), PARSE_CMDR_TYPE_BOOL, false,
     S("write the token stream instead of formatted source")},
    {S("--languages"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     S("list the languages that are known, and exit")},
    {S("--grammar-dir"), {0}, PARSE_CMDR_TYPE_WORD, true,
     S("where to look for the built-in grammars")},
    {S("--help"), S("-h"), PARSE_CMDR_TYPE_BOOL, false, S("show this help")},
    {{0}, {0}, 0, false, {0}},
};

static const parse_cmdr_cmd_t COMMANDS[] = {
    {{0}, {0}, nullptr, {0}, 0, 0},
};

static const char *
ext_of(const char *path)
{
    const char *dot   = nullptr;
    const char *slash = strrchr(path, '/');

    for (const char *p = slash ? slash + 1 : path; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    return dot;
}

static const language_t *
language_named(const char *name)
{
    for (size_t i = 0; i < N_LANGUAGES; i++) {
        if (!strcmp(LANGUAGES[i].name, name)) {
            return &LANGUAGES[i];
        }
    }

    return nullptr;
}

static const language_t *
language_for_path(const char *path)
{
    const char *ext = ext_of(path);

    if (!ext) {
        return nullptr;
    }

    size_t n = strlen(ext);

    for (size_t i = 0; i < N_LANGUAGES; i++) {
        const char *list = LANGUAGES[i].exts;

        for (const char *p = list; *p; p++) {
            if (*p != '.') {
                continue;
            }

            size_t len = 1;

            while (p[len] && p[len] != '.') {
                len++;
            }

            if (len == n && !strncmp(p, ext, n)) {
                return &LANGUAGES[i];
            }
        }
    }

    return nullptr;
}

static char *
slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        return nullptr;
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    if (n < 0) {
        fclose(f);

        return nullptr;
    }

    char *p = core_alloc_size(1, (size_t)n + 1);

    *len = fread(p, 1, (size_t)n, f);
    p[*len] = '\0';
    fclose(f);

    return p;
}

static void
list_languages(FILE *out)
{
    fprintf(out, "%-12s %-22s %s\n", "language", "grammar", "extensions");

    for (size_t i = 0; i < N_LANGUAGES; i++) {
        fprintf(out, "%-12s %-22s ", LANGUAGES[i].name, LANGUAGES[i].grammar);

        for (const char *p = LANGUAGES[i].exts; *p; p++) {
            if (*p == '.' && p != LANGUAGES[i].exts) {
                fputc(' ', out);
            }

            fputc(*p, out);
        }

        fputc('\n', out);
    }
}

static parse_grammar_t *
load_grammar(const char *path, const char *start, FILE *err)
{
    size_t n    = 0;
    char  *text = slurp(path, &n);

    if (!text) {
        fprintf(err, "pp: cannot read grammar %s\n", path);

        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();
    core_string_t   s = core_string_borrow(text, n);

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_declare_terminals(g, s);

    core_string_t start_s = start ? core_string_borrow(start, strlen(start))
                                 : (core_string_t){0};

    if (!parse_bnf_load_file(path, start_s, g)) {
        fprintf(err, "pp: %s did not load\n", path);
        parse_grammar_free(g);
        core_free(text);

        return nullptr;
    }

    size_t n_undef = parse_grammar_undefined(g, nullptr, 0);

    if (n_undef) {
        core_list_t(core_string_t) undefined
            = core_list_new_cap(core_string_t, n_undef);

        undefined.len = parse_grammar_undefined(g, undefined.data, n_undef);

        fprintf(err, "pp: %s references %zu undefined nonterminal(s):",
                path, undefined.len);

        for (size_t i = 0; i < undefined.len; i++) {
            fprintf(err, " <%.*s>", (int)undefined.data[i].u8_bytes,
                    undefined.data[i].data);
        }

        fputc('\n', err);
        core_list_free(undefined);
        parse_grammar_free(g);
        core_free(text);

        return nullptr;
    }

    if (!scan_grammar_lex(g)) {
        fprintf(err, "pp: %s declares no lexis\n", path);
    }

    const char *lex_err = scan_lex_build_error();

    if (lex_err) {
        fprintf(err, "pp: %s: %s\n", path, lex_err);
        parse_grammar_free(g);
        core_free(text);

        return nullptr;
    }

    core_free(text);

    return g;
}

static int
write_tokens(parse_grammar_t *g, parse_token_stream_t *ts, FILE *out)
{
    scan_stream_foreach(ts, tok) {
        core_string_t spelling = parse_terminal_name(g, tok->tid);
        core_string_t value    = core_option_is_set(tok->value)
                                  ? core_option_get(tok->value)
                                  : (core_string_t){0};

        if (spelling.data) {
            fprintf(out, "%-22.*s", (int)spelling.u8_bytes, spelling.data);
        }
        else if (tok->tid > 0 && tok->tid < 128) {
            fprintf(out, "%-22c", (char)tok->tid);
        }
        else {
            fprintf(out, "%-22d", tok->tid);
        }

        fprintf(out, " %u:%u", tok->line, tok->column);

        if (value.data && value.u8_bytes) {
            fprintf(out, "  %.*s", (int)value.u8_bytes, value.data);
        }

        fputc('\n', out);
    }

    return PP_OK;
}

int
main(int argc, char **argv)
{
    const parse_cmdr_spec_t spec = {
        .tool     = S("pp"),
        .usage    = S("pp [flags] FILE - pretty-print source code"),
        .flags    = FLAGS,
        .commands = COMMANDS,
        .arg      = S("FILE"),
        .min_args = 1,
        .max_args = 1,
    };

    parse_cmdr_t        *c = parse_cmdr_new(&spec);
    parse_cmdr_result_t *r = parse_cmdr_parse(c, argc - 1,
                                          (const char **)argv + 1);

    if (!r || !parse_cmdr_result_ok(r)) {
        parse_cmdr_print_usage(c, stderr);

        return PP_USAGE;
    }

    if (parse_cmdr_flag_present(r, S("--help"))) {
        parse_cmdr_print_usage(c, stdout);

        return PP_OK;
    }

    if (parse_cmdr_flag_present(r, S("--languages"))) {
        list_languages(stdout);

        return PP_OK;
    }

    if (parse_cmdr_arg_count(r) != 1) {
        fprintf(stderr, "pp: expected exactly one FILE\n");
        parse_cmdr_print_usage(c, stderr);

        return PP_USAGE;
    }

    core_string_t arg = parse_cmdr_arg_str(r, 0);
    const char *path = arg.data;

    const char *dir = getenv("PP_GRAMMAR_DIR");

    if (parse_cmdr_flag_present(r, S("--grammar-dir"))) {
        core_string_t d = parse_cmdr_flag_str(r, S("--grammar-dir"));
        dir = d.data;
    }


    const language_t *lang = nullptr;

    if (parse_cmdr_flag_present(r, S("--language"))) {
        core_string_t l = parse_cmdr_flag_str(r, S("--language"));
        lang = language_named(l.data);

        if (!lang) {
            fprintf(stderr, "pp: no language named '%.*s'\n",
                    (int)l.u8_bytes, l.data);
            list_languages(stderr);

            return PP_USAGE;
        }
    }

    char *owned_grammar_path = nullptr;
    const char *grammar_path = nullptr;

    if (parse_cmdr_flag_present(r, S("--grammar"))) {
        core_string_t gp = parse_cmdr_flag_str(r, S("--grammar"));

        grammar_path = gp.data;
    }
    else {
        if (!lang) {
            lang = language_for_path(path);
        }

        if (!lang) {
            const char *ext = ext_of(path);

            fprintf(stderr,
                    "pp: no language for %s%s%s; pass --language or "
                    "--grammar\n",
                    ext ? "extension " : "",
                    ext ? ext : path,
                    "");
            list_languages(stderr);

            return PP_USAGE;
        }

        const char *where[] = {
            dir,
#ifdef PP_GRAMMAR_DIR
            PP_GRAMMAR_DIR,
#endif
            "grammars",
            "../grammars",
            "../../grammars",
        };

        bool found = false;

        for (size_t i = dir ? 0 : 1;
             !found && i < sizeof(where) / sizeof(where[0]);
             i++) {
            core_buffer_t *candidate = core_buffer_empty();
            core_buffer_printf(candidate, "%s/%s", where[i], lang->grammar);
            char *candidate_path = core_buffer_take(candidate);

            FILE *probe = fopen(candidate_path, "r");

            if (probe) {
                fclose(probe);
                owned_grammar_path = candidate_path;
                grammar_path = owned_grammar_path;
                found = true;
            }
            else {
                core_free(candidate_path);
            }
        }

        if (!found) {
            fprintf(stderr,
                    "pp: cannot find %s; pass --grammar-dir or set "
                    "PP_GRAMMAR_DIR\n",
                    lang->grammar);

            return PP_ERROR;
        }
    }

    const char *start = lang ? lang->start : nullptr;

    if (parse_cmdr_flag_present(r, S("--start"))) {
        core_string_t st = parse_cmdr_flag_str(r, S("--start"));
        start = st.data;
    }

    parse_grammar_t *g = load_grammar(grammar_path, start, stderr);
    core_free(owned_grammar_path);

    if (!g) {
        return PP_ERROR;
    }

    size_t src_len = 0;
    char  *src     = slurp(path, &src_len);

    if (!src) {
        fprintf(stderr, "pp: cannot read %s\n", path);
        parse_grammar_free(g);

        return PP_ERROR;
    }

    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src, src_len, &buf, &sc);

    if (!ts) {
        fprintf(stderr, "pp: %s did not tokenize\n", path);
        core_free(src);
        parse_grammar_free(g);

        return PP_ERROR;
    }

    int rc = PP_OK;

    if (parse_cmdr_flag_present(r, S("--tokens"))) {
        rc = write_tokens(g, ts, stdout);
    }
    else {
        parse_nt_id_t start_id = start
            ? parse_nonterm_id(parse_find_nonterm(
                  g, core_string_from_cstr(start)))
            : g->nt_list.data[0]->id;
        parse_pwz_parser_t *p = parse_pwz_new_for_start(g, start_id);

        if (!parse_pwz_parse(p, ts)) {
            int32_t at = parse_pwz_error_pos(p);

            scan_stream_reset(ts);

            parse_token_info_t *tok = at >= 0 ? scan_stream_get(ts, at)
                                            : nullptr;

            if (tok) {
                core_string_t name = parse_terminal_name(g, tok->tid);
                core_string_t text = core_option_is_set(tok->value)
                                      ? core_option_get(tok->value)
                                      : (core_string_t){0};

                fprintf(stderr,
                        "pp: %s:%u:%u: no rule accepts ",
                        path,
                        tok->line,
                        tok->column);

                if (name.data) {
                    fprintf(stderr, "%%%.*s", (int)name.u8_bytes, name.data);
                }
                else if (tok->tid > 0 && tok->tid < 128) {
                    fprintf(stderr, "'%c'", (char)tok->tid);
                }
                else {
                    fprintf(stderr, "token %d", tok->tid);
                }

                if (text.data && text.u8_bytes && text.data[0] != '\n') {
                    fprintf(stderr, " (%.*s)", (int)text.u8_bytes, text.data);
                }

                fprintf(stderr, ", token %d of %d\n", at,
                        scan_stream_token_count(ts));
            }
            else {
                fprintf(stderr, "pp: %s did not parse\n", path);
            }

            rc = PP_ERROR;
        }
        else {
            int32_t width  = 80;
            int32_t indent = 4;

            width  = (int32_t)parse_cmdr_flag_int(r, S("--width"), width);
            indent = (int32_t)parse_cmdr_flag_int(r, S("--indent"), indent);

            parse_print_opts_t o = {
                .source = core_string_borrow(src, src_len),
                .width  = width,
                .indent = indent,
            };

            char *text = parse_tree_print(g, parse_pwz_get_tree(p), &o);

            if (text) {
                fputs(text, stdout);

                if (text[0] && text[strlen(text) - 1] != '\n') {
                    fputc('\n', stdout);
                }

                core_free(text);
            }
            else {
                fprintf(stderr, "pp: %s produced no output\n", path);
                rc = PP_ERROR;
            }
        }

        parse_pwz_free(p);
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
    core_free(src);
    parse_grammar_free(g);
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);

    return rc;
}
