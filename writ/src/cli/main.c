#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "writ/clang.h"
#include "writ/gen.h"
#include "writ/toolchain.h"

typedef int (*command_fn)(writ_session_t *session, int argc, char **argv);

enum {
    MAX_LIST = 64,
};

static writ_str_t
arg(const char *text)
{
    return (writ_str_t){text, strlen(text)};
}

static writ_str_t platform_triple;

static writ_status_t
write_stdout(void *context, writ_str_t path, writ_str_t contents)
{
    (void)context;
    (void)path;
    return fwrite(contents.data, 1, contents.length, stdout) == contents.length ? WRIT_OK : WRIT_IO_FAILED;
}

static void
output_for(writ_session_t *session, const char *directory, writ_output_t *output)
{
    if (!strcmp(directory, "-")) {
        *output = (writ_output_t){write_stdout, nullptr};
    } else {
        writ_output_directory(session, arg(directory), output);
    }
}

static void
print_diags(const writ_session_t *session)
{
    for (size_t i = 0; i < writ_diag_count(session); i++) {
        writ_str_t text;
        writ_diag_render(session, writ_diag_at(session, i), &text);
        fwrite(text.data, 1, text.length, stderr);
    }
}

static writ_unit_t *
load(writ_session_t *session, const char *path)
{
    writ_unit_t *unit = writ_unit_new(session);
    writ_status_t status = writ_unit_add_file(unit, (writ_str_t){path, strlen(path)}, nullptr);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot read %s\n", path);
    }
    return status == WRIT_OK ? unit : nullptr;
}

static int
cmd_parse(writ_session_t *session, int argc, char **argv)
{
    if (argc < 1) {
        return 2;
    }
    writ_unit_t *unit = load(session, argv[0]);
    if (!unit) {
        return 1;
    }
    writ_str_t dump;
    writ_unit_dump(unit, &dump);
    fwrite(dump.data, 1, dump.length, stdout);
    return 0;
}

static writ_platform_t *
platform_for(writ_session_t *session)
{
    writ_platform_t *platform = nullptr;
    if (writ_platform_from_triple(session, platform_triple, &platform) != WRIT_OK) {
        fprintf(stderr, "writ: unknown platform %.*s\n", (int)platform_triple.length, platform_triple.data);
    }
    return platform;
}

static const writ_model_t *
resolve(writ_session_t *session, const char *path)
{
    writ_platform_t *platform = platform_for(session);
    if (!platform) {
        return nullptr;
    }
    writ_unit_t *unit = load(session, path);
    const writ_model_t *model = nullptr;
    return unit && writ_resolve(unit, platform, &model) == WRIT_OK ? model : nullptr;
}

static int
cmd_check(writ_session_t *session, int argc, char **argv)
{
    if (argc < 1) {
        return 2;
    }
    return resolve(session, argv[0]) ? 0 : 1;
}

static int
cmd_lower(writ_session_t *session, int argc, char **argv)
{
    const char *directory = "-";
    if (argc == 3 && !strcmp(argv[1], "-o")) {
        directory = argv[2];
    } else if (argc != 1) {
        return 2;
    }
    const writ_model_t *model = resolve(session, argv[0]);
    if (!model) {
        return 1;
    }
    writ_output_t output;
    output_for(session, directory, &output);
    return writ_lower_json(model, &output) == WRIT_OK ? 0 : 1;
}

static int
cmd_plan(writ_session_t *session, int argc, char **argv)
{
    if (argc < 1) {
        return 2;
    }
    const writ_model_t *model = resolve(session, argv[0]);
    if (!model) {
        return 1;
    }
    int status = 0;
    for (size_t i = 0; i < writ_model_fn_count(model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        if (argc > 1 && (strlen(argv[1]) != fn->contract_name.length
                         || memcmp(argv[1], fn->contract_name.data, fn->contract_name.length))) {
            continue;
        }
        const writ_plan_view_t *plan = writ_model_fn_plan(model, fn);
        writ_str_t text;
        if (!plan || writ_model_plan_render(model, plan, &text) != WRIT_OK) {
            status = 1;
            continue;
        }
        fwrite(text.data, 1, text.length, stdout);
        fputc('\n', stdout);
    }
    return status;
}

static int
cmd_gen(writ_session_t *session, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[0], "c")) {
        return 2;
    }
    writ_gen_c_request_t request = {.struct_size = sizeof(request)};
    const char *directory = ".";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            directory = argv[++i];
        } else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) {
            request.prefix = (writ_str_t){argv[i + 1], strlen(argv[i + 1])};
            i++;
        } else if (!strcmp(argv[i], "--conformance")) {
            request.conformance = true;
        } else {
            return 2;
        }
    }
    const writ_model_t *model = resolve(session, argv[1]);
    if (!model) {
        return 1;
    }
    writ_output_t output;
    output_for(session, directory, &output);
    writ_status_t status = writ_gen_c(model, &request, &output);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot write into %s\n", directory);
    }
    return status == WRIT_OK ? 0 : 1;
}

static bool
take_list(char **argv, int *i, int argc, const char *flag, writ_str_t *list, size_t *count)
{
    if (strcmp(argv[*i], flag) || *i + 1 >= argc || *count >= MAX_LIST) {
        return false;
    }
    list[(*count)++] = arg(argv[++*i]);
    return true;
}

static int
cmd_import(writ_session_t *session, int argc, char **argv)
{
    writ_str_t headers[MAX_LIST], links[MAX_LIST], defines[MAX_LIST], includes[MAX_LIST], only[MAX_LIST];
    writ_import_request_t request = {.struct_size = sizeof(request), .headers = headers, .links = links,
                                     .defines = defines, .include_paths = includes, .only = only};
    const char *directory = ".";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--library") && i + 1 < argc) {
            request.library = arg(argv[++i]);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            directory = argv[++i];
        } else if (!take_list(argv, &i, argc, "--header", headers, &request.headers_count)
                   && !take_list(argv, &i, argc, "--link", links, &request.links_count)
                   && !take_list(argv, &i, argc, "--define", defines, &request.defines_count)
                   && !take_list(argv, &i, argc, "-I", includes, &request.include_paths_count)
                   && !take_list(argv, &i, argc, "--only", only, &request.only_count)) {
            return 2;
        }
    }
    writ_platform_t *platform = platform_for(session);
    if (!platform || !request.library.length || !request.headers_count) {
        return platform ? 2 : 1;
    }
    writ_output_t output;
    output_for(session, directory, &output);
    return writ_clang_import(session, platform, &request, &output) == WRIT_OK ? 0 : 1;
}

static int
cmd_verify(writ_session_t *session, int argc, char **argv)
{
    writ_str_t binary = {0};
    const char *path = nullptr;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--binary") && i + 1 < argc) {
            binary = arg(argv[++i]);
        } else if (!path) {
            path = argv[i];
        } else {
            return 2;
        }
    }
    if (!path) {
        return 2;
    }
    const writ_model_t *model = resolve(session, path);
    if (!model) {
        return 1;
    }
    writ_str_t report;
    int failed = 0;
    failed |= writ_clang_verify_layout(model, (writ_str_t){0}, &report) != WRIT_OK;
    printf("layout\n%.*s", (int)report.length, report.data);
    failed |= writ_toolchain_verify_probes(model, (writ_str_t){0}, &report) != WRIT_OK;
    printf("probe ok\n");
    failed |= writ_toolchain_abi_classes(model, (writ_str_t){0}, &report) != WRIT_OK;
    printf("abi\n%.*s", (int)report.length, report.data);
    if (binary.length || writ_model_library(model)->links_count) {
        failed |= writ_toolchain_verify_symbols(model, binary, (writ_str_t){0}, &report) != WRIT_OK;
        printf("symbols\n%.*s", (int)report.length, report.data);
    }
    return failed;
}

static const struct {
    const char *name;
    command_fn  run;
    const char *usage;
} commands[] = {
    {"import", cmd_import, "import --library NAME --header H... [--link L] [--define D] [-I DIR] [--only GLOB] [-o DIR]"},
    {"verify", cmd_verify, "verify FILE [--binary PATH]"},
    {"gen", cmd_gen, "gen c FILE [-o DIR] [--prefix NAME] [--conformance]"},
    {"parse", cmd_parse, "parse FILE"},
    {"check", cmd_check, "check FILE"},
    {"plan", cmd_plan, "plan FILE [FUNCTION]"},
    {"lower", cmd_lower, "lower FILE [-o DIR]"},
};

int
main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--version")) {
        printf("writ api %u\n", writ_api_version());
        return 0;
    }
    platform_triple = writ_platform_host_triple();
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--platform") && i + 1 < argc) {
            platform_triple = (writ_str_t){argv[i + 1], strlen(argv[i + 1])};
            i++;
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    for (size_t i = 0; argc >= 2 && i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(argv[1], commands[i].name)) {
            continue;
        }
        const char *llvm = getenv("WRIT_LLVM_PREFIX");
        writ_session_config_t config = {.struct_size = sizeof(config),
                                        .llvm_prefix = arg(llvm && *llvm ? llvm : WRIT_DEFAULT_LLVM_PREFIX)};
        writ_session_t *session = writ_session_new(&config);
        int status = commands[i].run(session, argc - 2, argv + 2);
        print_diags(session);
        writ_session_free(session);
        if (status == 2) {
            fprintf(stderr, "usage: writ %s\n", commands[i].usage);
        }
        return status;
    }
    fprintf(stderr, "usage: writ <command> [args]\n");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        fprintf(stderr, "  writ %s\n", commands[i].usage);
    }
    return 2;
}
