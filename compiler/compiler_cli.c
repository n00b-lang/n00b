#include "compiler_cli.h"

#include <string.h>

static const parse_cmdr_flag_t FLAGS[] = {
    {CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
     PARSE_CMDR_TYPE_BOOL, false, CORE_STRING_STATIC("show this help")},
    {CORE_STRING_STATIC("--output"), CORE_STRING_STATIC("-o"),
     PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("output file")},
    {CORE_STRING_STATIC("--check"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("check formatting without writing files")},
    {CORE_STRING_STATIC("--normalize"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("normalize source using inferred type information")},
    {CORE_STRING_STATIC("--link-object"), {0}, PARSE_CMDR_TYPE_WORD,
     true, CORE_STRING_STATIC("native object to link into an executable")},
    {CORE_STRING_STATIC("--native-root"), {0}, PARSE_CMDR_TYPE_WORD,
     true, CORE_STRING_STATIC("qualified n00b function used by native code")},
    {CORE_STRING_STATIC("--link-interface"), {0}, PARSE_CMDR_TYPE_WORD,
     true, CORE_STRING_STATIC("load a frozen semantic library interface")},
    {CORE_STRING_STATIC("--emit-interface"), {0}, PARSE_CMDR_TYPE_WORD,
     true, CORE_STRING_STATIC("write a frozen semantic interface archive")},
    {CORE_STRING_STATIC("--jobs"), {0}, PARSE_CMDR_TYPE_INT, true,
     CORE_STRING_STATIC("maximum worker count")},
    {CORE_STRING_STATIC("--dump-tokens"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump tokens after lexing and continue")},
    {CORE_STRING_STATIC("--dump-tree"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the parse tree and continue")},
    {CORE_STRING_STATIC("--dump-preprocessed"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the preprocessed tree and continue")},
    {CORE_STRING_STATIC("--dump-symbols"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump collected symbols after preprocessing")},
    {CORE_STRING_STATIC("--dump-link-unit"), {0}, PARSE_CMDR_TYPE_BOOL,
     false, CORE_STRING_STATIC("dump canonical link-unit analysis facts")},
    {CORE_STRING_STATIC("--dump-ir"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump lowered intermediate representation")},
    {CORE_STRING_STATIC("--dump-control-flow"), {0}, PARSE_CMDR_TYPE_BOOL,
     false, CORE_STRING_STATIC("dump control-flow edges")},
    {CORE_STRING_STATIC("--dump-pdg"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump the full program-dependence graph")},
    {CORE_STRING_STATIC("--dump-layout"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("dump target layouts for named types")},
    {CORE_STRING_STATIC("--define"), CORE_STRING_STATIC("-D"),
     PARSE_CMDR_TYPE_WORD, true, CORE_STRING_STATIC("preprocessor definition")},
    {CORE_STRING_STATIC("--contract-dir"), {0}, PARSE_CMDR_TYPE_WORD, true,
     CORE_STRING_STATIC("directory searched for writ contracts")},
    {CORE_STRING_STATIC("--show-ffi"), {0}, PARSE_CMDR_TYPE_WORD, true,
     CORE_STRING_STATIC("print the n00b source generated for MODULE.NAME")},
    {CORE_STRING_STATIC("--release"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("disable debug assertions and optimize at LLVM O3")},
    {CORE_STRING_STATIC("--lto"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("optimize runtime modules and C support together")},
    {CORE_STRING_STATIC("--native"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("tune generated code for this CPU (default on macOS)")},
    {CORE_STRING_STATIC("--generic"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("use baseline CPU code generation")},
    {CORE_STRING_STATIC("--no-specialize"), {0}, PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("keep concrete generic calls on the boxed path")},
    {CORE_STRING_STATIC("--gc-conservative-stacks"), {0},
     PARSE_CMDR_TYPE_BOOL, false,
     CORE_STRING_STATIC("scan all stacks conservatively")},
    {{0}, {0}, 0, false, {0}},
};

static const parse_cmdr_cmd_t COMMANDS[] = {
    {CORE_STRING_STATIC("fmt"),
     CORE_STRING_STATIC("format source files in place"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("build"),
     CORE_STRING_STATIC("build an executable from one module or file"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("library"),
     CORE_STRING_STATIC("emit a library interface or ABI metadata"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("compile"),
     CORE_STRING_STATIC("emit ABI metadata for one translation unit"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("preprocess"),
     CORE_STRING_STATIC("emit preprocessor output for one input"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("lower"),
     CORE_STRING_STATIC("lower a module and its imports to Lantern IR"),
     nullptr, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("locate-tu"),
     CORE_STRING_STATIC("locate and load translation units"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("lex"),
     CORE_STRING_STATIC("lex inputs and dump their tokens"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("parse"),
     CORE_STRING_STATIC("parse inputs and dump their syntax trees"),
     nullptr, CORE_STRING_STATIC("input"), 1, -1},
    {{0}, {0}, nullptr, {0}, 0, 0},
};

parse_cmdr_t *
n00bb_make_cli(void)
{
    const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("n00bb"),
        .usage = CORE_STRING_STATIC(
            "usage: n00bb [options] <module|file>\n"
            "       n00bb <command> [options] <module|file>..."),
        .flags = FLAGS,
        .commands = COMMANDS,
        .arg = CORE_STRING_STATIC("input"),
        .min_args = 1,
        .max_args = 1,
    };
    parse_cmdr_t *cli = parse_cmdr_new(&spec);
    parse_cmdr_flag_set_repeatable(cli, (core_string_t){0},
                                   CORE_STRING_STATIC("--define"), true);
    parse_cmdr_flag_set_repeatable(cli, (core_string_t){0},
                                   CORE_STRING_STATIC("--link-interface"),
                                   true);
    parse_cmdr_flag_set_repeatable(cli, (core_string_t){0},
                                   CORE_STRING_STATIC("--contract-dir"),
                                   true);
    return cli;
}

bool
n00bb_is_help_request(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
            return true;
    return false;
}
