#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

#ifdef _WIN32
#define CORE_OS_LIST_SEPARATOR ';'
#else
#define CORE_OS_LIST_SEPARATOR ':'
#endif

typedef struct {
    bool existed;
    int error;
    core_string_t failed_path;
} core_os_removal_t;

bool core_os_is_separator(char c);
core_string_t core_os_path_join(core_string_t parent, core_string_t child);
core_string_t core_os_path_parent(core_string_t path);
core_string_t core_os_find_upward(core_string_t name);
core_string_t core_os_executable_path(core_string_t argv0);
core_string_t core_os_environment(core_string_t name);
bool core_os_set_environment(core_string_t name, core_string_t value);
bool core_os_unset_environment(core_string_t name);
bool core_os_is_directory(core_string_t path);
bool core_os_remove_tree(core_string_t path, core_os_removal_t *result);
bool core_os_write_file_atomic(core_string_t path, const void *data,
                               size_t size);
void core_os_removal_release(core_os_removal_t *result);
core_string_t core_os_real_path(core_string_t path);
core_string_t core_os_temp_root(void);
core_string_t core_os_current_directory(void);
bool core_os_symlink_directory(core_string_t target, core_string_t link);
core_string_t core_os_make_unique_directory(core_string_t stem);
int core_os_make_unique_file(core_string_t stem, core_string_t *path);
int core_os_temp_fd(void);
int core_os_cpu_count(void);
int core_os_terminal_columns(int fd);
void core_os_binary_io(void);
bool core_os_make_directory(core_string_t path, unsigned mode);
bool core_os_set_mode(int fd, unsigned mode);
bool core_os_rename_replace(core_string_t from, core_string_t to);
bool core_os_is_executable(core_string_t path);
bool core_os_is_regular_file(core_string_t path);
void *core_os_map(int fd, size_t size, bool writable, bool private_copy,
                  void *requested);
bool core_os_unmap(void *mapping, size_t size);
long long core_os_read_at(int fd, void *data, size_t bytes, size_t offset);
bool core_os_sync(void *mapping, size_t size, int fd);

typedef enum {
    CORE_OS_KIND_OTHER,
    CORE_OS_KIND_FILE,
    CORE_OS_KIND_DIRECTORY,
} core_os_kind_t;

typedef enum {
    CORE_OS_FAILED,
    CORE_OS_MISSING,
    CORE_OS_FOUND,
} core_os_lookup_t;

typedef struct {
    core_os_kind_t kind;
    uint64_t size;
    uint64_t modified;
    uint64_t changed;
    uint64_t device;
    uint64_t inode;
    unsigned mode;
} core_os_file_info_t;

typedef struct core_os_dir_t core_os_dir_t;

core_os_lookup_t core_os_stat(core_string_t path, bool follow,
                              core_os_file_info_t *info);
core_os_dir_t *core_os_dir_open(core_string_t path, core_os_lookup_t *status);
bool core_os_dir_next(core_os_dir_t *dir, core_string_t *name,
                      core_os_kind_t *kind);
void core_os_dir_close(core_os_dir_t *dir);
bool core_os_read_file(core_string_t path, char **bytes, size_t *size);
bool core_os_write_all(int fd, const void *data, size_t size);
bool core_os_flush(int fd);
bool core_os_close(int fd);
bool core_os_remove_file(core_string_t path);
bool core_os_remove_directory(core_string_t path);
bool core_os_is_absolute(core_string_t path);
core_string_t core_os_root_relative(core_string_t absolute);
