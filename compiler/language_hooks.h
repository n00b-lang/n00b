#pragma once

#include "lib/string.h"
#include "th/environment.h"

bool n00bb_language_hook(const th_env *environment, core_string_t name);
core_string_t n00bb_nearest_language_hook(const th_env *environment,
                                          core_string_t name);
