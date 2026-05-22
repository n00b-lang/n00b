#include <assert.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "n00b.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"

static uint64_t
parse_u64_arg(const char *value)
{
    assert(value != nullptr);
    uint64_t out = 0;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        assert(*cursor >= '0' && *cursor <= '9');
        uint64_t digit = (uint64_t)(*cursor - '0');
        assert(out <= (UINT64_MAX - digit) / 10u);
        out = out * 10u + digit;
    }
    return out;
}

static bool
arg_eq(const char *left, const char *right)
{
    return left != nullptr && right != nullptr && strcmp(left, right) == 0;
}

static void
assert_default_heap_at_least(n00b_runtime_t *runtime, uint64_t expected)
{
    assert(runtime != nullptr);
    assert(runtime->default_arena != nullptr);
    assert(runtime->default_allocator ==
           (n00b_allocator_t *)runtime->default_arena);

    uint64_t actual = n00b_arena_size(runtime->default_arena);
    assert(actual >= expected);
    assert(actual < expected + (4u * n00b_page_size));
}

static int
run_init_mode(int argc, char **argv)
{
    n00b_runtime_t runtime = {};
    uint64_t expected = N00B_DEFAULT_SCRATCH_ARENA_SIZE;

    if (arg_eq(argv[1], "kwarg")) {
        assert(argc >= 3);
        expected = parse_u64_arg(argv[2]);
        n00b_init(&runtime, argc, argv, .default_heap_size = expected);
    }
    else if (arg_eq(argv[1], "env")) {
        assert(argc >= 3);
        expected = parse_u64_arg(argv[2]);
        n00b_init(&runtime, argc, argv);
    }
    else if (arg_eq(argv[1], "default")) {
        n00b_init(&runtime, argc, argv);
    }
    else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 2;
    }

    assert_default_heap_at_least(&runtime, expected);
    n00b_shutdown();
    return 0;
}

#ifndef _WIN32
static void
assert_invalid_case_aborts(char **argv, const char *env_value)
{
    pid_t child = fork();
    assert(child >= 0);

    if (child == 0) {
        if (env_value != nullptr) {
            setenv(N00B_DEFAULT_HEAP_SIZE_ENV, env_value, 1);
        }
        (void)run_init_mode(3, argv);
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

static int
run_invalid_cases(void)
{
    char *env_too_low[]    = {"test_runtime_heap_size", "env", "33554432"};
    char *env_not_pow2[]   = {"test_runtime_heap_size", "env", "33554432"};
    char *env_bad_suffix[] = {"test_runtime_heap_size", "env", "33554432"};
    char *env_empty[]      = {"test_runtime_heap_size", "env", "33554432"};
    char *env_bad_start[]  = {"test_runtime_heap_size", "env", "33554432"};
    char *kwarg_too_low[]  = {"test_runtime_heap_size", "kwarg", "16777216"};
    char *kwarg_not_pow2[] = {"test_runtime_heap_size", "kwarg", "33554431"};

    assert_invalid_case_aborts(env_too_low, "16M");
    assert_invalid_case_aborts(env_not_pow2, "48M");
    assert_invalid_case_aborts(env_bad_suffix, "64MiB");
    assert_invalid_case_aborts(env_empty, "");
    assert_invalid_case_aborts(env_bad_start, "heap64M");
    assert_invalid_case_aborts(kwarg_too_low, nullptr);
    assert_invalid_case_aborts(kwarg_not_pow2, nullptr);
    return 0;
}
#endif

int
main(int argc, char **argv)
{
    assert(argc >= 2);

    if (arg_eq(argv[1], "invalid")) {
#ifdef _WIN32
        return 77;
#else
        return run_invalid_cases();
#endif
    }

    return run_init_mode(argc, argv);
}
