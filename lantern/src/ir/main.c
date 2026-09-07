#include "ir/cli.h"

int main(int argc, char **argv)
{
    return ir_cli(argc, argv, stdout, stderr);
}
