#pragma once

/** @file lang_table.h — Source-codec language/comment metadata table.
 *
 *  Statically compiled set, ported verbatim from chalk's base config
 *  (`source_marks.extensions_to_languages_map` /
 *  `source_marks.language_to_comment_map`). */

#include <n00b.h>

typedef struct {
    n00b_string_t *ext;
    n00b_string_t *lang;
    n00b_string_t *comment_open;
    n00b_string_t *comment_close;
} n00b_chalk_lang_entry_t;

extern const n00b_chalk_lang_entry_t *n00b_chalk_lang_table;
extern const size_t                   n00b_chalk_lang_table_len;

/** Detect language given a file extension (with leading dot) or a
 *  shebang interpreter name (no path). Returns NULL when unknown. */
const n00b_chalk_lang_entry_t *
    n00b_chalk_lang_detect(n00b_string_t *ext, n00b_string_t *shebang_interp);
