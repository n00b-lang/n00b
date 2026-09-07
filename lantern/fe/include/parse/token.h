#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "lib/string.h"

#define NCC_TOK_START_ID   0x40000000

#define NCC_TOK_OTHER      (-3)

#define NCC_TOK_IGNORED    (-2)

#define NCC_TOK_EOF        (-1)

#define NCC_TOK_IDENTIFIER   (NCC_TOK_START_ID + 1)

#define NCC_TOK_TYPEDEF_NAME (NCC_TOK_START_ID + 2)

#define NCC_TOK_INTEGER      (NCC_TOK_START_ID + 3)

#define NCC_TOK_FLOAT        (NCC_TOK_START_ID + 4)

#define NCC_TOK_CHAR_LIT     (NCC_TOK_START_ID + 5)

#define NCC_TOK_STRING_LIT   (NCC_TOK_START_ID + 6)

typedef enum : uint8_t {
    NCC_TRIVIA_SPACE,
    NCC_TRIVIA_COMMENT,
} ncc_trivia_kind_t;

typedef struct ncc_trivia_t {
    ncc_string_t         text;
    struct ncc_trivia_t *next;
    ncc_trivia_kind_t    kind;
} ncc_trivia_t;

ncc_option_decl(ncc_string_t);

typedef struct ncc_token_info_t {
    void                          *user_info;
    ncc_option_t(ncc_string_t)   value;
    ncc_option_t(ncc_string_t)   file;
    ncc_trivia_t                 *leading_trivia;
    ncc_trivia_t                 *trailing_trivia;
    int32_t                        tid;
    int32_t                        index;
    uint32_t                       line;
    uint32_t                       column;
    uint32_t                       endcol;
    size_t                         start_byte;
    size_t                         end_byte;
    bool                           system_header;
    bool                           no_trivia;
} ncc_token_info_t;

ncc_list_decl(ncc_token_info_t);

typedef ncc_token_info_t *ncc_token_info_ptr_t;
ncc_array_decl(ncc_token_info_ptr_t);
ncc_list_decl(ncc_token_info_ptr_t);

extern int32_t ncc_token_list_build_ptrs(ncc_list_t(ncc_token_info_t) *tl,
                                           ncc_token_info_ptr_t **out);

typedef struct ncc_parser_t ncc_parser_t;

typedef int64_t (*ncc_tokenizer_fn)(ncc_parser_t *, void **);

void ncc_trivia_free(ncc_trivia_t *t);
