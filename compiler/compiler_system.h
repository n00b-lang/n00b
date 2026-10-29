#pragma once

#include "lib/string.h"

bool n00bb_system_module(core_string_t module);
bool n00bb_generated_source(core_string_t path);
bool n00bb_reserved_name(core_string_t name);
bool n00bb_builtin_member(core_string_t name);
