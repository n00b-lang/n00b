#pragma once

/** @file lang_table.h — Source-codec language/comment metadata table.
 *
 *  Statically compiled set, ported verbatim from chalk's base config
 *  (`source_marks.extensions_to_languages_map` /
 *  `source_marks.language_to_comment_map`). */

#include <n00b.h>

/** Look up the language for a file extension or shebang interpreter
 *  name. `ext` is the byte sequence (no leading dot), `ext_len` its
 *  length. Returns NULL if unknown. The result is a static string. */
const char *n00b_chalk_lang_lookup_ext(const char *ext, size_t ext_len);

/** Look up the comment prefix for a language name. NUL-terminated
 *  static result. NULL if unknown. */
const char *n00b_chalk_lang_lookup_comment(const char *lang);
