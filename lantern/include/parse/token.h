#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "lib/string.h"

#define PARSE_TOK_START_ID   0x40000000

#define PARSE_TOK_OTHER      (-3)

#define PARSE_TOK_IGNORED    (-2)

#define PARSE_TOK_EOF        (-1)

#define PARSE_TOK_IDENTIFIER   (PARSE_TOK_START_ID + 1)

#define PARSE_TOK_TYPEDEF_NAME (PARSE_TOK_START_ID + 2)

#define PARSE_TOK_INTEGER      (PARSE_TOK_START_ID + 3)

#define PARSE_TOK_FLOAT        (PARSE_TOK_START_ID + 4)

#define PARSE_TOK_CHAR_LIT     (PARSE_TOK_START_ID + 5)

#define PARSE_TOK_STRING_LIT   (PARSE_TOK_START_ID + 6)

typedef enum : uint8_t {
    PARSE_TRIVIA_SPACE,
    PARSE_TRIVIA_COMMENT,
} parse_trivia_kind_t;

typedef enum : uint8_t {
    PARSE_TOKEN_SOURCE,
    PARSE_TOKEN_SYNTHETIC,
} parse_token_origin_t;

typedef struct parse_trivia_t {
    core_string_t         text;
    struct parse_trivia_t *next;
    parse_trivia_kind_t    kind;
    bool                   owns_text;
} parse_trivia_t;

core_option_decl(core_string_t);

typedef struct parse_token_info_t {
    void                          *user_info;
    core_option_t(core_string_t)   value;
    core_option_t(core_string_t)   literal_type;
    core_option_t(core_string_t)   file;
    parse_trivia_t                 *leading_trivia;
    parse_trivia_t                 *trailing_trivia;
    int32_t                        tid;
    int32_t                        index;
    uint32_t                       line;
    uint32_t                       column;
    uint32_t                       endline;
    uint32_t                       endcol;
    size_t                         start_byte;
    size_t                         end_byte;
    size_t                         content_start_byte;
    size_t                         content_end_byte;
    size_t                         label_start_byte;
    size_t                         label_end_byte;
    uint32_t                       layout_depth;
    uint32_t                       layout_column;
    parse_token_origin_t           origin;
    bool                           system_header;
    bool                           no_trivia;
    bool                           owns_value;
    bool                           owns_literal_type;
    bool                           owns_file;
    bool                           tree_owned;
    bool                           has_content_span;
    bool                           has_label_span;
    bool                           has_layout_info;
    bool                           layout_mismatch;
    bool                           ignore_if_no_match;
} parse_token_info_t;

core_list_decl(parse_token_info_t);

typedef parse_token_info_t *parse_token_info_ptr_t;
core_array_decl(parse_token_info_ptr_t);
core_list_decl(parse_token_info_ptr_t);

extern int32_t parse_token_list_build_ptrs(core_list_t(parse_token_info_t) *tl,
                                           parse_token_info_ptr_t **out);

typedef struct parse_parser_t parse_parser_t;

typedef int64_t (*parse_tokenizer_fn)(parse_parser_t *, void **);

void parse_trivia_free(parse_trivia_t *t);
parse_token_info_t *parse_token_clone_for_tree(const parse_token_info_t *token);
void                parse_token_free_from_tree(parse_token_info_t *token);
