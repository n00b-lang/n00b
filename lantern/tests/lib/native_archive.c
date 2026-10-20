#include "lib/native_archive.h"
#include "lib/string.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    core_string_t names[4];
    core_string_t contents[4];
    size_t counts;
} visit_state_t;

static bool
visit(const core_native_archive_member_t *member, void *user)
{
    visit_state_t *state = user;
    size_t index = state->counts++;
    assert(index < 4);
    assert(core_string_eq(state->names[index],
        core_string_borrow((const char *)member->name, member->name_length)));
    assert(core_string_eq(state->contents[index],
        core_string_borrow((const char *)member->bytes, member->size)));
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
append(uint8_t *archive, size_t offset, core_string_t name,
       const char *body, size_t body_size)
{
    char header[61];
    assert(name.u8_bytes <= 16);
    memset(header, ' ', 16);
    memcpy(header, name.data, name.u8_bytes);
    int written = snprintf(header + 16, sizeof header - 16,
        "%-12d%-6d%-6d%-8d%-10zu`\n", 0, 0, 0, 0, body_size);
    assert(written == 44);
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
    size_t size = append(archive, 8, S("short.o/"), "abc", 3);
    size = append(archive, size, S("#1/16"), "bsd-name.o\0\0\0\0\0\0xyz", 19);
    size = append(archive, size, S("//"), "gnu-long-name.o/\n", 17);
    size = append(archive, size, S("/0"), "z", 1);
    visit_state_t state = {
        .names = {S("short.o"), S("bsd-name.o"), S("//"),
                  S("gnu-long-name.o")},
        .contents = {S("abc"), S("xyz"), S("gnu-long-name.o/\n"), S("z")},
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
