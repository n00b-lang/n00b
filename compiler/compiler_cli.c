#include "compiler_cli.h"

#include <string.h>

#define FLAG(name, alias, type, value, doc) \
    {CORE_STRING_STATIC(name), alias, PARSE_CMDR_TYPE_##type, value, \
     CORE_STRING_STATIC(doc)}
#define NO_ALIAS {0}
#define END_FLAGS {{0}, {0}, 0, false, {0}}

#define OUTPUT_FLAG \
    FLAG("--output", CORE_STRING_STATIC("-o"), WORD, true, "output file")
#define LINK_INTERFACE_FLAG \
    FLAG("--link-interface", NO_ALIAS, WORD, true, \
         "load a frozen semantic library interface")
#define SHOW_FFI_FLAG \
    FLAG("--show-ffi", NO_ALIAS, WORD, true, \
         "print the n00b source generated for MODULE.NAME")
#define FRONT_END_DUMP_FLAGS \
    FLAG("--dump-tokens", NO_ALIAS, BOOL, false, \
         "dump tokens after lexing and continue"), \
    FLAG("--dump-tree", NO_ALIAS, BOOL, false, \
         "dump the parse tree and continue"), \
    FLAG("--dump-preprocessed", NO_ALIAS, BOOL, false, \
         "dump the preprocessed tree and continue"), \
    FLAG("--dump-symbols", NO_ALIAS, BOOL, false, \
         "dump collected symbols after preprocessing")
#define LOWERING_DUMP_FLAGS \
    FLAG("--dump-link-unit", NO_ALIAS, BOOL, false, \
         "dump canonical link-unit analysis facts"), \
    FLAG("--dump-ir", NO_ALIAS, BOOL, false, \
         "dump lowered intermediate representation"), \
    FLAG("--dump-control-flow", NO_ALIAS, BOOL, false, \
         "dump control-flow edges"), \
    FLAG("--dump-pdg", NO_ALIAS, BOOL, false, \
         "dump the full program-dependence graph"), \
    FLAG("--dump-layout", NO_ALIAS, BOOL, false, \
         "dump target layouts for named types")

static const parse_cmdr_flag_t GLOBAL_FLAGS[] = {
    FLAG("--help", CORE_STRING_STATIC("-h"), BOOL, false, "show this help"),
    FLAG("--jobs", NO_ALIAS, INT, true, "maximum worker count"),
    FLAG("--define", CORE_STRING_STATIC("-D"), WORD, true,
         "preprocessor definition"),
    FLAG("--contract-dir", NO_ALIAS, WORD, true,
         "directory searched for writ contracts"),
    FLAG("--release", NO_ALIAS, BOOL, false,
         "disable debug assertions and optimize at LLVM O3"),
    FLAG("--warn-changed-files-only", NO_ALIAS, BOOL, false,
         "report warnings only for modules compiled in this build"),
    FLAG("--package-warnings", NO_ALIAS, BOOL, false,
         "report warnings from imported packages"),
    END_FLAGS,
};

static const parse_cmdr_flag_t FMT_FLAGS[] = {
    OUTPUT_FLAG,
    FLAG("--check", NO_ALIAS, BOOL, false,
         "check formatting without writing files"),
    FLAG("--normalize", NO_ALIAS, BOOL, false,
         "normalize source using inferred type information"),
    END_FLAGS,
};

static const parse_cmdr_flag_t BUILD_FLAGS[] = {
    OUTPUT_FLAG,
    LINK_INTERFACE_FLAG,
    SHOW_FFI_FLAG,
    FRONT_END_DUMP_FLAGS,
    LOWERING_DUMP_FLAGS,
    FLAG("--build-time", NO_ALIAS, BOOL, false,
         "compile the program that computes consts"),
    FLAG("--link-object", NO_ALIAS, WORD, true,
         "native object to link into an executable"),
    FLAG("--native-root", NO_ALIAS, WORD, true,
         "qualified n00b function used by native code"),
    FLAG("--lto", NO_ALIAS, BOOL, false,
         "optimize runtime modules and C support together"),
    FLAG("--native", NO_ALIAS, BOOL, false,
         "tune generated code for this CPU (default on macOS)"),
    FLAG("--generic", NO_ALIAS, BOOL, false,
         "use baseline CPU code generation"),
    FLAG("--no-specialize", NO_ALIAS, BOOL, false,
         "keep concrete generic calls on the boxed path"),
    FLAG("--gc-conservative-stacks", NO_ALIAS, BOOL, false,
         "scan all stacks conservatively"),
    END_FLAGS,
};

static const parse_cmdr_flag_t LIBRARY_FLAGS[] = {
    OUTPUT_FLAG,
    LINK_INTERFACE_FLAG,
    FRONT_END_DUMP_FLAGS,
    LOWERING_DUMP_FLAGS,
    FLAG("--emit-interface", NO_ALIAS, WORD, true,
         "write a frozen semantic interface archive"),
    END_FLAGS,
};

static const parse_cmdr_flag_t LOWERING_FLAGS[] = {
    OUTPUT_FLAG,
    LINK_INTERFACE_FLAG,
    FRONT_END_DUMP_FLAGS,
    LOWERING_DUMP_FLAGS,
    END_FLAGS,
};

static const parse_cmdr_flag_t PREPROCESS_FLAGS[] = {
    OUTPUT_FLAG,
    SHOW_FFI_FLAG,
    FRONT_END_DUMP_FLAGS,
    END_FLAGS,
};

static const parse_cmdr_cmd_t COMMANDS[] = {
    {CORE_STRING_STATIC("fmt"),
     CORE_STRING_STATIC("format source files in place"),
     FMT_FLAGS, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("build"),
     CORE_STRING_STATIC("build an executable from one module or file"),
     BUILD_FLAGS, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("library"),
     CORE_STRING_STATIC("emit a library interface or ABI metadata"),
     LIBRARY_FLAGS, CORE_STRING_STATIC("input"), 1, -1},
    {CORE_STRING_STATIC("compile"),
     CORE_STRING_STATIC("emit ABI metadata for one translation unit"),
     LOWERING_FLAGS, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("preprocess"),
     CORE_STRING_STATIC("emit preprocessor output for one input"),
     PREPROCESS_FLAGS, CORE_STRING_STATIC("input"), 1, 1},
    {CORE_STRING_STATIC("lower"),
     CORE_STRING_STATIC("lower a module and its imports to Lantern IR"),
     LOWERING_FLAGS, CORE_STRING_STATIC("input"), 1, 1},
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
        .flags = GLOBAL_FLAGS,
        .commands = COMMANDS,
        .default_command = CORE_STRING_STATIC("build"),
    };
    parse_cmdr_t *cli = parse_cmdr_new(&spec);
    const core_string_t repeatable[] = {
        CORE_STRING_STATIC("--define"),
        CORE_STRING_STATIC("--link-interface"),
        CORE_STRING_STATIC("--contract-dir"),
        CORE_STRING_STATIC("--link-object"),
        CORE_STRING_STATIC("--native-root"),
    };
    for (size_t i = 0; i < sizeof(repeatable) / sizeof(repeatable[0]); i++) {
        for (const parse_cmdr_flag_t *flag = GLOBAL_FLAGS; flag->name.data;
             flag++)
            if (core_string_eq(flag->name, repeatable[i]))
                parse_cmdr_flag_set_repeatable(cli, (core_string_t){0},
                                               repeatable[i], true);
        for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
             command++)
            for (const parse_cmdr_flag_t *flag = command->flags;
                 flag && flag->name.data; flag++)
                if (core_string_eq(flag->name, repeatable[i]))
                    parse_cmdr_flag_set_repeatable(cli, command->name,
                                                   repeatable[i], true);
    }
    return cli;
}

bool
n00bb_is_help_request(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        core_string_t arg = core_string_from_cstr(argv[i]);
        bool help = core_string_eq(arg, CORE_STRING_STATIC("--help"))
            || core_string_eq(arg, CORE_STRING_STATIC("-h"));
        core_free(arg.data);
        if (help) return true;
    }
    return false;
}
