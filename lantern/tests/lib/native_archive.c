#include "lib/native_archive.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *names[4];
    const char *contents[4];
    size_t counts;
} visit_state_t;

static bool
visit(const core_native_archive_member_t *member, void *user)
{
    visit_state_t *state = user;
    size_t index = state->counts++;
    assert(index < 4);
    assert(strlen(state->names[index]) == member->name_length);
    assert(!memcmp(state->names[index], member->name, member->name_length));
    assert(strlen(state->contents[index]) == member->size);
    assert(!memcmp(state->contents[index], member->bytes, member->size));
    return true;
}

static bool
stop(const core_native_archive_member_t *member, void *user)
{
    (void)member;
    (void)user;
    return false;
}

static bool
count_objects(const core_native_archive_member_t *member, void *user)
{
    size_t *count = user;
    if (member->name_length >= 2
        && !memcmp(member->name + member->name_length - 2, ".o", 2)) {
        assert(member->size);
        (*count)++;
    }
    return true;
}

static size_t
append(uint8_t *archive, size_t offset, const char *name,
       const char *body, size_t body_size)
{
    char header[61];
    int written = snprintf(header, sizeof header,
        "%-16s%-12d%-6d%-6d%-8d%-10zu`\n", name, 0, 0, 0, 0,
        body_size);
    assert(written == 60);
    memcpy(archive + offset, header, 60);
    offset += 60;
    memcpy(archive + offset, body, body_size);
    offset += body_size;
    if (body_size & 1) archive[offset++] = '\n';
    return offset;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    uint8_t archive[512] = {0};
    memcpy(archive, "!<arch>\n", 8);
    size_t size = append(archive, 8, "short.o/", "abc", 3);
    size = append(archive, size, "#1/16", "bsd-name.o\0\0\0\0\0\0xyz", 19);
    size = append(archive, size, "//", "gnu-long-name.o/\n", 17);
    size = append(archive, size, "/0", "z", 1);
    visit_state_t state = {
        .names = {"short.o", "bsd-name.o", "//", "gnu-long-name.o"},
        .contents = {"abc", "xyz", "gnu-long-name.o/\n", "z"},
    };
    assert(core_native_archive_visit(archive, size, visit, &state));
    assert(state.counts == 4);
    assert(!core_native_archive_visit(archive, size, stop, nullptr));
    assert(core_native_archive_visit(archive, 8, visit, &state));
    state.counts = 0;
    assert(!core_native_archive_visit(archive, size - 1, visit, &state));
    assert(!core_native_archive_visit("!<thin>\n", 8, visit, &state));
    archive[8 + 58] = '!';
    assert(!core_native_archive_visit(archive, size, visit, &state));

    FILE *file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long file_size = ftell(file);
    assert(file_size > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *real_archive = malloc((size_t)file_size);
    assert(real_archive && fread(real_archive, 1, (size_t)file_size, file)
        == (size_t)file_size && !fclose(file));
    size_t object_count = 0;
    assert(core_native_archive_visit(real_archive, (size_t)file_size,
                                     count_objects, &object_count));
    assert(object_count);
    free(real_archive);
    return 0;
}
