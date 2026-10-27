
#include "parse/bnf.h"
#include "lib/list.h"
#include "lib/os.h"
#include "parse/annot_args.h"
#include "parse/annotation.h"
#include "parse/pwz.h"
#include "private/parse/annot_grammar.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_decl.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t utf8_decode(const char **pp, const char *end) {
  const unsigned char *p = (const unsigned char *)*pp;
  size_t available = (size_t)(end - *pp);
  size_t needed = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
  int32_t cp;

  if (needed > available) {
    return -1;
  }

  if (*p < 0x80) {
    cp = *p++;
  } else if (*p < 0xE0) {
    cp = (*p++ & 0x1F) << 6;
    cp |= *p++ & 0x3F;
  } else if (*p < 0xF0) {
    cp = (*p++ & 0x0F) << 12;
    cp |= (*p++ & 0x3F) << 6;
    cp |= *p++ & 0x3F;
  } else {
    cp = (*p++ & 0x07) << 18;
    cp |= (*p++ & 0x3F) << 12;
    cp |= (*p++ & 0x3F) << 6;
    cp |= *p++ & 0x3F;
  }

  *pp = (const char *)p;
  return cp;
}

core_string_t parse_bnf_strip_comments(core_string_t input) {
  if (!input.data) {
    return core_string_empty();
  }

  size_t len = input.u8_bytes;
  const char *src = input.data;
  char *result = core_alloc_array(char, len + 1);
  size_t ri = 0;
  bool in_sq = false;
  bool in_dq = false;

  for (size_t i = 0; i < len; i++) {
    char c = src[i];

    if (c == '\'' && !in_dq) {
      in_sq = !in_sq;
    } else if (c == '"' && !in_sq) {
      in_dq = !in_dq;
    }

    if (c == '#' && !in_sq && !in_dq) {
      while (i < len && src[i] != '\n') {
        i++;
      }

      if (i < len) {
        result[ri++] = '\n';
      }

      continue;
    }

    result[ri++] = c;
  }

  result[ri] = '\0';

  return core_string_borrow(result, ri);
}

core_string_t parse_bnf_trim_lines(core_string_t input) {
  if (!input.data) {
    return core_string_empty();
  }

  size_t len = input.u8_bytes;
  const char *src = input.data;
  const char *end = src + len;
  char *result = core_alloc_array(char, len + 1);
  size_t ri = 0;
  const char *p = src;

  while (p < end) {
    while (p < end && *p != '\n' && (*p == ' ' || *p == '\t')) {
      p++;
    }

    const char *line_start = p;

    while (p < end && *p != '\n') {
      p++;
    }

    const char *line_end = p;

    while (
        line_end > line_start &&
        (line_end[-1] == ' ' || line_end[-1] == '\t' || line_end[-1] == '\r')) {
      line_end--;
    }

    size_t line_len = (size_t)(line_end - line_start);
    memcpy(result + ri, line_start, line_len);
    ri += line_len;

    if (p < end && *p == '\n') {
      result[ri++] = '\n';
      p++;
    }
  }

  result[ri] = '\0';

  return core_string_borrow(result, ri);
}

static core_string_t bnf_join_continuations(core_string_t input) {
  if (!input.data) {
    return core_string_empty();
  }

  size_t len = input.u8_bytes;
  const char *src = input.data;
  char *result = core_alloc_array(char, len + 2);
  size_t ri = 0;
  bool in_sq = false;
  bool in_dq = false;
  bool escape = false;
  bool annotation = false;
  size_t annotation_depth = 0;

  for (size_t i = 0; i < len; i++) {
    char c = src[i];

    if (escape) {
      escape = false;
    } else if ((in_sq || in_dq) && c == '\\') {
      escape = true;
    } else if (!in_dq && c == '\'') {
      in_sq = !in_sq;
    } else if (!in_sq && c == '"') {
      in_dq = !in_dq;
    } else if (!in_sq && !in_dq) {
      if (c == '@' && !annotation_depth) {
        annotation = true;
      } else if (annotation && c == '(') {
        annotation_depth++;
      } else if (annotation_depth && c == ')') {
        annotation_depth--;

        if (!annotation_depth) {
          annotation = false;
        }
      }
    }

    if (src[i] == '\n') {
      size_t j = i + 1;

      while (j < len && (src[j] == ' ' || src[j] == '\t')) {
        j++;
      }

      bool alt_next = j < len && src[j] == '|';
      bool assign_last = ri >= 3 && result[ri - 3] == ':' &&
                         result[ri - 2] == ':' && result[ri - 1] == '=';

      if ((annotation_depth && !in_sq && !in_dq) || alt_next ||
          (assign_last && j < len)) {
        result[ri++] = ' ';
        i = j - 1;
        continue;
      }

      if (!annotation_depth) {
        annotation = false;
      }
    }

    result[ri++] = src[i];
  }

  if (ri > 0 && result[ri - 1] != '\n') {
    result[ri++] = '\n';
  }
  result[ri] = '\0';

  return core_string_borrow(result, ri);
}

enum {
  BNF_TOK_LANGLE = PARSE_TOK_START_ID + 1,
  BNF_TOK_RANGLE = PARSE_TOK_START_ID + 2,
  BNF_TOK_ASSIGN = PARSE_TOK_START_ID + 3,
  BNF_TOK_PIPE = PARSE_TOK_START_ID + 4,
  BNF_TOK_NEWLINE = PARSE_TOK_START_ID + 5,
  BNF_TOK_NAME = PARSE_TOK_START_ID + 6,
  BNF_TOK_LITERAL = PARSE_TOK_START_ID + 7,
  BNF_TOK_CLASS = PARSE_TOK_START_ID + 8,
  BNF_TOK_TOKEN_TYPE = PARSE_TOK_START_ID + 9,
  BNF_TOK_TOKEN_LIT = PARSE_TOK_START_ID + 10,
  BNF_TOK_EMPTY_LIT = PARSE_TOK_START_ID + 11,
  BNF_TOK_QUESTION = PARSE_TOK_START_ID + 12,
  BNF_TOK_STAR = PARSE_TOK_START_ID + 13,
  BNF_TOK_PLUS_OP = PARSE_TOK_START_ID + 14,
  BNF_TOK_LPAREN = PARSE_TOK_START_ID + 15,
  BNF_TOK_RPAREN = PARSE_TOK_START_ID + 16,
  BNF_TOK_AT = PARSE_TOK_START_ID + 17,
  BNF_TOK_DOLLAR = PARSE_TOK_START_ID + 18,
  BNF_TOK_COMMA = PARSE_TOK_START_ID + 19,
  BNF_TOK_INT = PARSE_TOK_START_ID + 20,
};

typedef struct {
  size_t len;
  char data[1024];
} bnf_long_text_t;

typedef struct {
  size_t len;
  char data[512];
} bnf_list_text_t;

typedef struct {
  size_t len;
  char data[192];
} bnf_short_text_t;

[[gnu::format(printf, 5, 6)]]
static void bnf_text_format(char *data, size_t capacity, size_t *len,
                            size_t at, const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  int n = vsnprintf(data + at, capacity - at, fmt, args);
  va_end(args);

  size_t total = at + (n > 0 ? (size_t)n : 0);

  *len = total < capacity ? total : capacity - 1;
}

#define BNF_TEXT_SET(text, ...)                                                \
  bnf_text_format((text).data, sizeof((text).data), &(text).len, 0,          \
                  __VA_ARGS__)

#define BNF_TEXT_APPEND(text, ...)                                             \
  bnf_text_format((text).data, sizeof((text).data), &(text).len, (text).len, \
                  __VA_ARGS__)

#define BNF_TEXT_STRING(text) core_string_borrow((text).data, (text).len)

#define BNF_TEXT_ARG(text) (int)(text).len, (text).data

static _Thread_local bnf_short_text_t bnf_empty_lit;

static bool is_bnf_hws(core_codepoint_t cp, void *ctx) {
  (void)ctx;
  return cp == ' ' || cp == '\t';
}

static bool bnf_scan(parse_scanner_t *s) {
  bool *in_angle = (bool *)s->user_state;

  scan_skip_while(s, is_bnf_hws);

  if (scan_at_eof(s)) {
    return false;
  }

  core_codepoint_t ch = scan_peek(s, 0);

  if (ch == '\r' || ch == '\n') {
    scan_mark(s);

    if (ch == '\r' && scan_peek_byte(s, 1) == '\n') {
      scan_advance(s);
    }

    scan_advance(s);
    scan_emit_marked(s, BNF_TOK_NEWLINE);
    return true;
  }

  if (ch == ':') {
    scan_mark(s);

    if (scan_match_str(s, CORE_STRING_STATIC("::="))) {
      scan_emit_marked(s, BNF_TOK_ASSIGN);
      return true;
    }

    scan_advance(s);
    return true;
  }

  if (ch == '<') {
    scan_mark(s);
    scan_advance(s);
    scan_emit_marked(s, BNF_TOK_LANGLE);
    *in_angle = true;
    return true;
  }

  if (ch == '>') {
    scan_mark(s);
    scan_advance(s);
    scan_emit_marked(s, BNF_TOK_RANGLE);
    *in_angle = false;
    return true;
  }

  if (ch == '|') {
    scan_mark(s);
    scan_advance(s);
    scan_emit_marked(s, BNF_TOK_PIPE);
    return true;
  }

  if (ch == '%') {
    scan_advance(s);

    core_codepoint_t next = scan_peek(s, 0);

    if (next == '"' || next == '\'') {
      core_codepoint_t quote = next;
      scan_advance(s);
      scan_mark(s);

      while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (c == quote || c == '\n') {
          break;
        }

        scan_advance(s);
      }

      core_string_t val = scan_extract(s);

      if (!scan_at_eof(s) && scan_peek(s, 0) == quote) {
        scan_advance(s);
      }

      scan_emit_owned(s, BNF_TOK_TOKEN_LIT,
                      core_option_set(core_string_t, val));
      return true;
    }

    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        next == '_') {
      scan_mark(s);

      while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
          break;
        }

        scan_advance(s);
      }

      core_string_t val = scan_extract(s);
      scan_emit_owned(s, BNF_TOK_TOKEN_TYPE,
                      core_option_set(core_string_t, val));
      return true;
    }

    return true;
  }

  if (ch == '"' || ch == '\'') {
    core_codepoint_t quote = ch;
    scan_advance(s);
    scan_mark(s);

    while (!scan_at_eof(s)) {
      core_codepoint_t c = scan_peek(s, 0);

      if (c == quote || c == '\n') {
        break;
      }

      scan_advance(s);
    }

    core_string_t val = scan_extract(s);

    if (!scan_at_eof(s) && scan_peek(s, 0) == quote) {
      scan_advance(s);
    }

    if (val.u8_bytes == 0) {
      size_t at = bnf_empty_lit.len;

      if (at < sizeof(bnf_empty_lit.data) - 24) {
        BNF_TEXT_APPEND(bnf_empty_lit, "%.*s%u:%u", at ? 2 : 0, ", ",
                        s->mark_line, s->mark_col > 1 ? s->mark_col - 1 : 1);
      }
    }

    int32_t tid = (val.u8_bytes == 0) ? BNF_TOK_EMPTY_LIT : BNF_TOK_LITERAL;
    scan_emit_owned(s, tid, core_option_set(core_string_t, val));
    return true;
  }

  if (ch == '_' && scan_peek_byte(s, 1) == '_') {
    uint8_t third = scan_peek_byte(s, 2);

    if ((third >= 'a' && third <= 'z') || (third >= 'A' && third <= 'Z')) {
      scan_mark(s);
      scan_advance(s);
      scan_advance(s);

      while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
          break;
        }

        scan_advance(s);
      }

      core_string_t val = scan_extract(s);
      scan_emit_owned(s, BNF_TOK_CLASS, core_option_set(core_string_t, val));
      return true;
    }
  }

  if (ch >= '0' && ch <= '9') {
    scan_mark(s);

    while (!scan_at_eof(s)) {
      core_codepoint_t c = scan_peek(s, 0);

      if (c < '0' || c > '9') {
        break;
      }

      scan_advance(s);
    }

    core_string_t val = scan_extract(s);

    scan_emit_owned(s, BNF_TOK_INT, core_option_set(core_string_t, val));
    return true;
  }

  if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
    scan_mark(s);

    while (!scan_at_eof(s)) {
      core_codepoint_t c = scan_peek(s, 0);

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            (*in_angle && c == ','))) {
        break;
      }

      scan_advance(s);
    }

    core_string_t val = scan_extract(s);
    scan_emit_owned(s, BNF_TOK_NAME, core_option_set(core_string_t, val));
    return true;
  }

  scan_mark(s);
  scan_advance(s);

  switch (ch) {
  case '?':
    scan_emit_marked(s, BNF_TOK_QUESTION);
    return true;
  case '*':
    scan_emit_marked(s, BNF_TOK_STAR);
    return true;
  case '+':
    scan_emit_marked(s, BNF_TOK_PLUS_OP);
    return true;
  case '(':
    scan_emit_marked(s, BNF_TOK_LPAREN);
    return true;
  case ')':
    scan_emit_marked(s, BNF_TOK_RPAREN);
    return true;
  case '@':
    scan_emit_marked(s, BNF_TOK_AT);
    return true;
  case ',':
    scan_emit_marked(s, BNF_TOK_COMMA);
    return true;
  default:
    break;
  }

  if (ch == '$') {
    core_codepoint_t d = scan_peek(s, 0);

    if (d >= '0' && d <= '9') {
      scan_mark(s);

      while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (c < '0' || c > '9') {
          break;
        }

        scan_advance(s);
      }

      core_string_t val = scan_extract(s);
      scan_emit_owned(s, BNF_TOK_DOLLAR, core_option_set(core_string_t, val));
      return true;
    }

    scan_emit_marked(s, BNF_TOK_DOLLAR);
    return true;
  }

  return true;
}

static core_string_t tok_str(parse_token_info_t *tok) {
  if (!tok) {
    return (core_string_t){0};
  }

  core_option_t(core_string_t) value = parse_token_value(tok);

  if (!core_option_is_set(value)) {
    return (core_string_t){0};
  }

  return core_option_get(value);
}

static core_string_t tok_str_or(parse_token_info_t *tok,
                                core_string_t fallback) {
  core_string_t s = tok_str(tok);

  return s.data ? s : fallback;
}

static inline bool str_eq_lit(core_string_t s, core_string_t lit) {
  return core_string_eq(s, lit);
}

typedef struct {
  int tag;
  void *data;
} bnf_result_t;

enum {
  BNF_STRING = 1,
  BNF_LIST = 2,
  BNF_PAIR = 3,
  BNF_DICT = 4,
  BNF_GROUP = 5,
  BNF_ANNOT = 6,
  BNF_NAME = 7,
};

typedef void *bnf_ptr_t;
core_list_decl(bnf_ptr_t);
typedef core_list_t(bnf_ptr_t) bnf_list_t;

typedef struct {
  bnf_list_t *alternatives;
  char quantifier;
} bnf_group_info_t;

static inline bnf_list_t *slist_new(void) {
  bnf_list_t *l = core_alloc(bnf_list_t);
  *l = core_list_new(bnf_ptr_t);
  return l;
}

static inline void slist_push(bnf_list_t *l, void *item) {
  core_list_push(*l, (bnf_ptr_t)item);
}

static inline void *slist_get(bnf_list_t *l, size_t i) {
  return (l && i < l->len) ? l->data[i] : nullptr;
}

static inline void slist_prepend(bnf_list_t *l, void *item) {
  core_list_push_front(*l, (bnf_ptr_t)item);
}

static inline void slist_free(bnf_list_t *l) {
  if (l) {
    core_list_free(*l);
    core_free(l);
  }
}

static inline bnf_result_t *bnf_result(int tag, void *data) {
  bnf_result_t *r = core_alloc(bnf_result_t);
  r->tag = tag;
  r->data = data;
  return r;
}

typedef struct {
  char kind;
  core_string_t value;
} bnf_term_t;

static bnf_result_t *bnf_term(char kind, core_string_t value) {
  bnf_term_t *term = core_alloc(bnf_term_t);
  term->kind = kind;
  term->value = core_string_dup(value);
  return bnf_result(BNF_STRING, term);
}

static void bnf_term_free(bnf_term_t *term) {
  if (term) {
    core_free(term->value.data);
    core_free(term);
  }
}

static void *bnf_walk_atom(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_term('L', CORE_STRING_STATIC(""));
  }

  static const char kinds[] = {'L', 'N', 'C', 'T', 'K'};
  bnf_result_t *result;

  if (pn->rule_index >= 0 && pn->rule_index <= 4) {
    parse_token_info_t *tok =
        (parse_token_info_t *)kids[pn->rule_index == 1 ? 1 : 0];
    result = bnf_term(kinds[pn->rule_index],
                      tok_str_or(tok, CORE_STRING_STATIC("")));
  } else {
    result = bnf_term('E', CORE_STRING_STATIC(""));
  }

  core_free(kids);
  return result;
}

static void *bnf_walk_item(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_term('E', CORE_STRING_STATIC(""));
  }

  bnf_result_t *result;

  switch (pn->rule_index) {
  case 0:
    result = (bnf_result_t *)kids[0];
    core_free(kids);
    return result;

  case 1:
  case 2:
  case 3: {
    char quantifier = (pn->rule_index == 1)   ? '?'
                      : (pn->rule_index == 2) ? '*'
                                              : '+';

    bnf_group_info_t *gi = core_alloc(bnf_group_info_t);
    gi->quantifier = quantifier;
    gi->alternatives = slist_new();

    bnf_list_t *terms = slist_new();
    slist_push(terms, kids[0]);
    slist_push(gi->alternatives, bnf_result(BNF_LIST, terms));

    core_free(kids);
    return bnf_result(BNF_GROUP, gi);
  }

  case 4: {
    bnf_result_t *expr_r = (bnf_result_t *)kids[1];

    bnf_group_info_t *gi = core_alloc(bnf_group_info_t);
    gi->quantifier = 0;
    gi->alternatives = (bnf_list_t *)expr_r->data;

    core_free(expr_r);
    core_free(kids);
    return bnf_result(BNF_GROUP, gi);
  }

  case 5:
  case 6:
  case 7: {
    char quantifier = (pn->rule_index == 5)   ? '?'
                      : (pn->rule_index == 6) ? '*'
                                              : '+';

    bnf_result_t *expr_r = (bnf_result_t *)kids[1];

    bnf_group_info_t *gi = core_alloc(bnf_group_info_t);
    gi->quantifier = quantifier;
    gi->alternatives = (bnf_list_t *)expr_r->data;

    core_free(expr_r);
    core_free(kids);
    return bnf_result(BNF_GROUP, gi);
  }

  default:
    core_free(kids);
    return bnf_term('E', CORE_STRING_STATIC(""));
  }
}

static void *bnf_walk_list(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_result(BNF_LIST, slist_new());
  }

  bnf_list_t *result;

  if (pn->rule_index == 0) {
    result = slist_new();
    slist_push(result, kids[0]);
  } else {
    bnf_result_t *list_r = (bnf_result_t *)kids[1];
    result = list_r ? (bnf_list_t *)list_r->data : slist_new();
    slist_prepend(result, kids[0]);

    if (list_r) {
      core_free(list_r);
    }
  }

  core_free(kids);
  return bnf_result(BNF_LIST, result);
}

static void *bnf_walk_expression(parse_nt_node_t *pn, void *children,
                                 void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_result(BNF_LIST, slist_new());
  }

  bnf_list_t *result;

  if (pn->rule_index == 0) {
    result = slist_new();
    slist_push(result, kids[0]);
  } else {
    bnf_result_t *expr_r = (bnf_result_t *)kids[2];
    result = expr_r ? (bnf_list_t *)expr_r->data : slist_new();
    slist_prepend(result, kids[0]);

    if (expr_r) {
      core_free(expr_r);
    }
  }

  core_free(kids);
  return bnf_result(BNF_LIST, result);
}

static const int64_t bnf_arg_excluded[] = {
    BNF_TOK_LPAREN,
    BNF_TOK_RPAREN,
    BNF_TOK_NEWLINE,
};

static const parse_match_set_t bnf_arg_set = {
    .ids = bnf_arg_excluded,
    .n = sizeof bnf_arg_excluded / sizeof bnf_arg_excluded[0],
    .negate = true,
};

static _Thread_local core_string_t bnf_src;
static _Thread_local bnf_short_text_t bnf_bad_annot;
static _Thread_local bnf_long_text_t bnf_error;
static _Thread_local parse_bnf_error_info_t bnf_error_location;

core_string_t parse_bnf_error(void) {
  return bnf_error.len ? BNF_TEXT_STRING(bnf_error) : (core_string_t){};
}

bool parse_bnf_error_info(parse_bnf_error_info_t *out) {
  if (!out || !bnf_error.len) {
    return false;
  }

  *out = bnf_error_location;
  out->message = BNF_TEXT_STRING(bnf_error);

  return true;
}

static void bnf_error_at_token(parse_token_info_t *tok) {
  bnf_error_location = (parse_bnf_error_info_t){0};

  if (!tok) {
    return;
  }

  uint32_t line = parse_token_line(tok);
  uint32_t column = parse_token_column(tok);
  uint32_t endline = parse_token_endline(tok);
  uint32_t endcol = parse_token_endcol(tok);

  bnf_error_location.start_byte = parse_token_start_byte(tok);
  bnf_error_location.end_byte = parse_token_end_byte(tok);
  bnf_error_location.start_line = line;
  bnf_error_location.start_col = column;
  bnf_error_location.end_line = endline ? endline : line;
  bnf_error_location.end_col = endcol ? endcol : column + 1;
}

static void *bnf_walk_arg_ignore(parse_nt_node_t *pn, void *children,
                                 void *thunk) {
  (void)pn;
  (void)thunk;

  if (children) {
    core_free(children);
  }

  return nullptr;
}

static void *bnf_walk_annotation(parse_nt_node_t *pn, void *children,
                                 void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return nullptr;
  }

  parse_token_info_t *name_tok = (parse_token_info_t *)kids[1];
  core_string_t spelling = tok_str(name_tok);
  parse_annotation_t *info =
      parse_annot_new(parse_annot_kind_from_name(spelling));

  if (pn->rule_index == 0 && !parse_annot_building()) {
    parse_token_info_t *lp = (parse_token_info_t *)kids[2];
    parse_token_info_t *rp = (parse_token_info_t *)kids[4];
    core_string_t region = {0};

    if (lp && rp && bnf_src.data
        && parse_token_start_byte(rp) >= parse_token_end_byte(lp)
        && parse_token_start_byte(rp) <= bnf_src.u8_bytes) {
      region = core_string_from_raw(
          bnf_src.data + parse_token_end_byte(lp),
          (int64_t)(parse_token_start_byte(rp) - parse_token_end_byte(lp)));
    }

    bool args_ok = parse_annot_parse_args(info, region);

    if (!args_ok && !bnf_bad_annot.len) {
      BNF_TEXT_SET(bnf_bad_annot, "@%.*s(%.*s)",
                   spelling.data ? (int)spelling.u8_bytes : 1,
                   spelling.data ? spelling.data : "?", (int)region.u8_bytes,
                   region.data ? region.data : "");
    }

    core_free(region.data);
  } else if (pn->rule_index == 1 && !parse_annot_building() &&
             parse_annot_arg_grammar(info->kind).data && !bnf_bad_annot.len) {
    BNF_TEXT_SET(bnf_bad_annot,
             "@%.*s() with no arguments",
             spelling.data ? (int)spelling.u8_bytes : 1,
             spelling.data ? spelling.data : "?");
  }

  core_free(kids);
  return bnf_result(BNF_ANNOT, info);
}

static void *bnf_walk_annotations(parse_nt_node_t *pn, void *children,
                                  void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_result(BNF_LIST, slist_new());
  }

  bnf_list_t *result;

  if (pn->rule_index == 0) {
    result = slist_new();
  } else {
    bnf_result_t *rest_r = (bnf_result_t *)kids[1];
    result = rest_r ? (bnf_list_t *)rest_r->data : slist_new();
    slist_prepend(result, kids[0]);

    if (rest_r) {
      core_free(rest_r);
    }
  }

  core_free(kids);
  return bnf_result(BNF_LIST, result);
}

static void *bnf_walk_rule(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)pn;
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return nullptr;
  }

  parse_token_info_t *name_tok = (parse_token_info_t *)kids[1];
  bnf_result_t *annots_r = (bnf_result_t *)kids[3];
  bnf_result_t *expr_r = (bnf_result_t *)kids[5];

  core_string_t name_s = tok_str_or(name_tok, CORE_STRING_STATIC("?"));
  core_string_t *heap_name = core_alloc(core_string_t);
  *heap_name = name_s;

  bnf_list_t *triple = slist_new();
  slist_push(triple, bnf_result(BNF_NAME, heap_name));
  slist_push(triple, expr_r);
  slist_push(triple, annots_r);

  core_free(kids);
  return bnf_result(BNF_PAIR, triple);
}

static void *bnf_walk_syntax(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    return bnf_result(BNF_DICT, slist_new());
  }

  bnf_list_t *result;

  if (pn->rule_index == 0) {
    result = slist_new();
    if (kids[0])
      slist_push(result, kids[0]);
  } else if (pn->rule_index == 1) {
    bnf_result_t *syntax_r = (bnf_result_t *)kids[1];
    result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
    if (kids[0])
      slist_prepend(result, kids[0]);
    if (syntax_r)
      core_free(syntax_r);
  } else if (pn->rule_index == 2) {
    bnf_result_t *syntax_r = (bnf_result_t *)kids[1];
    result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
    if (syntax_r)
      core_free(syntax_r);
  } else if (pn->rule_index == 3) {
    result = slist_new();
  } else if (pn->rule_index == 4) {
    bnf_result_t *syntax_r = (bnf_result_t *)kids[2];
    result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
    if (syntax_r)
      core_free(syntax_r);

    if (kids[0]) {
      bnf_list_t *annot_list = slist_new();
      slist_push(annot_list, kids[0]);
      bnf_result_t *annots_r = bnf_result(BNF_LIST, annot_list);

      bnf_list_t *triple = slist_new();
      {
        core_string_t *empty = core_alloc(core_string_t);
        *empty = core_string_empty();
        slist_push(triple, bnf_result(BNF_NAME, empty));
      }
      slist_push(triple, nullptr);
      slist_push(triple, annots_r);
      slist_prepend(result, bnf_result(BNF_PAIR, triple));
    }
  } else {
    result = slist_new();

    if (kids[0]) {
      bnf_list_t *annot_list = slist_new();
      slist_push(annot_list, kids[0]);
      bnf_result_t *annots_r = bnf_result(BNF_LIST, annot_list);

      bnf_list_t *triple = slist_new();
      {
        core_string_t *empty = core_alloc(core_string_t);
        *empty = core_string_empty();
        slist_push(triple, bnf_result(BNF_NAME, empty));
      }
      slist_push(triple, nullptr);
      slist_push(triple, annots_r);
      slist_push(result, bnf_result(BNF_PAIR, triple));
    }
  }

  core_free(kids);
  return bnf_result(BNF_DICT, result);
}

static parse_grammar_t *build_bnf_grammar(void) {
  parse_grammar_t *g = parse_grammar_new();
  parse_grammar_set_error_recovery(g, false);
  scan_tokenizer_register(CORE_STRING_STATIC("bnf-bootstrap"), bnf_scan);
  scan_grammar_set_tokenizer(g, CORE_STRING_STATIC("bnf-bootstrap"));

  int64_t LANGLE = BNF_TOK_LANGLE;
  int64_t RANGLE = BNF_TOK_RANGLE;
  int64_t ASSIGN = BNF_TOK_ASSIGN;
  int64_t PIPE = BNF_TOK_PIPE;
  int64_t NEWLINE = BNF_TOK_NEWLINE;
  int64_t NAME = BNF_TOK_NAME;
  int64_t LITERAL = BNF_TOK_LITERAL;
  int64_t CLASS = BNF_TOK_CLASS;
  int64_t TOKEN_TYPE = BNF_TOK_TOKEN_TYPE;
  int64_t TOKEN_LIT = BNF_TOK_TOKEN_LIT;
  int64_t EMPTY_LIT = BNF_TOK_EMPTY_LIT;
  int64_t QUESTION = BNF_TOK_QUESTION;
  int64_t STAR = BNF_TOK_STAR;
  int64_t PLUS_OP = BNF_TOK_PLUS_OP;
  int64_t LPAREN = BNF_TOK_LPAREN;
  int64_t RPAREN = BNF_TOK_RPAREN;
  int64_t AT = BNF_TOK_AT;

  core_string_t s_syntax = CORE_STRING_STATIC("syntax");
  core_string_t s_rule = CORE_STRING_STATIC("rule");
  core_string_t s_expression = CORE_STRING_STATIC("expression");
  core_string_t s_list = CORE_STRING_STATIC("list");
  core_string_t s_item = CORE_STRING_STATIC("item");
  core_string_t s_atom = CORE_STRING_STATIC("atom");
  core_string_t s_annotations = CORE_STRING_STATIC("annotations");
  core_string_t s_annotation = CORE_STRING_STATIC("annotation");
  core_string_t s_arg_body = CORE_STRING_STATIC("arg-body");
  core_string_t s_arg_piece = CORE_STRING_STATIC("arg-piece");

  parse_nonterm(g, s_syntax);
  parse_nonterm(g, s_rule);
  parse_nonterm(g, s_expression);
  parse_nonterm(g, s_list);
  parse_nonterm(g, s_item);
  parse_nonterm(g, s_atom);
  parse_nonterm(g, s_annotations);
  parse_nonterm(g, s_annotation);
  parse_nonterm(g, s_arg_body);
  parse_nonterm(g, s_arg_piece);

  parse_nonterm_t *syntax = parse_nonterm(g, s_syntax);
  parse_nonterm_t *rule = parse_nonterm(g, s_rule);
  parse_nonterm_t *expression = parse_nonterm(g, s_expression);
  parse_nonterm_t *list = parse_nonterm(g, s_list);
  parse_nonterm_t *item = parse_nonterm(g, s_item);
  parse_nonterm_t *atom = parse_nonterm(g, s_atom);
  parse_nonterm_t *annotations = parse_nonterm(g, s_annotations);
  parse_nonterm_t *annotation = parse_nonterm(g, s_annotation);
  parse_nonterm_t *arg_body = parse_nonterm(g, s_arg_body);
  parse_nonterm_t *arg_piece = parse_nonterm(g, s_arg_piece);

  parse_add_rule(g, syntax, PARSE_NT(rule));
  parse_add_rule(g, syntax, PARSE_NT(rule), PARSE_NT(syntax));
  parse_add_rule(g, syntax, PARSE_TERMINAL(NEWLINE), PARSE_NT(syntax));
  parse_add_rule(g, syntax, PARSE_TERMINAL(NEWLINE));
  parse_add_rule(g, syntax, PARSE_NT(annotation), PARSE_TERMINAL(NEWLINE),
                 PARSE_NT(syntax));
  parse_add_rule(g, syntax, PARSE_NT(annotation), PARSE_TERMINAL(NEWLINE));

  parse_add_rule(g, rule, PARSE_TERMINAL(LANGLE), PARSE_TERMINAL(NAME),
                 PARSE_TERMINAL(RANGLE), PARSE_NT(annotations),
                 PARSE_TERMINAL(ASSIGN), PARSE_NT(expression),
                 PARSE_TERMINAL(NEWLINE));

  parse_add_rule(g, expression, PARSE_NT(list));
  parse_add_rule(g, expression, PARSE_NT(list), PARSE_TERMINAL(PIPE),
                 PARSE_NT(expression));

  parse_add_rule(g, list, PARSE_NT(item));
  parse_add_rule(g, list, PARSE_NT(item), PARSE_NT(list));

  parse_add_rule(g, item, PARSE_NT(atom));
  parse_add_rule(g, item, PARSE_NT(atom), PARSE_TERMINAL(QUESTION));
  parse_add_rule(g, item, PARSE_NT(atom), PARSE_TERMINAL(STAR));
  parse_add_rule(g, item, PARSE_NT(atom), PARSE_TERMINAL(PLUS_OP));
  parse_add_rule(g, item, PARSE_TERMINAL(LPAREN), PARSE_NT(expression),
                 PARSE_TERMINAL(RPAREN));
  parse_add_rule(g, item, PARSE_TERMINAL(LPAREN), PARSE_NT(expression),
                 PARSE_TERMINAL(RPAREN), PARSE_TERMINAL(QUESTION));
  parse_add_rule(g, item, PARSE_TERMINAL(LPAREN), PARSE_NT(expression),
                 PARSE_TERMINAL(RPAREN), PARSE_TERMINAL(STAR));
  parse_add_rule(g, item, PARSE_TERMINAL(LPAREN), PARSE_NT(expression),
                 PARSE_TERMINAL(RPAREN), PARSE_TERMINAL(PLUS_OP));

  parse_add_rule(g, atom, PARSE_TERMINAL(LITERAL));
  parse_add_rule(g, atom, PARSE_TERMINAL(LANGLE), PARSE_TERMINAL(NAME),
                 PARSE_TERMINAL(RANGLE));
  parse_add_rule(g, atom, PARSE_TERMINAL(CLASS));
  parse_add_rule(g, atom, PARSE_TERMINAL(TOKEN_TYPE));
  parse_add_rule(g, atom, PARSE_TERMINAL(TOKEN_LIT));
  parse_add_rule(g, atom, PARSE_TERMINAL(EMPTY_LIT));

  parse_add_rule(g, annotations, PARSE_EPSILON());
  parse_add_rule(g, annotations, PARSE_NT(annotation), PARSE_NT(annotations));

  parse_add_rule(g, annotation, PARSE_TERMINAL(AT), PARSE_TERMINAL(NAME),
                 PARSE_TERMINAL(LPAREN), PARSE_NT(arg_body),
                 PARSE_TERMINAL(RPAREN));
  parse_add_rule(g, annotation, PARSE_TERMINAL(AT), PARSE_TERMINAL(NAME),
                 PARSE_TERMINAL(LPAREN), PARSE_TERMINAL(RPAREN));
  parse_add_rule(g, annotation, PARSE_TERMINAL(AT), PARSE_TERMINAL(NAME));

  parse_add_rule(g, arg_body, PARSE_NT(arg_piece));
  parse_add_rule(g, arg_body, PARSE_NT(arg_piece), PARSE_NT(arg_body));

  parse_add_rule(g, arg_piece, PARSE_SET(&bnf_arg_set));
  parse_add_rule(g, arg_piece, PARSE_TERMINAL(LPAREN), PARSE_TERMINAL(RPAREN));
  parse_add_rule(g, arg_piece, PARSE_TERMINAL(LPAREN), PARSE_NT(arg_body),
                 PARSE_TERMINAL(RPAREN));

  parse_nonterm_set_action(syntax, bnf_walk_syntax);
  parse_nonterm_set_action(rule, bnf_walk_rule);
  parse_nonterm_set_action(expression, bnf_walk_expression);
  parse_nonterm_set_action(list, bnf_walk_list);
  parse_nonterm_set_action(item, bnf_walk_item);
  parse_nonterm_set_action(atom, bnf_walk_atom);
  parse_nonterm_set_action(annotations, bnf_walk_annotations);
  parse_nonterm_set_action(annotation, bnf_walk_annotation);
  parse_nonterm_set_action(arg_body, bnf_walk_arg_ignore);
  parse_nonterm_set_action(arg_piece, bnf_walk_arg_ignore);

  return g;
}

static bool reserved_to_class(core_string_t name, parse_char_class_t *cc_out) {
  if (!name.data) {
    return false;
  }

  struct {
    core_string_t lit;
    parse_char_class_t cc;
  } map[] = {
      {CORE_STRING_STATIC("__DIGIT"), PARSE_CC_ASCII_DIGIT},
      {CORE_STRING_STATIC("__ALPHA"), PARSE_CC_ASCII_ALPHA},
      {CORE_STRING_STATIC("__UPPER"), PARSE_CC_ASCII_UPPER},
      {CORE_STRING_STATIC("__LOWER"), PARSE_CC_ASCII_LOWER},
      {CORE_STRING_STATIC("__HEX"), PARSE_CC_HEX_DIGIT},
      {CORE_STRING_STATIC("__NONZERO_DIGIT"), PARSE_CC_NONZERO_DIGIT},
      {CORE_STRING_STATIC("__WHITESPACE"), PARSE_CC_WHITESPACE},
      {CORE_STRING_STATIC("__WS"), PARSE_CC_WHITESPACE},
      {CORE_STRING_STATIC("__ID_START"), PARSE_CC_ID_START},
      {CORE_STRING_STATIC("__ID_CONTINUE"), PARSE_CC_ID_CONTINUE},
      {CORE_STRING_STATIC("__PRINTABLE"), PARSE_CC_PRINTABLE},
      {CORE_STRING_STATIC("__UNICODE_DIGIT"), PARSE_CC_UNICODE_DIGIT},
      {CORE_STRING_STATIC("__JSON_STR"), PARSE_CC_JSON_STRING_CHAR},
      {CORE_STRING_STATIC("__REGEX_STR"), PARSE_CC_REGEX_BODY_CHAR},
      {{0}, 0},
  };

  for (int i = 0; map[i].lit.data; i++) {
    if (str_eq_lit(name, map[i].lit)) {
      *cc_out = map[i].cc;
      return true;
    }
  }

  return false;
}

static int64_t token_type_to_id(core_string_t name) {
  return parse_builtin_terminal_id(name);
}

static _Atomic int bnf_anon_counter = 0;

static _Thread_local bnf_list_text_t bnf_undeclared;

static void bnf_note_undeclared(core_string_t name) {
  char one[136];
  int n = snprintf(one, sizeof(one), " %%%.*s", (int)name.u8_bytes,
                   name.data ? name.data : "");
  size_t one_len = n < 0                       ? 0
                   : (size_t)n < sizeof(one) ? (size_t)n
                                             : sizeof(one) - 1;

  if (core_string_find(BNF_TEXT_STRING(bnf_undeclared),
                       core_string_borrow(one, one_len)) < 0) {
    BNF_TEXT_APPEND(bnf_undeclared, "%.*s", (int)one_len, one);
  }
}
static _Thread_local int32_t bnf_load_unit;

static bool term_char(char c, bool first) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
         (!first && c >= '0' && c <= '9');
}

size_t parse_bnf_declare_terminals(parse_grammar_t *g, core_string_t bnf_text) {
  if (!g || !bnf_text.data) {
    return 0;
  }

  size_t n = 0;

  for (size_t i = 0; i + 1 < bnf_text.u8_bytes; i++) {
    if (bnf_text.data[i] != '%' || !term_char(bnf_text.data[i + 1], true)) {
      continue;
    }

    size_t s = i + 1;
    size_t e = s;

    while (e < bnf_text.u8_bytes && term_char(bnf_text.data[e], false)) {
      e++;
    }

    core_string_t name = core_string_borrow(bnf_text.data + s, e - s);

    if (!token_type_to_id(name) && !parse_lookup_terminal(g, name)) {
      parse_register_named_terminal(g, name);
      n++;
    }

    i = e - 1;
  }

  return n;
}

int32_t parse_bnf_last_unit(void) { return bnf_load_unit; }

typedef struct {
  int32_t unit;
  core_string_t src;
  bnf_list_text_t undeclared;
  bnf_short_text_t bad_annot;
  bnf_short_text_t empty_lit;
} bnf_load_t;

static void bnf_restore(const bnf_load_t *saved) {
  bnf_load_unit = saved->unit;
  bnf_src = saved->src;

  bnf_undeclared = saved->undeclared;
  bnf_bad_annot = saved->bad_annot;
  bnf_empty_lit = saved->empty_lit;
}

static void resolve_term_to_matches(parse_grammar_t *user_g,
                                    const bnf_term_t *term,
                                    core_list_t(parse_match_t) *items) {
  if (!term) {
    return;
  }

  char type = term->kind;
  core_string_t val_s = term->value;

  switch (type) {
  case 'L': {
    const char *p = val_s.data;
    const char *end = p + val_s.u8_bytes;
    while (p && p < end) {
      int32_t cp = utf8_decode(&p, end);
      if (cp < 0)
        break;
      core_list_push(*items, ((parse_match_t){
          .kind = PARSE_MATCH_TERMINAL,
          .terminal_id = cp,
      }));
    }
    break;
  }
  case 'N': {
    int64_t ref_id = parse_nonterm(user_g, val_s)->id;
    core_list_push(*items, ((parse_match_t){
        .kind = PARSE_MATCH_NT,
        .nt_id = ref_id,
    }));
    break;
  }
  case 'C': {
    parse_char_class_t cc;
    if (reserved_to_class(val_s, &cc)) {
      core_list_push(*items, ((parse_match_t){
          .kind = PARSE_MATCH_CLASS,
          .char_class = cc,
      }));
    }
    break;
  }
  case 'T': {
    int64_t tok_id = token_type_to_id(val_s);

    if (!tok_id) {
      tok_id = parse_lookup_terminal(user_g, val_s);
    }

    if (!tok_id) {
      bnf_note_undeclared(val_s);

      break;
    }

    core_list_push(*items, ((parse_match_t){
        .kind = PARSE_MATCH_TERMINAL,
        .terminal_id = tok_id,
    }));
    break;
  }
  case 'K': {
    int64_t term_id = parse_register_terminal(user_g, val_s);
    core_list_push(*items, ((parse_match_t){
        .kind = PARSE_MATCH_TERMINAL,
        .terminal_id = term_id,
    }));
    break;
  }
  case 'E': {
    core_list_push(*items, ((parse_match_t){
        .kind = PARSE_MATCH_EMPTY,
    }));
    break;
  }
  }

}

static void resolve_terms_to_matches(parse_grammar_t *user_g, bnf_list_t *terms,
                                     core_list_t(parse_match_t) *items);

static void resolve_group_to_match(parse_grammar_t *user_g, bnf_group_info_t *gi,
                                   core_list_t(parse_match_t) *items) {
  int min = 0, max = 0;

  switch (gi->quantifier) {
  case '?':
    min = 0;
    max = 1;
    break;
  case '*':
    min = 0;
    max = 0;
    break;
  case '+':
    min = 1;
    max = 0;
    break;
  default:
    min = 1;
    max = 1;
    break;
  }

  parse_match_t group_match;

  if (gi->alternatives->len == 1) {
    bnf_result_t *alt_r = slist_get(gi->alternatives, 0);
    bnf_list_t *inner = (bnf_list_t *)alt_r->data;
    core_list_t(parse_match_t) inner_items = {0};
    resolve_terms_to_matches(user_g, inner, &inner_items);

    if (inner_items.len > 0) {
      group_match = parse_group_match_v(user_g, min, max,
                                       (int)inner_items.len, inner_items.data);
    } else {
      core_list_free(inner_items);
      return;
    }

    core_list_free(inner_items);
  } else {
    char namebuf[64];
    int n = snprintf(namebuf, sizeof(namebuf), "$$bnf_anon_%d",
                     atomic_fetch_add(&bnf_anon_counter, 1));
    core_string_t name_s = core_string_borrow(namebuf, n > 0 ? (size_t)n : 0);
    parse_nonterm_t *anon_nt = parse_nonterm(user_g, name_s);
    int64_t anon_id = anon_nt->id;

    for (size_t ai = 0; ai < gi->alternatives->len; ai++) {
      bnf_result_t *alt_r = slist_get(gi->alternatives, ai);
      bnf_list_t *inner = (bnf_list_t *)alt_r->data;
      core_list_t(parse_match_t) alt_items = {0};
      resolve_terms_to_matches(user_g, inner, &alt_items);

      if (alt_items.len > 0) {
        parse_add_rule_v(user_g, anon_id, (int)alt_items.len, alt_items.data);
      } else {
        parse_match_t empty = {.kind = PARSE_MATCH_EMPTY};
        parse_add_rule_v(user_g, anon_id, 1, &empty);
      }

      core_list_free(alt_items);
    }

    parse_match_t nt_match = {
        .kind = PARSE_MATCH_NT,
        .nt_id = anon_id,
    };

    group_match = parse_group_match_v(user_g, min, max, 1, &nt_match);
  }

  core_list_push(*items, group_match);
}

static void resolve_terms_to_matches(parse_grammar_t *user_g, bnf_list_t *terms,
                                     core_list_t(parse_match_t) *items) {
  for (size_t ti = 0; ti < terms->len; ti++) {
    bnf_result_t *term_r = slist_get(terms, ti);

    if (term_r->tag == BNF_GROUP) {
      resolve_group_to_match(user_g, (bnf_group_info_t *)term_r->data, items);
    } else {
      resolve_term_to_matches(user_g, (const bnf_term_t *)term_r->data, items);
    }
  }

}

static void preflight_term(parse_grammar_t *user_g, const bnf_term_t *term) {
  if (!term || term->kind != 'T') {
    return;
  }

  core_string_t text = term->value;

  if (token_type_to_id(text) || parse_lookup_terminal(user_g, text)) {
    return;
  }

  bnf_note_undeclared(text);
}

static void preflight_terms(parse_grammar_t *user_g, bnf_list_t *terms) {
  for (size_t i = 0; terms && i < terms->len; i++) {
    bnf_result_t *term = slist_get(terms, i);

    if (!term) {
      continue;
    }

    if (term->tag == BNF_GROUP) {
      bnf_group_info_t *group = term->data;

      for (size_t j = 0;
           group && group->alternatives && j < group->alternatives->len; j++) {
        bnf_result_t *alt = slist_get(group->alternatives, j);

        if (alt && alt->tag == BNF_LIST) {
          preflight_terms(user_g, alt->data);
        }
      }
    } else {
      preflight_term(user_g, term->data);
    }
  }
}

static bool preflight_grammar(parse_grammar_t *user_g, bnf_result_t *result,
                              core_string_t start_symbol) {
  if (!result || result->tag != BNF_DICT) {
    BNF_TEXT_SET(bnf_error, "the BNF did not reduce to a grammar");

    return false;
  }

  bnf_list_t *pairs = result->data;

  if (!pairs || !pairs->len) {
    BNF_TEXT_SET(bnf_error, "the grammar contains no productions");

    return false;
  }

  bool has_production = false;

  for (size_t i = 0; i < pairs->len; i++) {
    bnf_result_t *pair_result = slist_get(pairs, i);
    bnf_list_t *pair = pair_result ? pair_result->data : nullptr;
    bnf_result_t *name_result = slist_get(pair, 0);
    bnf_result_t *expr_result = slist_get(pair, 1);
    core_string_t *name = name_result ? name_result->data : nullptr;

    if (!name || !name->data || !name->u8_bytes || !expr_result) {
      continue;
    }

    has_production = true;

    parse_nonterm_t *existing = parse_find_nonterm(user_g, *name);

    if (existing && core_list_len(existing->rule_ids) > 0 &&
        !existing->defined_here) {
      BNF_TEXT_SET(bnf_error,
                   "<%.*s> is already defined by a grammar loaded earlier; "
                   "composing would merge two productions that share a "
                   "name",
                   (int)name->u8_bytes, name->data);

      return false;
    }

    bnf_list_t *alternatives = expr_result->data;

    for (size_t j = 0; alternatives && j < alternatives->len; j++) {
      bnf_result_t *alternative = slist_get(alternatives, j);

      if (alternative && alternative->tag == BNF_LIST) {
        preflight_terms(user_g, alternative->data);
      }
    }
  }

  if (!start_symbol.data && !has_production) {
    BNF_TEXT_SET(bnf_error,
                 "the grammar has no production to use as its start symbol");

    return false;
  }

  if (bnf_undeclared.len) {
    BNF_TEXT_SET(bnf_error, "tokens were not registered before loading:%.*s",
                 BNF_TEXT_ARG(bnf_undeclared));

    return false;
  }

  return true;
}

static bool populate_grammar(parse_grammar_t *user_g, bnf_result_t *result,
                             core_string_t start_symbol,
                             core_string_t *resolved_start) {
  if (!result || result->tag != BNF_DICT) {
    return false;
  }

  bnf_list_t *pairs = (bnf_list_t *)result->data;

  if (!pairs || !pairs->len) {
    return false;
  }

  core_string_t *first_name = nullptr;

  for (size_t i = 0; i < pairs->len; i++) {
    bnf_result_t *pair_r = slist_get(pairs, i);
    bnf_list_t *pair = (bnf_list_t *)pair_r->data;
    bnf_result_t *name_r = slist_get(pair, 0);
    core_string_t *name = (core_string_t *)name_r->data;

    bool is_grammar_annot = (!name || !name->data || !name->u8_bytes);

    if (!is_grammar_annot) {
      parse_nonterm(user_g, *name);

      if (!first_name) {
        first_name = name;
      }
    }
  }

  core_string_t start_s;

  if (start_symbol.data) {
    start_s = start_symbol;
  } else if (first_name) {
    start_s = *first_name;
  } else {
    return false;
  }

  if (resolved_start) {
    *resolved_start = core_string_dup(start_s);
  }

  parse_nonterm(user_g, start_s);

  for (size_t i = 0; i < pairs->len; i++) {
    bnf_result_t *pair_r = slist_get(pairs, i);
    bnf_list_t *pair = (bnf_list_t *)pair_r->data;
    bnf_result_t *name_r = slist_get(pair, 0);
    bnf_result_t *expr_r = slist_get(pair, 1);
    core_string_t *name = (core_string_t *)name_r->data;

    bnf_result_t *file_annots = (pair->len >= 3) ? slist_get(pair, 2) : nullptr;

    if (!name || !name->data || !name->u8_bytes || !expr_r) {
      if (file_annots && file_annots->tag == BNF_LIST) {
        bnf_list_t *fl = (bnf_list_t *)file_annots->data;

        for (size_t j = 0; j < fl->len; j++) {
          bnf_result_t *a_r = slist_get(fl, j);

          if (a_r && a_r->tag == BNF_ANNOT && a_r->data) {
            parse_annotation_t *an = a_r->data;

            if (an->kind == PARSE_ANNOT_TOKENIZER && an->scope_tag.data) {
              scan_grammar_set_tokenizer(user_g, an->scope_tag);
            }

            parse_grammar_annotate(user_g, an);
            a_r->data = nullptr;
          }
        }
      }

      continue;
    }

    int32_t penalty_cost = 0;
    core_string_t disambig_predicate = {0};
    int32_t disambig_cost = 0;
    bnf_result_t *annots_r = (pair->len >= 3) ? slist_get(pair, 2) : nullptr;
    parse_nonterm_t *lhs = parse_nonterm(user_g, *name);
    int64_t nt_id = lhs->id;

    if (core_list_len(lhs->rule_ids) > 0 && !lhs->defined_here) {
      BNF_TEXT_SET(bnf_error,
               "<%.*s> is already defined by a grammar loaded earlier; "
               "composing would merge two productions that share a "
               "name",
               name->data ? (int)name->u8_bytes : 1,
               name->data ? name->data : "?");

      return false;
    }
    lhs->defined_here = true;
    lhs->unit = bnf_load_unit;

    bnf_list_t *line_annots = slist_new();

    if (annots_r && annots_r->tag == BNF_LIST) {
      bnf_list_t *annot_list = (bnf_list_t *)annots_r->data;

      for (size_t j = 0; j < annot_list->len; j++) {
        bnf_result_t *a_r = slist_get(annot_list, j);

        if (a_r && a_r->tag == BNF_ANNOT) {
          parse_annotation_t *info = (parse_annotation_t *)a_r->data;

          if (info->penalty_cost > 0) {
            penalty_cost = info->penalty_cost;
          }
          if (info->disambig_predicate.data) {
            disambig_predicate = info->disambig_predicate;
            disambig_cost = info->disambig_cost;
          }

          slist_push(line_annots, info);
          a_r->data = nullptr;
        }
      }
    }

    bnf_list_t *alternatives = (bnf_list_t *)expr_r->data;

    for (size_t ai = 0; ai < alternatives->len; ai++) {
      bnf_result_t *alt_r = slist_get(alternatives, ai);
      bnf_list_t *terms = (bnf_list_t *)alt_r->data;
      core_list_t(parse_match_t) items = {0};
      resolve_terms_to_matches(user_g, terms, &items);

      parse_rule_t *rule_p = nullptr;

      if (penalty_cost > 0) {
        if (items.len > 0) {
          rule_p =
              parse_add_rule_with_cost_v(user_g, nt_id, penalty_cost,
                                         (int)items.len, items.data);
        } else {
          parse_match_t empty = {.kind = PARSE_MATCH_EMPTY};
          rule_p = parse_add_rule_with_cost_v(user_g, nt_id, penalty_cost, 1,
                                              &empty);
        }

        if (rule_p) {
          rule_p->penalty_rule = true;
        }
      } else {
        if (items.len > 0) {
          rule_p = parse_add_rule_v(user_g, nt_id, (int)items.len, items.data);
        } else {
          parse_match_t empty = {.kind = PARSE_MATCH_EMPTY};
          rule_p = parse_add_rule_v(user_g, nt_id, 1, &empty);
        }
      }

      if (rule_p && disambig_predicate.data) {
        rule_p->disambig_predicate = core_string_from_raw(
            disambig_predicate.data, (int64_t)disambig_predicate.u8_bytes);
        rule_p->disambig_cost = disambig_cost;
      }

      for (size_t k = 0; rule_p && k < line_annots->len; k++) {
        parse_annotation_t *info = slist_get(line_annots, k);

        parse_rule_annotate(rule_p, ai + 1 == alternatives->len
                                        ? info
                                        : parse_annot_clone(info));
      }

      core_list_free(items);
    }

    slist_free(line_annots);
  }

  return true;
}

static void free_bnf_result(bnf_result_t *r) {
  if (!r) {
    return;
  }

  switch (r->tag) {
  case BNF_STRING:
    bnf_term_free(r->data);
    break;

  case BNF_NAME:
    core_free(r->data);
    break;

  case BNF_LIST: {
    bnf_list_t *l = (bnf_list_t *)r->data;
    for (size_t i = 0; i < l->len; i++)
      free_bnf_result(slist_get(l, i));
    slist_free(l);
    break;
  }

  case BNF_PAIR: {
    bnf_list_t *pair = (bnf_list_t *)r->data;
    for (size_t i = 0; i < pair->len; i++)
      free_bnf_result(slist_get(pair, i));
    slist_free(pair);
    break;
  }

  case BNF_DICT: {
    bnf_list_t *pairs = (bnf_list_t *)r->data;
    for (size_t i = 0; i < pairs->len; i++)
      free_bnf_result(slist_get(pairs, i));
    slist_free(pairs);
    break;
  }

  case BNF_GROUP: {
    bnf_group_info_t *gi = (bnf_group_info_t *)r->data;
    if (gi->alternatives) {
      for (size_t i = 0; i < gi->alternatives->len; i++)
        free_bnf_result(slist_get(gi->alternatives, i));
      slist_free(gi->alternatives);
    }
    core_free(gi);
    break;
  }

  case BNF_ANNOT:
    parse_annot_free((parse_annotation_t *)r->data);
    break;
  }

  core_free(r);
}

static bool parse_bnf_load_impl(core_string_t bnf_text,
                                core_string_t start_symbol,
                                parse_grammar_t *user_g,
                                core_string_t *resolved_start) {
  if (!bnf_text.data || !user_g) {
    return false;
  }

  if (!parse_annot_grammar()) {
    return false;
  }

  static _Atomic int32_t next_unit = 1;

  bnf_error.len = 0;
  bnf_error_location = (parse_bnf_error_info_t){0};

  bnf_load_t saved = {
      .unit = bnf_load_unit,
      .src = bnf_src,
      .undeclared = bnf_undeclared,
      .bad_annot = bnf_bad_annot,
      .empty_lit = bnf_empty_lit,
  };

  bnf_load_unit = atomic_fetch_add(&next_unit, 1);
  bnf_undeclared.len = 0;
  bnf_bad_annot.len = 0;
  bnf_empty_lit.len = 0;

  core_string_t stripped = parse_bnf_strip_comments(bnf_text);
  core_string_t trimmed = parse_bnf_trim_lines(stripped);
  core_free(stripped.data);

  core_string_t joined = bnf_join_continuations(trimmed);
  core_free(trimmed.data);
  trimmed = joined;

  bnf_src = trimmed;

  core_buffer_t *bnf_buf =
      core_buffer_from_bytes(trimmed.data, (int64_t)trimmed.u8_bytes);
  bool in_angle = false;
  parse_scanner_t *bnf_sc =
      scan_scanner_new(bnf_buf, bnf_scan, nullptr,
                       core_option_none(core_string_t), &in_angle, nullptr);
  parse_token_stream_t *bnf_ts = scan_token_stream_new(bnf_sc);
  core_list_t(parse_token_info_t) bnf_tl = scan_stream_collect(bnf_ts);

  if (core_list_len(bnf_tl) == 0) {
    BNF_TEXT_SET(bnf_error, "the grammar contains no tokens");
    core_list_free(bnf_tl);
    scan_token_stream_free(bnf_ts);
    scan_scanner_free(bnf_sc);
    core_buffer_free(bnf_buf);
    bnf_restore(&saved);
    core_free(trimmed.data);
    return false;
  }

  parse_token_info_ptr_t *raw_ptrs;
  int32_t bnf_n = parse_token_list_build_ptrs(&bnf_tl, &raw_ptrs);

  parse_token_stream_t *parse_ts =
      scan_token_stream_from_array(raw_ptrs, bnf_n);

  parse_grammar_t *meta_g = build_bnf_grammar();
  int32_t error_pos = -1;
  parse_nt_id_t meta_start = parse_nonterm_id(
      parse_find_nonterm(meta_g, CORE_STRING_STATIC("syntax")));
  parse_forest_t forest =
      parse_pwz_parse_grammar_at(meta_g, meta_start, parse_ts, &error_pos);

  if (parse_forest_count(&forest) < 1) {
    BNF_TEXT_SET(bnf_error, "the grammar is not valid BNF");

    parse_token_info_t *at = nullptr;

    if (bnf_n > 0) {
      int32_t ix = error_pos < 0       ? 0
                   : error_pos < bnf_n ? error_pos
                                       : bnf_n - 1;

      at = raw_ptrs[ix];
    }
    bnf_error_at_token(at);
    parse_forest_free(&forest);
    core_free(raw_ptrs);
    scan_token_stream_free(parse_ts);
    core_list_free(bnf_tl);
    scan_token_stream_free(bnf_ts);
    scan_scanner_free(bnf_sc);
    core_buffer_free(bnf_buf);
    parse_grammar_free(meta_g);
    bnf_restore(&saved);
    core_free(trimmed.data);
    return false;
  }

  bnf_result_t *result = (bnf_result_t *)parse_tree_walk(
      forest.grammar, parse_forest_best(&forest), nullptr);

  bool success = true;

  if (bnf_empty_lit.len) {
    BNF_TEXT_SET(bnf_error,
                 "an empty string literal is not a production at %.*s; use an "
                 "optional group for the empty case",
                 BNF_TEXT_ARG(bnf_empty_lit));
    success = false;
  }

  if (success && bnf_bad_annot.len) {
    BNF_TEXT_SET(bnf_error,
                 "%.*s does not match the argument grammar for that annotation",
                 BNF_TEXT_ARG(bnf_bad_annot));
    success = false;
  }

  if (success) {
    success = preflight_grammar(user_g, result, start_symbol);
  }

  if (success) {
    success = populate_grammar(user_g, result, start_symbol, resolved_start);
  }

  if (!success && !bnf_error.len) {
    BNF_TEXT_SET(bnf_error, "the grammar could not be loaded");
  }

  for (size_t i = 0; success && i < core_list_len(user_g->nt_list); i++) {
    parse_nonterm_t *nt = user_g->nt_list.data[i];

    if (!nt->unit) {
      nt->unit = bnf_load_unit;
    }
  }

  free_bnf_result(result);
  parse_forest_free(&forest);
  core_free(raw_ptrs);
  scan_token_stream_free(parse_ts);
  core_list_free(bnf_tl);
  scan_token_stream_free(bnf_ts);
  scan_scanner_free(bnf_sc);
  core_buffer_free(bnf_buf);
  parse_grammar_free(meta_g);

  int32_t unit = bnf_load_unit;

  bnf_restore(&saved);
  bnf_load_unit = unit;
  core_free(trimmed.data);

  return success;
}

bool parse_bnf_load(core_string_t bnf_text, core_string_t start_symbol,
                    parse_grammar_t *user_g) {
  return parse_bnf_load_impl(bnf_text, start_symbol, user_g, nullptr);
}

static core_string_t read_bnf_file(core_string_t path) {
  char *c_path = core_string_to_cstr(path);
  FILE *f = c_path ? fopen(c_path, "rb") : nullptr;

  core_free(c_path);

  if (!f) {
    return (core_string_t){0};
  }

  if (fseek(f, 0, SEEK_END) || ftell(f) < 0) {
    fclose(f);

    return (core_string_t){0};
  }

  long size = ftell(f);

  if (fseek(f, 0, SEEK_SET)) {
    fclose(f);

    return (core_string_t){0};
  }

  char *data = core_alloc_array(char, (size_t)size + 1);
  size_t got = fread(data, 1, (size_t)size, f);

  fclose(f);

  if (got != (size_t)size || !got) {
    core_free(data);

    return (core_string_t){0};
  }

  return core_string_borrow(data, got);
}

static core_string_t relative_to(core_string_t origin, core_string_t imported) {
  if (!origin.data || !imported.data || !imported.u8_bytes) {
    return (core_string_t){};
  }

  if (core_os_is_absolute(imported)) {
    return core_string_dup(imported);
  }

  int64_t slash = core_string_rfind_byte(origin, '/');
  size_t base = slash >= 0 ? (size_t)slash + 1 : 0;
  char *result = core_alloc_array(char, base + imported.u8_bytes + 1);

  if (base) {
    memcpy(result, origin.data, base);
  }

  memcpy(result + base, imported.data, imported.u8_bytes);

  return core_string_borrow(result, base + imported.u8_bytes);
}

static bool load_lexer_file(core_string_t path,
                            core_list_t(scan_lex_decl_t) *decls,
                            core_list_t(core_string_t) *stack) {
  for (size_t i = 0; i < core_list_len(*stack); i++) {
    if (core_string_eq(stack->data[i], path)) {
      BNF_TEXT_SET(bnf_error, "lexer import cycle reaches '%.*s'",
                   (int)path.u8_bytes, path.data);

      return false;
    }
  }

  core_string_t text = read_bnf_file(path);

  if (!text.data) {
    BNF_TEXT_SET(bnf_error, "cannot read lexer '%.*s'", (int)path.u8_bytes,
                 path.data);

    return false;
  }

  core_list_push(*stack, path);

  size_t decl_start = decls->len;
  bool success = scan_lex_parse_file(text, path, decls);

  core_free(text.data);

  if (!success) {
    core_string_t reason = scan_lex_parse_error();

    if (!reason.data) {
      reason = CORE_STRING_STATIC("invalid lexer");
    }

    BNF_TEXT_SET(bnf_error, "lexer '%.*s': %.*s", (int)path.u8_bytes,
                 path.data, reason.u8_bytes < 900 ? (int)reason.u8_bytes : 900,
                 reason.data);
    return false;
  }

  size_t decl_end = decls->len;
  for (size_t i = decl_start; i < decl_end; i++) {
    scan_lex_decl_t *a = &decls->data[i];

    if (a->kind != SCAN_LEX_USE) {
      continue;
    }

    core_string_t imported = {0};
    if (a->lex_opts.len && a->lex_opts.data[0].args.len) {
      imported = a->lex_opts.data[0].args.data[0];
    }
    core_string_t fragment_path = relative_to(path, imported);

    if (!fragment_path.data) {
      BNF_TEXT_SET(bnf_error, "lexer '%.*s' has an empty fragment import",
                   (int)path.u8_bytes, path.data);

      return false;
    }

    success = load_lexer_file(fragment_path, decls, stack);
    core_free(fragment_path.data);

    if (!success) {
      return false;
    }
  }

  core_list_pop(core_string_t, *stack);
  return true;
}

bool parse_bnf_load_with_origin(core_string_t text, core_string_t origin,
                                core_string_t start_symbol,
                                parse_grammar_t *user_g) {
  if (!text.data || !origin.data || !user_g) {
    return false;
  }

  size_t annot_start = parse_grammar_annot_count(user_g);

  parse_bnf_declare_terminals(user_g, text);
  core_string_t resolved_start = {0};
  bool success =
      parse_bnf_load_impl(text, start_symbol, user_g, &resolved_start);

  if (!success) {
    core_free(resolved_start.data);
    return false;
  }

  for (size_t i = annot_start; i < parse_grammar_annot_count(user_g); i++) {
    parse_annotation_t *a = parse_grammar_annot_at(user_g, i);
    if (a && !a->source_origin.data) {
      a->source_origin = core_string_dup(origin);
    }
  }

  int32_t grammar_unit = bnf_load_unit;
  parse_annotation_t *lexer = nullptr;

  for (size_t i = annot_start; i < parse_grammar_annot_count(user_g); i++) {
    parse_annotation_t *a = parse_grammar_annot_at(user_g, i);

    if (!a || a->kind != PARSE_ANNOT_LEXER) {
      continue;
    }

    if (lexer) {
      BNF_TEXT_SET(bnf_error, "grammar '%.*s' imports more than one lexer",
                   (int)origin.u8_bytes, origin.data);

      core_free(resolved_start.data);
      return false;
    }

    lexer = a;
  }

  if (!lexer) {
    core_free(resolved_start.data);
    return true;
  }

  core_string_t lexer_path = relative_to(origin, lexer->scope_tag);
  core_list_t(scan_lex_decl_t) decls = core_list_new(scan_lex_decl_t);
  core_list_t(core_string_t) stack = core_list_new(core_string_t);

  success = lexer_path.data && load_lexer_file(lexer_path, &decls, &stack);

  if (success) {
    const scan_lex_spec_t *spec = scan_lex_spec_from_decls(user_g, &decls);
    if (!spec) {
      core_string_t reason = scan_lex_build_error();

      if (!reason.data) {
        reason = CORE_STRING_STATIC("contains no declarations");
      }

      BNF_TEXT_SET(bnf_error, "lexer '%.*s': %.*s",
                   (int)lexer_path.u8_bytes, lexer_path.data,
                   (int)reason.u8_bytes, reason.data);
      success = false;
    } else {
      scan_grammar_set_lex(user_g, spec);
      user_g->lex_spec_owned = true;
    }
  }

  scan_lex_decls_free(&decls);
  core_list_free(stack);
  core_free(lexer_path.data);
  core_free(resolved_start.data);
  bnf_load_unit = grammar_unit;

  return success;
}

bool parse_bnf_load_file(core_string_t path, core_string_t start_symbol,
                         parse_grammar_t *user_g) {
  if (!path.data || !user_g) {
    return false;
  }

  core_string_t text = read_bnf_file(path);

  if (!text.data) {
    BNF_TEXT_SET(bnf_error, "cannot read grammar '%.*s'", (int)path.u8_bytes,
                 path.data);

    return false;
  }

  bool success = parse_bnf_load_with_origin(text, path, start_symbol, user_g);
  core_free(text.data);
  return success;
}
