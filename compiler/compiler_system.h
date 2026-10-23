#pragma once

#include "lib/string.h"
#include "th/environment.h"

bool n00bb_system_module(core_string_t module);
bool n00bb_generated_source(core_string_t path);
bool n00bb_reserved_name(core_string_t name);
bool n00bb_builtin_member(core_string_t name);
bool n00bb_language_hook(const th_env *environment, core_string_t name);
core_string_t n00bb_nearest_language_hook(const th_env *environment,
                                          core_string_t name);
