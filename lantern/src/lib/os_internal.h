#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lib/string.h"

wchar_t *core_os_wide(core_string_t text);
core_string_t core_os_utf8(const wchar_t *wide, int units);
core_string_t core_os_utf8_path(const wchar_t *wide, int units);
int core_os_errno(DWORD error);
#endif
