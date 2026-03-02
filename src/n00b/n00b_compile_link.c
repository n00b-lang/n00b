/**
 * @file n00b_compile_link.c
 * @brief Link .o files into an executable via clang.
 */

#include "n00b.h"
#include "n00b/n00b_compile_binary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

static const char *
find_lib_dir(const char *hint)
{
    if (hint && access(hint, R_OK) == 0) {
        return hint;
    }

    // Check N00B_LIB_DIR environment variable.
    const char *env = getenv("N00B_LIB_DIR");

    if (env && access(env, R_OK) == 0) {
        return env;
    }

    // Check relative to executable: ../lib/
    // (common install layout: bin/n00b, lib/libn00b.a)
    char path[1024];

    snprintf(path, sizeof(path), "../lib/libn00b.a");

    if (access(path, R_OK) == 0) {
        return "../lib";
    }

    // Check build directory.
    snprintf(path, sizeof(path), "build_debug/libn00b.a");

    if (access(path, R_OK) == 0) {
        return "build_debug";
    }

    return NULL;
}

int
n00b_link_binary(const char **obj_paths, int n_objs,
                 const char *output, const char *lib_dir)
{
    const char *resolved_lib_dir = find_lib_dir(lib_dir);

    // Build argv for clang.
    // clang -o <output> obj1.o obj2.o ... -L<lib_dir> -ln00b
    //       -lpthread -lm -ldl [-framework CoreFoundation on macOS]
    int max_args = n_objs + 20;
    const char **argv = malloc(sizeof(const char *) * (size_t)(max_args + 1));
    int ai = 0;

    argv[ai++] = "clang";
    argv[ai++] = "-o";
    argv[ai++] = output;

    for (int i = 0; i < n_objs; i++) {
        argv[ai++] = obj_paths[i];
    }

    char lib_flag[1024];

    if (resolved_lib_dir) {
        snprintf(lib_flag, sizeof(lib_flag), "-L%s", resolved_lib_dir);
        argv[ai++] = lib_flag;
    }

    argv[ai++] = "-ln00b";
    argv[ai++] = "-lpthread";
    argv[ai++] = "-lm";

#ifdef __APPLE__
    argv[ai++] = "-framework";
    argv[ai++] = "CoreFoundation";
    argv[ai++] = "-framework";
    argv[ai++] = "AppKit";
#else
    argv[ai++] = "-ldl";
#endif

    argv[ai] = NULL;

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "n00b compile: fork() failed: %s\n", strerror(errno));
        free(argv);
        return 1;
    }

    if (pid == 0) {
        // Child: exec clang.
        execvp("clang", (char *const *)argv);
        fprintf(stderr, "n00b compile: execvp(clang) failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    // Parent: wait for clang.
    free(argv);

    int status;

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "n00b compile: waitpid() failed: %s\n",
                strerror(errno));
        return 1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return 1;
}
