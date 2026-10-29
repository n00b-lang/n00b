#include "compiler_cli.h"
#include "config.h"

#include "lib/alloc.h"

#include <stdint.h>

#define FLAG(name, alias, type, value, doc) \
    {CORE_STRING_STATIC(name), alias, PARSE_CMDR_TYPE_##type, value, \
     CORE_STRING_STATIC(doc), false, nullptr}
#define DEV_FLAG(name, alias, type, value, doc) \
    {CORE_STRING_STATIC(name), alias, PARSE_CMDR_TYPE_##type, value, \
     CORE_STRING_STATIC(doc), true, nullptr}
#define KEYED_FLAG(name, alias, type, value, doc, setting) \
    {CORE_STRING_STATIC(name), alias, PARSE_CMDR_TYPE_##type, value, \
     CORE_STRING_STATIC(doc), false, setting}
#define DEV_KEYED_FLAG(name, alias, type, value, doc, setting) \
    {CORE_STRING_STATIC(name), alias, PARSE_CMDR_TYPE_##type, value, \
     CORE_STRING_STATIC(doc), true, setting}
#define NO_ALIAS {0}
#define END_FLAGS {{0}, {0}, 0, false, {0}, false, nullptr}

#define SETS(setting_id, flag_value) \
    (&(const n00bb_flag_setting_t){.id = N00BB_KEY_##setting_id, \
                                   .value = (flag_value)})

#define OUTPUT_FLAG \
    FLAG("--output", CORE_STRING_STATIC("-o"), WORD, true, "output file")
#define LINK_INTERFACE_FLAG \
    FLAG("--link-interface", NO_ALIAS, WORD, true, \
         "load a frozen semantic library interface")
#define SHOW_FFI_FLAG \
    FLAG("--show-ffi", NO_ALIAS, WORD, true, \
         "print the n00b source generated for MODULE.NAME")
#define FRONT_END_DUMP_FLAGS \
    DEV_FLAG("--dump-tokens", NO_ALIAS, BOOL, false, \
             "dump tokens after lexing and continue"), \
    DEV_FLAG("--dump-tree", NO_ALIAS, BOOL, false, \
             "dump the parse tree and continue"), \
    DEV_FLAG("--dump-preprocessed", NO_ALIAS, BOOL, false, \
             "dump the preprocessed tree and continue"), \
    DEV_FLAG("--dump-symbols", NO_ALIAS, BOOL, false, \
             "dump collected symbols after preprocessing")
#define LOWERING_DUMP_FLAGS \
    DEV_FLAG("--dump-link-unit", NO_ALIAS, BOOL, false, \
             "dump canonical link-unit analysis facts"), \
    DEV_FLAG("--dump-ir", NO_ALIAS, BOOL, false, \
             "dump lowered intermediate representation"), \
    DEV_FLAG("--dump-control-flow", NO_ALIAS, BOOL, false, \
             "dump control-flow edges"), \
    DEV_FLAG("--dump-pdg", NO_ALIAS, BOOL, false, \
             "dump the full program-dependence graph"), \
    DEV_FLAG("--dump-summaries", NO_ALIAS, BOOL, false, \
             "dump whole-program function summaries"), \
    DEV_FLAG("--dump-layout", NO_ALIAS, BOOL, false, \
             "dump target layouts for named types"), \
    DEV_FLAG("--dump-ranges", NO_ALIAS, BOOL, false, \
             "dump AST and IR integer ranges at shared expressions")
#define NO_COMPTIME_FLAG \
    FLAG("--no-comptime", NO_ALIAS, BOOL, false, \
         "skip the comptime phase; a program that needs build-time values fails")
#define PROGRAM_FLAGS \
    NO_COMPTIME_FLAG, \
    LINK_INTERFACE_FLAG, \
    SHOW_FFI_FLAG, \
    FRONT_END_DUMP_FLAGS, \
    LOWERING_DUMP_FLAGS, \
    DEV_FLAG("--build-time", NO_ALIAS, BOOL, false, \
             "compile the program that computes consts"), \
    DEV_FLAG("--literal-stage", NO_ALIAS, BOOL, false, \
             "with --build-time, compile only the static literal producers"), \
    KEYED_FLAG("--unchecked-wide-narrowing", NO_ALIAS, BOOL, false, \
               "narrow 128-bit integers without the runtime range check", \
               SETS(BUILD_UNCHECKED_WIDE_NARROWING, true)), \
    DEV_FLAG("--verify-analyses", NO_ALIAS, BOOL, false, \
             "check memory and array SSA for every emitted function"), \
    FLAG("--link-object", NO_ALIAS, WORD, true, \
         "native object to link into an executable"), \
    DEV_FLAG("--native-root", NO_ALIAS, WORD, true, \
             "qualified n00b function used by native code"), \
    KEYED_FLAG("--lto", NO_ALIAS, BOOL, false, \
               "optimize runtime modules and C support together", \
               SETS(BUILD_LTO, true)), \
    KEYED_FLAG("--panic-trap", NO_ALIAS, BOOL, false, \
               "raise SIGTRAP on a runtime failure instead of exiting with " \
               "101", \
               SETS(BUILD_PANIC_TRAP, true)), \
    KEYED_FLAG("--native", NO_ALIAS, BOOL, false, \
               "tune generated code for this CPU (default on macOS)", \
               SETS(BUILD_NATIVE, true)), \
    KEYED_FLAG("--generic", NO_ALIAS, BOOL, false, \
               "use baseline CPU code generation", \
               SETS(BUILD_NATIVE, false)), \
    DEV_FLAG("--no-specialize", NO_ALIAS, BOOL, false, \
             "keep concrete generic calls on the boxed path"), \
    DEV_FLAG("--closed-world", NO_ALIAS, BOOL, false, \
             "omit the polymorphic implementations kept for dynamic linking"), \
    FLAG("--function-summaries", NO_ALIAS, BOOL, false, \
         "keep function summaries in the executable for dynamic linking, " \
         "reflection and debugging"), \
    DEV_FLAG("--report-specialization-gaps", NO_ALIAS, BOOL, false, \
             "list generic functions the program still reaches unspecialized"), \
    DEV_FLAG("--gc-conservative-stacks", NO_ALIAS, BOOL, false, \
             "scan all stacks conservatively")

static const parse_cmdr_flag_t GLOBAL_FLAGS[] = {
    FLAG("--help", CORE_STRING_STATIC("-h"), BOOL, false, "show help"),
    FLAG("--help-all", NO_ALIAS, BOOL, false,
         "show every command and option, developer ones included"),
    FLAG("--version", CORE_STRING_STATIC("-V"), BOOL, false,
         "show version and build information"),
    KEYED_FLAG("--jobs", CORE_STRING_STATIC("-j"), INT, true,
               "maximum worker count",
               SETS(BUILD_JOBS, true)),
    KEYED_FLAG("--define", CORE_STRING_STATIC("-D"), WORD, true,
               "preprocessor definition",
               SETS(BUILD_DEFINES, true)),
    KEYED_FLAG("--contract-dir", NO_ALIAS, WORD, true,
               "directory searched for writ contracts",
               SETS(PATHS_CONTRACT_DIRS, true)),
    KEYED_FLAG("--path", NO_ALIAS, WORD, true,
               "directory searched for packages",
               SETS(PATHS_PATH, true)),
    KEYED_FLAG("--cache", NO_ALIAS, WORD, true,
               "directory that holds compiler caches",
               SETS(PATHS_CACHE, true)),
    KEYED_FLAG("--clang", NO_ALIAS, WORD, true,
               "clang used to compile C and link programs",
               SETS(TOOLS_CLANG, true)),
    KEYED_FLAG("--cargo", NO_ALIAS, WORD, true,
               "cargo used to build Rust transports",
               SETS(TOOLS_CARGO, true)),
    KEYED_FLAG("--release", NO_ALIAS, BOOL, false,
               "disable debug assertions and optimize at LLVM O3",
               SETS(BUILD_RELEASE, true)),
    KEYED_FLAG("--target", NO_ALIAS, WORD, true,
               "platform to build for: host, arm64-macos, x86_64-linux, "
               "arm64-linux or x86_64-windows",
               SETS(BUILD_TARGET, true)),
    DEV_FLAG("--warn-changed-files-only", NO_ALIAS, BOOL, false,
             "report warnings only for modules compiled in this build"),
    KEYED_FLAG("--package-warnings", NO_ALIAS, BOOL, false,
               "report warnings from imported packages",
               SETS(WARNINGS_PACKAGE, true)),
    KEYED_FLAG("--syntax-preference", NO_ALIAS, WORD, true,
               "set preferred syntax as CLASS=SPELLING",
               SETS(SYNTAX_PREFERENCES, true)),
    KEYED_FLAG("--no-syntax-warnings", NO_ALIAS, BOOL, false,
               "disable syntax spelling warnings",
               SETS(SYNTAX_WARNINGS, false)),
    KEYED_FLAG("--terse-diagnostics", NO_ALIAS, BOOL, false,
               "print each diagnostic on one line with its code",
               SETS(DIAGNOSTICS_TERSE, true)),
    KEYED_FLAG("--no-hints", NO_ALIAS, BOOL, false,
               "hide hints such as parameters that could be `view`",
               SETS(DIAGNOSTICS_HINTS, false)),
    KEYED_FLAG("--color", NO_ALIAS, WORD, true,
               "color diagnostics: auto, always or never",
               SETS(DIAGNOSTICS_COLOR, true)),
    DEV_KEYED_FLAG("--profile-compile", NO_ALIAS, BOOL, false,
                   "print compile phase timings to stderr",
                   SETS(DEV_PROFILE_COMPILE, true)),
    DEV_KEYED_FLAG("--cache-log", NO_ALIAS, WORD, true,
                   "append object cache decisions to this file",
                   SETS(DEV_CACHE_LOG, true)),
    DEV_KEYED_FLAG("--no-object-cache", NO_ALIAS, BOOL, false,
                   "neither read nor write the object cache",
                   SETS(DEV_NO_OBJECT_CACHE, true)),
    DEV_KEYED_FLAG("--no-native-cache", NO_ALIAS, BOOL, false,
                   "neither read nor write the native code cache",
                   SETS(DEV_NO_NATIVE_CACHE, true)),
    DEV_KEYED_FLAG("--no-package-cache", NO_ALIAS, BOOL, false,
                   "rescan package directories instead of using the cache",
                   SETS(DEV_NO_PACKAGE_CACHE, true)),
    DEV_KEYED_FLAG("--opt-level", NO_ALIAS, INT, true,
                   "optimize generated code at LLVM O0 to O3 instead of the build's level",
                   SETS(DEV_OPT_LEVEL, true)),
    END_FLAGS,
};

static const parse_cmdr_flag_t FMT_FLAGS[] = {
    OUTPUT_FLAG,
    FLAG("--check", NO_ALIAS, BOOL, false,
         "check formatting without writing files"),
    FLAG("--normalize", NO_ALIAS, BOOL, false,
         "normalize source using inferred type information"),
    KEYED_FLAG("--ascii-tags", NO_ALIAS, BOOL, false,
               "write rich-string tags as [: :] instead of « »",
               SETS(FMT_ASCII_TAGS, true)),
    END_FLAGS,
};

static const parse_cmdr_flag_t BUILD_FLAGS[] = {
    OUTPUT_FLAG,
    PROGRAM_FLAGS,
    END_FLAGS,
};

static const parse_cmdr_flag_t RUN_FLAGS[] = {
    PROGRAM_FLAGS,
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
    NO_COMPTIME_FLAG,
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

static const parse_cmdr_flag_t TOOLCHAIN_FLAGS[] = {
    KEYED_FLAG("--keep", NO_ALIAS, INT, true,
               "with prune, the newest builds kept per version (default 1)",
               SETS(TOOLCHAIN_KEEP, true)),
    END_FLAGS,
};

static const parse_cmdr_flag_t CACHE_FLAGS[] = {
    FLAG("--all", NO_ALIAS, BOOL, false,
         "remove the caches of every toolchain, not just this one"),
    END_FLAGS,
};

static const parse_cmdr_flag_t VERSION_FLAGS[] = {
    DEV_FLAG("--marker", NO_ALIAS, BOOL, false,
             "print the toolchain marker written at install"),
    END_FLAGS,
};

#define COMMAND(name, doc, flags, arg, min, max, hidden) \
    {CORE_STRING_STATIC(name), CORE_STRING_STATIC(doc), flags, \
     CORE_STRING_STATIC(arg), min, max, hidden}

static const parse_cmdr_cmd_t COMMANDS[] = {
    COMMAND("build", "build an executable from one module or file",
            BUILD_FLAGS, "module|file", 1, -1, false),
    COMMAND("run", "build a program and run it; arguments after -- go to it",
            RUN_FLAGS, "module|file", 1, 1, false),
    COMMAND("check", "report errors and warnings without building",
            LOWERING_FLAGS, "module|file", 1, -1, false),
    COMMAND("fmt", "format source files in place",
            FMT_FLAGS, "file", 1, -1, false),
    COMMAND("library", "emit a library interface or ABI metadata",
            LIBRARY_FLAGS, "module|file", 1, -1, false),
    COMMAND("help", "show help for a command",
            nullptr, "command", 0, 1, false),
    {CORE_STRING_STATIC("version"),
     CORE_STRING_STATIC("show version and build information"),
     VERSION_FLAGS, {0}, 0, 0, false},
    {CORE_STRING_STATIC("env"),
     CORE_STRING_STATIC("show where this toolchain finds everything, and any problems"),
     nullptr, {0}, 0, 0, false},
    {CORE_STRING_STATIC("config"),
     CORE_STRING_STATIC("show every setting, its value and where it comes from"),
     nullptr, {0}, 0, 0, false},
    COMMAND("toolchain",
            "manage installed toolchains: list, use, remove, prune, pin, unpin",
            TOOLCHAIN_FLAGS, "action", 1, 2, false),
    COMMAND("cache", "manage compiler caches: clean",
            CACHE_FLAGS, "action", 1, 1, false),
    COMMAND("completions",
            "print tab completion for a shell: bash, zsh, fish, powershell",
            nullptr, "shell", 1, 1, false),
    COMMAND("explain", "explain a diagnostic code such as N00B-T0042",
            nullptr, "code", 1, 1, false),
    COMMAND("compile", "emit ABI metadata for one translation unit",
            LOWERING_FLAGS, "input", 1, 1, true),
    COMMAND("preprocess", "emit preprocessor output for one input",
            PREPROCESS_FLAGS, "input", 1, 1, true),
    COMMAND("lower", "lower a module and its imports to Lantern IR",
            LOWERING_FLAGS, "input", 1, 1, true),
    COMMAND("locate-tu", "locate and load translation units",
            nullptr, "input", 1, -1, true),
    COMMAND("lex", "lex inputs and dump their tokens",
            nullptr, "input", 1, -1, true),
    COMMAND("parse", "parse inputs and dump their syntax trees",
            nullptr, "input", 1, -1, true),
    {{0}, {0}, nullptr, {0}, 0, 0, false},
};

static const parse_cmdr_cmd_t *
command_spec(core_string_t name)
{
    for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
         command++)
        if (core_string_eq(command->name, name)) return command;
    return nullptr;
}

size_t
n00bb_cli_command_names(core_string_t *out, size_t capacity)
{
    size_t count = 0;
    for (const parse_cmdr_cmd_t *command = COMMANDS;
         command->name.data && count < capacity; command++)
        if (!command->hidden) out[count++] = command->name;
    return count;
}

static size_t
append_option_names(const parse_cmdr_flag_t *flags, core_string_t *out,
                    size_t count, size_t capacity)
{
    for (const parse_cmdr_flag_t *flag = flags;
         flag && flag->name.data && count < capacity; flag++)
        if (!flag->hidden) out[count++] = flag->name;
    return count;
}

size_t
n00bb_cli_option_names(core_string_t command, core_string_t *out,
                       size_t capacity)
{
    const parse_cmdr_cmd_t *spec = command.u8_bytes ? command_spec(command)
                                                    : nullptr;
    size_t count = append_option_names(spec ? spec->flags : nullptr, out, 0,
                                       capacity);
    return append_option_names(GLOBAL_FLAGS, out, count, capacity);
}

core_string_t
n00bb_cli_option_command(core_string_t option)
{
    for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
         command++) {
        if (command->hidden) continue;
        for (const parse_cmdr_flag_t *flag = command->flags;
             flag && flag->name.data; flag++)
            if (!flag->hidden && (core_string_eq(flag->name, option)
                                  || core_string_eq(flag->alias, option)))
                return command->name;
    }
    return (core_string_t){0};
}

const parse_cmdr_flag_t *
n00bb_cli_global_flags(void)
{
    return GLOBAL_FLAGS;
}

const parse_cmdr_flag_t *
n00bb_cli_command_flags(core_string_t command)
{
    const parse_cmdr_cmd_t *spec = command_spec(command);
    return spec ? spec->flags : nullptr;
}

core_string_t
n00bb_cli_command_argument(core_string_t command)
{
    const parse_cmdr_cmd_t *spec = command_spec(command);
    return spec ? spec->arg : (core_string_t){0};
}

static parse_cmdr_t *
make_cli(void)
{
    const parse_cmdr_spec_t spec = {
        .tool = CORE_STRING_STATIC("n00b"),
        .usage = CORE_STRING_STATIC(
            "usage: n00b <command> [options] <module|file>\n"
            "       n00b <module|file>              same as n00b build"),
        .flags = GLOBAL_FLAGS,
        .commands = COMMANDS,
        .default_command = CORE_STRING_STATIC("build"),
    };
    parse_cmdr_t *cli = parse_cmdr_new(&spec);
    const core_string_t repeatable[] = {
        CORE_STRING_STATIC("--define"),
        CORE_STRING_STATIC("--link-interface"),
        CORE_STRING_STATIC("--contract-dir"),
        CORE_STRING_STATIC("--path"),
        CORE_STRING_STATIC("--link-object"),
        CORE_STRING_STATIC("--native-root"),
        CORE_STRING_STATIC("--syntax-preference"),
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

static void
print_words(FILE *out, const parse_cmdr_flag_t *flags)
{
    for (const parse_cmdr_flag_t *flag = flags; flag && flag->name.data;
         flag++)
        if (!flag->hidden)
            fprintf(out, " %.*s", (int)flag->name.u8_bytes, flag->name.data);
}

static void
print_commands(FILE *out)
{
    for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
         command++)
        if (!command->hidden)
            fprintf(out, " %.*s", (int)command->name.u8_bytes,
                    command->name.data);
}

static const char TOOLCHAIN_ACTIONS[] = "list use remove prune pin unpin";
static const char TOOLCHAIN_SPECS[] =
    "n00b toolchain list 2>/dev/null | awk '{print ($1 == \"*\") ? $2 : $1}'";

static void
print_bash(FILE *out)
{
    fputs("_n00b() {\n"
          "    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
          "    local command=\"\"\n"
          "    local i\n"
          "    for ((i = 1; i < COMP_CWORD; i++)); do\n"
          "        case \"${COMP_WORDS[i]}\" in -*) ;; *) command=\"${COMP_WORDS[i]}\"; break ;; esac\n"
          "    done\n"
          "    local global=\"", out);
    print_words(out, GLOBAL_FLAGS);
    fputs("\"\n    local commands=\"", out);
    print_commands(out);
    fputs("\"\n    local options=\"\"\n    case \"$command\" in\n", out);
    for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
         command++) {
        if (command->hidden || !command->flags) continue;
        fprintf(out, "        %.*s) options=\"", (int)command->name.u8_bytes,
                command->name.data);
        print_words(out, command->flags);
        fputs("\" ;;\n", out);
    }
    fprintf(out,
          "    esac\n"
          "    if [ -z \"$command\" ]; then\n"
          "        COMPREPLY=($(compgen -W \"$commands $global\" -- \"$cur\"))\n"
          "        return\n"
          "    fi\n"
          "    case \"$command\" in\n"
          "        help) COMPREPLY=($(compgen -W \"$commands\" -- \"$cur\")); return ;;\n"
          "        completions) COMPREPLY=($(compgen -W \"bash zsh fish powershell\" -- \"$cur\")); return ;;\n"
          "        cache) COMPREPLY=($(compgen -W \"clean --all\" -- \"$cur\")); return ;;\n"
          "        toolchain)\n"
          "            if [ \"${COMP_WORDS[COMP_CWORD-1]}\" = toolchain ]; then\n"
          "                COMPREPLY=($(compgen -W \"%s\" -- \"$cur\"))\n"
          "            else\n"
          "                COMPREPLY=($(compgen -W \"latest $(%s)\" -- \"$cur\"))\n"
          "            fi\n"
          "            return ;;\n"
          "    esac\n"
          "    if [[ \"$cur\" == -* ]]; then\n"
          "        COMPREPLY=($(compgen -W \"$options $global\" -- \"$cur\"))\n"
          "    else\n"
          "        COMPREPLY=($(compgen -f -- \"$cur\"))\n"
          "    fi\n"
          "}\n"
          "complete -o filenames -F _n00b n00b\n",
          TOOLCHAIN_ACTIONS, TOOLCHAIN_SPECS);
}

static void
print_doc(FILE *out, core_string_t doc)
{
    for (size_t i = 0; i < doc.u8_bytes; i++) {
        if (doc.data[i] == '\'') fputs("\\'", out);
        else fputc(doc.data[i], out);
    }
}

static void
print_fish(FILE *out)
{
    fputs("complete -c n00b -f\n", out);
    for (const parse_cmdr_cmd_t *command = COMMANDS; command->name.data;
         command++) {
        if (command->hidden) continue;
        fprintf(out, "complete -c n00b -n __fish_use_subcommand -a %.*s -d '",
                (int)command->name.u8_bytes, command->name.data);
        print_doc(out, command->doc);
        fputs("'\n", out);
        for (const parse_cmdr_flag_t *flag = command->flags;
             flag && flag->name.data; flag++) {
            if (flag->hidden) continue;
            fprintf(out, "complete -c n00b -n '__fish_seen_subcommand_from %.*s' "
                    "-l %.*s -d '", (int)command->name.u8_bytes,
                    command->name.data, (int)flag->name.u8_bytes - 2,
                    flag->name.data + 2);
            print_doc(out, flag->doc);
            fputs("'\n", out);
        }
    }
    for (const parse_cmdr_flag_t *flag = GLOBAL_FLAGS; flag->name.data;
         flag++) {
        if (flag->hidden) continue;
        fprintf(out, "complete -c n00b -l %.*s -d '",
                (int)flag->name.u8_bytes - 2, flag->name.data + 2);
        print_doc(out, flag->doc);
        fputs("'\n", out);
    }
    fprintf(out,
            "complete -c n00b -n '__fish_seen_subcommand_from build run fmt library' -F\n"
            "complete -c n00b -n '__fish_seen_subcommand_from toolchain; and not __fish_seen_subcommand_from %s' -a '%s'\n"
            "complete -c n00b -n '__fish_seen_subcommand_from use remove pin' -a \"latest (n00b toolchain list 2>/dev/null | string replace -r '^[* ] ' '' | string split -f1 ' ')\"\n"
            "complete -c n00b -n '__fish_seen_subcommand_from completions' -a 'bash zsh fish powershell'\n"
            "complete -c n00b -n '__fish_seen_subcommand_from cache' -a clean\n",
            TOOLCHAIN_ACTIONS, TOOLCHAIN_ACTIONS);
}

static void
print_powershell(FILE *out)
{
    fputs("Register-ArgumentCompleter -Native -CommandName n00b -ScriptBlock {\n"
          "    param($word, $ast, $cursor)\n"
          "    $words = @($ast.CommandElements | Select-Object -Skip 1 | ForEach-Object { $_.ToString() })\n"
          "    $commands = '", out);
    print_commands(out);
    fputs("'.Trim() -split ' '\n    $global = '", out);
    print_words(out, GLOBAL_FLAGS);
    fprintf(out,
          "'.Trim() -split ' '\n"
          "    if ($words.Count -eq 0 -or ($words.Count -eq 1 -and $word)) {\n"
          "        $candidates = $commands + $global\n"
          "    } elseif ($words[0] -eq 'toolchain') {\n"
          "        $candidates = '%s' -split ' '\n"
          "        if ($words.Count -ge 2 -and $words[1] -in @('use', 'remove', 'pin')) {\n"
          "            $candidates = @('latest') + (n00b toolchain list 2>$null | ForEach-Object { ($_.Trim() -replace '^\\* ', '' -split '\\s+')[0] })\n"
          "        }\n"
          "    } else {\n"
          "        return\n"
          "    }\n"
          "    $candidates | Where-Object { $_ -like \"$word*\" } | ForEach-Object {\n"
          "        [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)\n"
          "    }\n"
          "}\n", TOOLCHAIN_ACTIONS);
}

bool
n00bb_print_completions(FILE *out, core_string_t shell)
{
    if (core_string_eq(shell, CORE_STRING_STATIC("bash"))) print_bash(out);
    else if (core_string_eq(shell, CORE_STRING_STATIC("zsh"))) {
        fputs("(( $+functions[compdef] )) || { autoload -Uz compinit && compinit; }\n"
              "autoload -U +X bashcompinit && bashcompinit\n", out);
        print_bash(out);
    }
    else if (core_string_eq(shell, CORE_STRING_STATIC("fish"))) print_fish(out);
    else if (core_string_eq(shell, CORE_STRING_STATIC("powershell")))
        print_powershell(out);
    else return false;
    return true;
}

void
n00bb_invocation_open(n00bb_invocation_t *invocation, int argc, char **argv,
                      core_string_t argv0)
{
    *invocation = (n00bb_invocation_t){
        .argc = argc,
        .argv = argv,
        .argv0 = argv0,
        .words = core_alloc_array(core_string_t, argc > 0 ? argc : 1),
        .end = argc,
        .cli = make_cli(),
    };
    for (int i = 0; i < argc; i++)
        invocation->words[i] = core_string_from_cstr(argv[i]);
    for (int i = 1; i < argc && invocation->end == argc; i++)
        if (core_string_eq(invocation->words[i], CORE_STRING_STATIC("--")))
            invocation->end = i;
    for (int i = 1; i < invocation->end && !invocation->command_at; i++)
        if (invocation->words[i].u8_bytes
            && invocation->words[i].data[0] != '-'
            && parse_cmdr_has_command(invocation->cli, invocation->words[i]))
            invocation->command_at = i;
    invocation->parsed = parse_cmdr_parse(invocation->cli,
        invocation->end - 1, (const char **)argv + 1);
}

void
n00bb_invocation_close(n00bb_invocation_t *invocation)
{
    for (int i = 0; i < invocation->argc; i++)
        core_free(invocation->words[i].data);
    core_free(invocation->words);
    parse_cmdr_result_free(invocation->parsed);
    parse_cmdr_free(invocation->cli);
    *invocation = (n00bb_invocation_t){};
}

bool
n00bb_invocation_flag(const n00bb_invocation_t *invocation,
                      core_string_t name, core_string_t alias)
{
    for (int i = 1; invocation && i < invocation->end; i++)
        if (core_string_eq(invocation->words[i], name)
            || (alias.data && core_string_eq(invocation->words[i], alias)))
            return true;
    return false;
}

bool
n00bb_invocation_command_is(const n00bb_invocation_t *invocation,
                            core_string_t name)
{
    return invocation && invocation->command_at
        && core_string_eq(invocation->words[invocation->command_at], name);
}
