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
    core_string_t name;
    core_string_t grammar;
    core_string_t start;
    core_string_t exts;
} language_t;

static const language_t LANGUAGES[] = {
    {S("cel"),        S("cel.bnf"),        S("expr"),                  S(".cel")          },
    {S("csharp"),     S("csharp.bnf"),     S("compilation-unit"),      S(".cs")           },
    {S("go"),         S("go.bnf"),         S("source-file"),           S(".go")           },
    {S("java"),       S("java.bnf"),       S("compilation-unit"),      S(".java")         },
    {S("javascript"), S("javascript.bnf"), S("module"),                S(".js.mjs.cjs.jsx")},
    {S("json"),       S("json.bnf"),       S("json"),                  S(".json")         },
    {S("kotlin"),     S("kotlin.bnf"),     S("kotlin-file"),           S(".kt.kts")       },
    {S("lua"),        S("lua.bnf"),        S("chunk"),                 S(".lua")          },
    {S("php"),        S("php.bnf"),        S("top-statement-list"),    S(".php")          },
    {S("python"),     S("python.bnf"),     S("file"),                  S(".py.pyi")       },
    {S("ruby"),       S("ruby.bnf"),       S("program"),               S(".rb")           },
    {S("rust"),       S("rust.bnf"),       S("crate"),                 S(".rs")           },
    {S("swift"),      S("swift.bnf"),      S("top-level-declaration"), S(".swift")        },
    {S("typescript"), S("typescript.bnf"), S("source-file"),           S(".ts.tsx.mts")   },
    {S("c"),          S("c_ncc.bnf"),      S("translation_unit"),      S(".c.h")          },
    {S("bce"),        S("bce.bnf"),        S("translation_unit"),      S(".bce")          },
};

#define N_LANGUAGES (sizeof(LANGUAGES) / sizeof(LANGUAGES[0]))

static const parse_cmdr_flag_t FLAGS[] = {
    {S("--language"), S("-l"), PARSE_CMDR_TYPE_WORD, true,
     S("override the language instead of guessing from the extension"), false,
     nullptr},
    {S("--grammar"), S("-g"), PARSE_CMDR_TYPE_WORD, true,
     S("read the grammar from this .bnf file instead of the built-in one"),
     false, nullptr},
    {S("--start"), {0}, PARSE_CMDR_TYPE_WORD, true,
     S("start symbol, when it is not the grammar's first production"), false,
     nullptr},
    {S("--width"), S("-w"), PARSE_CMDR_TYPE_INT, true,
     S("column to wrap at (default 80)"), false, nullptr},
    {S("--indent"), S("-i"), PARSE_CMDR_TYPE_INT, true,
     S("spaces per indent level (default 4)"), false, nullptr},
    {S("--tokens"), S("-t"), PARSE_CMDR_TYPE_BOOL, false,
     S("write the token stream instead of formatted source"), false, nullptr},
    {S("--languages"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     S("list the languages that are known, and exit"), false, nullptr},
    {S("--grammar-dir"), {0}, PARSE_CMDR_TYPE_WORD, true,
     S("where to look for the built-in grammars"), false, nullptr},
    {S("--help"), S("-h"), PARSE_CMDR_TYPE_BOOL, false, S("show this help"),
     false, nullptr},
    {0},
};

static const parse_cmdr_cmd_t COMMANDS[] = {
    {0},
};

static core_string_t
ext_of(core_string_t path)
{
    int64_t slash = core_string_rfind_byte(path, '/');
    size_t  base  = slash >= 0 ? (size_t)slash + 1 : 0;
    int64_t dot   = core_string_rfind_byte(
        core_string_slice(path, base, path.u8_bytes - base), '.');

    if (dot < 0) {
        return (core_string_t){0};
    }

    size_t at = base + (size_t)dot;

    return core_string_slice(path, at, path.u8_bytes - at);
}

static const language_t *
language_named(core_string_t name)
{
    for (size_t i = 0; i < N_LANGUAGES; i++) {
        if (core_string_eq(LANGUAGES[i].name, name)) {
            return &LANGUAGES[i];
        }
    }

    return nullptr;
}

static const language_t *
language_for_path(core_string_t path)
{
    core_string_t ext = ext_of(path);

    if (!ext.data) {
        return nullptr;
    }

    for (size_t i = 0; i < N_LANGUAGES; i++) {
        core_string_t list = LANGUAGES[i].exts;

        for (size_t p = 0; p < list.u8_bytes; p++) {
            if (list.data[p] != '.') {
                continue;
            }

            size_t len = 1;

            while (p + len < list.u8_bytes && list.data[p + len] != '.') {
                len++;
            }

            if (core_string_eq(core_string_slice(list, p, len), ext)) {
                return &LANGUAGES[i];
            }
        }
    }

    return nullptr;
}

static core_string_t
slurp(core_string_t path)
{
    char *cpath = core_string_to_cstr(path);
    FILE *f     = fopen(cpath, "rb");

    core_free(cpath);

    if (!f) {
        return (core_string_t){0};
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    if (n < 0) {
        fclose(f);

        return (core_string_t){0};
    }

    char  *p   = core_alloc_size(1, (size_t)n + 1);
    size_t len = fread(p, 1, (size_t)n, f);

    fclose(f);

    return (core_string_t){.data = p, .u8_bytes = len};
}

static void
list_languages(FILE *out)
{
    fprintf(out, "%-12s %-22s %s\n", "language", "grammar", "extensions");

    for (size_t i = 0; i < N_LANGUAGES; i++) {
        core_string_t exts = LANGUAGES[i].exts;

        fprintf(out, "%-12.*s %-22.*s ",
                (int)LANGUAGES[i].name.u8_bytes, LANGUAGES[i].name.data,
                (int)LANGUAGES[i].grammar.u8_bytes, LANGUAGES[i].grammar.data);

        for (size_t p = 0; p < exts.u8_bytes; p++) {
            if (exts.data[p] == '.' && p != 0) {
                fputc(' ', out);
            }

            fputc(exts.data[p], out);
        }

        fputc('\n', out);
    }
}

static parse_grammar_t *
load_grammar(core_string_t path, core_string_t start, FILE *err)
{
    core_string_t text = slurp(path);

    if (!text.data) {
        fprintf(err, "pp: cannot read grammar %.*s\n",
                (int)path.u8_bytes, path.data);

        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_declare_terminals(g, text);

    if (!parse_bnf_load_file(path, start, g)) {
        fprintf(err, "pp: %.*s did not load\n", (int)path.u8_bytes, path.data);
        parse_grammar_free(g);
        core_free(text.data);

        return nullptr;
    }

    size_t n_undef = parse_grammar_undefined(g, nullptr, 0);

    if (n_undef) {
        core_list_t(core_string_t) undefined
            = core_list_new_cap(core_string_t, n_undef);

        undefined.len = parse_grammar_undefined(g, undefined.data, n_undef);

        fprintf(err, "pp: %.*s references %zu undefined nonterminal(s):",
                (int)path.u8_bytes, path.data, undefined.len);

        for (size_t i = 0; i < undefined.len; i++) {
            fprintf(err, " <%.*s>", (int)undefined.data[i].u8_bytes,
                    undefined.data[i].data);
        }

        fputc('\n', err);
        core_list_free(undefined);
        parse_grammar_free(g);
        core_free(text.data);

        return nullptr;
    }

    if (!scan_grammar_lex(g)) {
        fprintf(err, "pp: %.*s declares no lexis\n",
                (int)path.u8_bytes, path.data);
    }

    core_string_t lex_err = scan_lex_build_error();

    if (lex_err.data) {
        fprintf(err, "pp: %.*s: %.*s\n", (int)path.u8_bytes, path.data,
                (int)lex_err.u8_bytes, lex_err.data);
        parse_grammar_free(g);
        core_free(text.data);

        return nullptr;
    }

    core_free(text.data);

    return g;
}

static int
write_tokens(parse_grammar_t *g, parse_token_stream_t *ts, FILE *out)
{
    scan_stream_foreach(ts, tok) {
        int32_t       tid      = parse_token_tid(tok);
        core_string_t spelling = parse_terminal_name(g, tid);
        core_option_t(core_string_t) token_value = parse_token_value(tok);
        core_string_t value    = core_option_is_set(token_value)
                                  ? core_option_get(token_value)
                                  : (core_string_t){0};

        if (spelling.data) {
            fprintf(out, "%-22.*s", (int)spelling.u8_bytes, spelling.data);
        }
        else if (tid > 0 && tid < 128) {
            fprintf(out, "%-22c", (char)tid);
        }
        else {
            fprintf(out, "%-22d", tid);
        }

        fprintf(out, " %u:%u", parse_token_line(tok), parse_token_column(tok));

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

    core_string_t path      = parse_cmdr_arg_str(r, 0);
    core_string_t env_dir   = core_string_from_cstr(getenv("PP_GRAMMAR_DIR"));
    core_string_t dir       = env_dir;

    if (parse_cmdr_flag_present(r, S("--grammar-dir"))) {
        dir = parse_cmdr_flag_str(r, S("--grammar-dir"));
    }

    const language_t *lang = nullptr;

    if (parse_cmdr_flag_present(r, S("--language"))) {
        core_string_t l = parse_cmdr_flag_str(r, S("--language"));
        lang = language_named(l);

        if (!lang) {
            fprintf(stderr, "pp: no language named '%.*s'\n",
                    (int)l.u8_bytes, l.data);
            list_languages(stderr);
            core_free(env_dir.data);

            return PP_USAGE;
        }
    }

    core_string_t owned_grammar_path = {0};
    core_string_t grammar_path       = {0};

    if (parse_cmdr_flag_present(r, S("--grammar"))) {
        grammar_path = parse_cmdr_flag_str(r, S("--grammar"));
    }
    else {
        if (!lang) {
            lang = language_for_path(path);
        }

        if (!lang) {
            core_string_t ext = ext_of(path);

            if (ext.data) {
                fprintf(stderr,
                        "pp: no language for extension %.*s; pass --language "
                        "or --grammar\n",
                        (int)ext.u8_bytes, ext.data);
            }
            else {
                fprintf(stderr,
                        "pp: no language for %.*s; pass --language or "
                        "--grammar\n",
                        (int)path.u8_bytes, path.data);
            }
            list_languages(stderr);
            core_free(env_dir.data);

            return PP_USAGE;
        }

        const core_string_t where[] = {
            dir,
#ifdef PP_GRAMMAR_DIR
            S(PP_GRAMMAR_DIR),
#endif
            S("grammars"),
            S("../grammars"),
            S("../../grammars"),
        };

        bool found = false;

        for (size_t i = dir.data ? 0 : 1;
             !found && i < sizeof(where) / sizeof(where[0]);
             i++) {
            core_buffer_t *candidate = core_buffer_empty();
            core_buffer_printf(candidate, "%.*s/%.*s",
                               (int)where[i].u8_bytes, where[i].data,
                               (int)lang->grammar.u8_bytes,
                               lang->grammar.data);
            core_string_t candidate_path = core_buffer_take_string(candidate);
            char         *probe_path     = core_string_to_cstr(candidate_path);
            FILE         *probe          = fopen(probe_path, "r");

            core_free(probe_path);

            if (probe) {
                fclose(probe);
                owned_grammar_path = candidate_path;
                grammar_path       = owned_grammar_path;
                found              = true;
            }
            else {
                core_free(candidate_path.data);
            }
        }

        if (!found) {
            fprintf(stderr,
                    "pp: cannot find %.*s; pass --grammar-dir or set "
                    "PP_GRAMMAR_DIR\n",
                    (int)lang->grammar.u8_bytes, lang->grammar.data);
            core_free(env_dir.data);

            return PP_ERROR;
        }
    }

    core_free(env_dir.data);

    core_string_t start = lang ? lang->start : (core_string_t){0};

    if (parse_cmdr_flag_present(r, S("--start"))) {
        start = parse_cmdr_flag_str(r, S("--start"));
    }

    parse_grammar_t *g = load_grammar(grammar_path, start, stderr);
    core_free(owned_grammar_path.data);

    if (!g) {
        return PP_ERROR;
    }

    core_string_t src = slurp(path);

    if (!src.data) {
        fprintf(stderr, "pp: cannot read %.*s\n", (int)path.u8_bytes, path.data);
        parse_grammar_free(g);

        return PP_ERROR;
    }

    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src, &buf, &sc);

    if (!ts) {
        fprintf(stderr, "pp: %.*s did not tokenize\n",
                (int)path.u8_bytes, path.data);
        core_free(src.data);
        parse_grammar_free(g);

        return PP_ERROR;
    }

    int rc = PP_OK;

    if (parse_cmdr_flag_present(r, S("--tokens"))) {
        rc = write_tokens(g, ts, stdout);
    }
    else {
        parse_nt_id_t start_id = start.data
            ? parse_nonterm_id(parse_find_nonterm(g, start))
            : g->nt_list.data[0]->id;
        parse_pwz_parser_t *p = parse_pwz_new_for_start(g, start_id);

        if (!parse_pwz_parse(p, ts)) {
            int32_t at = parse_pwz_error_pos(p);

            scan_stream_reset(ts);

            parse_token_info_t *tok = at >= 0 ? scan_stream_get(ts, at)
                                            : nullptr;

            if (tok) {
                int32_t       tid  = parse_token_tid(tok);
                core_string_t name = parse_terminal_name(g, tid);
                core_option_t(core_string_t) value = parse_token_value(tok);
                core_string_t text = core_option_is_set(value)
                                      ? core_option_get(value)
                                      : (core_string_t){0};

                fprintf(stderr,
                        "pp: %.*s:%u:%u: no rule accepts ",
                        (int)path.u8_bytes, path.data,
                        parse_token_line(tok),
                        parse_token_column(tok));

                if (name.data) {
                    fprintf(stderr, "%%%.*s", (int)name.u8_bytes, name.data);
                }
                else if (tid > 0 && tid < 128) {
                    fprintf(stderr, "'%c'", (char)tid);
                }
                else {
                    fprintf(stderr, "token %d", tid);
                }

                if (text.data && text.u8_bytes && text.data[0] != '\n') {
                    fprintf(stderr, " (%.*s)", (int)text.u8_bytes, text.data);
                }

                fprintf(stderr, ", token %d of %d\n", at,
                        scan_stream_token_count(ts));
            }
            else {
                fprintf(stderr, "pp: %.*s did not parse\n",
                        (int)path.u8_bytes, path.data);
            }

            rc = PP_ERROR;
        }
        else {
            int32_t width  = 80;
            int32_t indent = 4;

            width  = (int32_t)parse_cmdr_flag_int(r, S("--width"), width);
            indent = (int32_t)parse_cmdr_flag_int(r, S("--indent"), indent);

            parse_print_opts_t o = {
                .source = src,
                .width  = width,
                .indent = indent,
            };

            core_string_t text = parse_tree_print(g, parse_pwz_get_tree(p), &o);

            if (text.data) {
                fwrite(text.data, 1, text.u8_bytes, stdout);

                if (text.u8_bytes && text.data[text.u8_bytes - 1] != '\n') {
                    fputc('\n', stdout);
                }

                core_free(text.data);
            }
            else {
                fprintf(stderr, "pp: %.*s produced no output\n",
                        (int)path.u8_bytes, path.data);
                rc = PP_ERROR;
            }
        }

        parse_pwz_free(p);
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
    core_free(src.data);
    parse_grammar_free(g);
    parse_cmdr_result_free(r);
    parse_cmdr_free(c);

    return rc;
}
