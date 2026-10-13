#include <stdio.h>

#include "internal.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

int
main(void)
{
    writ_session_t *session = writ_session_new(nullptr);
    writ_sym_t first = writ_intern(session, "compress2", 9);
    writ_sym_t again = writ_intern(session, "compress2", 9);
    writ_sym_t other = writ_intern(session, "uncompress", 10);
    expect(first && first == again, "the same text interns to the same symbol");
    expect(first != other, "different text interns to different symbols");
    expect(writ_str_eq(writ_sym_str(session, other), writ_cstr("uncompress")), "symbols round-trip");

    uint32_t source = writ_source_add(session, WRIT_SOURCE_CONTRACT_FILE, (writ_origin_t){0}, writ_cstr("t.writ"),
                                      writ_cstr("a\nbc\r\nd\re"));
    uint32_t offsets[] = {0, 2, 3, 6, 8};
    uint32_t lines[] = {1, 2, 2, 3, 4};
    uint32_t columns[] = {1, 1, 2, 1, 1};
    for (size_t i = 0; i < 5; i++) {
        writ_span_t span = writ_loc_span(session, (writ_loc_t){source, offsets[i], offsets[i] + 1});
        expect(span.line == lines[i] && span.column == columns[i], "line and column for mixed line endings");
    }
    writ_session_free(session);
    if (!failures) {
        printf("internals ok\n");
    }
    return failures != 0;
}
