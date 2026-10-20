#include "parse/ambiguity.h"

#include <stdio.h>

int
main(int argc, char **argv)
{
    return parse_ambiguity_cli(argc, argv, stdout, stderr);
}
