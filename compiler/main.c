#include "lib/alloc.h"
#include "lib/string.h"

int n00bb_compiler_main(int argc, char **argv, core_string_t argv0);

int
main(int argc, char **argv)
{
    core_string_t argv0 = core_string_from_cstr(argc > 0 ? argv[0] : nullptr);
    int status = n00bb_compiler_main(argc, argv, argv0);
    core_free(argv0.data);
    return status;
}
