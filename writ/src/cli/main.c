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
static writ_str_t features[MAX_LIST];
static size_t     feature_count;
static writ_str_t contract_dirs[MAX_LIST];
static size_t     contract_dir_count;
static writ_str_t embedded_language;
static bool       fixing;

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

static void
print_fixed(const writ_session_t *session)
{
    const writ_diag_suggestion_t *edits[MAX_LIST];
    size_t count = 0;
    writ_str_t path = {0};
    for (size_t i = 0; i < writ_diag_count(session); i++) {
        const writ_diag_t *diag = writ_diag_at(session, i);
        for (size_t s = 0; s < diag->suggestion_count && count < MAX_LIST; s++) {
            if (!path.length || (path.length == diag->path.length && !memcmp(path.data, diag->path.data, path.length))) {
                path = diag->path;
                edits[count++] = &diag->suggestions[s];
            }
        }
    }
    char name[4096];
    FILE *file = path.length < sizeof(name) ? fopen((snprintf(name, sizeof(name), "%.*s", (int)path.length, path.data), name), "rb")
                                            : nullptr;
    if (!file) {
        return;
    }
    char text[1 << 16];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    for (size_t i = 1; i < count; i++) {
        for (size_t k = i; k > 0 && edits[k]->span.start_byte < edits[k - 1]->span.start_byte; k--) {
            const writ_diag_suggestion_t *swap = edits[k];
            edits[k] = edits[k - 1];
            edits[k - 1] = swap;
        }
    }
    size_t at = 0;
    for (size_t i = 0; i < count && edits[i]->span.end_byte <= length && edits[i]->span.start_byte >= at; i++) {
        fwrite(text + at, 1, edits[i]->span.start_byte - at, stdout);
        fwrite(edits[i]->replacement.data, 1, edits[i]->replacement.length, stdout);
        at = edits[i]->span.end_byte;
    }
    fwrite(text + at, 1, length - at, stdout);
}

static writ_status_t
add_embedded(writ_unit_t *unit, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return WRIT_IO_FAILED;
    }
    static char text[1 << 20];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    writ_origin_t origin = {.host_path = arg(path), .first_line = 1, .first_column = 1};
    return writ_unit_add_embedded(unit, embedded_language, origin, (writ_str_t){text, length}, nullptr);
}

static writ_unit_t *
load_unit(writ_session_t *session, const char *path, bool embedded)
{
    writ_unit_t *unit = writ_unit_new(session);
    writ_status_t status = embedded ? add_embedded(unit, path) : writ_unit_add_file(unit, arg(path), nullptr);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot read %s\n", path);
    }
    return status == WRIT_OK ? unit : nullptr;
}

static writ_unit_t *
load(writ_session_t *session, const char *path)
{
    return load_unit(session, path, embedded_language.length != 0);
}

static writ_unit_t *
load_file(writ_session_t *session, const char *path)
{
    return load_unit(session, path, false);
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
        return nullptr;
    }
    for (size_t i = 0; i < feature_count; i++) {
        writ_platform_set_feature(platform, features[i], true);
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
bind_embedded(writ_session_t *session, int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--profile")) {
        return 2;
    }
    writ_platform_t *platform = platform_for(session);
    writ_unit_t *unit = platform ? load(session, argv[0]) : nullptr;
    const writ_model_t *model = nullptr;
    writ_bind_request_t request = {.struct_size = sizeof(request), .binding = unit,
                                   .profiles = load_file(session, argv[2]), .language = embedded_language};
    const writ_binding_t *binding = nullptr;
    writ_str_t text;
    if (!unit || !request.profiles || writ_resolve(unit, platform, &model) != WRIT_OK
        || writ_bind(model, &request, &binding) != WRIT_OK || writ_binding_render(binding, &text) != WRIT_OK) {
        return 1;
    }
    fwrite(text.data, 1, text.length, stdout);
    return 0;
}

static int
cmd_bind(writ_session_t *session, int argc, char **argv)
{
    if (embedded_language.length) {
        return bind_embedded(session, argc, argv);
    }
    if (argc != 4 && !(argc == 6 && !strcmp(argv[4], "--language"))) {
        return 2;
    }
    if (strcmp(argv[2], "--profile")) {
        return 2;
    }
    const writ_model_t *model = resolve(session, argv[0]);
    writ_bind_request_t request = {.struct_size = sizeof(request), .binding = load(session, argv[1]),
                                   .profiles = load(session, argv[3]),
                                   .language = argc == 6 ? arg(argv[5]) : (writ_str_t){0}};
    const writ_binding_t *binding = nullptr;
    writ_str_t text;
    if (!model || !request.binding || !request.profiles || writ_bind(model, &request, &binding) != WRIT_OK
        || writ_binding_render(binding, &text) != WRIT_OK) {
        return 1;
    }
    fwrite(text.data, 1, text.length, stdout);
    return 0;
}

static int
cmd_fmt(writ_session_t *session, int argc, char **argv)
{
    if (argc != 1) {
        return 2;
    }
    writ_unit_t *unit = load(session, argv[0]);
    if (!unit) {
        return 1;
    }
    writ_output_t output;
    output_for(session, "-", &output);
    return writ_format_unit(unit, &output) == WRIT_OK ? 0 : 1;
}

static bool
file_then_output(writ_session_t *session, int argc, char **argv, writ_output_t *output)
{
    if (argc == 3 && !strcmp(argv[1], "-o")) {
        output_for(session, argv[2], output);
        return true;
    }
    output_for(session, "-", output);
    return argc == 1;
}

static int
cmd_derive(writ_session_t *session, int argc, char **argv)
{
    writ_output_t output;
    if (!file_then_output(session, argc, argv, &output)) {
        return 2;
    }
    writ_unit_t *unit = load(session, argv[0]);
    return unit && writ_derive_transport(unit, &output) == WRIT_OK ? 0 : 1;
}

static int
cmd_lower(writ_session_t *session, int argc, char **argv)
{
    writ_output_t output;
    if (!file_then_output(session, argc, argv, &output)) {
        return 2;
    }
    const writ_model_t *model = resolve(session, argv[0]);
    return model && writ_lower_json(model, &output) == WRIT_OK ? 0 : 1;
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
        for (size_t arm = 0;; arm++) {
            const writ_plan_view_t *plan = writ_model_fn_arm_plan(model, fn, arm);
            writ_str_t text;
            if (!plan && arm) {
                break;
            }
            if (!plan || writ_model_plan_render(model, plan, &text) != WRIT_OK) {
                status = 1;
                break;
            }
            fwrite(text.data, 1, text.length, stdout);
            fputc('\n', stdout);
        }
    }
    return status;
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

static void
print_links(const char *label, const writ_str_t *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        printf("%s %.*s\n", label, (int)items[i].length, items[i].data);
    }
}

static int
gen_n00b(writ_session_t *session, int argc, char **argv)
{
    writ_gen_n00b_request_t request = {.struct_size = sizeof(request)};
    const char *directory = ".";
    const char *profiles = WRIT_PROFILE_INSTALL_DIR "/n00b.writ";
    const char *source = nullptr;
    for (int i = 1; i < argc; i++) {
        size_t taken = 0;
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            directory = argv[++i];
        } else if (!strcmp(argv[i], "--embedded") && i + 1 < argc) {
            source = argv[++i];
        } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            profiles = argv[++i];
        } else if (!take_list(argv, &i, argc, "--prefix", &request.shim_prefix, &taken)
                   && !take_list(argv, &i, argc, "--module", &request.module_name, &taken)
                   && !take_list(argv, &i, argc, "--namespace", &request.type_namespace, &taken)) {
            return 2;
        }
    }
    if (!source) {
        return 2;
    }
    embedded_language = arg("n00b");
    writ_platform_t *platform = platform_for(session);
    writ_unit_t *unit = platform ? load(session, source) : nullptr;
    const writ_model_t *model = nullptr;
    writ_bind_request_t bind = {.struct_size = sizeof(bind), .binding = unit, .profiles = load_file(session, profiles),
                                .language = embedded_language};
    const writ_binding_t *binding = nullptr;
    if (!unit || !bind.profiles || writ_resolve(unit, platform, &model) != WRIT_OK
        || writ_bind(model, &bind, &binding) != WRIT_OK) {
        return 1;
    }
    request.binding = binding;
    writ_output_t output;
    output_for(session, directory, &output);
    writ_link_requirements_t links;
    writ_n00b_externs_t externs;
    writ_status_t status = writ_gen_n00b(&request, &output, &links, &externs);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot write into %s\n", directory);
    }
    if (status == WRIT_OK) {
        print_links("library", links.libraries, links.libraries_count);
        print_links("framework", links.frameworks, links.frameworks_count);
        print_links("search_path", links.search_paths, links.search_paths_count);
        printf("load %s\n", links.load_dynamic ? "dynamic" : "static");
        for (size_t i = 0; i < externs.count; i++) {
            printf("extern %.*s %.*s %.*s\n", (int)externs.items[i].name.length, externs.items[i].name.data,
                   (int)externs.items[i].symbol.length, externs.items[i].symbol.data,
                   (int)externs.items[i].signature.length, externs.items[i].signature.data);
        }
    }
    return status == WRIT_OK ? 0 : 1;
}

static int
cmd_gen(writ_session_t *session, int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[0], "n00b")) {
        return gen_n00b(session, argc, argv);
    }
    bool rust = argc >= 2 && !strcmp(argv[0], "rust");
    if (argc < 2 || (strcmp(argv[0], "c") && !rust)) {
        return 2;
    }
    writ_gen_c_request_t request = {.struct_size = sizeof(request)};
    writ_gen_rust_request_t transport = {.struct_size = sizeof(transport)};
    const char *directory = ".";
    for (int i = 2; i < argc; i++) {
        size_t taken = 0;
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            directory = argv[++i];
        } else if (take_list(argv, &i, argc, "--prefix", &request.prefix, &taken)
                   || take_list(argv, &i, argc, "--crate-path", &transport.crate_path, &taken)) {
            continue;
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
    writ_status_t status = rust ? writ_gen_rust(model, &transport, &output) : writ_gen_c(model, &request, &output);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot write into %s\n", directory);
    }
    return status == WRIT_OK ? 0 : 1;
}

static int
cmd_import(writ_session_t *session, int argc, char **argv)
{
    writ_str_t headers[MAX_LIST], links[MAX_LIST], defines[MAX_LIST], includes[MAX_LIST], only[MAX_LIST];
    writ_import_request_t request = {.struct_size = sizeof(request), .headers = headers, .links = links,
                                     .defines = defines, .include_paths = includes, .only = only};
    writ_rust_import_request_t rust = {.struct_size = sizeof(rust), .features = features,
                                       .features_count = feature_count};
    const char *directory = ".";
    for (int i = 0; i < argc; i++) {
        size_t taken = 0;
        if (!strcmp(argv[i], "--library") && i + 1 < argc) {
            request.library = arg(argv[++i]);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            directory = argv[++i];
        } else if (take_list(argv, &i, argc, "--rust", &rust.manifest, &taken)
                   || take_list(argv, &i, argc, "--rustdoc", &rust.rustdoc_json, &taken)
                   || take_list(argv, &i, argc, "--crate", &rust.crate, &taken)
                   || take_list(argv, &i, argc, "--package", &rust.package, &taken)) {
            continue;
        } else if (!take_list(argv, &i, argc, "--header", headers, &request.headers_count)
                   && !take_list(argv, &i, argc, "--link", links, &request.links_count)
                   && !take_list(argv, &i, argc, "--define", defines, &request.defines_count)
                   && !take_list(argv, &i, argc, "-I", includes, &request.include_paths_count)
                   && !take_list(argv, &i, argc, "--only", only, &request.only_count)) {
            return 2;
        }
    }
    writ_output_t output;
    output_for(session, directory, &output);
    if (rust.crate.length) {
        rust.library = request.library;
        rust.package = rust.package.length ? rust.package : rust.crate;
        return request.library.length ? writ_toolchain_import_rust(session, &rust, &output) != WRIT_OK : 2;
    }
    writ_platform_t *platform = platform_for(session);
    if (!platform || !request.library.length || !request.headers_count) {
        return platform ? 2 : 1;
    }
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
    {"gen", cmd_gen,
     "gen c FILE [-o DIR] [--prefix NAME] [--conformance] | gen rust FILE [--crate-path DIR] [-o DIR] | "
     "gen n00b --embedded FILE [--profile FILE] [--prefix NAME] [--module NAME] [-o DIR]"},
    {"parse", cmd_parse, "parse FILE"},
    {"check", cmd_check, "check FILE"},
    {"plan", cmd_plan, "plan FILE [FUNCTION]"},
    {"lower", cmd_lower, "lower FILE [-o DIR]"},
    {"derive", cmd_derive, "derive FILE [-o DIR]"},
    {"bind", cmd_bind, "bind CONTRACT BINDING --profile FILE [--language NAME] | --embedded NAME bind FILE --profile FILE"},
    {"fmt", cmd_fmt, "fmt FILE"},
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
        } else if (!strcmp(argv[i], "--feature") && i + 1 < argc && feature_count < MAX_LIST) {
            features[feature_count++] = arg(argv[++i]);
        } else if (!strcmp(argv[i], "--fix")) {
            fixing = true;
        } else if (!strcmp(argv[i], "--embedded") && i + 1 < argc && kept == 1) {
            embedded_language = arg(argv[++i]);
        } else if (!strcmp(argv[i], "--contract-dir") && i + 1 < argc && contract_dir_count < MAX_LIST) {
            contract_dirs[contract_dir_count++] = arg(argv[++i]);
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
        for (size_t d = 0; d < contract_dir_count; d++) {
            writ_session_add_contract_dir(session, contract_dirs[d]);
        }
        int status = commands[i].run(session, argc - 2, argv + 2);
        print_diags(session);
        if (fixing) {
            print_fixed(session);
        }
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
