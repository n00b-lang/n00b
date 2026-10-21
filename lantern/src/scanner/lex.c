#include "scanner/lex.h"
#include "parse/annotation.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_decl.h"
#include "private/parse/unicode_class.h"
#include "private/scanner/lex_program.h"
#include "private/scanner/unicode_profile.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "unicode/encoding.h"
#include "default_lex.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

static pthread_mutex_t lex_compile_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local core_string_t lex_compile_error;

static void lex_compile_error_set(core_string_t message) {
  core_free(lex_compile_error.data);
  lex_compile_error = message;
}

__attribute__((format(printf, 1, 2))) static void
lex_compile_error_printf(const char *fmt, ...) {
  core_buffer_t *buf = core_buffer_empty();
  va_list ap;

  va_start(ap, fmt);
  core_buffer_vprintf(buf, fmt, ap);
  va_end(ap);
  lex_compile_error_set(core_buffer_take_string(buf));
}

static bool is_line_whitespace(core_codepoint_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f'
      || cp == '\v' || cp == 0x85 || cp == 0xa0 || cp == 0x1680
      || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f
      || cp == 0x205f || cp == 0x3000;
}

typedef struct {
  parse_trivia_t *open;
  parse_trivia_t *close;
} lex_region_pair_t;

bool scan_lex_finalize_regions(parse_scanner_t *s);

static void clear_region_links(parse_trivia_t *trivia) {
  parse_trivia_region_link_t *link = trivia->regions;
  while (link) {
    parse_trivia_region_link_t *next = link->next;
    core_free(link);
    link = next;
  }
  trivia->regions = nullptr;
}

static bool normalize_region_name(parse_scanner_t *s, parse_trivia_t *trivia) {
  utf8proc_uint8_t *normalized = nullptr;
  utf8proc_ssize_t len = utf8proc_map(
      (const utf8proc_uint8_t *)trivia->region_name.data,
      (utf8proc_ssize_t)trivia->region_name.u8_bytes, &normalized,
      UTF8PROC_STABLE | UTF8PROC_COMPOSE);
  if (len < 0) {
    scan_set_invalid(s, CORE_STRING_STATIC("invalid comment-region name"),
                     trivia->start_byte - s->source_base);
    return false;
  }
  if (trivia->owns_region_name) core_free(trivia->region_name.data);
  trivia->region_name = core_string_borrow((const char *)normalized, (size_t)len);
  trivia->owns_region_name = true;
  return true;
}

static void add_region_link(parse_trivia_t *trivia, uint64_t region_id) {
  parse_trivia_region_link_t **tail = &trivia->regions;
  while (*tail) {
    if ((*tail)->region_id == region_id) return;
    tail = &(*tail)->next;
  }
  *tail = core_alloc(parse_trivia_region_link_t);
  **tail = (parse_trivia_region_link_t){.region_id = region_id};
}

static int compare_trivia_span(const void *left, const void *right) {
  const parse_trivia_t *a = *(parse_trivia_t *const *)left;
  const parse_trivia_t *b = *(parse_trivia_t *const *)right;
  if (a->start_byte < b->start_byte) return -1;
  if (a->start_byte > b->start_byte) return 1;
  if (a->end_byte < b->end_byte) return -1;
  if (a->end_byte > b->end_byte) return 1;
  return 0;
}

static size_t append_trivia_ptrs(parse_trivia_t *trivia,
                                 parse_trivia_t **items, size_t at) {
  for (; trivia; trivia = trivia->next) items[at++] = trivia;
  return at;
}

static bool token_overlaps_line(const parse_token_stream_t *stream,
                                uint32_t line) {
  for (int32_t i = 0; i < stream->token_count; i++) {
    const parse_token_info_t *token = stream->tokens[i];
    if (parse_token_tid(token) != PARSE_TOK_EOF
        && parse_token_line(token) <= line
        && parse_token_endline(token) >= line) {
      return true;
    }
  }
  return false;
}

static bool token_between(const parse_token_stream_t *stream,
                          size_t start_byte, size_t end_byte) {
  for (int32_t i = 0; i < stream->token_count; i++) {
    const parse_token_info_t *token = stream->tokens[i];
    if (parse_token_tid(token) != PARSE_TOK_EOF
        && parse_token_end_byte(token) > start_byte
        && parse_token_start_byte(token) < end_byte) {
      return true;
    }
  }
  return false;
}

static void group_anchor_comments(parse_token_stream_t *stream,
                                  parse_trivia_t **items, size_t count) {
  for (size_t i = 0; i < count; i++) {
    parse_trivia_t *anchor = items[i];
    if (anchor->kind != PARSE_TRIVIA_REGION_OPEN
        && anchor->kind != PARSE_TRIVIA_REGION_CLOSE) {
      continue;
    }
    uint64_t group = anchor->anchor_order;
    anchor->anchor_group = group;

    size_t boundary = anchor->start_byte;
    for (size_t j = i; j-- > 0;) {
      parse_trivia_t *candidate = items[j];
      if (candidate->kind == PARSE_TRIVIA_REGION_OPEN
          || candidate->kind == PARSE_TRIVIA_REGION_CLOSE) {
        break;
      }
      if (candidate->kind == PARSE_TRIVIA_SPACE) continue;
      if (candidate->kind != PARSE_TRIVIA_COMMENT
          || (candidate->anchor_group && candidate->anchor_group != group)
          || token_overlaps_line(stream, candidate->line)
          || token_between(stream, candidate->end_byte, boundary)) {
        break;
      }
      candidate->anchor_group = group;
      boundary = candidate->start_byte;
    }

    boundary = anchor->end_byte;
    for (size_t j = i + 1; j < count; j++) {
      parse_trivia_t *candidate = items[j];
      if (candidate->kind == PARSE_TRIVIA_REGION_OPEN
          || candidate->kind == PARSE_TRIVIA_REGION_CLOSE) {
        break;
      }
      if (candidate->kind == PARSE_TRIVIA_SPACE) continue;
      if (candidate->kind != PARSE_TRIVIA_COMMENT
          || (candidate->anchor_group && candidate->anchor_group != group)
          || token_overlaps_line(stream, candidate->line)
          || token_between(stream, boundary, candidate->start_byte)) {
        break;
      }
      candidate->anchor_group = group;
      boundary = candidate->end_byte;
    }
  }
}

bool scan_lex_finalize_regions(parse_scanner_t *s) {
  parse_token_stream_t *stream = s ? s->stream : nullptr;
  scan_lex_t *program = s ? scan_grammar_lex_compiled(s->grammar) : nullptr;
  if (!program || !stream || !program->spec->regions) return true;

  size_t count = 0;
  for (int32_t i = 0; i < stream->token_count; i++) {
    for (parse_trivia_t *t = parse_token_leading_trivia(stream->tokens[i]); t;
         t = t->next)
      count++;
    for (parse_trivia_t *t = parse_token_trailing_trivia(stream->tokens[i]); t;
         t = t->next)
      count++;
  }
  for (parse_trivia_t *t = s->pending_leading; t; t = t->next) count++;
  for (parse_trivia_t *t = s->pending_trailing; t; t = t->next) count++;
  if (!count) return true;

  parse_trivia_t **items = core_alloc_array(parse_trivia_t *, count);
  size_t at = 0;
  for (int32_t i = 0; i < stream->token_count; i++) {
    at = append_trivia_ptrs(parse_token_leading_trivia(stream->tokens[i]),
                            items, at);
    at = append_trivia_ptrs(parse_token_trailing_trivia(stream->tokens[i]),
                            items, at);
  }
  at = append_trivia_ptrs(s->pending_leading, items, at);
  at = append_trivia_ptrs(s->pending_trailing, items, at);
  qsort(items, count, sizeof(*items), compare_trivia_span);

  size_t anchor_count = 0;
  for (size_t i = 0; i < count; i++) {
    clear_region_links(items[i]);
    items[i]->region_id = 0;
    items[i]->anchor_order = 0;
    items[i]->anchor_group = 0;
    if (items[i]->kind == PARSE_TRIVIA_REGION_OPEN
        || items[i]->kind == PARSE_TRIVIA_REGION_CLOSE) {
      anchor_count++;
      items[i]->anchor_order = anchor_count;
      if (!normalize_region_name(s, items[i])) {
        core_free(items);
        return false;
      }
    }
  }

  lex_region_pair_t *pairs = core_alloc_array(lex_region_pair_t, anchor_count);
  parse_trivia_t **opens = core_alloc_array(parse_trivia_t *, anchor_count);
  size_t pair_count = 0;
  size_t open_count = 0;
  uint64_t next_id = 1;

  for (size_t i = 0; i < count; i++) {
    parse_trivia_t *anchor = items[i];
    if (anchor->kind == PARSE_TRIVIA_REGION_OPEN) {
      anchor->region_id = next_id++;
      opens[open_count++] = anchor;
      continue;
    }
    if (anchor->kind != PARSE_TRIVIA_REGION_CLOSE) continue;

    size_t match = open_count;
    while (match && !core_string_eq(opens[match - 1]->region_name,
                                    anchor->region_name)) {
      match--;
    }
    if (!match) {
      scan_set_invalid(s, CORE_STRING_STATIC("unmatched comment-region close"),
                       anchor->start_byte - s->source_base);
      core_free(opens);
      core_free(pairs);
      core_free(items);
      return false;
    }
    parse_trivia_t *open = opens[match - 1];
    memmove(opens + match - 1, opens + match,
            (open_count - match) * sizeof(*opens));
    open_count--;
    anchor->region_id = open->region_id;
    pairs[pair_count++] = (lex_region_pair_t){.open = open, .close = anchor};
  }

  if (open_count) {
    parse_trivia_t *open = opens[open_count - 1];
    scan_set_invalid(s, CORE_STRING_STATIC("unclosed comment region"),
                     open->start_byte - s->source_base);
    core_free(opens);
    core_free(pairs);
    core_free(items);
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    if (items[i]->kind != PARSE_TRIVIA_COMMENT) continue;
    for (size_t j = 0; j < pair_count; j++) {
      if (items[i]->start_byte >= pairs[j].open->end_byte
          && items[i]->end_byte <= pairs[j].close->start_byte) {
        add_region_link(items[i], pairs[j].open->region_id);
      }
    }
  }
  for (size_t i = 0; i < pair_count; i++) {
    for (size_t j = 0; j < pair_count; j++) {
      if (i == j) continue;
      if (pairs[i].open->start_byte >= pairs[j].open->end_byte
          && pairs[i].close->end_byte <= pairs[j].close->start_byte) {
        add_region_link(pairs[i].open, pairs[j].open->region_id);
        add_region_link(pairs[i].close, pairs[j].open->region_id);
      }
    }
  }
  group_anchor_comments(stream, items, count);

  core_free(opens);
  core_free(pairs);
  core_free(items);
  return true;
}

typedef struct {
  const scan_lex_string_t *str;
  int32_t depth;
  size_t delimiter_depth;
  bool in_hole;
  bool at_open;
} lex_mode_t;

typedef enum : uint8_t {
  NUMBER_NONE,
  NUMBER_PREFIX,
  NUMBER_INTEGER_DIGITS,
  NUMBER_POINT,
  NUMBER_FRACTION_DIGITS,
  NUMBER_EXP_START,
  NUMBER_EXP_SIGN,
  NUMBER_EXP_DIGITS,
  NUMBER_SUFFIX,
} lex_number_phase_t;

typedef enum : uint8_t {
  HEREDOC_HEADER_NONE,
  HEREDOC_HEADER_LEADING,
  HEREDOC_HEADER_TAG,
  HEREDOC_HEADER_LINE,
} lex_heredoc_header_phase_t;

core_list_decl(lex_mode_t);

typedef struct {
  const scan_lex_heredoc_t *recipe;
  core_string_t tag;
  int32_t token_index;
  uint8_t quote_kind;
} lex_heredoc_queue_t;

core_list_decl(lex_heredoc_queue_t);

typedef struct {
  const scan_lex_spec_t *spec;
  scan_lex_t *program;
} lex_compile_cache_entry_t;

core_list_decl(lex_compile_cache_entry_t);

static size_t tid_slot(int32_t tid, size_t capacity) {
  return ((uint32_t)tid * UINT32_C(2654435761)) & (capacity - 1);
}

static void tid_set_grow(lex_tid_set_t *set) {
  size_t new_capacity = set->capacity ? set->capacity * 2 : 16;
  int32_t *slots = core_alloc_array(int32_t, new_capacity);

  for (size_t i = 0; i < set->capacity; i++) {
    int32_t tid = set->slots[i];
    if (!tid) {
      continue;
    }
    size_t slot = tid_slot(tid, new_capacity);
    while (slots[slot]) {
      slot = (slot + 1) & (new_capacity - 1);
    }
    slots[slot] = tid;
  }
  core_free(set->slots);
  set->slots = slots;
  set->capacity = new_capacity;
}

static void tid_set_add(lex_tid_set_t *set, int32_t tid) {
  if (!tid) {
    return;
  }
  if ((set->count + 1) * 2 > set->capacity) {
    tid_set_grow(set);
  }
  size_t slot = tid_slot(tid, set->capacity);
  while (set->slots[slot] && set->slots[slot] != tid) {
    slot = (slot + 1) & (set->capacity - 1);
  }
  if (!set->slots[slot]) {
    set->slots[slot] = tid;
    set->count++;
  }
}

static bool tid_set_contains(const lex_tid_set_t *set, int32_t tid) {
  if (!tid || !set->capacity) {
    return false;
  }
  size_t slot = tid_slot(tid, set->capacity);
  while (set->slots[slot]) {
    if (set->slots[slot] == tid) {
      return true;
    }
    slot = (slot + 1) & (set->capacity - 1);
  }
  return false;
}

static bool id_cont_here(const scan_lex_t *lex, core_codepoint_t c);

static uint32_t trie_child(core_list_t(lex_term_node_t) *nodes,
                           core_list_t(lex_term_edge_t) *edges,
                           uint32_t node, uint8_t byte, bool create) {
  for (int32_t edge = nodes->data[node].edge; edge >= 0;
       edge = edges->data[edge].next_edge) {
    if (edges->data[edge].byte == byte) {
      return edges->data[edge].child;
    }
  }
  if (!create) {
    return UINT32_MAX;
  }

  uint32_t child = (uint32_t)nodes->len;
  int32_t edge = (int32_t)edges->len;
  core_list_push(*nodes, ((lex_term_node_t){.edge = -1}));
  core_list_push(*edges, ((lex_term_edge_t){
      .next_edge = nodes->data[node].edge, .child = child, .byte = byte}));
  nodes->data[node].edge = edge;
  return child;
}

static uint32_t trie_add(core_list_t(lex_term_node_t) *nodes,
                         core_list_t(lex_term_edge_t) *edges, uint32_t root,
                         core_string_t spelling, int32_t value) {
  uint32_t node = root;
  for (size_t i = 0; i < spelling.u8_bytes; i++) {
    node = trie_child(nodes, edges, node, (uint8_t)spelling.data[i], true);
  }
  nodes->data[node].tid = value;
  return node;
}

static void term_trie_add(scan_lex_t *lex, core_string_t spelling,
                          int32_t tid) {
  if (!spelling.data || !spelling.u8_bytes || !tid) {
    return;
  }
  uint32_t node = trie_add(&lex->term_nodes, &lex->term_edges, 0,
                           spelling, tid);
  uint32_t pos = 0;
  core_codepoint_t last = 0;
  while (pos < spelling.u8_bytes) {
    int32_t cp =
        unicode_utf8_decode(spelling.data, (uint32_t)spelling.u8_bytes, &pos);
    last = cp < 0 ? (core_codepoint_t)(uint8_t)spelling.data[pos - 1]
                  : (core_codepoint_t)cp;
  }
  lex->term_nodes.data[node].word_boundary = id_cont_here(lex, last);
}

static void opener_trie_add(scan_lex_t *lex, core_string_t spelling) {
  if (spelling.data && spelling.u8_bytes) {
    trie_add(&lex->opener_nodes, &lex->opener_edges, 0, spelling, 1);
  }
}

static void compile_recipe_openers(scan_lex_t *lex, const void *recipes,
                                   size_t stride, size_t opener_offset) {
  if (!recipes) {
    return;
  }
  for (size_t i = 0;; i++) {
    const char *entry = (const char *)recipes + i * stride;
    const core_string_t *opener =
        (const core_string_t *)(entry + opener_offset);
    if (!opener->data) {
      return;
    }
    opener_trie_add(lex, *opener);
  }
}

typedef struct {
  const void *recipe;
  core_string_t opener;
  size_t order;
} lex_dispatch_entry_t;

static int compare_dispatch_entry(const void *left, const void *right) {
  const lex_dispatch_entry_t *a = left;
  const lex_dispatch_entry_t *b = right;
  uint8_t af = (uint8_t)a->opener.data[0];
  uint8_t bf = (uint8_t)b->opener.data[0];

  if (af != bf) {
    return (af > bf) - (af < bf);
  }
  if (a->opener.u8_bytes != b->opener.u8_bytes) {
    return a->opener.u8_bytes < b->opener.u8_bytes ? 1 : -1;
  }
  int spelling = memcmp(a->opener.data, b->opener.data, a->opener.u8_bytes);
  if (spelling) {
    return spelling;
  }
  return (a->order > b->order) - (a->order < b->order);
}

static void compile_recipe_dispatch(const void *recipes, size_t stride,
                                    size_t opener_offset,
                                    lex_bucket_t buckets[256],
                                    const void ***dispatch_out) {
  if (!recipes) {
    return;
  }

  size_t count = 0;

  for (;;) {
    const char *entry = (const char *)recipes + count * stride;
    const core_string_t *opener =
        (const core_string_t *)(entry + opener_offset);

    if (!opener->data) {
      break;
    }

    if (opener->u8_bytes) {
      buckets[(uint8_t)opener->data[0]].count++;
    }

    count++;
  }

  if (!count) {
    return;
  }

  lex_dispatch_entry_t *entries = core_alloc_array(lex_dispatch_entry_t, count);
  size_t used = 0;
  for (size_t i = 0; i < count; i++) {
    const char *entry = (const char *)recipes + i * stride;
    const core_string_t *opener =
        (const core_string_t *)(entry + opener_offset);
    if (opener->u8_bytes) {
      entries[used++] = (lex_dispatch_entry_t){
          .recipe = entry,
          .opener = *opener,
          .order = i,
      };
    }
  }
  qsort(entries, used, sizeof(*entries), compare_dispatch_entry);

  const void **dispatch = core_alloc_array(const void *, used);
  size_t at = 0;
  for (size_t first = 0; first < 256; first++) {
    buckets[first].start = (uint32_t)at;
    while (at < used && (uint8_t)entries[at].opener.data[0] == first) {
      dispatch[at] = entries[at].recipe;
      at++;
    }
    buckets[first].count = (uint32_t)at - buckets[first].start;
  }
  core_free(entries);

  *dispatch_out = dispatch;
}

static void ascii_set_insert_slot(lex_ascii_slot_t *slots, size_t capacity,
                                  core_string_t key, lex_ascii_set_t bits) {
  size_t slot = (size_t)core_string_hash(key) & (capacity - 1);

  while (slots[slot].key.data) {
    slot = (slot + 1) & (capacity - 1);
  }
  slots[slot] = (lex_ascii_slot_t){.key = key, .bits = bits};
}

static void ascii_sets_grow(scan_lex_t *lex) {
  size_t new_capacity =
      lex->ascii_set_capacity ? lex->ascii_set_capacity * 2 : 16;
  lex_ascii_slot_t *slots = core_alloc_array(lex_ascii_slot_t, new_capacity);

  for (size_t i = 0; i < lex->ascii_set_capacity; i++) {
    if (lex->ascii_sets[i].key.data) {
      ascii_set_insert_slot(slots, new_capacity, lex->ascii_sets[i].key,
                            lex->ascii_sets[i].bits);
    }
  }
  core_free(lex->ascii_sets);
  lex->ascii_sets = slots;
  lex->ascii_set_capacity = new_capacity;
}

static const lex_ascii_set_t *ascii_set_lookup(const scan_lex_t *lex,
                                               core_string_t set) {
  if (!set.data || !lex->ascii_set_capacity) {
    return nullptr;
  }

  size_t slot = (size_t)core_string_hash(set) & (lex->ascii_set_capacity - 1);

  while (lex->ascii_sets[slot].key.data) {
    if (core_string_eq(lex->ascii_sets[slot].key, set)) {
      return &lex->ascii_sets[slot].bits;
    }
    slot = (slot + 1) & (lex->ascii_set_capacity - 1);
  }
  return nullptr;
}

static void compile_ascii_set(scan_lex_t *lex, core_string_t set) {
  if (!set.data || !set.u8_bytes) {
    return;
  }

  if (ascii_set_lookup(lex, set)) {
    return;
  }

  if ((lex->ascii_set_count + 1) * 2 > lex->ascii_set_capacity) {
    ascii_sets_grow(lex);
  }

  lex_ascii_set_t bits = {0};
  for (size_t i = 0; i < set.u8_bytes; i++) {
    uint8_t c = (uint8_t)set.data[i];
    if (c < 64) {
      bits.lo |= UINT64_C(1) << c;
    } else if (c < 128) {
      bits.hi |= UINT64_C(1) << (c - 64);
    }
  }
  ascii_set_insert_slot(lex->ascii_sets, lex->ascii_set_capacity, set, bits);
  lex->ascii_set_count++;
}

static void compile_ascii_sets(scan_lex_t *lex) {
  const scan_lex_spec_t *spec = lex->spec;

  compile_ascii_set(lex, spec->ident_start_extra);
  compile_ascii_set(lex, spec->ident_extra);
  for (const scan_lex_number_form_t *f = spec->number_forms;
       f && f->digits.data; f++) {
    compile_ascii_set(lex, f->digits);
    compile_ascii_set(lex, f->separator);
    compile_ascii_set(lex, f->exponent);
    compile_ascii_set(lex, f->exponent_digits);
    compile_ascii_set(lex, f->sign);
  }
  for (const scan_lex_string_t *str = spec->strings; str && str->opener.data;
       str++) {
    compile_ascii_set(lex, str->simple_escapes);
    for (const scan_lex_escape_t *e = str->escape_rules; e && e->prefix.data;
         e++) {
      compile_ascii_set(lex, e->digits);
    }
  }
  for (const scan_lex_regex_t *r = spec->regexes; r && r->opener.data; r++) {
    compile_ascii_set(lex, r->suffix_chars);
  }
}

struct scan_lex_session_t {
  const scan_lex_t *program;
  scan_lex_session_t *parent;
  scan_lex_session_t *child;
  const scan_lex_embed_t *parent_embed;
  size_t embed_depth;
  size_t embed_start;
  core_list_t(lex_mode_t) modes;
  core_list_t(int32_t) indents;
  core_list_t(int32_t) layout_blocks;
  int32_t pending_dedents;
  bool pending_dedent_mismatch;
  bool pending_block_newline;
  bool pending_layout_block;

  int32_t bracket_depth;
  bool line_start;
  bool continued_line;
  bool line_has_token;
  bool sent_eof;
  bool outside;

  bool pending_indent;
  size_t pending_indent_end;
  size_t pending_indent_first_tab;
  int32_t pending_indent_column;
  bool pending_indent_has_space;
  bool pending_indent_has_tab;

  const scan_lex_directive_t *in_directive;
  const scan_lex_comment_t *pending_comment;
  int32_t pending_comment_depth;
  uint32_t pending_comment_fill_level;
  const scan_lex_string_t *pending_string;
  size_t pending_string_scalars;
  size_t pending_string_delimiter_depth;
  bool pending_string_tail;
  const scan_lex_regex_t *pending_regex;
  bool pending_regex_in_class;
  const scan_lex_fence_t *pending_fence;
  uint32_t pending_fence_level;
  size_t pending_fence_content;
  size_t pending_fence_label_start;
  size_t pending_fence_label_end;
  bool pending_fence_open;
  bool pending_fence_fill;
  core_string_t pending_fence_pattern;
  size_t *pending_fence_prefix;
  size_t pending_fence_search;
  size_t pending_fence_match;
  size_t *fence_probe_skip_until;
  const scan_lex_heredoc_t *pending_heredoc;
  size_t pending_heredoc_tag_start;
  size_t pending_heredoc_tag_end;
  size_t pending_heredoc_body;
  size_t pending_heredoc_scan;
  size_t pending_heredoc_line_start;
  size_t pending_heredoc_match;
  bool pending_heredoc_indent;
  bool pending_heredoc_rejected;
  uint8_t pending_heredoc_quote;
  uint8_t pending_heredoc_quote_kind;
  lex_heredoc_header_phase_t pending_heredoc_header_phase;
  size_t pending_heredoc_header_scan;
  size_t pending_heredoc_token_end;
  core_list_t(lex_heredoc_queue_t) heredoc_queue;
  bool heredoc_queue_ready;
  bool heredoc_queue_scanning;
  bool pending_identifier;
  const scan_lex_sigil_t *pending_attached_sigil;
  int32_t pending_identifier_tid;
  bool pending_identifier_digits;
  bool pending_terminal;
  uint32_t pending_terminal_node;
  size_t pending_terminal_consumed;
  size_t pending_terminal_best_len;
  int32_t pending_terminal_best_tid;
  size_t pending_terminal_min_len;
  lex_number_phase_t pending_number_phase;
  const scan_lex_number_form_t *pending_number_form;
  size_t pending_number_prefix_at;
  uint32_t pending_number_prefix_node;
  size_t pending_number_prefix_consumed;
  int32_t pending_number_prefix_best;
  uint32_t pending_number_suffix_node;
  size_t pending_number_suffix_consumed;
  int32_t pending_number_suffix_best;
  size_t pending_number_digits;
  size_t pending_number_exp_digits;
  bool pending_number_after_digit;
  bool pending_number_need_digit;
  bool pending_number_malformed;
  bool pending_number_float;
  bool pending_number_point;
  bool pending_number_signed_only;
  bool pending_opener;
  uint32_t pending_opener_node;
  size_t pending_opener_consumed;

  const scan_lex_embed_t *pending_embed;
  size_t pending_embed_content;
  size_t pending_embed_search;
  size_t pending_embed_match;
  size_t *pending_embed_prefix;

  size_t skip_from;
  size_t skip_to;
  core_string_t hook_diagnostic;
  const lex_recovery_t *pending_recovery;
  core_string_t pending_recovery_message;
  core_string_t recovery_diagnostic;
  parse_token_info_t *pending_recovery_token;
  size_t pending_recovery_start;
  size_t pending_recovery_end;
};

static void mark_literal(scan_lex_t *lex, int32_t tid) {
  if (tid == PARSE_TOK_IGNORED) {
    return;
  }

  tid_set_add(&lex->is_literal, tid);
}

static bool is_literal_tid(const scan_lex_t *lex, int32_t tid) {
  return tid_set_contains(&lex->is_literal, tid);
}

static int literal_hook_compare(const void *left, const void *right) {
  const lex_literal_hook_t *a = left;
  const lex_literal_hook_t *b = right;

  return (a->tid > b->tid) - (a->tid < b->tid);
}

static const lex_literal_hook_t *literal_hook_for(const scan_lex_t *lex,
                                                  int32_t tid) {
  size_t lo = 0;
  size_t hi = lex->literal_hook_count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int32_t have = lex->literal_hooks[mid].tid;

    if (have < tid) {
      lo = mid + 1;
    } else if (have > tid) {
      hi = mid;
    } else {
      return &lex->literal_hooks[mid];
    }
  }
  return nullptr;
}

static int recovery_compare(const void *left, const void *right) {
  const lex_recovery_t *a = left;
  const lex_recovery_t *b = right;

  return (a->tid > b->tid) - (a->tid < b->tid);
}

static const lex_recovery_t *recovery_for(const scan_lex_t *lex, int32_t tid) {
  size_t lo = 0;
  size_t hi = lex->recovery_count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int32_t have = lex->recoveries[mid].tid;

    if (have < tid) {
      lo = mid + 1;
    } else if (have > tid) {
      hi = mid;
    } else {
      return &lex->recoveries[mid];
    }
  }
  return nullptr;
}

static int32_t tid_for(parse_grammar_t *g, core_string_t name,
                       int32_t fallback) {
  if (!name.data || !name.u8_bytes) {
    return fallback;
  }

  int64_t id = parse_terminal_id(g, name);

  return id ? (int32_t)id : fallback;
}

static scan_lex_t *scan_lex_compile_base(const scan_lex_spec_t *spec,
                                         parse_grammar_t *g) {
  if (!spec || !g) {
    return nullptr;
  }
  scan_lex_issue_t *issues = nullptr;
  size_t issue_count = 0;
  core_string_t analysis_error = {};
  bool analyzed = scan_lex_analyze_spec(spec, g, &analysis_error, &issues,
                                        &issue_count);
  lex_compile_error_set(analysis_error);
  if (!analyzed) {
    return nullptr;
  }

  scan_lex_t *lex = core_alloc(scan_lex_t);

  lex->spec = spec;
  lex->g = g;
  lex->refs = 1;
  lex->issues = issues;
  lex->issue_count = issue_count;
  lex->term_nodes = core_list_new(lex_term_node_t);
  lex->term_edges = core_list_new(lex_term_edge_t);
  lex->opener_nodes = core_list_new(lex_term_node_t);
  lex->opener_edges = core_list_new(lex_term_edge_t);
  lex->string_nodes = core_list_new(lex_term_node_t);
  lex->string_edges = core_list_new(lex_term_edge_t);
  lex->number_nodes = core_list_new(lex_term_node_t);
  lex->number_edges = core_list_new(lex_term_edge_t);
  lex->number_suffix_nodes = core_list_new(lex_term_node_t);
  lex->number_suffix_edges = core_list_new(lex_term_edge_t);
  lex->number_suffix_roots = core_list_new(int32_t);
  core_list_push(lex->term_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  core_list_push(lex->opener_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  core_list_push(lex->string_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  core_list_push(lex->number_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  compile_ascii_sets(lex);
  compile_recipe_openers(lex, spec->comments, sizeof(scan_lex_comment_t),
                         offsetof(scan_lex_comment_t, opener));
  compile_recipe_openers(lex, spec->strings, sizeof(scan_lex_string_t),
                         offsetof(scan_lex_string_t, opener));
  compile_recipe_openers(lex, spec->sigils, sizeof(scan_lex_sigil_t),
                         offsetof(scan_lex_sigil_t, prefix));
  compile_recipe_openers(lex, spec->regexes, sizeof(scan_lex_regex_t),
                         offsetof(scan_lex_regex_t, opener));
  compile_recipe_openers(lex, spec->heredocs, sizeof(scan_lex_heredoc_t),
                         offsetof(scan_lex_heredoc_t, opener));
  compile_recipe_openers(lex, spec->fences, sizeof(scan_lex_fence_t),
                         offsetof(scan_lex_fence_t, outer_open));
  compile_recipe_openers(lex, spec->directives, sizeof(scan_lex_directive_t),
                         offsetof(scan_lex_directive_t, open));
  lex->ident_enabled = spec->tok_ident.data || spec->tok_ident_upper.data ||
                       spec->ident_start_extra.data || spec->ident_extra.data ||
                       spec->identifier_profile.name.data;
  if (spec->identifier_profile.name.data) {
    core_string_t profile_error = {};
    lex->identifier_profile = scan_unicode_profile_compile(
        &spec->identifier_profile, &profile_error);
    if (!lex->identifier_profile) {
      lex_compile_error_set(profile_error);
      scan_lex_free(lex);
      return nullptr;
    }
  }
  lex->number_enabled = spec->number_forms != nullptr;

  compile_recipe_dispatch(spec->comments, sizeof(scan_lex_comment_t),
                          offsetof(scan_lex_comment_t, opener),
                          lex->comment_buckets, &lex->comment_dispatch);
  for (const scan_lex_string_t *s = spec->strings; s && s->opener.data; s++) {
    lex->string_count++;
    trie_add(&lex->string_nodes, &lex->string_edges, 0, s->opener,
             (int32_t)lex->string_count);
  }
  if (lex->string_count) {
    lex->string_escape_dispatch =
        core_alloc_array(const scan_lex_escape_t *, lex->string_count * 128);
    lex->string_escape_complex = core_alloc_array(bool, lex->string_count);
    size_t string_index = 0;
    for (const scan_lex_string_t *s = spec->strings; s && s->opener.data;
         s++, string_index++) {
      for (const scan_lex_escape_t *rule = s->escape_rules;
           rule && rule->prefix.data; rule++) {
        if (rule->prefix.u8_bytes == 1 &&
            (unsigned char)rule->prefix.data[0] < 128) {
          size_t slot =
              string_index * 128 + (unsigned char)rule->prefix.data[0];
          if (lex->string_escape_dispatch[slot]) {
            lex->string_escape_complex[string_index] = true;
          }
          lex->string_escape_dispatch[slot] = rule;
        } else {
          lex->string_escape_complex[string_index] = true;
        }
      }
    }
  }
  for (const scan_lex_number_form_t *f = spec->number_forms;
       f && f->digits.data; f++) {
    lex->number_form_count++;
    trie_add(&lex->number_nodes, &lex->number_edges, 0, f->prefix,
             (int32_t)lex->number_form_count);

    uint32_t root = (uint32_t)lex->number_suffix_nodes.len;
    core_list_push(lex->number_suffix_nodes,
                   ((lex_term_node_t){.edge = -1, .tid = 0}));
    core_list_push(lex->number_suffix_roots, (int32_t)root);
    int32_t suffix_index = 0;
    for (const scan_lex_suffix_t *suffix = f->suffixes;
         suffix && suffix->suffix.data; suffix++) {
      suffix_index++;
      trie_add(&lex->number_suffix_nodes, &lex->number_suffix_edges,
               root, suffix->suffix, suffix_index);
    }
  }
  compile_recipe_dispatch(spec->sigils, sizeof(scan_lex_sigil_t),
                          offsetof(scan_lex_sigil_t, prefix),
                          lex->sigil_buckets, &lex->sigil_dispatch);
  for (const scan_lex_sigil_t *sigil = spec->sigils;
       sigil && sigil->prefix.data; sigil++) {
    lex->sigil_count++;
  }
  if (lex->sigil_count) {
    lex->sigil_attach_after = core_alloc_array(lex_tid_set_t,
                                                lex->sigil_count);
    for (size_t i = 0; i < lex->sigil_count; i++) {
      const scan_lex_sigil_t *sigil = &spec->sigils[i];
      for (const core_string_t *name = sigil->attach_after;
           name && name->data; name++) {
        int64_t tid = parse_terminal_id(g, *name);
        if (!tid && name->u8_bytes == 1) {
          tid = (unsigned char)name->data[0];
        }
        if (tid) {
          tid_set_add(&lex->sigil_attach_after[i], (int32_t)tid);
        }
      }
    }
  }
  compile_recipe_dispatch(spec->regexes, sizeof(scan_lex_regex_t),
                          offsetof(scan_lex_regex_t, opener),
                          lex->regex_buckets, &lex->regex_dispatch);
  for (const scan_lex_regex_t *r = spec->regexes; r && r->opener.data; r++) {
    lex->regex_count++;
  }
  if (lex->regex_count) {
    lex->regex_not_after = core_alloc_array(lex_tid_set_t, lex->regex_count);

    size_t regex_index = 0;
    for (const scan_lex_regex_t *r = spec->regexes; r && r->opener.data;
         r++, regex_index++) {
      lex_tid_set_t *set = &lex->regex_not_after[regex_index];
      for (const core_string_t *name = r->not_after; name && name->data;
           name++) {
        int64_t tid = parse_terminal_id(g, *name);

        if (!tid && name->u8_bytes == 1) {
          tid = (unsigned char)name->data[0];
        }
        if (tid) {
          tid_set_add(set, (int32_t)tid);
        }
      }
    }
  }
  compile_recipe_dispatch(spec->heredocs, sizeof(scan_lex_heredoc_t),
                          offsetof(scan_lex_heredoc_t, opener),
                          lex->heredoc_buckets, &lex->heredoc_dispatch);
  compile_recipe_dispatch(spec->embeds, sizeof(scan_lex_embed_t),
                          offsetof(scan_lex_embed_t, open), lex->embed_buckets,
                          &lex->embed_dispatch);
  compile_recipe_dispatch(spec->embeds, sizeof(scan_lex_embed_t),
                          offsetof(scan_lex_embed_t, close),
                          lex->embed_close_buckets, &lex->embed_close_dispatch);
  for (const scan_lex_embed_t *e = spec->embeds; e && e->open.data; e++) {
    lex->embed_count++;
  }
  if (lex->embed_count) {
    lex->embed_programs = core_alloc_array(scan_lex_t *, lex->embed_count);
  }
  compile_recipe_dispatch(spec->fences, sizeof(scan_lex_fence_t),
                          offsetof(scan_lex_fence_t, outer_open),
                          lex->fence_buckets, &lex->fence_dispatch);
  for (const scan_lex_fence_t *f = spec->fences; f && f->outer_open.data; f++) {
    lex->fence_count++;
  }

  for (size_t i = 0; i < core_list_len(g->rules); i++) {
    const parse_rule_t *rule = &g->rules.data[i];

    for (size_t j = 0; j < core_list_len(rule->contents); j++) {
      const parse_match_t *match = &rule->contents.data[j];

      if (match->kind != PARSE_MATCH_TERMINAL || match->terminal_id <= 0 ||
          match->terminal_id >= PARSE_TOK_START_ID) {
        continue;
      }

      tid_set_add(&lex->codepoint_terminals, (int32_t)match->terminal_id);
    }
  }

  for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
    core_string_t v = g->named_terms.data[i].value;

    if (!v.data || !v.u8_bytes) {
      continue;
    }
    if (parse_contextual_source_tid(g, g->named_terms.data[i].id)) {
      continue;
    }

    unsigned char first = (unsigned char)v.data[0];

    if (v.u8_bytes > lex->opwidth[first]) {
      lex->opwidth[first] = v.u8_bytes;
    }

    term_trie_add(lex, v, g->named_terms.data[i].id);
  }

  for (const core_string_t *op = spec->operators; op && op->data; op++) {
    if (!op->u8_bytes) {
      continue;
    }

    unsigned char first = (unsigned char)op->data[0];

    if (op->u8_bytes > lex->opwidth[first]) {
      lex->opwidth[first] = op->u8_bytes;
    }

    term_trie_add(lex, *op, (int32_t)parse_lookup_terminal(g, *op));
  }

  lex->tid_newline = tid_for(g, spec->tok_newline, PARSE_TOK_IGNORED);
  lex->tid_indent = tid_for(g, spec->tok_indent, PARSE_TOK_IGNORED);
  lex->tid_dedent = tid_for(g, spec->tok_dedent, PARSE_TOK_IGNORED);
  lex->tid_eof = tid_for(g, spec->tok_eof, PARSE_TOK_IGNORED);
  lex->tid_ident = tid_for(g, spec->tok_ident, PARSE_TOK_IDENTIFIER);
  lex->tid_terminator = tid_for(g, spec->tok_terminator, PARSE_TOK_IGNORED);
  lex->tid_ident_upper = tid_for(g, spec->tok_ident_upper, PARSE_TOK_IGNORED);

  mark_literal(lex, lex->tid_ident);
  mark_literal(lex, lex->tid_ident_upper);

  for (const scan_lex_string_t *st = spec->strings; st && st->opener.data;
       st++) {
    int32_t ids[] = {
        tid_for(g, st->tok, PARSE_TOK_IGNORED),
        tid_for(g, st->tok_end, PARSE_TOK_IGNORED),
    };

    for (size_t i = 0; i < 2; i++) {
      mark_literal(lex, ids[i]);
    }
  }

  for (const scan_lex_fence_t *f = spec->fences; f && f->outer_open.data; f++) {
    mark_literal(lex, tid_for(g, f->tok, PARSE_TOK_IGNORED));
  }

  for (const scan_lex_number_form_t *f = spec->number_forms;
       f && f->digits.data; f++) {
    mark_literal(lex, tid_for(g, f->tok_int, PARSE_TOK_IGNORED));
    mark_literal(lex, tid_for(g, f->tok_float, PARSE_TOK_IGNORED));
    for (const scan_lex_suffix_t *sf = f->suffixes; sf && sf->suffix.data;
         sf++) {
      mark_literal(lex, tid_for(g, sf->tok, PARSE_TOK_IGNORED));
    }
  }

  core_string_t after = spec->terminator_after;

  for (size_t at = 0; after.data && at < after.u8_bytes;) {
    size_t end = at;

    while (end < after.u8_bytes && after.data[end] != ' ') {
      end++;
    }

    if (end > at) {
      core_list_push(lex->term_after,
                     core_string_borrow(after.data + at, end - at));
    }

    at = end + 1;
  }

  for (const scan_lex_literal_hook_t *hook = spec->literal_hooks;
       hook && hook->token.data; hook++) {
    lex->literal_hook_count++;
  }
  if (lex->literal_hook_count) {
    lex->literal_hooks =
        core_alloc_array(lex_literal_hook_t, lex->literal_hook_count);

    size_t index = 0;
    for (const scan_lex_literal_hook_t *hook = spec->literal_hooks;
         hook && hook->token.data; hook++, index++) {
      int32_t tid = tid_for(g, hook->token, PARSE_TOK_IGNORED);
      if (tid == PARSE_TOK_IGNORED ||
          !scan_lex_literal_hook_exists(hook->hook)) {
        if (tid == PARSE_TOK_IGNORED) {
          lex_compile_error_printf(
                   "@lex-literal-hook: unknown input token '%.*s'",
                   (int)hook->token.u8_bytes, hook->token.data);
        } else {
          lex_compile_error_printf(
                   "@lex-literal-hook: unknown hook '%.*s'",
                   (int)hook->hook.u8_bytes, hook->hook.data);
        }
        scan_lex_free(lex);
        return nullptr;
      }
      for (const core_string_t *name = hook->outputs; name && name->data;
           name++) {
        if (!parse_terminal_id(g, *name)) {
          lex_compile_error_printf(
                   "@lex-literal-hook: unknown output token '%.*s'",
                   (int)name->u8_bytes, name->data);
          scan_lex_free(lex);
          return nullptr;
        }
      }
      lex->literal_hooks[index] = (lex_literal_hook_t){
          .recipe = hook,
          .tid = tid,
      };
    }
    qsort(lex->literal_hooks, lex->literal_hook_count,
          sizeof(*lex->literal_hooks), literal_hook_compare);
    for (size_t i = 1; i < lex->literal_hook_count; i++) {
      if (lex->literal_hooks[i - 1].tid == lex->literal_hooks[i].tid) {
        const scan_lex_literal_hook_t *hook = lex->literal_hooks[i].recipe;
        lex_compile_error_printf(
                 "@lex-literal-hook: duplicate input token '%.*s'",
                 (int)hook->token.u8_bytes, hook->token.data);
        scan_lex_free(lex);
        return nullptr;
      }
    }
  }

  for (const scan_lex_recovery_t *recovery = spec->recoveries;
       recovery && recovery->token.data; recovery++) {
    lex->recovery_count++;
  }
  if (lex->recovery_count) {
    lex->recoveries = core_alloc_array(lex_recovery_t, lex->recovery_count);
    size_t index = 0;

    for (const scan_lex_recovery_t *recovery = spec->recoveries;
         recovery && recovery->token.data; recovery++, index++) {
      int64_t tid = parse_terminal_id(g, recovery->token);
      int64_t replacement = parse_terminal_id(g, recovery->replacement);

      if (!tid || !replacement) {
        core_string_t unknown = tid ? recovery->replacement : recovery->token;
        lex_compile_error_printf(
                 "@lex-recover: unknown token '%.*s'", (int)unknown.u8_bytes,
                 unknown.data);
        scan_lex_free(lex);
        return nullptr;
      }
      for (const core_string_t *sync = recovery->synchronize;
           sync && sync->data; sync++) {
        if (!sync->u8_bytes) {
          lex_compile_error_printf(
                   "@lex-recover: synchronization delimiters "
                   "cannot be empty");
          scan_lex_free(lex);
          return nullptr;
        }
      }
      lex->recoveries[index] = (lex_recovery_t){
          .recipe = recovery,
          .tid = (int32_t)tid,
          .replacement_tid = (int32_t)replacement,
      };
    }
    qsort(lex->recoveries, lex->recovery_count, sizeof(*lex->recoveries),
          recovery_compare);
    for (size_t i = 1; i < lex->recovery_count; i++) {
      if (lex->recoveries[i - 1].tid == lex->recoveries[i].tid) {
        const scan_lex_recovery_t *recovery = lex->recoveries[i].recipe;
        lex_compile_error_printf(
                 "@lex-recover: duplicate input token '%.*s'",
                 (int)recovery->token.u8_bytes, recovery->token.data);
        scan_lex_free(lex);
        return nullptr;
      }
    }
  }

  return lex;
}

typedef struct {
  core_list_t(core_string_t) stack;
  core_list_t(lex_compile_cache_entry_t) cache;
} lex_compile_context_t;

core_string_t scan_lex_compile_error(void) { return lex_compile_error; }

void scan_lex_compile_error_reset(void) {
  lex_compile_error_set((core_string_t){});
}

static scan_lex_t *scan_lex_compile_tree(const scan_lex_spec_t *spec,
                                         parse_grammar_t *g,
                                         lex_compile_context_t *context) {
  for (size_t i = 0; i < core_list_len(context->stack); i++) {
    if (core_string_eq(context->stack.data[i], spec->name)) {
      lex_compile_error_printf(
               "embedded lexer cycle reaches '%.*s'", (int)spec->name.u8_bytes,
               spec->name.data);
      return nullptr;
    }
  }

  for (size_t i = 0; i < core_list_len(context->cache); i++) {
    lex_compile_cache_entry_t *entry = &context->cache.data[i];

    if (entry->spec == spec) {
      entry->program->refs++;
      return entry->program;
    }
  }

  core_list_push(context->stack, spec->name);
  scan_lex_t *lex = scan_lex_compile_base(spec, g);

  if (!lex) {
    return nullptr;
  }

  core_list_push(context->cache, ((lex_compile_cache_entry_t){
                                     .spec = spec,
                                     .program = lex,
                                 }));

  size_t index = 0;

  for (const scan_lex_embed_t *e = spec->embeds; e && e->open.data;
       e++, index++) {
    if (!e->lexer.data) {
      continue;
    }

    if (!e->close.data || !e->close.u8_bytes) {
      lex_compile_error_printf(
               "@lex-embed: a delegated embed requires a closing delimiter");
      scan_lex_free(lex);
      return nullptr;
    }
    if (e->tokens && (!e->open_tok.data || !e->close_tok.data)) {
      lex_compile_error_printf(
               "@lex-embed: tokens mode requires open_as() and close_as()");
      scan_lex_free(lex);
      return nullptr;
    }
    if (!e->tokens && !e->tok.data) {
      lex_compile_error_printf(
               "@lex-embed: single-token mode requires as()");
      scan_lex_free(lex);
      return nullptr;
    }

    const scan_lex_spec_t *child = scan_lex_for(e->lexer);

    if (!child) {
      lex_compile_error_printf(
               "@lex-embed: unknown lexer '%.*s'", (int)e->lexer.u8_bytes,
               e->lexer.data);
      scan_lex_free(lex);
      return nullptr;
    }

    lex->embed_programs[index] = scan_lex_compile_tree(child, g, context);

    if (!lex->embed_programs[index]) {
      scan_lex_free(lex);
      return nullptr;
    }
  }

  core_list_pop(core_string_t, context->stack);
  return lex;
}

scan_lex_t *scan_lex_compile(const scan_lex_spec_t *spec, parse_grammar_t *g) {
  scan_lex_compile_error_reset();

  if (spec && g) {
    parse_grammar_prepare_contextual(g, spec->soft_keywords, spec->tok_ident);
  }

  lex_compile_context_t context = {
      .stack = core_list_new(core_string_t),
      .cache = core_list_new(lex_compile_cache_entry_t),
  };

  scan_lex_t *result =
      spec && g ? scan_lex_compile_tree(spec, g, &context) : nullptr;

  core_list_free(context.stack);
  core_list_free(context.cache);
  return result;
}

static bool ends_a_statement(const scan_lex_t *lex, parse_scanner_t *s) {
  parse_token_info_t *prev = scan_prev_token(s);

  if (!prev) {
    return false;
  }

  if (is_literal_tid(lex, parse_token_tid(prev))) {
    return true;
  }

  core_option_t(core_string_t) prev_value = parse_token_value(prev);

  if (core_option_is_set(prev_value)) {
    core_string_t v = core_option_get(prev_value);

    if (v.u8_bytes == 1 &&
        (v.data[0] == ')' || v.data[0] == ']' || v.data[0] == '}')) {
      return true;
    }

    for (size_t i = 0; i < lex->term_after.len; i++) {
      if (core_string_eq(lex->term_after.data[i], v)) {
        return true;
      }
    }
  }

  return false;
}

void scan_lex_free(scan_lex_t *lex) {
  if (!lex) {
    return;
  }

  if (--lex->refs) {
    return;
  }

  for (size_t i = 0; i < lex->embed_count; i++) {
    scan_lex_free(lex->embed_programs[i]);
  }
  core_free(lex->embed_programs);

  core_free(lex->is_literal.slots);

  core_free(lex->literal_hooks);
  core_free(lex->recoveries);
  for (size_t i = 0; i < lex->issue_count; i++) {
    core_free(lex->issues[i].token.data);
    core_free(lex->issues[i].origin.data);
  }
  core_free(lex->issues);
  scan_unicode_profile_free(lex->identifier_profile);

  core_free(lex->codepoint_terminals.slots);

  core_free(lex->ascii_sets);

  core_list_free(lex->term_after);
  core_list_free(lex->term_nodes);
  core_list_free(lex->term_edges);
  core_list_free(lex->opener_nodes);
  core_list_free(lex->opener_edges);
  core_list_free(lex->string_nodes);
  core_list_free(lex->string_edges);
  core_list_free(lex->number_nodes);
  core_list_free(lex->number_edges);
  core_list_free(lex->number_suffix_nodes);
  core_list_free(lex->number_suffix_edges);
  core_list_free(lex->number_suffix_roots);
  core_free(lex->comment_dispatch);
  core_free(lex->sigil_dispatch);
  for (size_t i = 0; i < lex->sigil_count; i++) {
    core_free(lex->sigil_attach_after[i].slots);
  }
  core_free(lex->sigil_attach_after);
  core_free(lex->regex_dispatch);
  for (size_t i = 0; i < lex->regex_count; i++) {
    core_free(lex->regex_not_after[i].slots);
  }
  core_free(lex->regex_not_after);
  core_free(lex->heredoc_dispatch);
  core_free(lex->embed_dispatch);
  core_free(lex->embed_close_dispatch);
  core_free(lex->fence_dispatch);
  core_free(lex->string_escape_dispatch);
  core_free(lex->string_escape_complex);
  core_free(lex);
}

scan_lex_session_t *scan_lex_session_new(const scan_lex_t *program) {
  if (!program) {
    return nullptr;
  }

  scan_lex_session_t *session = core_alloc(scan_lex_session_t);

  session->program = program;
  session->line_start = true;
  for (const scan_lex_embed_t *e = program->spec->embeds; e && e->open.data;
       e++) {
    if (!e->lexer.data) {
      session->outside = true;
      break;
    }
  }
  if (program->fence_count) {
    session->fence_probe_skip_until =
        core_alloc_array(size_t, program->fence_count);
  }

  return session;
}

static parse_token_info_t *
clone_pending_recovery_token(const parse_token_info_t *source) {
  return parse_token_copy(source, true);
}

static scan_lex_session_t *
scan_lex_session_clone_frame(const scan_lex_session_t *source,
                             scan_lex_session_t *parent) {
  if (!source) {
    return nullptr;
  }

  scan_lex_session_t *copy = core_alloc(scan_lex_session_t);
  *copy = *source;
  copy->parent = parent;
  copy->child = nullptr;
  copy->modes = (core_list_t(lex_mode_t)){0};
  copy->indents = (core_list_t(int32_t)){0};
  copy->layout_blocks = (core_list_t(int32_t)){0};
  copy->heredoc_queue = (core_list_t(lex_heredoc_queue_t)){0};
  copy->pending_fence_pattern = (core_string_t){0};
  copy->pending_fence_prefix = nullptr;
  copy->pending_embed_prefix = nullptr;
  copy->fence_probe_skip_until = nullptr;
  copy->hook_diagnostic = (core_string_t){0};
  copy->pending_recovery_message = (core_string_t){0};
  copy->recovery_diagnostic = (core_string_t){0};
  copy->pending_recovery_token = nullptr;

  if (source->modes.len) {
    copy->modes = core_list_new_cap(lex_mode_t, source->modes.len);
    memcpy(copy->modes.data, source->modes.data,
           source->modes.len * sizeof(*copy->modes.data));
    copy->modes.len = source->modes.len;
  }
  if (source->indents.len) {
    copy->indents = core_list_new_cap(int32_t, source->indents.len);
    memcpy(copy->indents.data, source->indents.data,
           source->indents.len * sizeof(*copy->indents.data));
    copy->indents.len = source->indents.len;
  }
  if (source->layout_blocks.len) {
    copy->layout_blocks = core_list_new_cap(int32_t, source->layout_blocks.len);
    memcpy(copy->layout_blocks.data, source->layout_blocks.data,
           source->layout_blocks.len * sizeof(*copy->layout_blocks.data));
    copy->layout_blocks.len = source->layout_blocks.len;
  }
  if (source->heredoc_queue.len) {
    copy->heredoc_queue =
        core_list_new_cap(lex_heredoc_queue_t, source->heredoc_queue.len);
    for (size_t i = 0; i < source->heredoc_queue.len; i++) {
      copy->heredoc_queue.data[i] = source->heredoc_queue.data[i];
      copy->heredoc_queue.data[i].tag =
          core_string_dup(source->heredoc_queue.data[i].tag);
    }
    copy->heredoc_queue.len = source->heredoc_queue.len;
  }
  if (source->pending_fence_pattern.u8_bytes) {
    copy->pending_fence_pattern =
        core_string_dup(source->pending_fence_pattern);
    copy->pending_fence_prefix =
        core_alloc_array(size_t, source->pending_fence_pattern.u8_bytes);
    memcpy(copy->pending_fence_prefix, source->pending_fence_prefix,
           source->pending_fence_pattern.u8_bytes *
               sizeof(*copy->pending_fence_prefix));
  }
  if (source->pending_embed && source->pending_embed_prefix) {
    size_t n = source->pending_embed->close.u8_bytes;
    copy->pending_embed_prefix = core_alloc_array(size_t, n);
    memcpy(copy->pending_embed_prefix, source->pending_embed_prefix,
           n * sizeof(*copy->pending_embed_prefix));
  }
  if (source->program->fence_count) {
    copy->fence_probe_skip_until =
        core_alloc_array(size_t, source->program->fence_count);
    memcpy(copy->fence_probe_skip_until, source->fence_probe_skip_until,
           source->program->fence_count *
               sizeof(*copy->fence_probe_skip_until));
  }
  if (source->hook_diagnostic.data) {
    copy->hook_diagnostic = core_string_dup(source->hook_diagnostic);
  }
  if (source->pending_recovery_message.data) {
    copy->pending_recovery_message =
        core_string_dup(source->pending_recovery_message);
  }
  if (source->recovery_diagnostic.data) {
    copy->recovery_diagnostic = core_string_dup(source->recovery_diagnostic);
  }
  copy->pending_recovery_token =
      clone_pending_recovery_token(source->pending_recovery_token);
  copy->child = scan_lex_session_clone_frame(source->child, copy);
  return copy;
}

void *scan_lex_session_clone(const void *state) {
  const scan_lex_session_t *current = state;
  if (!current) {
    return nullptr;
  }

  size_t depth = 0;
  const scan_lex_session_t *root = current;
  while (root->parent) {
    root = root->parent;
    depth++;
  }

  scan_lex_session_t *copy = scan_lex_session_clone_frame(root, nullptr);
  scan_lex_session_t *result = copy;
  for (size_t i = 0; i < depth; i++) {
    result = result->child;
  }
  return result;
}

static void normalize_session_for_comparison(scan_lex_session_t *session) {
  session->parent = nullptr;
  session->child = nullptr;
  session->modes = (core_list_t(lex_mode_t)){0};
  session->indents = (core_list_t(int32_t)){0};
  session->layout_blocks = (core_list_t(int32_t)){0};
  session->heredoc_queue = (core_list_t(lex_heredoc_queue_t)){0};
  session->pending_fence_pattern.data = nullptr;
  session->pending_fence_prefix = nullptr;
  session->pending_embed_prefix = nullptr;
  session->fence_probe_skip_until = nullptr;
  session->hook_diagnostic = (core_string_t){0};
  session->pending_recovery_message = (core_string_t){0};
  session->recovery_diagnostic = (core_string_t){0};
  session->pending_recovery_token = nullptr;

  session->embed_start = 0;
  session->pending_indent_end = 0;
  session->pending_indent_first_tab = 0;
  session->pending_fence_content = 0;
  session->pending_fence_label_start = 0;
  session->pending_fence_label_end = 0;
  session->pending_fence_search = 0;
  session->pending_heredoc_tag_start = 0;
  session->pending_heredoc_tag_end = 0;
  session->pending_heredoc_body = 0;
  session->pending_heredoc_scan = 0;
  session->pending_heredoc_line_start = 0;
  session->pending_heredoc_header_scan = 0;
  session->pending_heredoc_token_end = 0;
  session->pending_number_prefix_at = 0;
  session->pending_embed_content = 0;
  session->pending_embed_search = 0;
  session->skip_from = 0;
  session->skip_to = 0;
}

static bool scan_lex_session_equal_frame(const scan_lex_session_t *left,
                                         const scan_lex_session_t *right) {
  if (!left || !right) {
    return left == right;
  }

  scan_lex_session_t a = *left;
  scan_lex_session_t b = *right;
  normalize_session_for_comparison(&a);
  normalize_session_for_comparison(&b);
  if (memcmp(&a, &b, sizeof(a))) {
    return false;
  }
  if (!!left->pending_recovery_token != !!right->pending_recovery_token) {
    return false;
  }
  if (left->pending_recovery_token) {
    const parse_token_info_t *x = left->pending_recovery_token;
    const parse_token_info_t *y = right->pending_recovery_token;
    core_option_t(core_string_t) x_option = parse_token_value(x);
    core_option_t(core_string_t) y_option = parse_token_value(y);
    bool x_value = core_option_is_set(x_option);
    bool y_value = core_option_is_set(y_option);

    if (parse_token_tid(x) != parse_token_tid(y)
        || parse_token_start_byte(x) != parse_token_start_byte(y)
        || parse_token_end_byte(x) != parse_token_end_byte(y)
        || x_value != y_value
        || (x_value && !core_string_eq(core_option_get(x_option),
                                       core_option_get(y_option)))) {
      return false;
    }
  }
  if (left->modes.len != right->modes.len ||
      (left->modes.len &&
       memcmp(left->modes.data, right->modes.data,
              left->modes.len * sizeof(*left->modes.data)))) {
    return false;
  }
  if (left->indents.len != right->indents.len ||
      (left->indents.len &&
       memcmp(left->indents.data, right->indents.data,
              left->indents.len * sizeof(*left->indents.data)))) {
    return false;
  }
  if (left->layout_blocks.len != right->layout_blocks.len ||
      (left->layout_blocks.len &&
       memcmp(left->layout_blocks.data, right->layout_blocks.data,
              left->layout_blocks.len * sizeof(*left->layout_blocks.data)))) {
    return false;
  }
  if (left->heredoc_queue.len != right->heredoc_queue.len) {
    return false;
  }
  for (size_t i = 0; i < left->heredoc_queue.len; i++) {
    const lex_heredoc_queue_t *x = &left->heredoc_queue.data[i];
    const lex_heredoc_queue_t *y = &right->heredoc_queue.data[i];
    if (x->recipe != y->recipe || x->token_index != y->token_index ||
        x->quote_kind != y->quote_kind || !core_string_eq(x->tag, y->tag)) {
      return false;
    }
  }
  if (!core_string_eq(left->pending_fence_pattern,
                      right->pending_fence_pattern)) {
    return false;
  }
  return scan_lex_session_equal_frame(left->child, right->child);
}

bool scan_lex_session_equal(const void *left, const void *right) {
  const scan_lex_session_t *a = left;
  const scan_lex_session_t *b = right;

  while (a && a->parent) {
    a = a->parent;
  }
  while (b && b->parent) {
    b = b->parent;
  }
  return scan_lex_session_equal_frame(a, b);
}

static size_t shift_session_offset(size_t value, size_t edit_start,
                                   size_t old_end, ptrdiff_t delta) {
  if (value == SIZE_MAX) {
    return value;
  }
  if (value >= old_end) {
    return delta < 0 ? value - (size_t)-delta : value + (size_t)delta;
  }
  return value > edit_start ? edit_start : value;
}

static void scan_lex_session_shift_frame(scan_lex_session_t *run,
                                         size_t edit_start, size_t old_end,
                                         ptrdiff_t delta) {
  if (!run) {
    return;
  }
#define SHIFT_FIELD(name)                                                      \
  run->name = shift_session_offset(run->name, edit_start, old_end, delta)
  SHIFT_FIELD(embed_start);
  SHIFT_FIELD(pending_indent_end);
  SHIFT_FIELD(pending_indent_first_tab);
  SHIFT_FIELD(pending_fence_content);
  SHIFT_FIELD(pending_fence_label_start);
  SHIFT_FIELD(pending_fence_label_end);
  SHIFT_FIELD(pending_fence_search);
  SHIFT_FIELD(pending_heredoc_tag_start);
  SHIFT_FIELD(pending_heredoc_tag_end);
  SHIFT_FIELD(pending_heredoc_body);
  SHIFT_FIELD(pending_heredoc_scan);
  SHIFT_FIELD(pending_heredoc_line_start);
  SHIFT_FIELD(pending_heredoc_header_scan);
  SHIFT_FIELD(pending_heredoc_token_end);
  SHIFT_FIELD(pending_number_prefix_at);
  SHIFT_FIELD(pending_embed_content);
  SHIFT_FIELD(pending_embed_search);
  SHIFT_FIELD(skip_from);
  SHIFT_FIELD(skip_to);
  SHIFT_FIELD(pending_recovery_start);
  SHIFT_FIELD(pending_recovery_end);
#undef SHIFT_FIELD
  for (size_t i = 0; i < run->program->fence_count; i++) {
    run->fence_probe_skip_until[i] = shift_session_offset(
        run->fence_probe_skip_until[i], edit_start, old_end, delta);
  }
  scan_lex_session_shift_frame(run->child, edit_start, old_end, delta);
}

void scan_lex_session_shift(void *state, size_t edit_start, size_t old_end,
                            ptrdiff_t byte_delta) {
  scan_lex_session_t *run = state;
  while (run && run->parent) {
    run = run->parent;
  }
  scan_lex_session_shift_frame(run, edit_start, old_end, byte_delta);
}

static void free_pending_recovery_token(parse_token_info_t *token) {
  if (!token) {
    return;
  }
  parse_token_free(token);
}

static void scan_lex_session_free_frame(scan_lex_session_t *session) {
  if (!session) {
    return;
  }

  scan_lex_session_free_frame(session->child);
  core_list_free(session->modes);
  core_list_free(session->indents);
  core_list_free(session->layout_blocks);
  for (size_t i = 0; i < core_list_len(session->heredoc_queue); i++) {
    core_free(session->heredoc_queue.data[i].tag.data);
  }
  core_list_free(session->heredoc_queue);
  core_free(session->pending_fence_pattern.data);
  core_free(session->pending_fence_prefix);
  core_free(session->pending_embed_prefix);
  core_free(session->fence_probe_skip_until);
  core_free(session->hook_diagnostic.data);
  core_free(session->pending_recovery_message.data);
  core_free(session->recovery_diagnostic.data);
  free_pending_recovery_token(session->pending_recovery_token);
  core_free(session);
}

void scan_lex_session_free(void *state) {
  scan_lex_session_t *session = state;

  while (session && session->parent) {
    session = session->parent;
  }

  scan_lex_session_free_frame(session);
}

size_t scan_lex_issue_count(const scan_lex_t *lex) {
  return lex ? lex->issue_count : 0;
}

bool scan_lex_issue(const scan_lex_t *lex, size_t index,
                    scan_lex_issue_t *out) {
  if (!lex || !out || index >= lex->issue_count) {
    return false;
  }
  *out = lex->issues[index];
  return true;
}

void scan_lex_reset_state(parse_scanner_t *s) {
  scan_lex_session_t *lex = (scan_lex_session_t *)s->user_state;

  if (!lex) {
    return;
  }

  while (lex->parent) {
    lex = lex->parent;
  }

  scan_lex_session_free_frame(lex->child);
  lex->child = nullptr;
  s->user_state = lex;
  s->lexer_context = lex->program->spec->name;
  s->token_suffix_lookahead = false;
  for (const scan_lex_sigil_t *sigil = lex->program->spec->sigils;
       sigil && sigil->prefix.data; sigil++) {
    if (sigil->attach_previous || sigil->attach_after) {
      s->token_suffix_lookahead = true;
      break;
    }
  }

  lex->modes.len = 0;
  lex->indents.len = 0;
  lex->pending_dedents = 0;
  lex->pending_block_newline = false;
  lex->bracket_depth = 0;
  lex->line_start = true;
  lex->continued_line = false;
  lex->line_has_token = false;
  lex->sent_eof = false;
  lex->pending_indent = false;
  lex->pending_indent_end = 0;
  lex->pending_indent_first_tab = SIZE_MAX;
  lex->pending_indent_column = 0;
  lex->pending_indent_has_space = false;
  lex->pending_indent_has_tab = false;
  lex->skip_from = 0;
  lex->skip_to = 0;
  lex->outside = false;
  for (const scan_lex_embed_t *e = lex->program->spec->embeds;
       e && e->open.data; e++) {
    if (!e->lexer.data) {
      lex->outside = true;
      break;
    }
  }
  lex->in_directive = nullptr;
  lex->pending_comment = nullptr;
  lex->pending_comment_depth = 0;
  lex->pending_comment_fill_level = 0;
  lex->pending_string = nullptr;
  lex->pending_string_scalars = 0;
  lex->pending_string_delimiter_depth = 0;
  lex->pending_string_tail = false;
  lex->pending_regex = nullptr;
  lex->pending_regex_in_class = false;
  lex->pending_fence = nullptr;
  core_free(lex->pending_fence_pattern.data);
  core_free(lex->pending_fence_prefix);
  lex->pending_fence_pattern = (core_string_t){0};
  lex->pending_fence_prefix = nullptr;
  lex->pending_fence_search = 0;
  lex->pending_fence_match = 0;
  if (lex->fence_probe_skip_until) {
    memset(lex->fence_probe_skip_until, 0,
           lex->program->fence_count * sizeof(*lex->fence_probe_skip_until));
  }
  lex->pending_heredoc = nullptr;
  lex->pending_heredoc_header_phase = HEREDOC_HEADER_NONE;
  for (size_t i = 0; i < core_list_len(lex->heredoc_queue); i++) {
    core_free(lex->heredoc_queue.data[i].tag.data);
  }
  lex->heredoc_queue.len = 0;
  lex->heredoc_queue_ready = false;
  lex->heredoc_queue_scanning = false;
  lex->pending_identifier = false;
  lex->pending_attached_sigil = nullptr;
  lex->pending_terminal = false;
  lex->pending_number_phase = NUMBER_NONE;
  lex->pending_opener = false;
  lex->pending_embed = nullptr;
  lex->pending_embed_content = 0;
  lex->pending_embed_search = 0;
  lex->pending_embed_match = 0;
  core_free(lex->pending_embed_prefix);
  lex->pending_embed_prefix = nullptr;
  core_free(lex->hook_diagnostic.data);
  lex->hook_diagnostic = (core_string_t){0};
  core_free(lex->pending_recovery_message.data);
  lex->pending_recovery_message = (core_string_t){0};
  core_free(lex->recovery_diagnostic.data);
  lex->recovery_diagnostic = (core_string_t){0};
  free_pending_recovery_token(lex->pending_recovery_token);
  lex->pending_recovery_token = nullptr;
  lex->pending_recovery = nullptr;
  lex->pending_recovery_start = 0;
  lex->pending_recovery_end = 0;
}

static size_t compact_offset(size_t offset, size_t removed) {
  return offset >= removed ? offset - removed : 0;
}

void scan_lex_compact_state(parse_scanner_t *s, size_t removed_bytes) {
  scan_lex_session_t *run = s ? s->user_state : nullptr;

  if (!run || !removed_bytes) {
    return;
  }

  if (run->pending_indent) {
    run->pending_indent_end =
        compact_offset(run->pending_indent_end, removed_bytes);

    if (run->pending_indent_first_tab != SIZE_MAX) {
      run->pending_indent_first_tab =
          compact_offset(run->pending_indent_first_tab, removed_bytes);
    }
  }

  if (run->parent) {
    run->embed_start = compact_offset(run->embed_start, removed_bytes);
  }

  if (run->pending_embed) {
    run->pending_embed_content =
        compact_offset(run->pending_embed_content, removed_bytes);
    run->pending_embed_search =
        compact_offset(run->pending_embed_search, removed_bytes);
  }

  run->pending_fence_content =
      compact_offset(run->pending_fence_content, removed_bytes);
  run->pending_fence_label_start =
      compact_offset(run->pending_fence_label_start, removed_bytes);
  run->pending_fence_label_end =
      compact_offset(run->pending_fence_label_end, removed_bytes);
  run->pending_fence_search =
      compact_offset(run->pending_fence_search, removed_bytes);
  run->pending_heredoc_tag_start =
      compact_offset(run->pending_heredoc_tag_start, removed_bytes);
  run->pending_heredoc_tag_end =
      compact_offset(run->pending_heredoc_tag_end, removed_bytes);
  run->pending_heredoc_body =
      compact_offset(run->pending_heredoc_body, removed_bytes);
  run->pending_heredoc_scan =
      compact_offset(run->pending_heredoc_scan, removed_bytes);
  run->pending_heredoc_line_start =
      compact_offset(run->pending_heredoc_line_start, removed_bytes);
  run->pending_heredoc_header_scan =
      compact_offset(run->pending_heredoc_header_scan, removed_bytes);
  run->pending_heredoc_token_end =
      compact_offset(run->pending_heredoc_token_end, removed_bytes);
  run->pending_number_prefix_at =
      compact_offset(run->pending_number_prefix_at, removed_bytes);
  run->skip_from = compact_offset(run->skip_from, removed_bytes);
  run->skip_to = compact_offset(run->skip_to, removed_bytes);
}

bool scan_grammar_set_lex(parse_grammar_t *g, const scan_lex_spec_t *spec) {
  if (!g) {
    return false;
  }

  if (g->lex_compiled) {
    scan_lex_free((scan_lex_t *)g->lex_compiled);
    g->lex_compiled = nullptr;
  }

  if (g->lex_spec_owned) {
    scan_lex_spec_free((const scan_lex_spec_t *)g->lex_spec);
    g->lex_spec_owned = false;
  }

  g->lex_spec = spec;
  g->lex_resolved = true;

  return true;
}

const scan_lex_spec_t *scan_grammar_lex(parse_grammar_t *g) {
  if (!g) {
    return nullptr;
  }

  if (!g->lex_resolved) {
    g->lex_resolved = true;
    if (parse_grammar_find_annot(g, PARSE_ANNOT_LEXER)) {
      scan_lex_note_unresolved_import();
    } else {
      g->lex_spec = scan_lex_for(scan_grammar_tokenizer(g));
      if (!g->lex_spec && core_string_eq(scan_grammar_tokenizer(g),
                           CORE_STRING_STATIC(SCAN_TOKENIZER_DEFAULT))) {
        core_list_t(scan_lex_decl_t) decls = core_list_new(scan_lex_decl_t);
        if (scan_lex_parse_file(core_string_borrow(DEFAULT_LEX,
                                                   sizeof(DEFAULT_LEX) - 1),
                                CORE_STRING_STATIC("default.lex"), &decls)) {
          g->lex_spec = scan_lex_spec_from_decls(g, &decls);
          g->lex_spec_owned = g->lex_spec != nullptr;
        }
        scan_lex_decls_free(&decls);
      }
    }
  }

  return (const scan_lex_spec_t *)g->lex_spec;
}

scan_lex_t *scan_grammar_lex_compiled(parse_grammar_t *g) {
  const scan_lex_spec_t *spec = scan_grammar_lex(g);
  pthread_mutex_lock(&lex_compile_lock);

  if (!spec) {
    pthread_mutex_unlock(&lex_compile_lock);
    return nullptr;
  }

  if (!g->lex_compiled) {
    g->lex_compiled = scan_lex_compile(spec, g);
  }

  scan_lex_t *result = (scan_lex_t *)g->lex_compiled;

  pthread_mutex_unlock(&lex_compile_lock);
  return result;
}

static bool at_str(parse_scanner_t *s, core_string_t lit) {
  if (!lit.data || !lit.u8_bytes) {
    return false;
  }

  if (s->cursor + lit.u8_bytes > s->input.u8_bytes) {
    return false;
  }

  return memcmp(s->input.data + s->cursor, lit.data, lit.u8_bytes) == 0;
}

static bool at_fence_body_open(parse_scanner_t *s, const scan_lex_fence_t *f) {
  return !f->body_open.u8_bytes || at_str(s, f->body_open);
}

static bool partial_at_end(parse_scanner_t *s, core_string_t lit) {
  size_t remaining = scan_remaining(s);

  return lit.data && remaining > 0 && remaining < lit.u8_bytes &&
         !memcmp(s->input.data + s->cursor, lit.data, remaining);
}

static bool recipe_opener_may_continue(parse_scanner_t *s, bool resume) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  uint32_t node = resume ? run->pending_opener_node : 0;
  size_t consumed = resume ? run->pending_opener_consumed : 0;
  size_t before = consumed;

  if (!resume) {
    scan_mark(s);
  }

  while (s->mark + consumed < s->input.u8_bytes) {
    uint8_t byte = (uint8_t)s->input.data[s->mark + consumed];
    uint32_t child = trie_child(&((scan_lex_t *)lex)->opener_nodes,
                                &((scan_lex_t *)lex)->opener_edges,
                                node, byte, false);
    if (child == UINT32_MAX) {
      run->pending_opener = false;
      s->work_bytes += consumed - before;
      return false;
    }
    node = child;
    consumed++;
  }

  s->work_bytes += consumed - before;
  if (!s->source_final && lex->opener_nodes.data[node].edge >= 0) {
    run->pending_opener = true;
    run->pending_opener_node = node;
    run->pending_opener_consumed = consumed;
    scan_set_incomplete(s, CORE_STRING_STATIC("token opener may continue"),
                        s->mark);
    return true;
  }

  run->pending_opener = false;
  return false;
}

static bool in_set(const scan_lex_t *lex, core_string_t set,
                   core_codepoint_t c) {
  if (!set.data || c > 127) {
    return false;
  }

  const lex_ascii_set_t *bits = ascii_set_lookup(lex, set);

  return bits && (c < 64 ? bits->lo & (UINT64_C(1) << c)
                         : bits->hi & (UINT64_C(1) << (c - 64)));
}

static bool id_start_here(const scan_lex_t *lex, core_codepoint_t c) {
  return !core_codepoint_is_line_terminator(c) &&
         ((lex->identifier_profile
               ? scan_unicode_profile_is_start(lex->identifier_profile, c)
               : unicode_is_id_start(c)) ||
          in_set(lex, lex->spec->ident_start_extra, c));
}

static bool id_cont_here(const scan_lex_t *lex, core_codepoint_t c) {
  return !core_codepoint_is_line_terminator(c) &&
         ((lex->identifier_profile
               ? scan_unicode_profile_is_continue(lex->identifier_profile, c)
               : unicode_is_id_continue(c)) ||
          in_set(lex, lex->spec->ident_extra, c));
}

static void emit_named(parse_scanner_t *s, core_string_t name,
                       int32_t fallback);

static bool fence_pad_matches(const scan_lex_fence_t *f, int32_t cp) {
  if (!f->pad_chars.data) {
    return parse_codepoint_matches_class(cp, PARSE_CC_WHITESPACE);
  }

  uint32_t at = 0;
  while (at < f->pad_chars.u8_bytes) {
    int32_t candidate = unicode_utf8_decode(
        f->pad_chars.data, (uint32_t)f->pad_chars.u8_bytes, &at);
    if (candidate < 0) {
      break;
    }
    if (candidate == cp) {
      return true;
    }
  }
  return false;
}

static bool fence_pad_at_cursor(parse_scanner_t *s, const scan_lex_fence_t *f) {
  return fence_pad_matches(f, scan_peek(s, 0));
}

static size_t fence_trim_pad_before(parse_scanner_t *s,
                                    const scan_lex_fence_t *f, size_t boundary,
                                    bool *found) {
  size_t at = boundary;
  *found = false;

  while (at > 0) {
    size_t start = at - 1;
    while (start > 0 && (((uint8_t)s->input.data[start] & 0xC0) == 0x80)) {
      start--;
    }

    uint32_t decoded = (uint32_t)start;
    int32_t cp = unicode_utf8_decode(s->input.data, (uint32_t)at, &decoded);
    if (cp < 0 || decoded != at || !fence_pad_matches(f, cp)) {
      break;
    }
    at = start;
    *found = true;
  }

  return at;
}

static bool scan_fence_body(parse_scanner_t *s, const scan_lex_fence_t *f,
                            uint32_t level, size_t content_start,
                            size_t label_start, size_t label_end) {
  scan_lex_session_t *run = s->user_state;

  if (!run->pending_fence_pattern.data) {
    size_t fill_bytes = f->fill.u8_bytes * (size_t)level;
    size_t pattern_len =
        f->body_close.u8_bytes + fill_bytes + f->outer_close.u8_bytes;

    run->pending_fence_pattern = (core_string_t){
        .data = core_alloc_size(1, pattern_len),
        .u8_bytes = pattern_len,
    };
    run->pending_fence_prefix = core_alloc_array(size_t, pattern_len);
    run->pending_fence_search = content_start;
    run->pending_fence_match = 0;

    char *out = run->pending_fence_pattern.data;

    memcpy(out, f->body_close.data, f->body_close.u8_bytes);
    out += f->body_close.u8_bytes;
    for (uint32_t i = 0; i < level; i++) {
      memcpy(out, f->fill.data, f->fill.u8_bytes);
      out += f->fill.u8_bytes;
    }
    memcpy(out, f->outer_close.data, f->outer_close.u8_bytes);

    for (size_t i = 1, matched = 0; i < pattern_len; i++) {
      while (matched && run->pending_fence_pattern.data[i] !=
                            run->pending_fence_pattern.data[matched]) {
        matched = run->pending_fence_prefix[matched - 1];
      }
      if (run->pending_fence_pattern.data[i] ==
          run->pending_fence_pattern.data[matched]) {
        matched++;
      }
      run->pending_fence_prefix[i] = matched;
    }
  }

  size_t search = run->pending_fence_search;
  size_t matched = run->pending_fence_match;
  size_t close_cursor = 0;
  bool closed = false;

  size_t search_start = search;

  while (search < s->input.u8_bytes) {
    char byte = s->input.data[search];

    while (matched && byte != run->pending_fence_pattern.data[matched]) {
      matched = run->pending_fence_prefix[matched - 1];
    }
    if (byte == run->pending_fence_pattern.data[matched]) {
      matched++;
    }
    search++;

    if (matched == run->pending_fence_pattern.u8_bytes) {
      close_cursor = search - matched;
      bool has_pad = false;
      size_t trimmed = fence_trim_pad_before(s, f, close_cursor, &has_pad);
      if (f->pad != SCAN_LEX_FENCE_PAD_REQUIRED || has_pad) {
        close_cursor = trimmed;
        closed = true;
        break;
      }
      matched = run->pending_fence_prefix[matched - 1];
    }
  }

  s->work_bytes += search - search_start;

  while (s->cursor < search) {
    scan_advance(s);
  }

  if (closed) {
    core_free(run->pending_fence_pattern.data);
    core_free(run->pending_fence_prefix);
    run->pending_fence_pattern = (core_string_t){0};
    run->pending_fence_prefix = nullptr;
    run->pending_fence_search = 0;
    run->pending_fence_match = 0;
    run->pending_fence = nullptr;

    emit_named(s, f->tok, PARSE_TOK_STRING_LIT);

    parse_token_info_t *tok = scan_prev_token(s);

    if (tok) {
      if (close_cursor < content_start) {
        content_start = close_cursor;
      }
      parse_token_set_content_span(tok, s->source_base + content_start,
                                   s->source_base + close_cursor);

      if (label_end > label_start) {
        parse_token_set_label_span(tok, s->source_base + label_start,
                                   s->source_base + label_end);
      }
    }

    return true;
  }

  run->pending_fence = f;
  run->pending_fence_level = level;
  run->pending_fence_content = content_start;
  run->pending_fence_label_start = label_start;
  run->pending_fence_label_end = label_end;
  run->pending_fence_search = search;
  run->pending_fence_match = matched;
  scan_set_incomplete(s, CORE_STRING_STATIC("unterminated fenced literal"),
                      s->mark);

  return false;
}

static void record_failed_fence_probe(scan_lex_session_t *run,
                                      parse_scanner_t *s,
                                      const scan_lex_fence_t *f) {
  if (!run->fence_probe_skip_until || !core_string_eq(f->outer_open, f->fill)) {
    return;
  }

  size_t index = (size_t)(f - run->program->spec->fences);

  run->fence_probe_skip_until[index] = s->source_base + s->cursor;
}

static bool try_fence(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  lex_bucket_t bucket = lex->fence_buckets[scan_peek_byte(s, 0)];

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_fence_t *f = lex->fence_dispatch[i];
    size_t fence_index = (size_t)(f - lex->spec->fences);

    if (run->fence_probe_skip_until &&
        s->source_base + s->cursor < run->fence_probe_skip_until[fence_index]) {
      continue;
    }

    if (!at_str(s, f->outer_open)) {
      continue;
    }

    size_t start = s->cursor;
    uint32_t start_line = s->line;
    uint32_t start_col = s->column;

    scan_mark(s);
    scan_advance_bytes(s, f->outer_open.u8_bytes);

    uint32_t level = 0;

    while (at_str(s, f->fill)) {
      scan_advance_bytes(s, f->fill.u8_bytes);
      level++;
    }

    if (level < f->minimum) {
      if (!s->source_final && (scan_at_eof(s) || partial_at_end(s, f->fill))) {
        scan_lex_session_t *run = s->user_state;
        run->pending_fence = f;
        run->pending_fence_level = level;
        run->pending_fence_fill = true;
        run->pending_fence_open = true;
        run->pending_fence_label_start = s->cursor;
        run->pending_fence_label_end = s->cursor;
        scan_set_incomplete(
            s, CORE_STRING_STATIC("incomplete fenced literal opener"), s->mark);
        return false;
      }
      record_failed_fence_probe(run, s, f);
      s->cursor = start;
      s->line = start_line;
      s->column = start_col;
      continue;
    }

    size_t label_start = s->cursor;
    size_t label_end = label_start;

    if (!at_fence_body_open(s, f) && f->label &&
        id_start_here(lex, scan_peek(s, 0))) {
      scan_advance(s);

      while (!scan_at_eof(s) && id_cont_here(lex, scan_peek(s, 0))) {
        scan_advance(s);
      }

      label_end = s->cursor;
    }

    if (!at_fence_body_open(s, f)) {
      if (scan_at_eof(s) || partial_at_end(s, f->body_open)) {
        scan_lex_session_t *run = s->user_state;

        run->pending_fence = f;
        run->pending_fence_level = level;
        run->pending_fence_label_start = label_start;
        run->pending_fence_label_end = label_end;
        run->pending_fence_open = true;
        scan_set_incomplete(
            s, CORE_STRING_STATIC("incomplete fenced literal opener"), s->mark);

        return false;
      }

      record_failed_fence_probe(run, s, f);
      s->cursor = start;
      s->line = start_line;
      s->column = start_col;
      continue;
    }

    scan_advance_bytes(s, f->body_open.u8_bytes);
    bool has_pad = false;
    if (f->pad != SCAN_LEX_FENCE_PAD_NONE) {
      while (!scan_at_eof(s) && fence_pad_at_cursor(s, f)) {
        has_pad = true;
        scan_advance(s);
      }
      if (f->pad == SCAN_LEX_FENCE_PAD_REQUIRED && !has_pad) {
        record_failed_fence_probe(run, s, f);
        s->cursor = start;
        s->line = start_line;
        s->column = start_col;
        continue;
      }
    }
    size_t content_start = s->cursor;

    return scan_fence_body(s, f, level, content_start, label_start, label_end);
  }

  return false;
}

static bool resume_fence(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_fence_t *f = run->pending_fence;
  uint32_t level = run->pending_fence_level;
  size_t content = run->pending_fence_content;
  size_t label_start = run->pending_fence_label_start;
  size_t label_end = run->pending_fence_label_end;
  bool awaiting_open = run->pending_fence_open;
  bool awaiting_fill = run->pending_fence_fill;

  run->pending_fence = nullptr;
  run->pending_fence_open = false;
  run->pending_fence_fill = false;

  if (awaiting_fill) {
    while (at_str(s, f->fill)) {
      scan_advance_bytes(s, f->fill.u8_bytes);
      level++;
    }
    if (!s->source_final && (scan_at_eof(s) || partial_at_end(s, f->fill))) {
      run->pending_fence = f;
      run->pending_fence_level = level;
      run->pending_fence_fill = true;
      run->pending_fence_open = true;
      run->pending_fence_label_start = s->cursor;
      run->pending_fence_label_end = s->cursor;
      scan_set_incomplete(
          s, CORE_STRING_STATIC("incomplete fenced literal opener"), s->mark);
      return false;
    }
    if (level < f->minimum) {
      s->cursor = s->mark;
      s->line = s->mark_line;
      s->column = s->mark_col;
      return scan_lex_scan(s);
    }
    label_start = s->cursor;
    label_end = label_start;
  }

  if (awaiting_open) {
    while (!scan_at_eof(s) && !at_fence_body_open(s, f)) {
      if (partial_at_end(s, f->body_open)) {
        break;
      }

      if (!f->label || !id_cont_here(run->program, scan_peek(s, 0))) {
        record_failed_fence_probe(run, s, f);
        s->cursor = s->mark;
        s->line = s->mark_line;
        s->column = s->mark_col;
        return scan_lex_scan(s);
      }

      scan_advance(s);
      label_end = s->cursor;
    }

    if (!at_fence_body_open(s, f) && !s->source_final) {
      run->pending_fence = f;
      run->pending_fence_level = level;
      run->pending_fence_label_start = label_start;
      run->pending_fence_label_end = label_end;
      run->pending_fence_open = true;
      scan_set_incomplete(
          s, CORE_STRING_STATIC("incomplete fenced literal opener"), s->mark);

      return false;
    }

    if (!at_fence_body_open(s, f)) {
      record_failed_fence_probe(run, s, f);
      s->cursor = s->mark;
      s->line = s->mark_line;
      s->column = s->mark_col;
      return scan_lex_scan(s);
    }

    scan_advance_bytes(s, f->body_open.u8_bytes);
    content = s->cursor;
  }

  return scan_fence_body(s, f, level, content, label_start, label_end);
}

static void literal_hook_invalid(parse_scanner_t *s, core_string_t message,
                                 size_t relative_start, size_t relative_end) {
  parse_token_info_t *token = scan_prev_token(s);
  size_t local_start = parse_token_start_byte(token) - s->source_base
                     + relative_start;

  scan_set_invalid(s, message, local_start);
  s->status_end_byte = parse_token_start_byte(token) + relative_end;
}

static void previous_token_invalid(parse_scanner_t *s, core_string_t message) {
  parse_token_info_t *token = scan_prev_token(s);
  size_t start = token && parse_token_start_byte(token) >= s->source_base
                     ? parse_token_start_byte(token) - s->source_base
                     : s->mark;

  scan_set_invalid(s, message, start);
}

static bool at_bytes(parse_scanner_t *s, size_t pos, core_string_t text) {
  return pos + text.u8_bytes <= s->input.u8_bytes &&
         memcmp(s->input.data + pos, text.data, text.u8_bytes) == 0;
}

static core_string_t comment_close_pattern(const scan_lex_comment_t *c,
                                           uint32_t fill_level) {
  size_t fill_bytes = c->fill.u8_bytes * (size_t)fill_level;
  size_t len = fill_bytes + c->closer.u8_bytes;
  char *data = core_alloc_size(1, len + 1);
  char *out = data;
  for (uint32_t i = 0; i < fill_level; i++) {
    memcpy(out, c->fill.data, c->fill.u8_bytes);
    out += c->fill.u8_bytes;
  }
  memcpy(out, c->closer.data, c->closer.u8_bytes);
  data[len] = '\0';
  return (core_string_t){.data = data, .u8_bytes = len};
}

static bool comment_fill_open(parse_scanner_t *s, const scan_lex_comment_t *c,
                              uint32_t *level, size_t *open_bytes) {
  if (!at_bytes(s, s->cursor, c->opener)) {
    return false;
  }

  size_t at = s->cursor + c->opener.u8_bytes;
  *level = 0;
  while (at_bytes(s, at, c->fill)) {
    at += c->fill.u8_bytes;
    (*level)++;
  }
  if (!*level) {
    return false;
  }
  *open_bytes = at - s->cursor;
  return true;
}

static bool literal_hook_declares(const core_string_t *declared,
                                  core_string_t value) {
  for (const core_string_t *item = declared; item && item->data; item++) {
    if (core_string_eq(*item, value)) {
      return true;
    }
  }
  return false;
}

static void apply_literal_hook(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  parse_token_info_t *token = scan_prev_token(s);

  if (!lex->literal_hook_count || !token || !parse_token_has_value(token)) {
    return;
  }

  const lex_literal_hook_t *compiled =
      literal_hook_for(lex, parse_token_tid(token));
  if (!compiled) {
    return;
  }

  const scan_lex_literal_hook_t *recipe = compiled->recipe;
  scan_lex_literal_input_t input = {
      .spelling = core_option_get(parse_token_value(token)),
      .token = recipe->token,
      .modifier = recipe->modifier,
  };
  scan_lex_literal_result_t result = {0};

  if (!scan_lex_literal_hook_call(recipe->hook, &input, &result)) {
    literal_hook_invalid(
        s, CORE_STRING_STATIC("literal hook is no longer registered"), 0,
        input.spelling.u8_bytes);
    return;
  }

  if (!result.accepted) {
    core_free(run->hook_diagnostic.data);
    run->hook_diagnostic = result.diagnostic;
    result.diagnostic = (core_string_t){0};
    literal_hook_invalid(s, run->hook_diagnostic, result.diagnostic_start,
                         result.diagnostic_end);
    scan_lex_literal_result_free(&result);
    return;
  }

  if (result.token.data && !core_string_eq(result.token, recipe->token) &&
      !literal_hook_declares(recipe->outputs, result.token)) {
    literal_hook_invalid(
        s, CORE_STRING_STATIC("literal hook selected an undeclared token"), 0,
        input.spelling.u8_bytes);
    scan_lex_literal_result_free(&result);
    return;
  }
  if (result.type.data && !literal_hook_declares(recipe->types, result.type)) {
    literal_hook_invalid(
        s, CORE_STRING_STATIC("literal hook selected undeclared type metadata"),
        0, input.spelling.u8_bytes);
    scan_lex_literal_result_free(&result);
    return;
  }

  if (result.token.data) {
    parse_token_set_tid(token,
                        tid_for(lex->g, result.token, parse_token_tid(token)));
  }
  if (result.value.data) {
    parse_token_set_value(token, core_option_set(core_string_t, result.value),
                          true);
    result.value = (core_string_t){0};
  }
  if (result.type.data) {
    parse_token_set_literal_type(
        token, core_option_set(core_string_t, result.type), true);
    result.type = (core_string_t){0};
  }
  scan_lex_literal_result_free(&result);
}

static void emit_named(parse_scanner_t *s, core_string_t name,
                       int32_t fallback) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  scan_emit_marked(s, tid_for(lex->g, name, fallback));
  if (lex->literal_hook_count) {
    apply_literal_hook(s);
  }
}

static const scan_lex_t *embedded_program(const scan_lex_t *lex,
                                          const scan_lex_embed_t *embed) {
  if (!lex || !embed || !lex->spec->embeds) {
    return nullptr;
  }

  size_t index = (size_t)(embed - lex->spec->embeds);

  return index < lex->embed_count ? lex->embed_programs[index] : nullptr;
}

static bool validate_embedded_body(parse_scanner_t *outer,
                                   scan_lex_session_t *run,
                                   const scan_lex_embed_t *embed,
                                   size_t content_start, size_t content_end) {
  const scan_lex_t *program = embedded_program(run->program, embed);
  core_buffer_t buffer = {
      .data = (char *)outer->input.data + content_start,
      .byte_len = content_end - content_start,
      .alloc_len = content_end - content_start,
  };
  scan_lex_session_t *session = scan_lex_session_new(program);
  parse_scanner_t *scanner = scan_scanner_new(
      &buffer, scan_lex_scan, program->g, core_option_none(core_string_t),
      session, scan_lex_reset_state);

  scan_scanner_take_state(scanner, scan_lex_session_free);
  scan_scanner_set_compact_callback(scanner, scan_lex_compact_state);
  if (program->spec->regions) {
    scan_scanner_set_finalize_callback(scanner, scan_lex_finalize_regions);
  }
  scan_lex_reset_state(scanner);

  parse_token_stream_t *stream = scan_token_stream_new(scanner);

  scan_token_stream_take_scanner(stream);
  while (scan_stream_next(stream)) {
  }

  scan_status_t status = scan_status(scanner);

  if (status != SCAN_STATUS_COMPLETE) {
    scan_diagnostic_t diagnostic = {0};

    if (scan_diagnostic(scanner, &diagnostic)) {
      scan_set_invalid(outer, diagnostic.message,
                       content_start + diagnostic.start_byte);
      outer->status_end_byte =
          outer->source_base + content_start + diagnostic.end_byte;
      outer->status_lexer =
          diagnostic.lexer.data ? diagnostic.lexer : program->spec->name;
    } else {
      scan_set_invalid(outer, CORE_STRING_STATIC("invalid embedded region"),
                       content_start);
      outer->status_lexer = program->spec->name;
    }
  }

  scan_token_stream_free(stream);
  return status == SCAN_STATUS_COMPLETE;
}

static bool resume_embed_token(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_embed_t *embed = run->pending_embed;
  size_t search = run->pending_embed_search;
  size_t matched = run->pending_embed_match;
  size_t close_at = 0;
  bool closed = false;

  while (search < s->input.u8_bytes) {
    char byte = s->input.data[search];

    while (matched && byte != embed->close.data[matched]) {
      matched = run->pending_embed_prefix[matched - 1];
    }
    if (byte == embed->close.data[matched]) {
      matched++;
    }
    search++;

    if (matched == embed->close.u8_bytes) {
      close_at = search - matched;
      closed = true;
      break;
    }
  }

  s->work_bytes += search - run->pending_embed_search;
  while (s->cursor < search) {
    scan_advance(s);
  }

  if (!closed) {
    run->pending_embed_search = search;
    run->pending_embed_match = matched;
    scan_set_incomplete(s, CORE_STRING_STATIC("unterminated embedded region"),
                        s->mark);
    return false;
  }

  size_t content_start = run->pending_embed_content;
  bool valid = validate_embedded_body(s, run, embed, content_start, close_at);

  run->pending_embed = nullptr;
  run->pending_embed_content = 0;
  run->pending_embed_search = 0;
  run->pending_embed_match = 0;
  core_free(run->pending_embed_prefix);
  run->pending_embed_prefix = nullptr;

  if (!valid) {
    return false;
  }

  emit_named(s, embed->tok, PARSE_TOK_STRING_LIT);

  parse_token_info_t *tok = scan_prev_token(s);

  if (tok) {
    parse_token_set_content_span(tok, s->source_base + content_start,
                                 s->source_base + close_at);
  }

  run->line_start = false;
  run->line_has_token = true;
  return true;
}

typedef enum {
  INLINE_EMBED_NONE,
  INLINE_EMBED_EMITTED,
  INLINE_EMBED_STOP,
} inline_embed_result_t;

static inline_embed_result_t try_inline_embed(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  const scan_lex_embed_t *hit = nullptr;
  bool longer_partial = false;
  size_t available = scan_remaining(s);

  lex_bucket_t bucket = lex->embed_buckets[scan_peek_byte(s, 0)];

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_embed_t *e = lex->embed_dispatch[i];

    if (!e->lexer.data) {
      continue;
    }

    size_t compare =
        available < e->open.u8_bytes ? available : e->open.u8_bytes;

    if (memcmp(s->input.data + s->cursor, e->open.data, compare)) {
      continue;
    }

    if (available < e->open.u8_bytes) {
      longer_partial = true;
      continue;
    }

    if (!hit || e->open.u8_bytes > hit->open.u8_bytes) {
      hit = e;
    }
  }

  if (!s->source_final && longer_partial &&
      (!hit || available == hit->open.u8_bytes)) {
    scan_set_incomplete(
        s, CORE_STRING_STATIC("embedded delimiter may continue"), s->cursor);
    return INLINE_EMBED_STOP;
  }

  if (!hit) {
    return INLINE_EMBED_NONE;
  }

  size_t open_at = s->cursor;

  scan_mark(s);
  scan_advance_bytes(s, hit->open.u8_bytes);

  if (!hit->tokens) {
    run->pending_embed = hit;
    run->pending_embed_content = s->cursor;
    run->pending_embed_search = s->cursor;
    run->pending_embed_match = 0;
    run->pending_embed_prefix = core_alloc_array(size_t, hit->close.u8_bytes);
    run->pending_embed_prefix[0] = 0;

    for (size_t i = 1, matched = 0; i < hit->close.u8_bytes; i++) {
      while (matched && hit->close.data[i] != hit->close.data[matched]) {
        matched = run->pending_embed_prefix[matched - 1];
      }
      if (hit->close.data[i] == hit->close.data[matched]) {
        matched++;
      }
      run->pending_embed_prefix[i] = matched;
    }

    return resume_embed_token(s) ? INLINE_EMBED_EMITTED : INLINE_EMBED_STOP;
  }

  const scan_lex_t *program = embedded_program(lex, hit);
  scan_lex_session_t *child = scan_lex_session_new(program);

  child->parent = run;
  child->parent_embed = hit;
  child->embed_depth = run->embed_depth + 1;
  child->embed_start = open_at;
  run->child = child;
  run->line_start = false;
  run->line_has_token = true;

  emit_named(s, hit->open_tok, PARSE_TOK_OTHER);
  s->user_state = child;
  s->lexer_context = child->program->spec->name;
  return INLINE_EMBED_EMITTED;
}

static bool scan_outside(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  scan_mark(s);

  const scan_lex_embed_t *hit = nullptr;

  while (!scan_at_eof(s) && !hit) {
    lex_bucket_t bucket = lex->embed_buckets[scan_peek_byte(s, 0)];

    for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
      const scan_lex_embed_t *e = lex->embed_dispatch[i];

      if (e->lexer.data) {
        continue;
      }

      if (at_str(s, e->open)) {
        hit = e;

        break;
      }

      if (!s->source_final && partial_at_end(s, e->open)) {
        scan_set_incomplete(
            s, CORE_STRING_STATIC("embedded delimiter may continue"),
            s->cursor);
        break;
      }
    }

    if (!hit && s->status != SCAN_STATUS_INCOMPLETE) {
      scan_advance(s);
    } else if (!hit) {
      break;
    }
  }

  if (scan_mark_len(s) > 0) {
    core_string_t outside_tok = hit ? hit->tok : (core_string_t){0};

    if (!hit) {
      for (const scan_lex_embed_t *e = lex->spec->embeds; e && e->open.data;
           e++) {
        if (!e->lexer.data) {
          outside_tok = e->tok;
          break;
        }
      }
    }

    emit_named(s, outside_tok, PARSE_TOK_OTHER);

    return true;
  }

  if (!hit) {
    return false;
  }

  scan_mark(s);
  scan_advance_bytes(s, hit->open.u8_bytes);

  run->outside = false;
  run->line_start = true;
  run->line_has_token = false;

  if (hit->open_tok.data) {
    emit_named(s, hit->open_tok, PARSE_TOK_OTHER);

    return true;
  }

  return false;
}

#define EMBED_NOT_HERE 0
#define EMBED_EMITTED 1
#define EMBED_CONSUMED 2

static int at_embed_close(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  lex_bucket_t bucket = lex->embed_close_buckets[scan_peek_byte(s, 0)];

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_embed_t *e = lex->embed_close_dispatch[i];

    if (e->lexer.data) {
      continue;
    }

    if (e->close.u8_bytes && at_str(s, e->close)) {
      scan_mark(s);
      scan_advance_bytes(s, e->close.u8_bytes);
      run->outside = true;

      if (e->close_tok.data) {
        emit_named(s, e->close_tok, PARSE_TOK_OTHER);

        return EMBED_EMITTED;
      }

      return EMBED_CONSUMED;
    }
  }

  return EMBED_NOT_HERE;
}

typedef enum {
  EMBED_CLOSE_NONE,
  EMBED_CLOSE_EMITTED,
  EMBED_CLOSE_STOP,
} embed_close_result_t;

static void emit_dedent(parse_scanner_t *s, scan_lex_session_t *run) {
  const scan_lex_t *lex = run->program;
  bool mismatch = run->pending_dedent_mismatch && run->pending_dedents == 1;
  if (run->indents.len) {
    run->indents.len--;
  }
  uint32_t depth = (uint32_t)run->indents.len;
  uint32_t column = depth ? (uint32_t)run->indents.data[depth - 1] : 0;
  bool in_block = run->layout_blocks.len > 0;
  while (run->layout_blocks.len &&
         depth <
             (uint32_t)run->layout_blocks.data[run->layout_blocks.len - 1]) {
    run->layout_blocks.len--;
    run->pending_block_newline = lex->spec->dedent_newline &&
                                 lex->tid_newline != PARSE_TOK_IGNORED;
  }
  run->pending_dedents--;
  if (!run->pending_dedents) {
    run->pending_dedent_mismatch = false;
  }

  scan_mark(s);
  scan_emit_marked(s, lex->tid_dedent);
  parse_token_info_t *token = scan_prev_token(s);
  if (token) {
    parse_token_set_layout(token, depth, column);
    parse_token_set_layout_mismatch(token, mismatch);
    parse_token_set_ignore_if_no_match(token, !in_block);
  }
}

static bool newlines_visible(const scan_lex_t *lex,
                             const scan_lex_session_t *run) {
  return run->bracket_depth == 0 || lex->spec->bracket_newlines;
}

static void emit_layout_newline(parse_scanner_t *s, scan_lex_session_t *run) {
  const scan_lex_t *lex = run->program;
  parse_token_info_t *previous = scan_prev_token(s);
  int64_t colon = parse_terminal_id(lex->g, CORE_STRING_STATIC(":"));
  bool block_header = previous && colon && parse_token_tid(previous) == colon;

  scan_mark(s);
  scan_emit_marked(s, lex->tid_newline);
  if (block_header) {
    run->pending_layout_block = true;
  }
}

static bool finish_embedded_layout(parse_scanner_t *s,
                                   scan_lex_session_t *run) {
  const scan_lex_t *lex = run->program;

  if (lex->tid_terminator != PARSE_TOK_IGNORED && run->line_has_token &&
      run->bracket_depth == 0 && ends_a_statement(lex, s)) {
    run->line_has_token = false;
    scan_mark(s);
    scan_emit_marked(s, lex->tid_terminator);
    return true;
  }

  if (lex->tid_newline != PARSE_TOK_IGNORED && run->line_has_token &&
      newlines_visible(lex, run)) {
    run->line_has_token = false;
    emit_layout_newline(s, run);
    return true;
  }

  if (run->indents.len) {
    run->pending_dedents = 1;
    run->pending_dedent_mismatch = false;
    emit_dedent(s, run);
    return true;
  }

  if (!run->sent_eof && lex->tid_eof != PARSE_TOK_IGNORED) {
    run->sent_eof = true;
    scan_mark(s);
    scan_emit_marked(s, lex->tid_eof);
    return true;
  }

  return false;
}

static embed_close_result_t try_embedded_close(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;

  if (!run->parent || !run->parent_embed) {
    return EMBED_CLOSE_NONE;
  }

  const scan_lex_embed_t *embed = run->parent_embed;

  if (at_str(s, embed->close)) {
    if (finish_embedded_layout(s, run)) {
      return EMBED_CLOSE_EMITTED;
    }

    scan_mark(s);
    scan_advance_bytes(s, embed->close.u8_bytes);

    scan_lex_session_t *parent = run->parent;

    parent->child = nullptr;
    run->parent = nullptr;
    s->user_state = parent;
    s->lexer_context = parent->program->spec->name;
    parent->line_start = false;
    parent->line_has_token = true;
    scan_lex_session_free_frame(run);
    emit_named(s, embed->close_tok, PARSE_TOK_OTHER);
    return EMBED_CLOSE_EMITTED;
  }

  if (scan_at_eof(s)) {
    if (!s->source_final) {
      scan_set_incomplete(
          s, CORE_STRING_STATIC("embedded closing delimiter may continue"),
          s->cursor);
    } else {
      scan_set_incomplete(s, CORE_STRING_STATIC("unterminated embedded region"),
                          run->embed_start);
    }

    return EMBED_CLOSE_STOP;
  }

  if (partial_at_end(s, embed->close)) {
    scan_set_incomplete(
        s,
        s->source_final
            ? CORE_STRING_STATIC("unterminated embedded region")
            : CORE_STRING_STATIC("embedded closing delimiter may continue"),
        s->source_final ? run->embed_start : s->cursor);
    return EMBED_CLOSE_STOP;
  }

  return EMBED_CLOSE_NONE;
}

typedef enum : uint8_t {
  BODY_CLOSED,
  BODY_HOLE,
  BODY_UNTERMINATED,
  BODY_INVALID_NEWLINE,
} body_stop;

typedef enum : uint8_t {
  ESCAPE_OK,
  ESCAPE_INVALID,
  ESCAPE_INCOMPLETE,
} escape_result_t;

static escape_result_t scan_string_escape(parse_scanner_t *s,
                                          const scan_lex_string_t *str) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  const scan_lex_escape_t *best = nullptr;
  size_t string_index = (size_t)(str - lex->spec->strings);

  if (scan_at_eof(s)) {
    return ESCAPE_INCOMPLETE;
  }

  if (!lex->string_escape_complex[string_index]) {
    core_codepoint_t c = scan_peek(s, 0);
    if (c < 128) {
      best = lex->string_escape_dispatch[string_index * 128 + c];
    }
  } else {
    for (const scan_lex_escape_t *rule = str->escape_rules;
         rule && rule->prefix.data; rule++) {
      if (partial_at_end(s, rule->prefix)) {
        return ESCAPE_INCOMPLETE;
      }

      if (at_str(s, rule->prefix) &&
          (!best || rule->prefix.u8_bytes > best->prefix.u8_bytes)) {
        best = rule;
      }
    }
  }

  if (best) {
    scan_advance_bytes(s, best->prefix.u8_bytes);

    for (uint32_t i = 0; i < best->width; i++) {
      core_codepoint_t c = scan_peek(s, 0);

      if (scan_at_eof(s) || !in_set(lex, best->digits, c)) {
        return scan_at_eof(s) ? ESCAPE_INCOMPLETE : ESCAPE_INVALID;
      }

      scan_advance(s);
    }

    return ESCAPE_OK;
  }

  core_codepoint_t c = scan_peek(s, 0);
  bool constrained = str->simple_escapes.data || str->escape_rules;
  bool valid = !constrained || in_set(lex, str->simple_escapes, c);

  scan_advance(s);

  return valid ? ESCAPE_OK : ESCAPE_INVALID;
}

static body_stop scan_body(parse_scanner_t *s, const scan_lex_string_t *str,
                           size_t *scalar_count, size_t *delimiter_depth) {
  bool raw = (str->flags & SCAN_LEX_STR_RAW) != 0;
  bool multi = (str->flags & SCAN_LEX_STR_MULTILINE) != 0;
  bool nested = (str->flags & SCAN_LEX_STR_NESTED) != 0;

  while (!scan_at_eof(s)) {
    if (partial_at_end(s, str->closer) ||
        (nested && partial_at_end(s, str->nest_open)) ||
        (str->interp_open.u8_bytes && partial_at_end(s, str->interp_open))) {
      return BODY_UNTERMINATED;
    }

    if (!raw && str->escape.u8_bytes && at_str(s, str->escape)) {
      size_t escape_at = s->cursor;
      uint32_t escape_line = s->line;
      uint32_t escape_col = s->column;

      scan_advance_bytes(s, str->escape.u8_bytes);

      escape_result_t result = scan_string_escape(s, str);

      if (result == ESCAPE_INCOMPLETE) {
        s->cursor = escape_at;
        s->line = escape_line;
        s->column = escape_col;

        return BODY_UNTERMINATED;
      }

      if (result == ESCAPE_INVALID) {
        scan_set_invalid(s, CORE_STRING_STATIC("invalid string escape"),
                         s->mark);
      }

      (*scalar_count)++;

      continue;
    }

    if (str->interp_open.u8_bytes && at_str(s, str->interp_open)) {
      if (!(str->flags & SCAN_LEX_STR_HOLE_TOKENS)) {
        scan_advance_bytes(s, str->interp_open.u8_bytes);
      }

      return BODY_HOLE;
    }

    if (nested && at_str(s, str->nest_open)) {
      scan_advance_bytes(s, str->nest_open.u8_bytes);
      (*delimiter_depth)++;
      (*scalar_count)++;
      continue;
    }

    if (at_str(s, str->closer)) {
      scan_advance_bytes(s, str->closer.u8_bytes);

      if (*delimiter_depth) {
        (*delimiter_depth)--;
        (*scalar_count)++;
        continue;
      }

      if ((str->flags & SCAN_LEX_STR_ONE_SCALAR) && *scalar_count != 1) {
        scan_set_invalid(
            s,
            CORE_STRING_STATIC(
                "character literal must contain exactly one scalar"),
            s->mark);
      }

      return BODY_CLOSED;
    }

    if (!multi && core_codepoint_is_line_terminator(scan_peek(s, 0))) {
      return BODY_INVALID_NEWLINE;
    }

    scan_advance(s);
    (*scalar_count)++;
  }

  return BODY_UNTERMINATED;
}

static bool push_mode(scan_lex_session_t *run, const scan_lex_string_t *str,
                      size_t delimiter_depth) {
  core_list_push(run->modes,
                 ((lex_mode_t){
                     .str = str,
                     .depth = 0,
                     .delimiter_depth = delimiter_depth,
                     .in_hole = true,
                     .at_open = (str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0,
                 }));

  return true;
}

static bool try_string(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  const scan_lex_string_t *best = nullptr;
  uint32_t node = 0;
  size_t at = s->cursor;

  while (at < s->input.u8_bytes) {
    uint32_t child = trie_child(&((scan_lex_t *)lex)->string_nodes,
                                &((scan_lex_t *)lex)->string_edges,
                                node, (uint8_t)s->input.data[at], false);

    if (child == UINT32_MAX) {
      break;
    }

    node = child;
    at++;

    int32_t recipe = lex->string_nodes.data[node].tid;
    if (recipe > 0) {
      best = &lex->spec->strings[recipe - 1];
    }
  }

  s->work_bytes += at - s->cursor;

  if (!best) {
    return false;
  }

  scan_mark(s);
  scan_advance_bytes(s, best->opener.u8_bytes);

  size_t scalars = 0;
  size_t delimiter_depth = 0;
  body_stop stop = scan_body(s, best, &scalars, &delimiter_depth);

  if (stop == BODY_HOLE) {
    emit_named(s, best->tok, PARSE_TOK_STRING_LIT);
    push_mode(run, best, delimiter_depth);

    return true;
  }

  if (stop == BODY_UNTERMINATED) {
    run->pending_string = best;
    run->pending_string_scalars = scalars;
    run->pending_string_delimiter_depth = delimiter_depth;
    run->pending_string_tail = false;
    scan_set_incomplete(s, CORE_STRING_STATIC("unterminated string literal"),
                        s->mark);

    return false;
  }

  emit_named(s, best->tok, PARSE_TOK_STRING_LIT);

  if (stop == BODY_INVALID_NEWLINE) {
    previous_token_invalid(s,
                           CORE_STRING_STATIC("newline in single-line string"));
  }

  return true;
}

static bool resume_string(parse_scanner_t *s, bool mark) {
  scan_lex_session_t *run = s->user_state;
  lex_mode_t *m = &run->modes.data[run->modes.len - 1];

  if (mark) {
    scan_mark(s);
  }

  size_t scalars = 0;
  body_stop stop = scan_body(s, m->str, &scalars, &m->delimiter_depth);

  if (stop == BODY_HOLE) {
    emit_named(s, m->str->tok_mid, PARSE_TOK_STRING_LIT);
    m->in_hole = true;
    m->at_open = (m->str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0;

    return true;
  }

  if (stop == BODY_UNTERMINATED) {
    run->pending_string = m->str;
    run->pending_string_scalars = scalars;
    run->pending_string_delimiter_depth = m->delimiter_depth;
    run->pending_string_tail = true;
    scan_set_incomplete(s, CORE_STRING_STATIC("unterminated string literal"),
                        s->mark);

    return false;
  }

  emit_named(s, m->str->tok_end, PARSE_TOK_STRING_LIT);
  run->modes.len--;

  if (stop == BODY_INVALID_NEWLINE) {
    previous_token_invalid(s,
                           CORE_STRING_STATIC("newline in single-line string"));
  }

  return true;
}

static bool resume_pending_string(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_string_t *str = run->pending_string;
  bool tail = run->pending_string_tail;
  body_stop stop = scan_body(s, str, &run->pending_string_scalars,
                             &run->pending_string_delimiter_depth);

  if (stop == BODY_UNTERMINATED) {
    scan_set_incomplete(s, CORE_STRING_STATIC("unterminated string literal"),
                        s->mark);

    return false;
  }

  size_t delimiter_depth = run->pending_string_delimiter_depth;

  run->pending_string = nullptr;
  run->pending_string_scalars = 0;
  run->pending_string_delimiter_depth = 0;
  run->pending_string_tail = false;

  if (stop == BODY_HOLE) {
    emit_named(s, tail ? str->tok_mid : str->tok, PARSE_TOK_STRING_LIT);

    if (tail) {
      lex_mode_t *m = &run->modes.data[run->modes.len - 1];

      m->delimiter_depth = delimiter_depth;
      m->in_hole = true;
      m->at_open = (str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0;
    } else {
      push_mode(run, str, delimiter_depth);
    }

    return true;
  }

  emit_named(s, tail ? str->tok_end : str->tok, PARSE_TOK_STRING_LIT);

  if (tail) {
    run->modes.len--;
  }

  if (stop == BODY_INVALID_NEWLINE) {
    previous_token_invalid(s,
                           CORE_STRING_STATIC("newline in single-line string"));
  }

  return true;
}

typedef enum : uint8_t {
  REGION_NONE,
  REGION_CONSUMED,
  REGION_STOP,
} region_result_t;

static region_result_t try_region(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_region_t *matched = nullptr;
  bool opening = false;
  size_t prefix_bytes = 0;
  for (const scan_lex_region_t *region = run->program->spec->regions;
       region && region->opener.data; region++) {
    core_string_t prefixes[] = {region->opener, region->closer};
    for (size_t i = 0; i < 2; i++) {
      if (at_str(s, prefixes[i]) && prefixes[i].u8_bytes > prefix_bytes) {
        matched = region;
        opening = i == 0;
        prefix_bytes = prefixes[i].u8_bytes;
      }
      else if (!s->source_final && partial_at_end(s, prefixes[i])) {
        scan_set_incomplete(s,
            CORE_STRING_STATIC("region anchor may continue"), s->cursor);
        return REGION_STOP;
      }
    }
  }
  if (!matched) return REGION_NONE;
  if (run->line_has_token) {
    scan_set_invalid(s,
        CORE_STRING_STATIC("region anchor must be the only non-whitespace text on its line"),
        s->cursor);
    return REGION_STOP;
  }

  scan_mark(s);
  scan_advance_bytes(s, prefix_bytes);
  size_t name_start = s->cursor;
  while (!scan_at_eof(s)
         && !core_codepoint_is_line_terminator(scan_peek(s, 0))
         && !at_str(s, matched->terminator)) {
    scan_advance(s);
  }
  if (scan_at_eof(s) && !s->source_final) {
    scan_set_incomplete(s,
        CORE_STRING_STATIC("region anchor may continue"), s->mark);
    return REGION_STOP;
  }
  if (scan_at_eof(s) || core_codepoint_is_line_terminator(scan_peek(s, 0))) {
    scan_set_invalid(s, CORE_STRING_STATIC("malformed region anchor"),
                     s->mark);
    return REGION_STOP;
  }
  size_t name_end = s->cursor;
  scan_advance_bytes(s, matched->terminator.u8_bytes);
  while (!scan_at_eof(s) && !core_codepoint_is_line_terminator(scan_peek(s, 0))) {
    if (!is_line_whitespace(scan_peek(s, 0))) {
      scan_set_invalid(s,
          CORE_STRING_STATIC("region anchor must be the only non-whitespace text on its line"),
          s->mark);
      return REGION_STOP;
    }
    scan_advance(s);
  }

  core_string_t name = core_string_borrow(
      s->input.data + name_start, name_end - name_start);
  parse_trivia_t *trivia = scan_add_trivia(
      s, scan_extract(s), opening ? PARSE_TRIVIA_REGION_OPEN
                                  : PARSE_TRIVIA_REGION_CLOSE);
  if (!trivia) return REGION_STOP;
  trivia->region_name = core_string_dup(name);
  trivia->owns_region_name = true;
  return REGION_CONSUMED;
}

static bool try_comment(parse_scanner_t *s) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  lex_bucket_t bucket = lex->comment_buckets[scan_peek_byte(s, 0)];

  const scan_lex_comment_t *c = nullptr;
  uint32_t fill_level = 0;
  size_t opener_bytes = 0;

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_comment_t *k = lex->comment_dispatch[i];
    uint32_t candidate_level = 0;
    size_t candidate_bytes = k->opener.u8_bytes;
    bool matches = k->fill.data ? comment_fill_open(s, k, &candidate_level,
                                                    &candidate_bytes)
                                : at_str(s, k->opener);
    if (matches && (!c || candidate_bytes > opener_bytes)) {
      c = k;
      fill_level = candidate_level;
      opener_bytes = candidate_bytes;
    }
  }

  if (c) {
    scan_mark(s);
    scan_advance_bytes(s, opener_bytes);

    if (c->kind == SCAN_LEX_LINE) {
      while (!scan_at_eof(s) && !core_codepoint_is_line_terminator(scan_peek(s, 0))) {
        scan_advance(s);
      }

      if (scan_at_eof(s) && !s->source_final) {
        scan_lex_session_t *run = s->user_state;

        run->pending_comment = c;
        run->pending_comment_depth = 0;
        scan_set_incomplete(s, CORE_STRING_STATIC("line comment may continue"),
                            s->mark);

        return false;
      }
    } else {
      int32_t depth = 1;
      core_string_t filled_close = c->fill.data
                                       ? comment_close_pattern(c, fill_level)
                                       : (core_string_t){0};
      core_string_t close = c->fill.data ? filled_close : c->closer;

      while (!scan_at_eof(s) && depth > 0) {
        if (c->nested && at_str(s, c->opener)) {
          scan_advance_bytes(s, c->opener.u8_bytes);
          depth++;

          continue;
        }

        if (at_str(s, close)) {
          scan_advance_bytes(s, close.u8_bytes);
          depth--;

          continue;
        }

        if (partial_at_end(s, close) ||
            (c->nested && partial_at_end(s, c->opener))) {
          break;
        }

        scan_advance(s);
      }

      if (depth > 0) {
        scan_lex_session_t *run = s->user_state;

        run->pending_comment = c;
        run->pending_comment_depth = depth;
        run->pending_comment_fill_level = fill_level;
        core_free(filled_close.data);
        scan_set_incomplete(s, CORE_STRING_STATIC("unterminated block comment"),
                            s->mark);

        return false;
      }
      core_free(filled_close.data);
    }

    if (c->tok.data && c->tok.u8_bytes) {
      emit_named(s, c->tok, PARSE_TOK_OTHER);
    } else {
      scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);
    }

    return true;
  }

  return false;
}

static bool resume_comment(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_comment_t *c = run->pending_comment;
  int32_t depth = run->pending_comment_depth;
  uint32_t fill_level = run->pending_comment_fill_level;

  if (c->kind == SCAN_LEX_LINE) {
    while (!scan_at_eof(s) && !core_codepoint_is_line_terminator(scan_peek(s, 0))) {
      scan_advance(s);
    }

    if (scan_at_eof(s) && !s->source_final) {
      scan_set_incomplete(s, CORE_STRING_STATIC("line comment may continue"),
                          s->mark);

      return false;
    }

    run->pending_comment = nullptr;

    if (c->tok.data && c->tok.u8_bytes) {
      emit_named(s, c->tok, PARSE_TOK_OTHER);
    } else {
      scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);
    }

    return true;
  }

  core_string_t filled_close =
      c->fill.data ? comment_close_pattern(c, fill_level) : (core_string_t){0};
  core_string_t close = c->fill.data ? filled_close : c->closer;

  while (!scan_at_eof(s) && depth > 0) {
    if (c->nested && at_str(s, c->opener)) {
      scan_advance_bytes(s, c->opener.u8_bytes);
      depth++;

      continue;
    }

    if (at_str(s, close)) {
      scan_advance_bytes(s, close.u8_bytes);
      depth--;

      continue;
    }

    if (partial_at_end(s, close) ||
        (c->nested && partial_at_end(s, c->opener))) {
      break;
    }

    scan_advance(s);
  }

  if (depth > 0) {
    run->pending_comment_depth = depth;
    core_free(filled_close.data);
    scan_set_incomplete(s, CORE_STRING_STATIC("unterminated block comment"),
                        s->mark);

    return false;
  }

  run->pending_comment = nullptr;
  run->pending_comment_depth = 0;
  run->pending_comment_fill_level = 0;
  core_free(filled_close.data);

  if (c->tok.data && c->tok.u8_bytes) {
    emit_named(s, c->tok, PARSE_TOK_OTHER);
  } else {
    scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);
  }

  return true;
}

static bool number_digit(const scan_lex_t *lex, core_codepoint_t c,
                         core_string_t digits) {
  return digits.data ? in_set(lex, digits, c) : unicode_cp_is_ascii_digit(c);
}

static void number_wait(parse_scanner_t *s) {
  scan_set_incomplete(s, CORE_STRING_STATIC("numeric literal may continue"),
                      s->mark);
}

static void number_scan_digits(parse_scanner_t *s, core_string_t digits,
                               core_string_t separator, size_t *count,
                               bool *after_digit, bool *need_digit,
                               bool *malformed) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  while (!scan_at_eof(s)) {
    core_codepoint_t c = scan_peek(s, 0);

    if (number_digit(lex, c, digits)) {
      scan_advance(s);
      (*count)++;
      *after_digit = true;
      *need_digit = false;
      continue;
    }

    if (!in_set(lex, separator, c)) {
      break;
    }

    scan_advance(s);
    if (!*after_digit) {
      *malformed = true;
    }
    *after_digit = false;
    *need_digit = true;
  }
}

static const scan_lex_number_form_t *number_form_at(parse_scanner_t *s,
                                                    size_t at, bool *partial) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  uint32_t node =
      run->pending_number_prefix_consumed ? run->pending_number_prefix_node : 0;
  int32_t best = run->pending_number_prefix_consumed
                     ? run->pending_number_prefix_best
                     : lex->number_nodes.data[0].tid;
  size_t cursor = at + run->pending_number_prefix_consumed;

  *partial = false;
  while (cursor < s->input.u8_bytes) {
    uint32_t child = trie_child(&((scan_lex_t *)lex)->number_nodes,
                                &((scan_lex_t *)lex)->number_edges,
                                node, (uint8_t)s->input.data[cursor], false);
    if (child == UINT32_MAX) {
      break;
    }
    node = child;
    cursor++;
    s->work_bytes++;
    if (lex->number_nodes.data[node].tid) {
      best = lex->number_nodes.data[node].tid;
    }
  }
  if (cursor == s->input.u8_bytes && lex->number_nodes.data[node].edge >= 0) {
    *partial = true;
    run->pending_number_prefix_node = node;
    run->pending_number_prefix_consumed = cursor - at;
    run->pending_number_prefix_best = best;
  } else {
    run->pending_number_prefix_node = 0;
    run->pending_number_prefix_consumed = 0;
    run->pending_number_prefix_best = 0;
  }

  return best ? &lex->spec->number_forms[best - 1] : nullptr;
}

static const scan_lex_suffix_t *
number_form_suffix(parse_scanner_t *s, const scan_lex_number_form_t *form,
                   bool *partial) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  size_t form_index = (size_t)(form - lex->spec->number_forms);
  uint32_t root = (uint32_t)lex->number_suffix_roots.data[form_index];
  uint32_t node = run->pending_number_suffix_consumed
                      ? run->pending_number_suffix_node
                      : root;
  int32_t best =
      run->pending_number_suffix_consumed ? run->pending_number_suffix_best : 0;
  size_t at = s->cursor + run->pending_number_suffix_consumed;

  *partial = false;
  while (at < s->input.u8_bytes) {
    uint32_t child = trie_child(&((scan_lex_t *)lex)->number_suffix_nodes,
                                &((scan_lex_t *)lex)->number_suffix_edges,
                                node, (uint8_t)s->input.data[at], false);
    if (child == UINT32_MAX) {
      break;
    }
    node = child;
    at++;
    s->work_bytes++;
    if (lex->number_suffix_nodes.data[node].tid) {
      best = lex->number_suffix_nodes.data[node].tid;
    }
  }
  if (at == s->input.u8_bytes &&
      (lex->number_suffix_nodes.data[node].edge >= 0 || best)) {
    *partial = true;
    run->pending_number_suffix_node = node;
    run->pending_number_suffix_consumed = at - s->cursor;
    run->pending_number_suffix_best = best;
  } else {
    run->pending_number_suffix_node = 0;
    run->pending_number_suffix_consumed = 0;
    run->pending_number_suffix_best = 0;
  }

  return best ? &form->suffixes[best - 1] : nullptr;
}

static bool continue_number(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  const scan_lex_number_form_t *form = run->pending_number_form;
  core_string_t separator = form ? form->separator : (core_string_t){0};

  for (;;) {
    switch (run->pending_number_phase) {
    case NUMBER_PREFIX: {
      if (run->pending_number_signed_only) {
        if (scan_at_eof(s) && !s->source_final) {
          number_wait(s);
          return false;
        }

        core_codepoint_t first = scan_peek(s, 0);
        bool starts =
            unicode_cp_is_ascii_digit(first) ||
            (first == '.' && unicode_cp_is_ascii_digit(scan_peek(s, 1)));

        if (!starts) {
          run->pending_number_phase = NUMBER_NONE;
          run->pending_number_signed_only = false;
          s->cursor = s->mark;
          s->line = s->mark_line;
          s->column = s->mark_col;

          return scan_lex_scan(s);
        }
        run->pending_number_signed_only = false;
      }

      bool partial = false;
      form = number_form_at(s, run->pending_number_prefix_at, &partial);
      if (partial && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (!form) {
        run->pending_number_phase = NUMBER_NONE;
        s->cursor = s->mark;
        s->line = s->mark_line;
        s->column = s->mark_col;
        return false;
      }
      run->pending_number_form = form;
      separator = form->separator;
      scan_advance_bytes(s, form->prefix.u8_bytes);
      if (form->prefix.u8_bytes &&
          (form->flags & SCAN_LEX_NUM_SEPARATOR_AFTER_PREFIX)) {
        run->pending_number_after_digit = true;
      }
      run->pending_number_phase = NUMBER_INTEGER_DIGITS;
      continue;
    }

    case NUMBER_INTEGER_DIGITS:
      number_scan_digits(
          s, form->digits, separator,
          &run->pending_number_digits, &run->pending_number_after_digit,
          &run->pending_number_need_digit, &run->pending_number_malformed);
      if (scan_at_eof(s) && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (run->pending_number_need_digit) {
        run->pending_number_malformed = true;
      }
      run->pending_number_phase = NUMBER_POINT;
      continue;

    case NUMBER_POINT:
      if ((form->flags & SCAN_LEX_NUM_POINT) && scan_peek(s, 0) == '.') {
        if (scan_remaining(s) == 1 && !s->source_final) {
          number_wait(s);
          return false;
        }
        bool next_digit = number_digit(
            lex, scan_peek(s, 1), form->digits);
        bool trailing = (form->flags & SCAN_LEX_NUM_TRAILING_POINT) &&
                        scan_peek(s, 1) != '.';
        if (next_digit || trailing) {
          run->pending_number_float = true;
          run->pending_number_point = true;
          scan_advance(s);
          run->pending_number_after_digit = false;
          run->pending_number_need_digit = false;
          run->pending_number_phase = NUMBER_FRACTION_DIGITS;
          continue;
        }
      }
      run->pending_number_phase = NUMBER_EXP_START;
      continue;

    case NUMBER_FRACTION_DIGITS:
      number_scan_digits(
          s, form->digits, separator,
          &run->pending_number_digits, &run->pending_number_after_digit,
          &run->pending_number_need_digit, &run->pending_number_malformed);
      if (scan_at_eof(s) && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (run->pending_number_need_digit) {
        run->pending_number_malformed = true;
      }
      run->pending_number_phase = NUMBER_EXP_START;
      continue;

    case NUMBER_EXP_START:
      if (in_set(lex, form->exponent, scan_peek(s, 0))) {
        run->pending_number_float = true;
        scan_advance(s);
        run->pending_number_phase = NUMBER_EXP_SIGN;
        continue;
      }
      if (run->pending_number_point &&
          (form->flags & SCAN_LEX_NUM_EXPONENT_REQUIRED_FOR_POINT)) {
        run->pending_number_malformed = true;
      }
      run->pending_number_phase = NUMBER_SUFFIX;
      continue;

    case NUMBER_EXP_SIGN:
      if (scan_at_eof(s) && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (scan_peek(s, 0) == '+' || scan_peek(s, 0) == '-') {
        scan_advance(s);
        run->pending_number_phase = NUMBER_EXP_DIGITS;
        if (scan_at_eof(s) && !s->source_final) {
          number_wait(s);
          return false;
        }
      } else {
        run->pending_number_phase = NUMBER_EXP_DIGITS;
      }
      run->pending_number_after_digit = false;
      run->pending_number_need_digit = false;
      continue;

    case NUMBER_EXP_DIGITS:
      number_scan_digits(
          s,
          form->exponent_digits.data ? form->exponent_digits
                                             : (core_string_t){0},
          separator, &run->pending_number_exp_digits,
          &run->pending_number_after_digit, &run->pending_number_need_digit,
          &run->pending_number_malformed);
      if (scan_at_eof(s) && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (!run->pending_number_exp_digits || run->pending_number_need_digit) {
        run->pending_number_malformed = true;
      }
      run->pending_number_phase = NUMBER_SUFFIX;
      continue;

    case NUMBER_SUFFIX: {
      if (scan_at_eof(s) && !s->source_final) {
        number_wait(s);
        return false;
      }
      bool partial = false;
      const scan_lex_suffix_t *suffix = number_form_suffix(s, form, &partial);
      if (partial && !s->source_final) {
        number_wait(s);
        return false;
      }
      if (suffix) {
        scan_advance_bytes(s, suffix->suffix.u8_bytes);
      }
      core_string_t tok = suffix ? suffix->tok
          : run->pending_number_float ? form->tok_float : form->tok_int;
      int32_t fallback =
          run->pending_number_float ? PARSE_TOK_FLOAT : PARSE_TOK_INTEGER;
      bool malformed = run->pending_number_malformed;

      if (!run->pending_number_digits) {
        malformed = true;
      }
      if (suffix && suffix->forms &&
          !(suffix->forms &
            (run->pending_number_float ? SCAN_LEX_SUFFIX_FLOAT
                                       : SCAN_LEX_SUFFIX_INTEGER))) {
        malformed = true;
      }
      if (id_cont_here(lex, scan_peek(s, 0))) {
        malformed = true;
      }

      run->pending_number_phase = NUMBER_NONE;
      emit_named(s, tok, fallback);
      if (malformed) {
        previous_token_invalid(s,
                               CORE_STRING_STATIC("malformed numeric literal"));
      }
      return true;
    }

    case NUMBER_NONE:
      return false;
    }
  }
}

static bool try_number(parse_scanner_t *s) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;

  if (!lex->number_enabled) {
    return false;
  }

  const scan_lex_number_form_t *plain = nullptr;

  if (lex->spec->number_forms && lex->number_nodes.data[0].tid) {
    plain = &lex->spec->number_forms[lex->number_nodes.data[0].tid - 1];
  }

  core_codepoint_t c = scan_peek(s, 0);
  bool signed_start = plain && in_set(lex, plain->sign, c);

  if (signed_start && scan_remaining(s) == 1 && !s->source_final) {
    scan_mark(s);
    scan_advance(s);
  } else {
    int32_t lead = signed_start ? 1 : 0;
    core_codepoint_t first = scan_peek(s, lead);

    bool leading_point =
        first == '.' &&
        plain && (plain->flags & SCAN_LEX_NUM_LEADING_POINT) &&
        number_digit(lex, scan_peek(s, lead + 1),
                     plain->digits);

    if (!unicode_cp_is_ascii_digit(first) && !leading_point) {
      return false;
    }

    scan_mark(s);
    if (lead) {
      scan_advance(s);
    }
  }

  scan_lex_session_t *run = s->user_state;

  run->pending_number_phase = NUMBER_PREFIX;
  run->pending_number_form = nullptr;
  run->pending_number_prefix_at = s->cursor;
  run->pending_number_prefix_node = 0;
  run->pending_number_prefix_consumed = 0;
  run->pending_number_prefix_best = 0;
  run->pending_number_suffix_node = 0;
  run->pending_number_suffix_consumed = 0;
  run->pending_number_suffix_best = 0;
  run->pending_number_digits = 0;
  run->pending_number_exp_digits = 0;
  run->pending_number_after_digit = false;
  run->pending_number_need_digit = false;
  run->pending_number_malformed = false;
  run->pending_number_float = false;
  run->pending_number_point = false;
  run->pending_number_signed_only =
      signed_start && s->cursor == s->input.u8_bytes;

  if (scan_peek(s, 0) == '.') {
    run->pending_number_form = plain;
    run->pending_number_float = true;
    run->pending_number_point = true;
    scan_advance(s);
    run->pending_number_phase = NUMBER_FRACTION_DIGITS;
  }

  return continue_number(s);
}

static bool sigil_attaches(parse_scanner_t *s, const scan_lex_sigil_t *sigil) {
  if (sigil->attach_previous) return true;
  if (!sigil->attach_after) return false;

  parse_token_info_t *previous = scan_prev_token(s);
  if (!previous
      || parse_token_end_byte(previous) != s->source_base + s->cursor) {
    return false;
  }

  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  size_t index = (size_t)(sigil - lex->spec->sigils);
  return tid_set_contains(&lex->sigil_attach_after[index],
                          parse_token_tid(previous));
}

static bool try_sigil(parse_scanner_t *s, bool before_string) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  lex_bucket_t bucket = lex->sigil_buckets[scan_peek_byte(s, 0)];

  const scan_lex_sigil_t *best = nullptr;
  bool awaiting_body = false;

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_sigil_t *g = lex->sigil_dispatch[i];
    if (g->before_string != before_string || !at_str(s, g->prefix)) {
      continue;
    }
    if (g->attach_previous) {
      parse_token_info_t *previous = scan_prev_token(s);
      if (!previous
          || parse_token_end_byte(previous) != s->source_base + s->cursor
          || (g->prefix.u8_bytes == 1
              && parse_token_tid(previous) == (uint8_t)g->prefix.data[0]))
        continue;
    }

    if (!s->source_final &&
        s->cursor + g->prefix.u8_bytes == s->input.u8_bytes) {
      awaiting_body = true;
    }

    core_codepoint_t after = scan_peek(s, (int32_t)g->prefix.u8_bytes);

    bool ok = g->digits_only ? unicode_cp_is_ascii_digit(after)
                             : unicode_is_id_start(after);

    if (!ok) {
      continue;
    }

    if (g->unless_closed) {
      size_t at = s->cursor + g->prefix.u8_bytes;
      while (at < s->input.u8_bytes) {
        uint32_t next = (uint32_t)at;
        int32_t cp = unicode_utf8_decode(s->input.data,
            (uint32_t)s->input.u8_bytes, &next);
        if (cp < 0 || !unicode_is_id_continue(cp)) break;
        at = next;
      }
      if (at == s->input.u8_bytes && !s->source_final) {
        scan_set_incomplete(s, CORE_STRING_STATIC("sigil may continue"),
                            s->cursor);
        return false;
      }
      if (at + g->prefix.u8_bytes <= s->input.u8_bytes
          && !memcmp(s->input.data + at, g->prefix.data, g->prefix.u8_bytes))
        continue;
    }

    if (!best || g->prefix.u8_bytes > best->prefix.u8_bytes) {
      best = g;
    }
  }

  if (!best) {
    if (awaiting_body) {
      scan_set_incomplete(s, CORE_STRING_STATIC("sigil needs a body"),
                          s->cursor);
    }
    return false;
  }

  bool attach = sigil_attaches(s, best);
  scan_mark(s);
  scan_advance_bytes(s, best->prefix.u8_bytes);

  if (best->digits_only) {
    while (unicode_cp_is_ascii_digit(scan_peek(s, 0))) {
      scan_advance(s);
    }
  } else {
    while (!scan_at_eof(s) && unicode_is_id_continue(scan_peek(s, 0))) {
      scan_advance(s);
    }
  }

  if (scan_at_eof(s) && !s->source_final) {
    scan_lex_session_t *run = s->user_state;

    if (attach) {
      run->pending_attached_sigil = best;
      scan_set_incomplete(s, CORE_STRING_STATIC("sigil may continue"),
                          s->mark);
      return false;
    }

    run->pending_identifier = true;
    run->pending_identifier_tid = tid_for(lex->g, best->tok, lex->tid_ident);
    run->pending_identifier_digits = best->digits_only;
    scan_set_incomplete(s, CORE_STRING_STATIC("sigil may continue"), s->mark);

    return false;
  }

  if (attach) {
    parse_token_info_t *previous = scan_prev_token(s);
    if (!previous
        || core_option_is_set(parse_token_postfix_modifier(previous))) {
      scan_set_invalid(s, CORE_STRING_STATIC("invalid postfix modifier"),
                       s->mark);
      return false;
    }
    core_string_t name = core_string_from_raw(
        s->input.data + s->mark + best->prefix.u8_bytes,
        (int64_t)(s->cursor - s->mark - best->prefix.u8_bytes));
    parse_token_set_postfix_modifier(
        previous, core_option_set(core_string_t, name), true);
    parse_token_set_span(previous, parse_token_start_byte(previous),
                         s->source_base + s->cursor);
    return scan_lex_scan(s)
        || (scan_at_eof(s) && s->status == SCAN_STATUS_ACTIVE);
  }

  emit_named(s, best->tok, lex->tid_ident);

  return true;
}

static bool regex_may_start(const scan_lex_t *lex, const scan_lex_regex_t *r,
                            parse_scanner_t *s) {
  parse_token_info_t *prev = scan_prev_token(s);

  if (!prev) {
    return true;
  }

  size_t index = (size_t)(r - lex->spec->regexes);
  return !tid_set_contains(&lex->regex_not_after[index], parse_token_tid(prev));
}

static body_stop scan_regex_body(parse_scanner_t *s, const scan_lex_regex_t *r,
                                 bool *in_class) {
  while (!scan_at_eof(s)) {
    core_codepoint_t k = scan_peek(s, 0);

    if (core_codepoint_is_line_terminator(k)) {
      return BODY_INVALID_NEWLINE;
    }

    if (partial_at_end(s, r->closer)) {
      return BODY_UNTERMINATED;
    }

    if (k == '\\') {
      if (scan_remaining(s) == 1) {
        return BODY_UNTERMINATED;
      }

      scan_advance_n(s, 2);

      continue;
    }

    if (k == '[') {
      *in_class = true;
      scan_advance(s);

      continue;
    }

    if (k == ']') {
      *in_class = false;
      scan_advance(s);

      continue;
    }

    if (!*in_class && at_str(s, r->closer)) {
      scan_advance_bytes(s, r->closer.u8_bytes);

      return BODY_CLOSED;
    }

    scan_advance(s);
  }

  return BODY_UNTERMINATED;
}

static bool try_regex(parse_scanner_t *s) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  lex_bucket_t bucket = lex->regex_buckets[scan_peek_byte(s, 0)];

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_regex_t *r = lex->regex_dispatch[i];
    if (!at_str(s, r->opener) || !regex_may_start(lex, r, s)) {
      continue;
    }

    scan_mark(s);
    scan_advance_bytes(s, r->opener.u8_bytes);

    bool in_class = false;
    body_stop stop = scan_regex_body(s, r, &in_class);

    if (stop != BODY_CLOSED) {
      if (stop == BODY_UNTERMINATED) {
        scan_lex_session_t *run = s->user_state;

        run->pending_regex = r;
        run->pending_regex_in_class = in_class;
        scan_set_incomplete(
            s, CORE_STRING_STATIC("unterminated regular expression"), s->mark);

        return false;
      }

      emit_named(s, r->tok, PARSE_TOK_STRING_LIT);
      previous_token_invalid(
          s, CORE_STRING_STATIC("newline in regular expression"));

      return true;
    }

    while (!scan_at_eof(s) && in_set(lex, r->suffix_chars, scan_peek(s, 0))) {
      scan_advance(s);
    }

    emit_named(s, r->tok, PARSE_TOK_STRING_LIT);

    return true;
  }

  return false;
}

static bool resume_regex(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_regex_t *r = run->pending_regex;
  body_stop stop = scan_regex_body(s, r, &run->pending_regex_in_class);

  if (stop == BODY_UNTERMINATED) {
    scan_set_incomplete(
        s, CORE_STRING_STATIC("unterminated regular expression"), s->mark);

    return false;
  }

  run->pending_regex = nullptr;
  run->pending_regex_in_class = false;

  if (stop == BODY_INVALID_NEWLINE) {
    emit_named(s, r->tok, PARSE_TOK_STRING_LIT);
    previous_token_invalid(s,
                           CORE_STRING_STATIC("newline in regular expression"));

    return true;
  }

  while (!scan_at_eof(s) &&
         in_set(((scan_lex_session_t *)s->user_state)->program, r->suffix_chars,
                scan_peek(s, 0))) {
    scan_advance(s);
  }

  emit_named(s, r->tok, PARSE_TOK_STRING_LIT);

  return true;
}

static core_string_t heredoc_token(const scan_lex_heredoc_t *h,
                                   uint8_t quote_kind);

static core_string_t heredoc_body_value(parse_scanner_t *s,
                                        const scan_lex_heredoc_t *h,
                                        size_t body, size_t end) {
  if (!h->body_indent_strip || body >= end) {
    return core_string_from_raw(s->input.data + body, (int64_t)(end - body));
  }

  size_t minimum = SIZE_MAX;
  size_t line = body;

  while (line < end) {
    size_t at = line;
    while (at < end &&
           (s->input.data[at] == ' ' || s->input.data[at] == '\t')) {
      at++;
    }
    if (at < end && s->input.data[at] != '\n' && at - line < minimum) {
      minimum = at - line;
    }
    while (at < end && s->input.data[at] != '\n') {
      at++;
    }
    line = at < end ? at + 1 : at;
  }

  if (minimum == SIZE_MAX || !minimum) {
    return core_string_from_raw(s->input.data + body, (int64_t)(end - body));
  }

  core_buffer_t *result = core_buffer_empty();
  line = body;
  while (line < end) {
    size_t at = line;
    size_t remove = 0;
    while (at < end && remove < minimum &&
           (s->input.data[at] == ' ' || s->input.data[at] == '\t')) {
      at++;
      remove++;
    }
    size_t line_end = at;
    while (line_end < end && s->input.data[line_end] != '\n') {
      line_end++;
    }
    core_buffer_append(result, s->input.data + at, line_end - at);
    if (line_end < end) {
      core_buffer_putc(result, '\n');
      line_end++;
    }
    line = line_end;
  }

  size_t len = result->byte_len;
  return (core_string_t){.data = core_buffer_take(result), .u8_bytes = len};
}

static void emit_heredoc(parse_scanner_t *s, const scan_lex_heredoc_t *h,
                         size_t tag_a, size_t tag_b, size_t body,
                         size_t body_end, size_t consumed_end) {
  scan_lex_session_t *run = s->user_state;
  core_string_t name = heredoc_token(h, run->pending_heredoc_quote_kind);
  core_string_t value = heredoc_body_value(s, h, body, body_end);

  run->pending_heredoc = nullptr;
  run->skip_from = body;
  run->skip_to = consumed_end;
  scan_emit_owned(s, tid_for(run->program->g, name, PARSE_TOK_STRING_LIT),
                  core_option_set(core_string_t, value));

  parse_token_info_t *tok = scan_prev_token(s);
  if (tok) {
    parse_token_set_content_span(tok, s->source_base + body,
                                 s->source_base + body_end);
    parse_token_set_label_span(tok, s->source_base + tag_a,
                               s->source_base + tag_b);
  }
}

static bool scan_heredoc_lines(parse_scanner_t *s, const scan_lex_heredoc_t *h,
                               size_t tag_a, size_t tag_b, size_t body,
                               size_t scan) {
  scan_lex_session_t *run = s->user_state;
  size_t work_start = scan;
  size_t matched = run->pending_heredoc_match;
  bool at_indent = run->pending_heredoc_indent;
  bool rejected = run->pending_heredoc_rejected;
  size_t line_start = run->pending_heredoc_line_start;
  size_t tag_len = tag_b - tag_a;

  while (scan < s->input.u8_bytes) {
    uint8_t byte = (uint8_t)s->input.data[scan];
    if (byte == '\n') {
      if (!rejected && matched == tag_len) {
        size_t end = scan + 1;
        s->work_bytes += end - work_start;
        emit_heredoc(s, h, tag_a, tag_b, body, line_start, end);
        return true;
      }

      scan++;
      line_start = scan;
      matched = 0;
      rejected = false;
      at_indent = h->terminator_indent;
      continue;
    }

    if (at_indent && (byte == ' ' || byte == '\t')) {
      scan++;
      continue;
    }
    at_indent = false;

    if (!rejected && matched < tag_len &&
        byte == (uint8_t)s->input.data[tag_a + matched]) {
      matched++;
    } else {
      rejected = true;
    }
    scan++;
  }

  if (s->source_final && !rejected && matched == tag_len) {
    size_t end = scan;
    s->work_bytes += end - work_start;
    emit_heredoc(s, h, tag_a, tag_b, body, line_start, end);
    return true;
  }

  run->pending_heredoc = h;
  run->pending_heredoc_tag_start = tag_a;
  run->pending_heredoc_tag_end = tag_b;
  run->pending_heredoc_body = body;
  run->pending_heredoc_scan = scan;
  run->pending_heredoc_line_start = line_start;
  run->pending_heredoc_match = matched;
  run->pending_heredoc_indent = at_indent;
  run->pending_heredoc_rejected = rejected;
  s->work_bytes += s->input.u8_bytes - work_start;
  scan_set_incomplete(s, CORE_STRING_STATIC("unterminated heredoc"), s->mark);
  s->status_end_byte = s->source_base + s->input.u8_bytes;

  return false;
}

static bool heredoc_tag_extra(core_string_t extra, uint8_t byte) {
  for (size_t i = 0; extra.data && i < extra.u8_bytes; i++) {
    if ((uint8_t)extra.data[i] == byte) {
      return true;
    }
  }
  return false;
}

static core_string_t heredoc_token(const scan_lex_heredoc_t *h,
                                   uint8_t quote_kind) {
  if ((h->interpolate & quote_kind) && h->interpolated_tok.data) {
    return h->interpolated_tok;
  }
  return h->tok;
}

static bool invalid_heredoc_header(parse_scanner_t *s, core_string_t message,
                                   size_t at) {
  scan_lex_session_t *run = s->user_state;

  run->pending_heredoc = nullptr;
  run->pending_heredoc_header_phase = HEREDOC_HEADER_NONE;
  scan_set_invalid(s, message, at);
  return false;
}

static bool continue_heredoc_header(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_heredoc_t *h = run->pending_heredoc;
  const scan_lex_heredoc_header_t *header = &h->header;
  size_t scan = run->pending_heredoc_header_scan;
  size_t work_start = scan;

  if (run->pending_heredoc_header_phase == HEREDOC_HEADER_LEADING) {
    while (header->leading_space && scan < s->input.u8_bytes &&
           (s->input.data[scan] == ' ' || s->input.data[scan] == '\t')) {
      scan++;
    }
    if (scan == s->input.u8_bytes && !s->source_final) {
      goto wait;
    }

    uint8_t quote = scan < s->input.u8_bytes ? (uint8_t)s->input.data[scan] : 0;
    uint8_t quote_kind = SCAN_LEX_HEREDOC_UNQUOTED;

    if (quote == '\'' || quote == '"') {
      quote_kind = quote == '\'' ? SCAN_LEX_HEREDOC_SINGLE_QUOTED
                                 : SCAN_LEX_HEREDOC_DOUBLE_QUOTED;
      if (!(header->quotes & quote_kind)) {
        return invalid_heredoc_header(
            s, CORE_STRING_STATIC("quoted heredoc tag is not permitted"), scan);
      }
      run->pending_heredoc_quote = quote;
      scan++;
    } else if (!(header->quotes & SCAN_LEX_HEREDOC_UNQUOTED)) {
      return invalid_heredoc_header(
          s, CORE_STRING_STATIC("heredoc tag must be quoted"), scan);
    }

    run->pending_heredoc_quote_kind = quote_kind;
    run->pending_heredoc_tag_start = scan;
    run->pending_heredoc_header_phase = HEREDOC_HEADER_TAG;
  }

  if (run->pending_heredoc_header_phase == HEREDOC_HEADER_TAG) {
    uint8_t quote = run->pending_heredoc_quote;

    if (quote) {
      while (scan < s->input.u8_bytes &&
             (uint8_t)s->input.data[scan] != quote &&
             s->input.data[scan] != '\n') {
        scan++;
      }
      if (scan == s->input.u8_bytes && !s->source_final) {
        goto wait;
      }
      if (scan == s->input.u8_bytes || s->input.data[scan] == '\n') {
        return invalid_heredoc_header(
            s, CORE_STRING_STATIC("unterminated quoted heredoc tag"),
            run->pending_heredoc_tag_start - 1);
      }
      run->pending_heredoc_tag_end = scan;
      scan++;
    } else {
      if (scan == run->pending_heredoc_tag_start) {
        uint32_t next = (uint32_t)scan;
        int32_t cp =
            unicode_utf8_decode(s->input.data, (uint32_t)s->input.u8_bytes,
                                &next);
        if (cp < 0 && !s->source_final) {
          goto wait;
        }
        if (cp < 0 || (!unicode_is_id_start((core_codepoint_t)cp) &&
                       !(cp < 128 && heredoc_tag_extra(header->tag_start_extra,
                                                       (uint8_t)cp)))) {
          return invalid_heredoc_header(
              s, CORE_STRING_STATIC("invalid heredoc tag"), scan);
        }
        scan = next;
      }
      while (scan < s->input.u8_bytes) {
        uint32_t next = (uint32_t)scan;
        int32_t cp =
            unicode_utf8_decode(s->input.data, (uint32_t)s->input.u8_bytes,
                                &next);
        if (cp < 0) {
          if (!s->source_final) {
            goto wait;
          }
          return invalid_heredoc_header(
              s, CORE_STRING_STATIC("invalid UTF-8 in heredoc tag"), scan);
        }
        if (!unicode_is_id_continue((core_codepoint_t)cp) &&
            !(cp < 128 &&
              heredoc_tag_extra(header->tag_continue_extra, (uint8_t)cp))) {
          break;
        }
        scan = next;
      }
      if (scan == s->input.u8_bytes && !s->source_final) {
        goto wait;
      }
      run->pending_heredoc_tag_end = scan;
    }

    if (run->pending_heredoc_tag_end == run->pending_heredoc_tag_start) {
      return invalid_heredoc_header(s, CORE_STRING_STATIC("empty heredoc tag"),
                                    scan);
    }
    run->pending_heredoc_header_phase = HEREDOC_HEADER_LINE;
    run->pending_heredoc_token_end = scan;
  }

  if (h->queued) {
    core_string_t tag =
        core_string_from_raw(s->input.data + run->pending_heredoc_tag_start,
                             (int64_t)(run->pending_heredoc_tag_end -
                                       run->pending_heredoc_tag_start));
    size_t label_start = s->source_base + run->pending_heredoc_tag_start;
    size_t label_end = s->source_base + run->pending_heredoc_tag_end;

    s->work_bytes += scan - work_start;
    scan_advance_bytes(s, run->pending_heredoc_token_end - s->cursor);
    emit_named(s, heredoc_token(h, run->pending_heredoc_quote_kind),
               PARSE_TOK_STRING_LIT);

    parse_token_info_t *token = scan_prev_token(s);
    if (token) {
      parse_token_set_label_span(token, label_start, label_end);
    }
    core_list_push(run->heredoc_queue,
                   ((lex_heredoc_queue_t){
                       .recipe = h,
                       .tag = tag,
                       .token_index = token ? parse_token_index(token) : -1,
                       .quote_kind = run->pending_heredoc_quote_kind,
                   }));
    run->pending_heredoc = nullptr;
    run->pending_heredoc_header_phase = HEREDOC_HEADER_NONE;
    return true;
  }

  while (scan < s->input.u8_bytes && s->input.data[scan] != '\n') {
    if (!header->trailing_text && s->input.data[scan] != ' ' &&
        s->input.data[scan] != '\t') {
      return invalid_heredoc_header(
          s, CORE_STRING_STATIC("unexpected text after heredoc tag"), scan);
    }
    scan++;
  }
  if (scan == s->input.u8_bytes && !s->source_final) {
    goto wait;
  }

  s->work_bytes += scan - work_start;
  scan_advance_bytes(s, run->pending_heredoc_token_end - s->cursor);
  size_t body = scan < s->input.u8_bytes ? scan + 1 : scan;
  run->pending_heredoc_body = body;
  run->pending_heredoc_scan = body;
  run->pending_heredoc_line_start = body;
  run->pending_heredoc_match = 0;
  run->pending_heredoc_indent = h->terminator_indent;
  run->pending_heredoc_rejected = false;
  run->pending_heredoc_header_phase = HEREDOC_HEADER_NONE;
  return scan_heredoc_lines(s, h, run->pending_heredoc_tag_start,
                            run->pending_heredoc_tag_end, body, body);

wait:
  s->work_bytes += scan - work_start;
  run->pending_heredoc_header_scan = scan;
  scan_set_incomplete(s, CORE_STRING_STATIC("heredoc header may continue"),
                      s->mark);
  return false;
}

static bool try_heredoc(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  lex_bucket_t bucket = lex->heredoc_buckets[scan_peek_byte(s, 0)];

  for (uint32_t i = bucket.start; i < bucket.start + bucket.count; i++) {
    const scan_lex_heredoc_t *h = lex->heredoc_dispatch[i];
    if (!at_str(s, h->opener)) {
      continue;
    }
    scan_mark(s);
    run->pending_heredoc = h;
    run->pending_heredoc_header_phase = HEREDOC_HEADER_LEADING;
    run->pending_heredoc_header_scan = s->cursor + h->opener.u8_bytes;
    run->pending_heredoc_token_end = 0;
    run->pending_heredoc_quote = 0;
    run->pending_heredoc_quote_kind = 0;
    return continue_heredoc_header(s);
  }

  return false;
}

static bool resume_heredoc(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;

  if (run->pending_heredoc_header_phase != HEREDOC_HEADER_NONE) {
    return continue_heredoc_header(s);
  }

  return scan_heredoc_lines(
      s, run->pending_heredoc, run->pending_heredoc_tag_start,
      run->pending_heredoc_tag_end, run->pending_heredoc_body,
      run->pending_heredoc_scan);
}

static bool scan_terminal_trie(parse_scanner_t *s, size_t min_len,
                               uint32_t node, size_t consumed, size_t best_len,
                               int32_t best_tid) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  size_t start_consumed = consumed;

  while (s->mark + consumed < s->input.u8_bytes) {
    uint8_t byte = (uint8_t)s->input.data[s->mark + consumed];
    uint32_t child = trie_child(&((scan_lex_t *)lex)->term_nodes,
                                &((scan_lex_t *)lex)->term_edges,
                                node, byte, false);

    if (child == UINT32_MAX) {
      break;
    }

    node = child;
    consumed++;

    const lex_term_node_t *candidate = &lex->term_nodes.data[node];

    if (!candidate->tid || consumed < min_len) {
      continue;
    }

    if (candidate->word_boundary) {
      core_codepoint_t next;

      if (s->mark + consumed == s->input.u8_bytes && !s->source_final) {
        continue;
      }

      next = scan_peek(s, (int32_t)consumed);

      if (next > 0 && id_cont_here(lex, next)) {
        continue;
      }
    }

    best_len = consumed;
    best_tid = candidate->tid;
  }

  s->work_bytes += consumed - start_consumed;

  if (s->mark + consumed == s->input.u8_bytes && !s->source_final &&
      (lex->term_nodes.data[node].edge >= 0 ||
       (lex->term_nodes.data[node].tid &&
        lex->term_nodes.data[node].word_boundary))) {
    run->pending_terminal = true;
    run->pending_terminal_node = node;
    run->pending_terminal_consumed = consumed;
    run->pending_terminal_best_len = best_len;
    run->pending_terminal_best_tid = best_tid;
    run->pending_terminal_min_len = min_len;
    scan_set_incomplete(s, CORE_STRING_STATIC("terminal may continue"),
                        s->mark);

    return false;
  }

  if (best_tid) {
    scan_advance_bytes(s, best_len);
    scan_emit_marked(s, best_tid);

    return true;
  }

  return false;
}

static bool try_terminal_min(parse_scanner_t *s, size_t min_len) {
  scan_mark(s);
  return scan_terminal_trie(s, min_len, 0, 0, 0, 0);
}

static bool resume_terminal(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  uint32_t node = run->pending_terminal_node;
  size_t consumed = run->pending_terminal_consumed;
  size_t best_len = run->pending_terminal_best_len;
  int32_t best_tid = run->pending_terminal_best_tid;
  size_t min_len = run->pending_terminal_min_len;

  run->pending_terminal = false;

  bool emitted =
      scan_terminal_trie(s, min_len, node, consumed, best_len, best_tid);

  if (!emitted && !run->pending_terminal &&
      s->status != SCAN_STATUS_INCOMPLETE) {
    return scan_lex_scan(s);
  }

  return emitted;
}

static bool try_terminal(parse_scanner_t *s) { return try_terminal_min(s, 1); }

static void apply_identifier_profile(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  parse_token_info_t *token = scan_prev_token(s);

  if (!lex->identifier_profile || !token || !parse_token_has_value(token)) {
    return;
  }

  core_string_t spelling = core_option_get(parse_token_value(token));
  scan_unicode_identifier_result_t result;

  if (!scan_unicode_profile_process(lex->identifier_profile, spelling,
                                    &result)) {
    return;
  }

  if (result.normalized.data && core_string_eq(result.normalized, spelling)) {
    core_free(result.normalized.data);
  }
  else if (result.normalized.data) {
    parse_token_set_value(token,
                          core_option_set(core_string_t, result.normalized),
                          true);
  }

  if (result.diagnostic.data) {
    size_t local_start = parse_token_start_byte(token) - s->source_base
                       + result.diagnostic_start;
    scan_set_invalid(s, result.diagnostic, local_start);
    s->status_end_byte = parse_token_start_byte(token)
                       + result.diagnostic_end;
  }
}

static bool try_identifier(parse_scanner_t *s) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;

  if (!lex->ident_enabled) {
    return false;
  }

  if (!id_start_here(lex, scan_peek(s, 0))) {
    return false;
  }

  core_codepoint_t first = scan_peek(s, 0);

  scan_mark(s);

  while (!scan_at_eof(s) && id_cont_here(lex, scan_peek(s, 0))) {
    scan_advance(s);
  }

  bool upper =
      lex->tid_ident_upper != PARSE_TOK_IGNORED && first >= 'A' && first <= 'Z';

  if (scan_at_eof(s) && !s->source_final) {
    scan_lex_session_t *run = s->user_state;

    run->pending_identifier = true;
    run->pending_identifier_tid = upper ? lex->tid_ident_upper : lex->tid_ident;
    run->pending_identifier_digits = false;
    scan_set_incomplete(s, CORE_STRING_STATIC("identifier may continue"),
                        s->mark);

    return false;
  }

  scan_emit_marked(s, upper ? lex->tid_ident_upper : lex->tid_ident);
  apply_identifier_profile(s);

  return true;
}

static bool resume_identifier(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  while (!scan_at_eof(s) && (run->pending_identifier_digits
                                 ? unicode_cp_is_ascii_digit(scan_peek(s, 0))
                                 : id_cont_here(lex, scan_peek(s, 0)))) {
    scan_advance(s);
  }

  if (scan_at_eof(s) && !s->source_final) {
    scan_set_incomplete(s, CORE_STRING_STATIC("identifier may continue"),
                        s->mark);

    return false;
  }

  int32_t tid = run->pending_identifier_tid;

  run->pending_identifier = false;
  run->pending_identifier_tid = 0;
  run->pending_identifier_digits = false;
  scan_emit_marked(s, tid);
  apply_identifier_profile(s);

  return true;
}

static void track_bracket(scan_lex_session_t *run, core_codepoint_t c) {
  const scan_lex_t *lex = run->program;

  if (!lex->spec->brackets.data) {
    return;
  }

  core_string_t b = lex->spec->brackets;

  for (size_t i = 0; i + 1 < b.u8_bytes; i += 2) {
    if ((core_codepoint_t)(unsigned char)b.data[i] == c) {
      run->bracket_depth++;

      return;
    }

    if ((core_codepoint_t)(unsigned char)b.data[i + 1] == c) {
      if (run->bracket_depth > 0) {
        run->bracket_depth--;
      }

      return;
    }
  }
}

typedef enum {
  INDENT_LINE_CONTENT,
  INDENT_LINE_BLANK,
  INDENT_LINE_COMMENT,
  INDENT_LINE_INCOMPLETE,
} indent_line_kind_t;

typedef enum {
  LAYOUT_NONE,
  LAYOUT_EMITTED,
  LAYOUT_STOP,
} layout_result_t;

typedef struct {
  size_t end;
  size_t first_tab;
  int32_t column;
  bool has_space;
  bool has_tab;
} indent_measure_t;

static indent_measure_t measure_indent(parse_scanner_t *s, uint8_t tab_width,
                                       indent_measure_t result) {
  uint8_t width = tab_width ? tab_width : 8;

  while (result.end < s->input.u8_bytes) {
    if (s->input.data[result.end] == ' ') {
      result.has_space = true;
      result.column++;
    } else if (s->input.data[result.end] == '\t') {
      if (!result.has_tab) {
        result.first_tab = result.end;
      }

      result.has_tab = true;
      result.column += width - (result.column % width);
    } else {
      break;
    }

    result.end++;
  }

  return result;
}

static indent_measure_t pending_indent(const parse_scanner_t *s,
                                       const scan_lex_session_t *run) {
  if (!run->pending_indent) {
    return (indent_measure_t){
        .end = s->cursor,
        .first_tab = SIZE_MAX,
    };
  }

  return (indent_measure_t){
      .end = run->pending_indent_end,
      .first_tab = run->pending_indent_first_tab,
      .column = run->pending_indent_column,
      .has_space = run->pending_indent_has_space,
      .has_tab = run->pending_indent_has_tab,
  };
}

static void save_pending_indent(scan_lex_session_t *run, indent_measure_t m) {
  run->pending_indent = true;
  run->pending_indent_end = m.end;
  run->pending_indent_first_tab = m.first_tab;
  run->pending_indent_column = m.column;
  run->pending_indent_has_space = m.has_space;
  run->pending_indent_has_tab = m.has_tab;
}

static indent_line_kind_t indent_line_kind(parse_scanner_t *s,
                                           const scan_lex_t *lex, size_t at) {
  if (at >= s->input.u8_bytes) {
    return s->source_final ? INDENT_LINE_BLANK : INDENT_LINE_INCOMPLETE;
  }

  if (s->input.data[at] == '\n' || s->input.data[at] == '\r') {
    return INDENT_LINE_BLANK;
  }

  bool partial = false;

  for (const scan_lex_comment_t *c = lex->spec->comments; c && c->opener.data;
       c++) {
    if (c->kind != SCAN_LEX_LINE || c->tok.data) {
      continue;
    }

    size_t available = s->input.u8_bytes - at;
    size_t compare =
        available < c->opener.u8_bytes ? available : c->opener.u8_bytes;

    if (memcmp(s->input.data + at, c->opener.data, compare)) {
      continue;
    }

    if (available >= c->opener.u8_bytes) {
      return INDENT_LINE_COMMENT;
    }

    if (!s->source_final) {
      partial = true;
    }
  }

  return partial ? INDENT_LINE_INCOMPLETE : INDENT_LINE_CONTENT;
}

static void invalid_indent(parse_scanner_t *s, core_string_t message,
                           size_t start, size_t end) {
  scan_set_invalid(s, message, start);
  s->status_end_byte = s->source_base + end;
}

static layout_result_t do_layout(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  const scan_lex_spec_t *spec = lex->spec;

  if (!(spec->layout & SCAN_LEX_LAYOUT_INDENT) || !run->line_start) {
    return LAYOUT_NONE;
  }

  indent_measure_t m =
      measure_indent(s, spec->indent_tab_width, pending_indent(s, run));
  indent_line_kind_t kind = indent_line_kind(s, lex, m.end);

  if (kind == INDENT_LINE_INCOMPLETE) {
    save_pending_indent(run, m);
    scan_set_incomplete(s, CORE_STRING_STATIC("indentation may continue"),
                        s->cursor);
    return LAYOUT_STOP;
  }

  run->pending_indent = false;

  bool ignore = (kind == INDENT_LINE_BLANK &&
                 spec->indent_blank_lines == SCAN_LEX_INDENT_IGNORE) ||
                (kind == INDENT_LINE_COMMENT &&
                 spec->indent_comment_lines == SCAN_LEX_INDENT_IGNORE);

  if (ignore) {
    run->continued_line = false;
    return LAYOUT_NONE;
  }

  if (m.has_tab && spec->indent_tabs == SCAN_LEX_INDENT_REJECT) {
    invalid_indent(s, CORE_STRING_STATIC("tabs are not allowed in indentation"),
                   m.first_tab, m.first_tab + 1);
    return LAYOUT_STOP;
  }

  if (m.has_tab && m.has_space &&
      spec->indent_mixed == SCAN_LEX_INDENT_REJECT) {
    invalid_indent(s, CORE_STRING_STATIC("mixed indentation is not allowed"),
                   s->cursor, m.end);
    return LAYOUT_STOP;
  }

  if (run->bracket_depth > 0 || run->continued_line) {
    run->continued_line = false;

    if (m.end > s->cursor && spec->indent_alignment == SCAN_LEX_INDENT_REJECT) {
      invalid_indent(s,
                     CORE_STRING_STATIC("alignment indentation is not allowed"),
                     s->cursor, m.end);
      return LAYOUT_STOP;
    }

    run->line_start = false;
    return LAYOUT_NONE;
  }

  int32_t cur = run->indents.len ? run->indents.data[run->indents.len - 1] : 0;

  if (m.column > cur) {
    core_list_push(run->indents, m.column);
    if (run->pending_layout_block) {
      core_list_push(run->layout_blocks, (int32_t)run->indents.len);
      run->pending_layout_block = false;
    }

    scan_mark(s);
    scan_emit_marked(s, lex->tid_indent);
    parse_token_info_t *token = scan_prev_token(s);
    if (token) {
      parse_token_set_layout(token, (uint32_t)run->indents.len,
                             (uint32_t)m.column);
      parse_token_set_ignore_if_no_match(token, run->layout_blocks.len == 0);
    }
    run->line_start = false;

    return LAYOUT_EMITTED;
  }

  run->pending_layout_block = false;

  if (m.column < cur) {
    size_t keep = run->indents.len;

    while (keep && run->indents.data[keep - 1] > m.column) {
      keep--;
    }

    int32_t landed = keep ? run->indents.data[keep - 1] : 0;

    if (landed != m.column &&
        spec->indent_unmatched == SCAN_LEX_INDENT_REJECT) {
      invalid_indent(s,
                     CORE_STRING_STATIC(
                         "dedent does not match an earlier indentation level"),
                     s->cursor, m.end);
      return LAYOUT_STOP;
    }

    run->pending_dedents = (int32_t)(run->indents.len - keep);
    run->pending_dedent_mismatch = landed != m.column;

    run->line_start = false;

    if (run->pending_dedents > 0) {
      emit_dedent(s, run);

      return LAYOUT_EMITTED;
    }

    return LAYOUT_NONE;
  }

  run->line_start = false;

  return LAYOUT_NONE;
}

static bool process_queued_heredocs(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;

  while (core_list_len(run->heredoc_queue)) {
    lex_heredoc_queue_t *queued = &run->heredoc_queue.data[0];
    const scan_lex_heredoc_t *h = queued->recipe;

    if (!run->heredoc_queue_scanning) {
      run->pending_heredoc_body = s->cursor;
      run->pending_heredoc_scan = s->cursor;
      run->pending_heredoc_line_start = s->cursor;
      run->pending_heredoc_match = 0;
      run->pending_heredoc_indent = h->terminator_indent;
      run->pending_heredoc_rejected = false;
      run->heredoc_queue_scanning = true;
    }

    size_t body = run->pending_heredoc_body;
    size_t scan = run->pending_heredoc_scan;
    size_t line_start = run->pending_heredoc_line_start;
    size_t matched = run->pending_heredoc_match;
    bool at_indent = run->pending_heredoc_indent;
    bool rejected = run->pending_heredoc_rejected;
    size_t work_start = scan;
    bool complete = false;
    size_t body_end = 0;
    size_t consumed = 0;

    while (scan < s->input.u8_bytes) {
      uint8_t byte = (uint8_t)s->input.data[scan];
      if (byte == '\n') {
        if (!rejected && matched == queued->tag.u8_bytes) {
          body_end = line_start;
          consumed = scan + 1;
          complete = true;
          break;
        }
        scan++;
        line_start = scan;
        matched = 0;
        rejected = false;
        at_indent = h->terminator_indent;
        continue;
      }
      if (at_indent && (byte == ' ' || byte == '\t')) {
        scan++;
        continue;
      }
      at_indent = false;
      if (!rejected && matched < queued->tag.u8_bytes &&
          byte == (uint8_t)queued->tag.data[matched]) {
        matched++;
      } else {
        rejected = true;
      }
      scan++;
    }

    if (!complete && s->source_final && !rejected &&
        matched == queued->tag.u8_bytes) {
      body_end = line_start;
      consumed = scan;
      complete = true;
    }

    s->work_bytes += scan - work_start;
    if (!complete) {
      run->pending_heredoc_scan = scan;
      run->pending_heredoc_line_start = line_start;
      run->pending_heredoc_match = matched;
      run->pending_heredoc_indent = at_indent;
      run->pending_heredoc_rejected = rejected;
      scan_set_incomplete(s, CORE_STRING_STATIC("unterminated queued heredoc"),
                          body);
      s->status_end_byte = s->source_base + s->input.u8_bytes;
      return false;
    }

    core_string_t value = heredoc_body_value(s, h, body, body_end);
    parse_token_info_t *token = nullptr;
    if (s->stream && s->stream->token_count && s->stream->tokens[0]) {
      int32_t slot = queued->token_index
                   - parse_token_index(s->stream->tokens[0]);
      if (slot >= 0 && slot < s->stream->token_count &&
          s->stream->tokens[slot] &&
          parse_token_index(s->stream->tokens[slot]) == queued->token_index) {
        token = s->stream->tokens[slot];
      }
    }
    if (token) {
      parse_token_set_value(token, core_option_set(core_string_t, value),
                            true);
      parse_token_set_content_span(token, s->source_base + body,
                                   s->source_base + body_end);
    } else {
      core_free(value.data);
    }

    scan_advance_bytes(s, consumed - s->cursor);
    core_free(queued->tag.data);
    if (run->heredoc_queue.len > 1) {
      memmove(run->heredoc_queue.data, run->heredoc_queue.data + 1,
              (run->heredoc_queue.len - 1) * sizeof(*run->heredoc_queue.data));
    }
    run->heredoc_queue.len--;
    run->heredoc_queue_scanning = false;
  }

  run->heredoc_queue_ready = false;
  return true;
}

typedef enum : uint8_t {
  RECOVERY_NO_BOUNDARY,
  RECOVERY_PARTIAL_BOUNDARY,
  RECOVERY_BOUNDARY,
} recovery_boundary_t;

static recovery_boundary_t recovery_boundary(parse_scanner_t *s,
                                             const scan_lex_recovery_t *recipe,
                                             size_t *width) {
  recovery_boundary_t result = RECOVERY_NO_BOUNDARY;

  *width = 0;
  for (const core_string_t *sync = recipe->synchronize; sync && sync->data;
       sync++) {
    size_t remaining = scan_remaining(s);
    size_t compare = remaining < sync->u8_bytes ? remaining : sync->u8_bytes;

    if (memcmp(s->input.data + s->cursor, sync->data, compare)) {
      continue;
    }
    if (remaining < sync->u8_bytes) {
      result = RECOVERY_PARTIAL_BOUNDARY;
      continue;
    }
    if (sync->u8_bytes > *width) {
      *width = sync->u8_bytes;
      result = RECOVERY_BOUNDARY;
    }
  }
  return result;
}

static bool finish_recovery(parse_scanner_t *s, scan_lex_session_t *run) {
  const lex_recovery_t *recovery = run->pending_recovery;
  parse_token_info_t *token = run->pending_recovery_token;

  if (token) {
    size_t token_start = parse_token_start_byte(token);

    parse_token_set_tid(token, recovery->replacement_tid);
    parse_token_set_span(token, token_start, s->source_base + s->cursor);
    if (!parse_source_slice(parse_token_source(token), token_start,
                            s->source_base + s->cursor, nullptr)) {
      parse_token_set_position(token, parse_token_line(token),
                               parse_token_column(token), s->line, s->column);
    }
    parse_token_clear_content_span(token);
    parse_token_clear_label_span(token);

    if (token_start >= s->source_base
        && token_start - s->source_base <= s->cursor
        && !parse_token_use_source_value(token)) {
      size_t start = token_start - s->source_base;
      parse_token_set_value(
          token,
          core_option_set(core_string_t,
                          core_string_from_raw(s->input.data + start,
                                               (int64_t)(s->cursor - start))),
          true);
    }
  }

  core_string_t message = run->pending_recovery_message;
  if (!s->had_recovery) {
    core_free(run->recovery_diagnostic.data);
    run->recovery_diagnostic = run->pending_recovery_message;
    run->pending_recovery_message = (core_string_t){0};
    message = run->recovery_diagnostic;
  }

  size_t start = run->pending_recovery_start >= s->source_base
                     ? run->pending_recovery_start - s->source_base
                     : 0;
  size_t end = run->pending_recovery_end >= s->source_base
                   ? run->pending_recovery_end - s->source_base
                   : start;
  scan_set_recovered(s, message, start, end);

  core_free(run->pending_recovery_message.data);
  run->pending_recovery_message = (core_string_t){0};
  run->pending_recovery = nullptr;
  run->pending_recovery_token = nullptr;
  run->pending_recovery_start = 0;
  run->pending_recovery_end = 0;
  run->line_start = s->column == 1;
  run->continued_line = false;
  run->line_has_token = !run->line_start;
  scan_mark(s);

  if (token && s->stream) {
    s->stream->tokens[s->stream->token_count++] = token;
  }
  return true;
}

static bool continue_recovery(parse_scanner_t *s, scan_lex_session_t *run) {
  const scan_lex_recovery_t *recipe = run->pending_recovery->recipe;

  if (!recipe->synchronize) {
    return finish_recovery(s, run);
  }

  while (!scan_at_eof(s)) {
    size_t width = 0;
    recovery_boundary_t boundary = recovery_boundary(s, recipe, &width);

    if (boundary == RECOVERY_BOUNDARY) {
      if (recipe->consume) {
        scan_advance_bytes(s, width);
      }
      return finish_recovery(s, run);
    }
    if (boundary == RECOVERY_PARTIAL_BOUNDARY && !s->source_final) {
      scan_set_incomplete(
          s, CORE_STRING_STATIC("recovery boundary may continue"), s->cursor);
      return false;
    }
    scan_advance(s);
  }

  if (!s->source_final) {
    scan_set_incomplete(s, CORE_STRING_STATIC("recovery boundary may continue"),
                        s->cursor);
    return false;
  }
  return finish_recovery(s, run);
}

static bool begin_recovery(parse_scanner_t *s, scan_lex_session_t *run) {
  parse_token_info_t *token = scan_prev_token(s);
  const lex_recovery_t *recovery =
      token ? recovery_for(run->program, parse_token_tid(token)) : nullptr;

  if (!recovery) {
    return false;
  }

  run->pending_recovery = recovery;
  run->pending_recovery_token = token;
  if (s->stream && s->stream->token_count &&
      s->stream->tokens[s->stream->token_count - 1] == token) {
    s->stream->tokens[--s->stream->token_count] = nullptr;
  }
  run->pending_recovery_message = core_string_dup(s->status_message);
  run->pending_recovery_start = s->status_start_byte;
  run->pending_recovery_end = s->status_end_byte;
  s->status = SCAN_STATUS_ACTIVE;
  s->status_message = (core_string_t){0};
  s->status_lexer = (core_string_t){0};
  s->status_start_byte = 0;
  s->status_end_byte = 0;
  continue_recovery(s, run);
  return true;
}

static bool scan_lex_scan_one(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;

  if (!run) {
    return false;
  }

  const scan_lex_t *lex = run->program;

  if (run->heredoc_queue_ready && !process_queued_heredocs(s)) {
    return false;
  }

  if (run->pending_embed) {
    return resume_embed_token(s);
  }

  if (run->pending_number_phase != NUMBER_NONE) {
    return continue_number(s);
  }

  if (run->pending_opener && recipe_opener_may_continue(s, true)) {
    return false;
  }

  if (run->pending_terminal) {
    return resume_terminal(s);
  }

  if (run->pending_identifier) {
    return resume_identifier(s);
  }

  if (run->pending_attached_sigil) {
    const scan_lex_sigil_t *sigil = run->pending_attached_sigil;
    while (!scan_at_eof(s) && unicode_is_id_continue(scan_peek(s, 0)))
      scan_advance(s);
    if (scan_at_eof(s) && !s->source_final) {
      scan_set_incomplete(s, CORE_STRING_STATIC("sigil may continue"),
                          s->mark);
      return false;
    }
    run->pending_attached_sigil = nullptr;
    parse_token_info_t *previous = scan_prev_token(s);
    if (!previous
        || core_option_is_set(parse_token_postfix_modifier(previous))) {
      scan_set_invalid(s, CORE_STRING_STATIC("invalid postfix modifier"),
                       s->mark);
      return false;
    }
    core_string_t name = core_string_from_raw(
        s->input.data + s->mark + sigil->prefix.u8_bytes,
        (int64_t)(s->cursor - s->mark - sigil->prefix.u8_bytes));
    parse_token_set_postfix_modifier(
        previous, core_option_set(core_string_t, name), true);
    parse_token_set_span(previous, parse_token_start_byte(previous),
                         s->source_base + s->cursor);
    return scan_lex_scan(s)
        || (scan_at_eof(s) && s->status == SCAN_STATUS_ACTIVE);
  }

  if (run->pending_string) {
    return resume_pending_string(s);
  }

  if (run->pending_regex) {
    return resume_regex(s);
  }

  if (run->pending_fence) {
    return resume_fence(s);
  }

  if (run->pending_heredoc) {
    return resume_heredoc(s);
  }

  if (run->pending_comment) {
    return resume_comment(s);
  }

  if (run->pending_dedents > 0) {
    run->line_start = false;
    emit_dedent(s, run);

    return true;
  }

  if (run->pending_block_newline) {
    run->pending_block_newline = false;
    scan_mark(s);
    scan_emit_marked(s, lex->tid_newline);

    return true;
  }

  if (run->outside) {
    if (scan_outside(s)) {
      return true;
    }

    if (scan_at_eof(s) || s->status == SCAN_STATUS_INCOMPLETE) {
      return false;
    }
  }

  if (run->modes.len && !run->modes.data[run->modes.len - 1].in_hole) {
    return resume_string(s, true);
  }

  embed_close_result_t embed_close = try_embedded_close(s);

  if (embed_close == EMBED_CLOSE_EMITTED) {
    return true;
  }

  if (embed_close == EMBED_CLOSE_STOP) {
    return false;
  }

  for (;;) {
    if (run->skip_to > run->skip_from && s->cursor >= run->skip_from) {
      scan_advance_bytes(s, run->skip_to - s->cursor);
      run->skip_from = 0;
      run->skip_to = 0;
      run->line_start = true;

      continue;
    }

    layout_result_t layout = do_layout(s);

    if (layout == LAYOUT_EMITTED) {
      return true;
    }

    if (layout == LAYOUT_STOP) {
      return false;
    }

    if (scan_at_eof(s)) {
      break;
    }

    core_codepoint_t c = scan_peek(s, 0);

    if (core_codepoint_is_line_terminator(c)) {
      bool significant = (lex->spec->layout &
                          (SCAN_LEX_LAYOUT_NEWLINE | SCAN_LEX_LAYOUT_INDENT)) &&
                         newlines_visible(lex, run) &&
                         (run->line_has_token ||
                          ((lex->spec->layout & SCAN_LEX_LAYOUT_NEWLINE) &&
                           lex->spec->newline_each_line));

      if (run->in_directive) {
        core_string_t eol = run->in_directive->tok_eol;

        run->in_directive = nullptr;
        run->line_start = true;
        run->line_has_token = false;

        scan_advance(s);
        scan_mark(s);
        emit_named(s, eol, PARSE_TOK_IGNORED);

        return true;
      }

      if (lex->spec->whitespace == SCAN_LEX_WHITESPACE_CODEPOINT &&
          !(lex->spec->layout &
            (SCAN_LEX_LAYOUT_NEWLINE | SCAN_LEX_LAYOUT_INDENT)) &&
          lex->tid_terminator == PARSE_TOK_IGNORED) {
        break;
      }

      bool insert = lex->tid_terminator != PARSE_TOK_IGNORED &&
                    run->bracket_depth == 0 && run->line_has_token &&
                    ends_a_statement(lex, s);

      scan_mark(s);
      scan_advance(s);
      run->line_start = true;
      run->continued_line = false;
      run->line_has_token = false;
      if (core_list_len(run->heredoc_queue)) {
        run->heredoc_queue_ready = true;
      }

      if (insert) {
        scan_mark(s);
        scan_emit_marked(s, lex->tid_terminator);

        return true;
      }

      if (significant) {
        emit_layout_newline(s, run);

        return true;
      }

      if (run->heredoc_queue_ready && !process_queued_heredocs(s)) {
        return false;
      }

      continue;
    }

    if (c == '\t' && lex->spec->indent_tabs == SCAN_LEX_INDENT_REJECT) {
      size_t start = s->cursor;
      scan_advance(s);
      scan_set_invalid(s, CORE_STRING_STATIC("tabs are not allowed"), start);
      return false;
    }

    if ((c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') &&
        lex->spec->whitespace == SCAN_LEX_WHITESPACE_SKIP) {
      scan_advance(s);

      continue;
    }

    if (lex->spec->line_continue.data && !s->source_final &&
        partial_at_end(s, lex->spec->line_continue)) {
      scan_set_incomplete(
          s, CORE_STRING_STATIC("line continuation may continue"), s->cursor);
      return false;
    }

    if (lex->spec->line_continue.data && at_str(s, lex->spec->line_continue)) {
      size_t at = s->cursor + lex->spec->line_continue.u8_bytes;

      while (at < s->input.u8_bytes &&
             (s->input.data[at] == ' ' || s->input.data[at] == '\t')) {
        at++;
      }

      if (at < s->input.u8_bytes && s->input.data[at] == '\n') {
        scan_advance_bytes(s, at + 1 - s->cursor);
        run->line_start = true;
        run->continued_line = true;

        continue;
      }

      if (at == s->input.u8_bytes && !s->source_final) {
        scan_set_incomplete(
            s, CORE_STRING_STATIC("line continuation may continue"), s->cursor);
        return false;
      }
    }

    if (lex->spec->embeds) {
      int what = at_embed_close(s);

      if (what == EMBED_EMITTED) {
        return true;
      }

      if (what == EMBED_CONSUMED) {
        return scan_lex_scan(s);
      }
    }

    inline_embed_result_t inline_embed = try_inline_embed(s);

    if (inline_embed == INLINE_EMBED_EMITTED) {
      return true;
    }

    if (inline_embed == INLINE_EMBED_STOP) {
      return false;
    }

    if (recipe_opener_may_continue(s, false)) {
      return false;
    }

    region_result_t region = try_region(s);
    if (region == REGION_STOP) return false;
    if (region == REGION_CONSUMED) continue;

    if (try_comment(s)) {
      if (s->pending_leading || s->pending_trailing) {
        continue;
      }

      return true;
    }

    if (s->status == SCAN_STATUS_INCOMPLETE) {
      return false;
    }

    break;
  }

  if (scan_at_eof(s)) {
    if (!s->source_final) {
      return false;
    }

    if (core_list_len(run->heredoc_queue)) {
      scan_set_incomplete(
          s, CORE_STRING_STATIC("queued heredoc bodies did not begin"),
          s->cursor);
      return false;
    }

    if (lex->tid_terminator != PARSE_TOK_IGNORED && run->line_has_token &&
        run->bracket_depth == 0 && ends_a_statement(lex, s)) {
      run->line_has_token = false;
      scan_mark(s);
      scan_emit_marked(s, lex->tid_terminator);

      return true;
    }

    if (lex->tid_newline != PARSE_TOK_IGNORED && run->line_has_token &&
        newlines_visible(lex, run)) {
      run->line_has_token = false;
      emit_layout_newline(s, run);

      return true;
    }

    if (!run->pending_dedents) {
      run->pending_dedents = (int32_t)run->indents.len;
      run->pending_dedent_mismatch = false;
    }

    if (run->pending_dedents > 0) {
      emit_dedent(s, run);

      return true;
    }

    if (!run->sent_eof && lex->tid_eof != PARSE_TOK_IGNORED) {
      run->sent_eof = true;
      scan_mark(s);
      scan_emit_marked(s, lex->tid_eof);

      return true;
    }

    return false;
  }

  bool at_line_start = run->line_start;

  run->line_start = false;
  run->line_has_token = true;

  const scan_lex_directive_t *dir = nullptr;

  for (const scan_lex_directive_t *d = lex->spec->directives;
       at_line_start && d && d->open.data; d++) {
    if (at_str(s, d->open)) {
      dir = d;

      break;
    }
  }

  if (dir) {
    if (dir->tok_other.data && try_terminal_min(s, dir->open.u8_bytes + 1)) {
      run->in_directive = dir;

      return true;
    }

    if (s->status == SCAN_STATUS_INCOMPLETE) {
      return false;
    }

    scan_mark(s);

    while (!scan_at_eof(s)) {
      core_codepoint_t k = scan_peek(s, 0);

      if (k == '\\') {
        int32_t at = 1;

        while (scan_peek(s, at) == ' ' || scan_peek(s, at) == '\t' ||
               scan_peek(s, at) == '\r') {
          at++;
        }

        if (scan_peek(s, at) == '\n') {
          scan_advance_n(s, at + 1);

          continue;
        }
      }

      if (core_codepoint_is_line_terminator(k)) {
        break;
      }

      scan_advance(s);
    }

    if (!dir->tok_other.data) {
      scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);

      return scan_lex_scan(s);
    }

    emit_named(s, dir->tok_other, PARSE_TOK_OTHER);

    return true;
  }

  if (try_heredoc(s)) {
    return true;
  }

  if (try_fence(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_sigil(s, true)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_string(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_number(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_sigil(s, false)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_regex(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  core_codepoint_t c = scan_peek(s, 0);

  if (run->modes.len && run->modes.data[run->modes.len - 1].in_hole) {
    lex_mode_t *m = &run->modes.data[run->modes.len - 1];

    core_string_t open = m->str->interp_open;
    core_codepoint_t nest =
        open.u8_bytes
            ? (core_codepoint_t)(unsigned char)open.data[open.u8_bytes - 1]
            : 0;

    if (m->at_open && at_str(s, open)) {
      m->at_open = false;

      goto dispatch;
    }

    if (at_str(s, m->str->interp_close)) {
      if (m->depth == 0) {
        m->in_hole = false;

        if (m->str->flags & SCAN_LEX_STR_HOLE_TOKENS) {
          goto dispatch;
        }

        scan_mark(s);
        scan_advance_bytes(s, m->str->interp_close.u8_bytes);

        return resume_string(s, false);
      }

      m->depth--;
    } else if (nest && c == nest) {
      m->depth++;
    }
  }

dispatch:
  track_bracket(run, c);

  if (try_terminal(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  if (try_identifier(s)) {
    return true;
  }

  if (s->status == SCAN_STATUS_INCOMPLETE) {
    return false;
  }

  scan_mark(s);
  scan_advance(s);

  bool declared_codepoint =
      tid_set_contains(&lex->codepoint_terminals, (int32_t)c);

  if (lex->spec->fallback == SCAN_LEX_FALLBACK_REJECT && !declared_codepoint) {
    scan_set_invalid(s, CORE_STRING_STATIC("unexpected character"), s->mark);
    scan_emit_marked(s, PARSE_TOK_OTHER);
  } else {
    scan_emit_marked(s, (int32_t)c);
  }

  return true;
}

bool scan_lex_scan(parse_scanner_t *s) {
  scan_lex_session_t *run = s ? s->user_state : nullptr;

  if (!run) {
    return false;
  }
  if (run->pending_recovery) {
    return continue_recovery(s, run);
  }

  bool emitted = scan_lex_scan_one(s);

  if (emitted && s->recovery_enabled && s->status == SCAN_STATUS_INVALID &&
      begin_recovery(s, (scan_lex_session_t *)s->user_state)) {
    return !((scan_lex_session_t *)s->user_state)->pending_recovery;
  }
  return emitted;
}

#define BIDI_CONTROLS(X)                                                       \
  X(0x061C, "U+061C (ARABIC LETTER MARK)")                                     \
  X(0x200E, "U+200E (LEFT-TO-RIGHT MARK)")                                     \
  X(0x200F, "U+200F (RIGHT-TO-LEFT MARK)")                                     \
  X(0x202A, "U+202A (LEFT-TO-RIGHT EMBEDDING)")                                \
  X(0x202B, "U+202B (RIGHT-TO-LEFT EMBEDDING)")                                \
  X(0x202C, "U+202C (POP DIRECTIONAL FORMATTING)")                             \
  X(0x202D, "U+202D (LEFT-TO-RIGHT OVERRIDE)")                                 \
  X(0x202E, "U+202E (RIGHT-TO-LEFT OVERRIDE)")                                 \
  X(0x2066, "U+2066 (LEFT-TO-RIGHT ISOLATE)")                                  \
  X(0x2067, "U+2067 (RIGHT-TO-LEFT ISOLATE)")                                  \
  X(0x2068, "U+2068 (FIRST STRONG ISOLATE)")                                   \
  X(0x2069, "U+2069 (POP DIRECTIONAL ISOLATE)")

typedef struct {
  core_codepoint_t codepoint;
  core_string_t rejected;
  core_string_t warned;
} bidi_control_t;

static const bidi_control_t bidi_controls[] = {
#define BIDI_ENTRY(cp, name)                                                   \
  {cp,                                                                         \
   CORE_STRING_STATIC("invisible bidirectional control character " name       \
                      " can make code display differently from how it "        \
                      "compiles, so it is not allowed in source text; a "      \
                      "string literal can write it as an escape"),             \
   CORE_STRING_STATIC("fence literal contains invisible bidirectional "       \
                      "control character " name ", which can make it display " \
                      "differently from its contents")},
    BIDI_CONTROLS(BIDI_ENTRY)
#undef BIDI_ENTRY
};

static const bidi_control_t *bidi_control_at(core_string_t text, size_t at,
                                             size_t *length) {
  uint8_t lead = at < text.u8_bytes ? (uint8_t)text.data[at] : 0;
  if (lead != 0xD8 && lead != 0xE2) {
    return nullptr;
  }
  uint32_t pos = (uint32_t)at;
  int32_t cp = unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes, &pos);
  for (size_t i = 0; cp >= 0 && i < sizeof(bidi_controls) / sizeof(*bidi_controls);
       i++) {
    if (bidi_controls[i].codepoint == (core_codepoint_t)cp) {
      *length = pos - (uint32_t)at;
      return &bidi_controls[i];
    }
  }
  return nullptr;
}

static scan_lex_bidi_policy_t bidi_policy_for(const scan_lex_t *program,
                                              int32_t tid) {
  for (const scan_lex_fence_t *f = program->spec->fences;
       tid >= 0 && f && f->outer_open.data; f++) {
    if (f->bidi != SCAN_LEX_BIDI_DEFAULT && f->tok.data &&
        parse_terminal_id(program->g, f->tok) == tid) {
      return f->bidi;
    }
  }
  return program->spec->bidi;
}

bool scan_lex_checks_bidi(const scan_lex_t *program) {
  if (!program) {
    return false;
  }
  if (program->spec->bidi > SCAN_LEX_BIDI_ALLOW) {
    return true;
  }
  for (const scan_lex_fence_t *f = program->spec->fences;
       f && f->outer_open.data; f++) {
    if (f->bidi > SCAN_LEX_BIDI_ALLOW) {
      return true;
    }
  }
  return false;
}

void scan_lex_check_span(parse_scanner_t *s, size_t start, size_t end,
                         int32_t tid) {
  scan_lex_session_t *run = s ? s->user_state : nullptr;
  const scan_lex_t *program = run ? run->program : nullptr;
  if (!program) {
    return;
  }
  for (size_t at = start; at < end && at < s->input.u8_bytes; at++) {
    size_t length = 0;
    const bidi_control_t *control = bidi_control_at(s->input, at, &length);
    if (!control) {
      continue;
    }
    scan_lex_bidi_policy_t policy = bidi_policy_for(program, tid);
    if (policy == SCAN_LEX_BIDI_REJECT) {
      size_t cursor = s->cursor;
      s->cursor = at + length;
      scan_set_invalid(s, control->rejected, at);
      s->cursor = cursor;
      return;
    }
    if (policy == SCAN_LEX_BIDI_WARN) {
      scan_add_warning(s, control->warned, at, at + length);
    }
    at += length - 1;
  }
}
