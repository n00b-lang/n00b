#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "internal.h"

typedef struct {
    writ_session_t *session;
    writ_str_t      directory;
} directory_output_t;

static writ_status_t
make_parents(char *path)
{
    for (char *cursor = path + 1; *cursor; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = 0;
        int made = mkdir(path, 0777);
        *cursor = '/';
        if (made && errno != EEXIST) {
            return WRIT_IO_FAILED;
        }
    }
    return WRIT_OK;
}

static writ_status_t
write_into_directory(void *context, writ_str_t relative_path, writ_str_t contents)
{
    directory_output_t *output = context;
    writ_str_t path = writ_str_format(output->session, "%.*s/%.*s", (int)output->directory.length,
                                      output->directory.data, (int)relative_path.length, relative_path.data);
    if (make_parents((char *)path.data) != WRIT_OK) {
        return WRIT_IO_FAILED;
    }
    FILE *file = fopen(path.data, "wb");
    if (!file) {
        return WRIT_IO_FAILED;
    }
    size_t written = fwrite(contents.data, 1, contents.length, file);
    return fclose(file) == 0 && written == contents.length ? WRIT_OK : WRIT_IO_FAILED;
}

writ_status_t
writ_output_directory(writ_session_t *session, writ_str_t directory, writ_output_t *output)
{
    if (!session || !output) {
        return WRIT_INVALID_ARGUMENT;
    }
    directory_output_t *state = writ_alloc(session, sizeof(directory_output_t));
    *state = (directory_output_t){session, writ_str_copy(session, directory.data, directory.length)};
    *output = (writ_output_t){write_into_directory, state};
    return WRIT_OK;
}
