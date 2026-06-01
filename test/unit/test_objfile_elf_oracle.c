#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"

#include "objfile_elf_casegen.h"

#if defined(_WIN32)

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("  [SKIP] objfile_elf_oracle is POSIX-only for now.\n");
    return 0;
}

#else

#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int  exit_status;
    char output[4096];
} oracle_run_result_t;

static bool
env_is_one(const char *name)
{
    const char *value = getenv(name);
    return value != nullptr && strcmp(value, "1") == 0;
}

static bool
case_filter_matches(const char *filter, const char *name)
{
    if (filter == nullptr || filter[0] == '\0') {
        return true;
    }

    const char *p = filter;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ') {
            p++;
        }

        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }

        const char *end = p;
        while (end > start && end[-1] == ' ') {
            end--;
        }

        size_t len = (size_t)(end - start);
        if (strlen(name) == len && strncmp(start, name, len) == 0) {
            return true;
        }
    }

    return false;
}

static bool
parse_succeeds(n00b_buffer_t *buf)
{
    n00b_bstream_t *stream = n00b_bstream_new(buf);
    auto            result = n00b_elf_parse(stream);

    return n00b_result_is_ok(result);
}

static bool
write_fixture_file(const n00b_test_elf_case_t *test_case,
                   n00b_buffer_t              *buf,
                   char                       *path,
                   size_t                      path_len)
{
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == nullptr || tmpdir[0] == '\0') {
        tmpdir = "/tmp";
    }

    int n = snprintf(path,
                     path_len,
                     "%s/n00b_elf_oracle_%ld_%s.elf",
                     tmpdir,
                     (long)getpid(),
                     test_case->name);
    if (n < 0 || (size_t)n >= path_len) {
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }

    size_t written = fwrite(buf->data, 1, n00b_buffer_len(buf), f);
    int    err     = ferror(f);
    int    closed  = fclose(f);

    return written == n00b_buffer_len(buf) && err == 0 && closed == 0;
}

static oracle_run_result_t
run_oracle(const char *oracle_bin, const char *mode, const char *fixture_path)
{
    oracle_run_result_t result = {
        .exit_status = 127,
    };

    int fds[2];
    if (pipe(fds) != 0) {
        snprintf(result.output, sizeof(result.output), "pipe failed: %s\n",
                 strerror(errno));
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(result.output, sizeof(result.output), "fork failed: %s\n",
                 strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return result;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);

        execl(oracle_bin,
              oracle_bin,
              "--mode",
              mode,
              fixture_path,
              (char *)nullptr);
        _exit(127);
    }

    close(fds[1]);

    size_t used = 0;
    while (used + 1 < sizeof(result.output)) {
        ssize_t n = read(fds[0],
                         result.output + used,
                         sizeof(result.output) - used - 1);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    result.output[used] = '\0';
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.exit_status = 127;
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_status = WEXITSTATUS(status);
    }
    else {
        result.exit_status = 128;
    }

    return result;
}

static bool
extract_verdict(const char *output, char *verdict, size_t verdict_len)
{
    const char *p = strstr(output, "verdict=");
    if (p == nullptr) {
        return false;
    }

    p += strlen("verdict=");

    size_t i = 0;
    while (p[i] != '\0' && p[i] != '\n' && p[i] != '\r') {
        if (i + 1 >= verdict_len) {
            return false;
        }

        verdict[i] = p[i];
        i++;
    }

    verdict[i] = '\0';
    return i > 0;
}

static bool
run_oracle_case(const char *oracle_bin, const n00b_test_elf_case_t *test_case)
{
    if (test_case->oracle_mode == N00B_TEST_ELF_ORACLE_NONE) {
        fprintf(stderr, "Case %s has no oracle mode.\n", test_case->name);
        return false;
    }

    n00b_buffer_t *buf = n00b_test_elf_case_generate(test_case);
    if (buf == nullptr) {
        fprintf(stderr, "Could not generate case %s.\n", test_case->name);
        return false;
    }

    bool n00b_parse_ok = parse_succeeds(buf);

    char fixture_path[512];
    if (!write_fixture_file(test_case, buf, fixture_path, sizeof(fixture_path))) {
        fprintf(stderr, "Could not write fixture for %s.\n", test_case->name);
        return false;
    }

    const char *mode = n00b_test_elf_oracle_mode_arg(test_case->oracle_mode);
    oracle_run_result_t oracle = run_oracle(oracle_bin, mode, fixture_path);

    if (!env_is_one("N00B_ELF_ORACLE_KEEP_TMP")) {
        unlink(fixture_path);
    }
    else {
        printf("  [INFO] kept %s\n", fixture_path);
    }

    if (oracle.exit_status != 0) {
        fprintf(stderr,
                "Oracle failed for %s with exit status %d:\n%s\n",
                test_case->name,
                oracle.exit_status,
                oracle.output);
        return false;
    }

    char verdict[128];
    if (!extract_verdict(oracle.output, verdict, sizeof(verdict))) {
        fprintf(stderr,
                "Oracle output for %s did not contain verdict=...:\n%s\n",
                test_case->name,
                oracle.output);
        return false;
    }

    const char *expected = n00b_test_elf_oracle_expect_name(test_case->oracle_expect);
    if (strcmp(verdict, expected) != 0) {
        fprintf(stderr,
                "Oracle verdict mismatch for %s: expected %s, got %s\n%s\n",
                test_case->name,
                expected,
                verdict,
                oracle.output);
        return false;
    }

    printf("  [PASS] %s n00b_parse=%s oracle=%s state=%s\n",
           test_case->name,
           n00b_parse_ok ? "ok" : "reject",
           verdict,
           n00b_test_elf_case_state_name(test_case->state));
    return true;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    if (!env_is_one("N00B_TEST_ELF_ORACLE")) {
        printf("  [SKIP] N00B_TEST_ELF_ORACLE!=1\n");
        return 0;
    }

    const char *oracle_bin = getenv("N00B_ELF_ORACLE_BIN");
    if (oracle_bin == nullptr || oracle_bin[0] == '\0') {
        if (getenv("N00B_ELF_ORACLE_ROOT") != nullptr) {
            printf("  [SKIP] N00B_ELF_ORACLE_ROOT auto-build is not implemented yet; "
                   "set N00B_ELF_ORACLE_BIN.\n");
        }
        else {
            printf("  [SKIP] N00B_ELF_ORACLE_BIN is not set.\n");
        }
        return 0;
    }

    if (access(oracle_bin, X_OK) != 0) {
        printf("  [SKIP] N00B_ELF_ORACLE_BIN is not executable: %s\n",
               oracle_bin);
        return 0;
    }

    const char *filter = getenv("N00B_ELF_ORACLE_CASE");
    size_t      ran    = 0;

    for (size_t i = 0; i < n00b_test_elf_case_count; i++) {
        const n00b_test_elf_case_t *test_case = &n00b_test_elf_cases[i];

        if (!case_filter_matches(filter, test_case->name)) {
            continue;
        }

        if (test_case->oracle_mode == N00B_TEST_ELF_ORACLE_NONE) {
            if (filter != nullptr && filter[0] != '\0') {
                fprintf(stderr, "Requested case %s has no oracle mode.\n",
                        test_case->name);
                return 1;
            }
            continue;
        }

        if (!run_oracle_case(oracle_bin, test_case)) {
            return 1;
        }
        ran++;
    }

    if (filter != nullptr && filter[0] != '\0' && ran == 0) {
        fprintf(stderr, "No oracle cases matched N00B_ELF_ORACLE_CASE=%s\n",
                filter);
        return 1;
    }

    printf("ELF oracle checks passed (%zu cases).\n", ran);
    return 0;
}

#endif
