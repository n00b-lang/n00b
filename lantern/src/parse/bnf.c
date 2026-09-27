
#include "parse/bnf.h"
#include "lib/list.h"
#include "parse/annot_args.h"
#include "parse/annotation.h"
#include "parse/pwz.h"
#include "private/parse/annot_grammar.h"
#include "private/parse/grammar.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include <assert.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t utf8_decode(const char **pp) {
  const unsigned char *p = (const unsigned char *)*pp;
  int32_t cp;

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
  char *result = core_alloc_array(char, len + 1);
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

static _Thread_local char bnf_empty_lit[192];

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
      size_t at = strlen(bnf_empty_lit);

      if (at < sizeof(bnf_empty_lit) - 24) {
        snprintf(bnf_empty_lit + at, sizeof(bnf_empty_lit) - at, "%s%u:%u",
                 at ? ", " : "", s->mark_line,
                 s->mark_col > 1 ? s->mark_col - 1 : 1);
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
  if (!tok || !core_option_is_set(tok->value)) {
    return (core_string_t){0};
  }

  return core_option_get(tok->value);
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

static void *bnf_walk_atom(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    char *r = core_alloc_array(char, 3);
    r[0] = 'L';
    r[1] = ':';
    r[2] = '\0';
    return bnf_result(BNF_STRING, r);
  }

  char *result;

  if (pn->rule_index == 0) {
    parse_token_info_t *tok = (parse_token_info_t *)kids[0];
    core_string_t val = tok_str_or(tok, CORE_STRING_STATIC(""));
    size_t len = val.u8_bytes;
    result = core_alloc_array(char, len + 3);
    result[0] = 'L';
    result[1] = ':';
    memcpy(result + 2, val.data, len + 1);
  } else if (pn->rule_index == 1) {
    parse_token_info_t *tok = (parse_token_info_t *)kids[1];
    core_string_t val = tok_str_or(tok, CORE_STRING_STATIC(""));
    size_t len = val.u8_bytes;
    result = core_alloc_array(char, len + 3);
    result[0] = 'N';
    result[1] = ':';
    memcpy(result + 2, val.data, len + 1);
  } else if (pn->rule_index == 2) {
    parse_token_info_t *tok = (parse_token_info_t *)kids[0];
    core_string_t val = tok_str_or(tok, CORE_STRING_STATIC(""));
    size_t len = val.u8_bytes;
    result = core_alloc_array(char, len + 3);
    result[0] = 'C';
    result[1] = ':';
    memcpy(result + 2, val.data, len + 1);
  } else if (pn->rule_index == 3) {
    parse_token_info_t *tok = (parse_token_info_t *)kids[0];
    core_string_t val = tok_str_or(tok, CORE_STRING_STATIC(""));
    size_t len = val.u8_bytes;
    result = core_alloc_array(char, len + 3);
    result[0] = 'T';
    result[1] = ':';
    memcpy(result + 2, val.data, len + 1);
  } else if (pn->rule_index == 4) {
    parse_token_info_t *tok = (parse_token_info_t *)kids[0];
    core_string_t val = tok_str_or(tok, CORE_STRING_STATIC(""));
    size_t len = val.u8_bytes;
    result = core_alloc_array(char, len + 3);
    result[0] = 'K';
    result[1] = ':';
    memcpy(result + 2, val.data, len + 1);
  } else {
    result = core_alloc_array(char, 3);
    result[0] = 'E';
    result[1] = ':';
    result[2] = '\0';
  }

  core_free(kids);
  return bnf_result(BNF_STRING, result);
}

static void *bnf_walk_item(parse_nt_node_t *pn, void *children, void *thunk) {
  (void)thunk;
  void **kids = (void **)children;

  if (!kids) {
    char *s = core_alloc_array(char, 3);
    s[0] = 'E';
    s[1] = ':';
    s[2] = '\0';
    return bnf_result(BNF_STRING, s);
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
    {
      char *es = core_alloc_array(char, 3);
      es[0] = 'E';
      es[1] = ':';
      es[2] = '\0';
      return bnf_result(BNF_STRING, es);
    }
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

static _Thread_local const char *bnf_src;
static _Thread_local size_t bnf_src_len;
static _Thread_local char bnf_bad_annot[192];
static _Thread_local char bnf_error[1024];
static _Thread_local parse_bnf_error_info_t bnf_error_location;

const char *parse_bnf_error(void) { return bnf_error[0] ? bnf_error : nullptr; }

bool parse_bnf_error_info(parse_bnf_error_info_t *out) {
  if (!out || !bnf_error[0]) {
    return false;
  }

  *out = bnf_error_location;
  out->message = bnf_error;

  return true;
}

static void bnf_error_at_token(parse_token_info_t *tok) {
  bnf_error_location = (parse_bnf_error_info_t){0};

  if (!tok) {
    return;
  }

  bnf_error_location.start_byte = tok->start_byte;
  bnf_error_location.end_byte = tok->end_byte;
  bnf_error_location.start_line = tok->line;
  bnf_error_location.start_col = tok->column;
  bnf_error_location.end_line = tok->endline ? tok->endline : tok->line;
  bnf_error_location.end_col = tok->endcol ? tok->endcol : tok->column + 1;
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

    if (lp && rp && bnf_src && rp->start_byte >= lp->end_byte &&
        rp->start_byte <= bnf_src_len) {
      region = core_string_from_raw(bnf_src + lp->end_byte,
                                    (int64_t)(rp->start_byte - lp->end_byte));
    }

    bool args_ok = parse_annot_parse_args(info, region);

    if (!args_ok && !bnf_bad_annot[0]) {
      snprintf(bnf_bad_annot, sizeof(bnf_bad_annot), "@%s(%.*s)",
               spelling.data ? spelling.data : "?", (int)region.u8_bytes,
               region.data ? region.data : "");
    }

    core_free(region.data);
  } else if (pn->rule_index == 1 && !parse_annot_building() &&
             parse_annot_arg_grammar(info->kind).data && !bnf_bad_annot[0]) {
    snprintf(bnf_bad_annot, sizeof(bnf_bad_annot), "@%s() with no arguments",
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

static _Thread_local char bnf_undeclared[512];
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
  const char *src;
  size_t src_len;
  char undeclared[512];
  char bad_annot[192];
  char empty_lit[192];
} bnf_load_t;

static void bnf_restore(const bnf_load_t *saved) {
  bnf_load_unit = saved->unit;
  bnf_src = saved->src;
  bnf_src_len = saved->src_len;

  memcpy(bnf_undeclared, saved->undeclared, sizeof(bnf_undeclared));
  memcpy(bnf_bad_annot, saved->bad_annot, sizeof(bnf_bad_annot));
  memcpy(bnf_empty_lit, saved->empty_lit, sizeof(bnf_empty_lit));
}

static int resolve_term_to_matches(parse_grammar_t *user_g, const char *tagged,
                                   parse_match_t **items_p, int *cap_p, int n) {
  if (!tagged || strlen(tagged) < 2) {
    return n;
  }

  parse_match_t *items = *items_p;
  int cap = *cap_p;
  char type = tagged[0];
  const char *val = tagged + 2;
  core_string_t val_s = core_string_borrow(val, strlen(val));

#define ENSURE_CAP()                                                           \
  do {                                                                         \
    if (n >= cap) {                                                            \
      int old_cap = cap;                                                       \
      cap = cap ? cap * 2 : 8;                                                 \
      parse_match_t *ni = core_alloc_array(parse_match_t, (size_t)cap);        \
      if (items && old_cap > 0)                                                \
        memcpy(ni, items, (size_t)old_cap * sizeof(parse_match_t));            \
      if (items)                                                               \
        core_free(items);                                                      \
      items = ni;                                                              \
    }                                                                          \
  } while (0)

  switch (type) {
  case 'L': {
    const char *p = val;
    while (*p) {
      int32_t cp = utf8_decode(&p);
      if (cp < 0)
        break;
      ENSURE_CAP();
      items[n++] = (parse_match_t){
          .kind = PARSE_MATCH_TERMINAL,
          .terminal_id = cp,
      };
    }
    break;
  }
  case 'N': {
    int64_t ref_id = parse_nonterm(user_g, val_s)->id;
    ENSURE_CAP();
    items[n++] = (parse_match_t){
        .kind = PARSE_MATCH_NT,
        .nt_id = ref_id,
    };
    break;
  }
  case 'C': {
    parse_char_class_t cc;
    if (reserved_to_class(val_s, &cc)) {
      ENSURE_CAP();
      items[n++] = (parse_match_t){
          .kind = PARSE_MATCH_CLASS,
          .char_class = cc,
      };
    }
    break;
  }
  case 'T': {
    int64_t tok_id = token_type_to_id(val_s);

    if (!tok_id) {
      tok_id = parse_lookup_terminal(user_g, val_s);
    }

    if (!tok_id) {
      char one[136];

      snprintf(one, sizeof(one), " %%%s", val);

      if (!strstr(bnf_undeclared, one)) {
        size_t at = strlen(bnf_undeclared);

        snprintf(bnf_undeclared + at, sizeof(bnf_undeclared) - at, "%s", one);
      }

      break;
    }

    ENSURE_CAP();
    items[n++] = (parse_match_t){
        .kind = PARSE_MATCH_TERMINAL,
        .terminal_id = tok_id,
    };
    break;
  }
  case 'K': {
    int64_t term_id = parse_register_terminal(user_g, val_s);
    ENSURE_CAP();
    items[n++] = (parse_match_t){
        .kind = PARSE_MATCH_TERMINAL,
        .terminal_id = term_id,
    };
    break;
  }
  case 'E': {
    ENSURE_CAP();
    items[n++] = (parse_match_t){
        .kind = PARSE_MATCH_EMPTY,
    };
    break;
  }
  }

#undef ENSURE_CAP

  *items_p = items;
  *cap_p = cap;

  return n;
}

static int resolve_terms_to_matches(parse_grammar_t *user_g, bnf_list_t *terms,
                                    parse_match_t **items_p, int *cap_p, int n);

static int resolve_group_to_match(parse_grammar_t *user_g, bnf_group_info_t *gi,
                                  parse_match_t **items_p, int *cap_p, int n) {
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
    parse_match_t *inner_items = nullptr;
    int inner_cap = 0;
    int inner_n = 0;

    inner_n = resolve_terms_to_matches(user_g, inner, &inner_items, &inner_cap,
                                       inner_n);

    if (inner_n > 0) {
      group_match = parse_group_match_v(user_g, min, max, inner_n, inner_items);
    } else {
      core_free(inner_items);
      return n;
    }

    core_free(inner_items);
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
      parse_match_t *alt_items = nullptr;
      int alt_cap = 0;
      int alt_n = 0;

      alt_n =
          resolve_terms_to_matches(user_g, inner, &alt_items, &alt_cap, alt_n);

      if (alt_n > 0) {
        parse_add_rule_v(user_g, anon_id, alt_n, alt_items);
      } else {
        parse_match_t empty = {.kind = PARSE_MATCH_EMPTY};
        parse_add_rule_v(user_g, anon_id, 1, &empty);
      }

      core_free(alt_items);
    }

    parse_match_t nt_match = {
        .kind = PARSE_MATCH_NT,
        .nt_id = anon_id,
    };

    group_match = parse_group_match_v(user_g, min, max, 1, &nt_match);
  }

  parse_match_t *items = *items_p;
  int cap = *cap_p;

  if (n >= cap) {
    int old_cap = cap;
    cap = cap ? cap * 2 : 8;
    parse_match_t *new_items = core_alloc_array(parse_match_t, (size_t)cap);
    if (items && old_cap > 0)
      memcpy(new_items, items, (size_t)old_cap * sizeof(parse_match_t));
    if (items)
      core_free(items);
    items = new_items;
  }

  items[n++] = group_match;
  *items_p = items;
  *cap_p = cap;

  return n;
}

static int resolve_terms_to_matches(parse_grammar_t *user_g, bnf_list_t *terms,
                                    parse_match_t **items_p, int *cap_p,
                                    int n) {
  for (size_t ti = 0; ti < terms->len; ti++) {
    bnf_result_t *term_r = slist_get(terms, ti);

    if (term_r->tag == BNF_GROUP) {
      n = resolve_group_to_match(user_g, (bnf_group_info_t *)term_r->data,
                                 items_p, cap_p, n);
    } else {
      n = resolve_term_to_matches(user_g, (char *)term_r->data, items_p, cap_p,
                                  n);
    }
  }

  return n;
}

static void preflight_term(parse_grammar_t *user_g, const char *tagged) {
  if (!tagged || strlen(tagged) < 2 || tagged[0] != 'T') {
    return;
  }

  const char *name = tagged + 2;
  core_string_t text = core_string_borrow(name, strlen(name));

  if (token_type_to_id(text) || parse_lookup_terminal(user_g, text)) {
    return;
  }

  char one[136];

  snprintf(one, sizeof(one), " %%%s", name);

  if (!strstr(bnf_undeclared, one)) {
    size_t at = strlen(bnf_undeclared);

    snprintf(bnf_undeclared + at, sizeof(bnf_undeclared) - at, "%s", one);
  }
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
    snprintf(bnf_error, sizeof(bnf_error),
             "the BNF did not reduce to a grammar");

    return false;
  }

  bnf_list_t *pairs = result->data;

  if (!pairs || !pairs->len) {
    snprintf(bnf_error, sizeof(bnf_error),
             "the grammar contains no productions");

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
      snprintf(bnf_error, sizeof(bnf_error),
               "<%s> is already defined by a grammar loaded earlier; "
               "composing would merge two productions that share a "
               "name",
               name->data);

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
    snprintf(bnf_error, sizeof(bnf_error),
             "the grammar has no production to use as its start symbol");

    return false;
  }

  if (bnf_undeclared[0]) {
    snprintf(bnf_error, sizeof(bnf_error),
             "tokens were not registered before loading:%s", bnf_undeclared);

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

    bool is_grammar_annot = (!name || !name->data || name->data[0] == '\0');

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

    if (!name || !name->data || name->data[0] == '\0' || !expr_r) {
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
      snprintf(bnf_error, sizeof(bnf_error),
               "<%s> is already defined by a grammar loaded earlier; "
               "composing would merge two productions that share a "
               "name",
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
      parse_match_t *items = nullptr;
      int n = 0;
      int cap = 0;

      n = resolve_terms_to_matches(user_g, terms, &items, &cap, n);

      parse_rule_t *rule_p = nullptr;

      if (penalty_cost > 0) {
        if (n > 0) {
          rule_p =
              parse_add_rule_with_cost_v(user_g, nt_id, penalty_cost, n, items);
        } else {
          parse_match_t empty = {.kind = PARSE_MATCH_EMPTY};
          rule_p = parse_add_rule_with_cost_v(user_g, nt_id, penalty_cost, 1,
                                              &empty);
        }

        if (rule_p) {
          rule_p->penalty_rule = true;
        }
      } else {
        if (n > 0) {
          rule_p = parse_add_rule_v(user_g, nt_id, n, items);
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

      core_free(items);
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
    core_free(r->data);
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

  bnf_error[0] = 0;
  bnf_error_location = (parse_bnf_error_info_t){0};

  bnf_load_t saved = {
      .unit = bnf_load_unit,
      .src = bnf_src,
      .src_len = bnf_src_len,
  };

  memcpy(saved.undeclared, bnf_undeclared, sizeof(saved.undeclared));
  memcpy(saved.bad_annot, bnf_bad_annot, sizeof(saved.bad_annot));
  memcpy(saved.empty_lit, bnf_empty_lit, sizeof(saved.empty_lit));

  bnf_load_unit = atomic_fetch_add(&next_unit, 1);
  bnf_undeclared[0] = 0;
  bnf_bad_annot[0] = 0;
  bnf_empty_lit[0] = 0;

  core_string_t stripped = parse_bnf_strip_comments(bnf_text);
  core_string_t trimmed = parse_bnf_trim_lines(stripped);
  core_free(stripped.data);

  core_string_t joined = bnf_join_continuations(trimmed);
  core_free(trimmed.data);
  trimmed = joined;

  size_t len = trimmed.u8_bytes;

  if (len > 0 && trimmed.data[len - 1] != '\n') {
    trimmed.data = core_realloc(trimmed.data, len + 2);
    trimmed.data[len] = '\n';
    trimmed.data[len + 1] = '\0';
    trimmed.u8_bytes = len + 1;
  }

  bnf_src = trimmed.data;
  bnf_src_len = trimmed.u8_bytes;

  core_buffer_t *bnf_buf =
      core_buffer_from_bytes(trimmed.data, (int64_t)trimmed.u8_bytes);
  bool in_angle = false;
  parse_scanner_t *bnf_sc =
      scan_scanner_new(bnf_buf, bnf_scan, nullptr,
                       core_option_none(core_string_t), &in_angle, nullptr);
  parse_token_stream_t *bnf_ts = scan_token_stream_new(bnf_sc);
  core_list_t(parse_token_info_t) bnf_tl = scan_stream_collect(bnf_ts);

  if (core_list_len(bnf_tl) == 0) {
    snprintf(bnf_error, sizeof(bnf_error), "the grammar contains no tokens");
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
    snprintf(bnf_error, sizeof(bnf_error), "the grammar is not valid BNF");

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

  bnf_result_t *result =
      (bnf_result_t *)parse_forest_walk_best(&forest, nullptr);

  bool success = true;

  if (bnf_empty_lit[0]) {
    snprintf(bnf_error, sizeof(bnf_error),
             "an empty string literal is not a production at %s; use an "
             "optional group for the empty case",
             bnf_empty_lit);
    success = false;
  }

  if (success && bnf_bad_annot[0]) {
    snprintf(bnf_error, sizeof(bnf_error),
             "%s does not match the argument grammar for that annotation",
             bnf_bad_annot);
    success = false;
  }

  if (success) {
    success = preflight_grammar(user_g, result, start_symbol);
  }

  if (success) {
    success = populate_grammar(user_g, result, start_symbol, resolved_start);
  }

  if (!success && !bnf_error[0]) {
    snprintf(bnf_error, sizeof(bnf_error), "the grammar could not be loaded");
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

static core_string_t read_bnf_file(const char *path) {
  FILE *f = path ? fopen(path, "rb") : nullptr;

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

  if (got != (size_t)size) {
    core_free(data);

    return (core_string_t){0};
  }

  data[got] = '\0';

  return core_string_from_raw(data, (int64_t)got);
}

static char *relative_to(const char *origin, core_string_t imported) {
  if (!origin || !imported.data || !imported.u8_bytes) {
    return nullptr;
  }

  if (imported.data[0] == '/') {
    char *result = core_alloc_array(char, imported.u8_bytes + 1);

    memcpy(result, imported.data, imported.u8_bytes);
    result[imported.u8_bytes] = '\0';

    return result;
  }

  const char *slash = strrchr(origin, '/');
  size_t base = slash ? (size_t)(slash - origin + 1) : 0;
  char *result = core_alloc_array(char, base + imported.u8_bytes + 1);

  if (base) {
    memcpy(result, origin, base);
  }

  memcpy(result + base, imported.data, imported.u8_bytes);
  result[base + imported.u8_bytes] = '\0';

  return result;
}

static bool load_lexer_file(const char *path, core_string_t start_symbol,
                            parse_grammar_t *grammar,
                            core_list_t(core_string_t) * stack) {
  for (size_t i = 0; i < core_list_len(*stack); i++) {
    if (core_string_eq(stack->data[i], core_string_from_cstr(path))) {
      snprintf(bnf_error, sizeof(bnf_error), "lexer import cycle reaches '%s'",
               path);

      return false;
    }
  }

  core_string_t text = read_bnf_file(path);

  if (!text.data) {
    snprintf(bnf_error, sizeof(bnf_error), "cannot read lexer '%s'", path);

    return false;
  }

  core_list_push(*stack, core_string_borrow(path, strlen(path)));

  size_t annot_start = parse_grammar_annot_count(grammar);
  size_t rules_start = core_list_len(grammar->rules);
  bool success = parse_bnf_load(text, start_symbol, grammar);

  core_free(text.data);

  if (!success) {
    char reason[sizeof(bnf_error)];

    snprintf(reason, sizeof(reason), "%s",
             bnf_error[0] ? bnf_error : "invalid BNF");
    snprintf(bnf_error, sizeof(bnf_error), "lexer '%s': %.900s", path, reason);
    return false;
  }

  size_t annot_end = parse_grammar_annot_count(grammar);

  for (size_t i = annot_start; i < annot_end; i++) {
    parse_annotation_t *a = parse_grammar_annot_at(grammar, i);
    if (a && !a->source_origin.data) {
      a->source_origin = core_string_from_cstr(path);
    }
  }

  if (core_list_len(grammar->rules) != rules_start) {
    snprintf(bnf_error, sizeof(bnf_error),
             "lexer '%s' may contain only lexical declarations", path);

    return false;
  }

  for (size_t i = annot_start; i < annot_end; i++) {
    parse_annotation_t *a = parse_grammar_annot_at(grammar, i);

    if (a && (a->kind < PARSE_ANNOT_LEX_COMMENT ||
              a->kind > PARSE_ANNOT_LEX_SOFT_KEYWORDS)) {
      snprintf(bnf_error, sizeof(bnf_error),
               "lexer '%s' may contain only lexical declarations", path);

      return false;
    }
  }

  for (size_t i = annot_start; i < annot_end; i++) {
    parse_annotation_t *a = parse_grammar_annot_at(grammar, i);

    if (!a || a->kind != PARSE_ANNOT_LEX_USE) {
      continue;
    }

    char *fragment_path = relative_to(path, a->scope_tag);

    if (!fragment_path) {
      snprintf(bnf_error, sizeof(bnf_error),
               "lexer '%s' has an empty fragment import", path);

      return false;
    }

    success = load_lexer_file(fragment_path, start_symbol, grammar, stack);
    core_free(fragment_path);

    if (!success) {
      return false;
    }
  }

  core_list_pop(core_string_t, *stack);
  return true;
}

bool parse_bnf_load_with_origin(core_string_t text, const char *origin,
                                core_string_t start_symbol,
                                parse_grammar_t *user_g) {
  if (!text.data || !origin || !user_g) {
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
      a->source_origin = core_string_from_cstr(origin);
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
      snprintf(bnf_error, sizeof(bnf_error),
               "grammar '%s' imports more than one lexer", origin);

      core_free(resolved_start.data);
      return false;
    }

    lexer = a;
  }

  if (!lexer) {
    core_free(resolved_start.data);
    return true;
  }

  char *lexer_path = relative_to(origin, lexer->scope_tag);
  core_string_t import_start = resolved_start;

  core_list_t(core_string_t) stack = core_list_new(core_string_t);

  success = load_lexer_file(lexer_path, import_start, user_g, &stack);

  core_list_free(stack);
  core_free(lexer_path);
  core_free(resolved_start.data);
  bnf_load_unit = grammar_unit;

  return success;
}

bool parse_bnf_load_file(const char *path, core_string_t start_symbol,
                         parse_grammar_t *user_g) {
  if (!path || !user_g) {
    return false;
  }

  core_string_t text = read_bnf_file(path);

  if (!text.data) {
    snprintf(bnf_error, sizeof(bnf_error), "cannot read grammar '%s'", path);

    return false;
  }

  bool success = parse_bnf_load_with_origin(text, path, start_symbol, user_g);
  core_free(text.data);
  return success;
}
