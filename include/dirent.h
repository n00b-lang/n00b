#pragma once

#ifdef _WIN32
#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    intptr_t           handle;
    struct _finddata_t data;
    int                first;
    struct dirent      *entry;
} DIR;

struct dirent {
    char d_name[260];
};

static inline DIR *
opendir(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    size_t len     = strlen(path);
    int    has_sep = len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\');
    char  *pattern = malloc(len + (has_sep ? 2 : 3));

    if (!pattern) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(pattern, path, len);
    size_t ix = len;
    if (!has_sep) {
        pattern[ix++] = '/';
    }
    pattern[ix++] = '*';
    pattern[ix]   = '\0';

    DIR *dir = malloc(sizeof(*dir));
    if (!dir) {
        free(pattern);
        errno = ENOMEM;
        return NULL;
    }

    dir->entry = malloc(sizeof(*dir->entry));
    if (!dir->entry) {
        free(pattern);
        free(dir);
        errno = ENOMEM;
        return NULL;
    }

    dir->handle = _findfirst(pattern, &dir->data);
    free(pattern);

    if (dir->handle == -1) {
        free(dir->entry);
        free(dir);
        return NULL;
    }

    dir->first = 1;
    return dir;
}

static inline struct dirent *
readdir(DIR *dir)
{
    if (!dir) {
        errno = EINVAL;
        return NULL;
    }

    if (!dir->first && _findnext(dir->handle, &dir->data) != 0) {
        return NULL;
    }
    dir->first = 0;

    size_t name_len = strlen(dir->data.name);
    if (name_len >= sizeof(dir->entry->d_name)) {
        name_len = sizeof(dir->entry->d_name) - 1;
    }
    memcpy(dir->entry->d_name, dir->data.name, name_len);
    dir->entry->d_name[name_len] = '\0';
    return dir->entry;
}

static inline int
closedir(DIR *dir)
{
    if (!dir) {
        errno = EINVAL;
        return -1;
    }

    int result = _findclose(dir->handle);
    free(dir->entry);
    free(dir);
    return result;
}

#else
#include_next <dirent.h>
#endif
