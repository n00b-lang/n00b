#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "lib/string.h"
#include "parse/source.h"

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
    PARSE_TRIVIA_REGION_OPEN,
    PARSE_TRIVIA_REGION_CLOSE,
} parse_trivia_kind_t;

typedef struct parse_trivia_region_link_t {
    uint64_t                           region_id;
    struct parse_trivia_region_link_t *next;
} parse_trivia_region_link_t;

typedef enum : uint8_t {
    PARSE_TOKEN_SOURCE,
    PARSE_TOKEN_SYNTHETIC,
} parse_token_origin_t;

typedef struct parse_trivia_t {
    core_string_t         text;
    core_string_t         region_name;
    struct parse_trivia_t *next;
    parse_trivia_region_link_t *regions;
    parse_trivia_kind_t    kind;
    uint32_t               line;
    uint32_t               column;
    uint32_t               endline;
    uint32_t               endcol;
    size_t                 start_byte;
    size_t                 end_byte;
    uint64_t               region_id;
    uint64_t               anchor_order;
    uint64_t               anchor_group;
    bool                   owns_text;
    bool                   owns_region_name;
} parse_trivia_t;

typedef struct parse_token_extras_t {
    void                         *user_info;
    core_option_t(core_string_t)  value;
    core_option_t(core_string_t)  literal_type;
    core_option_t(core_string_t)  postfix_modifier;
    core_option_t(core_string_t)  file;
    parse_trivia_t               *leading_trivia;
    parse_trivia_t               *trailing_trivia;
    size_t                        content_start_byte;
    size_t                        content_end_byte;
    size_t                        label_start_byte;
    size_t                        label_end_byte;
    uint32_t                      line;
    uint32_t                      column;
    uint32_t                      endline;
    uint32_t                      endcol;
    uint32_t                      layout_depth;
    uint32_t                      layout_column;
    bool                          owns_value;
    bool                          owns_literal_type;
    bool                          owns_postfix_modifier;
    bool                          owns_file;
    bool                          has_position;
    bool                          has_content_span;
    bool                          has_label_span;
    bool                          has_layout_info;
} parse_token_extras_t;

typedef struct {
    int32_t  tid;
    int32_t  index;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t value_offset;
    uint32_t source             : 24;
    uint32_t origin             : 1;
    uint32_t system_header      : 1;
    uint32_t no_trivia          : 1;
    uint32_t tree_owned         : 1;
    uint32_t layout_mismatch    : 1;
    uint32_t ignore_if_no_match : 1;
    uint32_t value_from_source  : 1;
    uint32_t holds_source       : 1;
} parse_token_fields_t;

typedef struct parse_token_info_t {
    parse_token_extras_t *extras;
    parse_token_fields_t  fields;
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
size_t parse_trivia_region_count(const parse_trivia_t *trivia);
uint64_t parse_trivia_region_at(const parse_trivia_t *trivia, size_t index);
bool parse_trivia_in_region(const parse_trivia_t *trivia, uint64_t region_id);
parse_token_info_t *parse_token_clone_for_tree(const parse_token_info_t *token);
void                parse_token_free_from_tree(parse_token_info_t *token);

parse_token_info_t *parse_token_new(void);
void parse_token_free(parse_token_info_t *token);
void parse_token_free_contents(parse_token_info_t *token);
parse_token_info_t *parse_token_copy(const parse_token_info_t *token,
                                     bool with_trivia);
parse_token_extras_t *parse_token_extras_for_update(parse_token_info_t *token);

static inline int32_t
parse_token_tid(const parse_token_info_t *token)
{
    return token->fields.tid;
}

static inline void
parse_token_set_tid(parse_token_info_t *token, int32_t tid)
{
    token->fields.tid = tid;
}

static inline int32_t
parse_token_index(const parse_token_info_t *token)
{
    return token->fields.index;
}

static inline void
parse_token_set_index(parse_token_info_t *token, int32_t index)
{
    token->fields.index = index;
}

static inline size_t
parse_token_start_byte(const parse_token_info_t *token)
{
    return token->fields.start_byte;
}

static inline size_t
parse_token_end_byte(const parse_token_info_t *token)
{
    return token->fields.end_byte;
}

void parse_token_set_span(parse_token_info_t *token, size_t start,
                          size_t end);

static inline uint32_t
parse_token_source(const parse_token_info_t *token)
{
    return token->fields.source;
}

void parse_token_set_source(parse_token_info_t *token, uint32_t source);

static inline void
parse_token_set_stream_source(parse_token_info_t *token, uint32_t source)
{
    token->fields.source = source;
}

static inline parse_token_origin_t
parse_token_origin(const parse_token_info_t *token)
{
    return (parse_token_origin_t)token->fields.origin;
}

static inline void
parse_token_set_origin(parse_token_info_t *token, parse_token_origin_t origin)
{
    token->fields.origin = origin;
}

static inline bool
parse_token_system_header(const parse_token_info_t *token)
{
    return token->fields.system_header;
}

static inline void
parse_token_set_system_header(parse_token_info_t *token, bool value)
{
    token->fields.system_header = value;
}

static inline bool
parse_token_no_trivia(const parse_token_info_t *token)
{
    return token->fields.no_trivia;
}

static inline void
parse_token_set_no_trivia(parse_token_info_t *token, bool value)
{
    token->fields.no_trivia = value;
}

static inline bool
parse_token_tree_owned(const parse_token_info_t *token)
{
    return token->fields.tree_owned;
}

static inline void
parse_token_set_tree_owned(parse_token_info_t *token, bool value)
{
    token->fields.tree_owned = value;
}

static inline bool
parse_token_layout_mismatch(const parse_token_info_t *token)
{
    return token->fields.layout_mismatch;
}

static inline void
parse_token_set_layout_mismatch(parse_token_info_t *token, bool value)
{
    token->fields.layout_mismatch = value;
}

static inline bool
parse_token_ignore_if_no_match(const parse_token_info_t *token)
{
    return token->fields.ignore_if_no_match;
}

static inline void
parse_token_set_ignore_if_no_match(parse_token_info_t *token, bool value)
{
    token->fields.ignore_if_no_match = value;
}

core_option_t(core_string_t) parse_token_value(const parse_token_info_t *token);

bool parse_token_has_value(const parse_token_info_t *token);

static inline bool
parse_token_value_is_source(const parse_token_info_t *token)
{
    return token->fields.value_from_source;
}

void parse_token_set_value(parse_token_info_t *token,
                           core_option_t(core_string_t) value, bool owned);

bool parse_token_use_source_value(parse_token_info_t *token);

core_option_t(core_string_t) parse_token_file(const parse_token_info_t *token);

void parse_token_set_file(parse_token_info_t *token,
                          core_option_t(core_string_t) file, bool owned);

uint32_t parse_token_line(const parse_token_info_t *token);
uint32_t parse_token_column(const parse_token_info_t *token);
uint32_t parse_token_endline(const parse_token_info_t *token);
uint32_t parse_token_endcol(const parse_token_info_t *token);

void parse_token_set_position(parse_token_info_t *token, uint32_t line,
                              uint32_t column, uint32_t endline,
                              uint32_t endcol);

void parse_token_copy_location(parse_token_info_t *token,
                               const parse_token_info_t *from);

static inline core_option_t(core_string_t)
parse_token_literal_type(const parse_token_info_t *token)
{
    return token->extras ? token->extras->literal_type
                         : core_option_none(core_string_t);
}

void parse_token_set_literal_type(parse_token_info_t *token,
                                  core_option_t(core_string_t) value,
                                  bool owned);

static inline core_option_t(core_string_t)
parse_token_postfix_modifier(const parse_token_info_t *token)
{
    return token->extras ? token->extras->postfix_modifier
                         : core_option_none(core_string_t);
}

void parse_token_set_postfix_modifier(parse_token_info_t *token,
                                      core_option_t(core_string_t) value,
                                      bool owned);

static inline void *
parse_token_user_info(const parse_token_info_t *token)
{
    return token->extras ? token->extras->user_info : nullptr;
}

void parse_token_set_user_info(parse_token_info_t *token, void *info);

static inline parse_trivia_t *
parse_token_leading_trivia(const parse_token_info_t *token)
{
    return token->extras ? token->extras->leading_trivia : nullptr;
}

static inline parse_trivia_t *
parse_token_trailing_trivia(const parse_token_info_t *token)
{
    return token->extras ? token->extras->trailing_trivia : nullptr;
}

void parse_token_set_leading_trivia(parse_token_info_t *token,
                                    parse_trivia_t *trivia);

void parse_token_set_trailing_trivia(parse_token_info_t *token,
                                     parse_trivia_t *trivia);

static inline bool
parse_token_content_span(const parse_token_info_t *token, size_t *start,
                         size_t *end)
{
    if (!token->extras || !token->extras->has_content_span) return false;
    if (start) *start = token->extras->content_start_byte;
    if (end) *end = token->extras->content_end_byte;
    return true;
}

void parse_token_set_content_span(parse_token_info_t *token, size_t start,
                                  size_t end);

void parse_token_clear_content_span(parse_token_info_t *token);

static inline bool
parse_token_label_span(const parse_token_info_t *token, size_t *start,
                       size_t *end)
{
    if (!token->extras || !token->extras->has_label_span) return false;
    if (start) *start = token->extras->label_start_byte;
    if (end) *end = token->extras->label_end_byte;
    return true;
}

void parse_token_set_label_span(parse_token_info_t *token, size_t start,
                                size_t end);

void parse_token_clear_label_span(parse_token_info_t *token);

static inline bool
parse_token_layout(const parse_token_info_t *token, uint32_t *depth,
                   uint32_t *column)
{
    if (!token->extras || !token->extras->has_layout_info) return false;
    if (depth) *depth = token->extras->layout_depth;
    if (column) *column = token->extras->layout_column;
    return true;
}

void parse_token_set_layout(parse_token_info_t *token, uint32_t depth,
                            uint32_t column);
