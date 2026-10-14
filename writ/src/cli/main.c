#include <stdio.h>
#include <string.h>

#include "writ/gen.h"

typedef int (*command_fn)(writ_session_t *session, int argc, char **argv);

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

static writ_str_t platform_triple;

static const writ_model_t *
resolve(writ_session_t *session, const char *path)
{
    writ_platform_t *platform = nullptr;
    if (writ_platform_from_triple(session, platform_triple, &platform) != WRIT_OK) {
        fprintf(stderr, "writ: unknown platform %.*s\n", (int)platform_triple.length, platform_triple.data);
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
    writ_output_directory(session, (writ_str_t){directory, strlen(directory)}, &output);
    writ_status_t status = writ_gen_c(model, &request, &output);
    if (status == WRIT_IO_FAILED) {
        fprintf(stderr, "writ: cannot write into %s\n", directory);
    }
    return status == WRIT_OK ? 0 : 1;
}

static const struct {
    const char *name;
    command_fn  run;
    const char *usage;
} commands[] = {
    {"gen", cmd_gen, "gen c FILE [-o DIR] [--prefix NAME] [--conformance]"},
    {"parse", cmd_parse, "parse FILE"},
    {"check", cmd_check, "check FILE"},
    {"plan", cmd_plan, "plan FILE [FUNCTION]"},
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
        writ_session_t *session = writ_session_new(nullptr);
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
