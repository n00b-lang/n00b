#pragma once

#include "parse/commander.h"

parse_cmdr_t *n00bb_make_cli(void);
bool n00bb_is_help_request(int argc, char **argv);
