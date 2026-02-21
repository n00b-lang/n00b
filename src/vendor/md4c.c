/*
 * @file md4c.c
 * @brief Compilation unit for the md4c vendor library.
 *
 * md4c is a single-header library (~10K lines). Compiling it through
 * ncc's packrat parser is extremely expensive, so it's built with
 * `--no-ncc` as a separate static library.
 */
#define _MD4C_INLINE_IN_THIS_MODULE
#include "vendor/md4c.h"
