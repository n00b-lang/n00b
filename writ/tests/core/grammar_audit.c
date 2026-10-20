#include <stdio.h>
#include <string.h>

#include "syntax/syntax.h"

int
main(int argc, char **argv)
{
    bool candidates = argc > 1 && !strcmp(argv[1], "--candidates");
    writ_session_t *session = writ_session_new(nullptr);
    writ_str_t report = writ_syntax_audit(session, candidates);
    fwrite(report.data, 1, report.length, stdout);
    writ_session_free(session);
    return !candidates && report.length != 0;
}
