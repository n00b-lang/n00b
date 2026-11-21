#include "scanner/lex.h"
#include "parse/annotation.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "private/parse/grammar.h"
#include "private/scanner/lex_decl.h"
#include "private/parse/unicode_class.h"
#include "private/scanner/lex_program.h"
#include "private/scanner/unicode_profile.h"
#include "private/unicode/ucd.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "unicode/encoding.h"
#include "default_lex.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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


bool scan_lex_finalize_regions(parse_scanner_t *s);

static bool check_region_name(parse_scanner_t *s, parse_trivia_t *trivia) {
  core_string_t normalized = ucd_normalize(trivia->region_name, UCD_NFC);
  bool decoded = normalized.data != nullptr;
  bool valid = decoded && core_string_eq(normalized, trivia->region_name);
  core_free(normalized.data);
  if (!valid) {
    scan_set_invalid(s, decoded
                            ? SCAN_PROBLEM(COMMENT_REGION_NAME_NOT_NFC)
                            : SCAN_PROBLEM(COMMENT_REGION_NAME_INVALID),
                     trivia->start_byte - s->source_base);
  }
  return valid;
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
    if (items[i]->kind == PARSE_TRIVIA_REGION_OPEN
        || items[i]->kind == PARSE_TRIVIA_REGION_CLOSE) {
      anchor_count++;
      if (!check_region_name(s, items[i])) {
        core_free(items);
        return false;
      }
    }
  }

  parse_trivia_t **opens = core_alloc_array(parse_trivia_t *, anchor_count);
  size_t open_count = 0;

  for (size_t i = 0; i < count; i++) {
    parse_trivia_t *anchor = items[i];
    if (anchor->kind == PARSE_TRIVIA_REGION_OPEN) {
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
      scan_set_invalid(s, SCAN_PROBLEM(COMMENT_REGION_UNMATCHED_CLOSE),
                       anchor->start_byte - s->source_base);
      core_free(opens);
      core_free(items);
      return false;
    }
    memmove(opens + match - 1, opens + match,
            (open_count - match) * sizeof(*opens));
    open_count--;
  }

  if (open_count) {
    parse_trivia_t *open = opens[open_count - 1];
    scan_set_invalid(s, SCAN_PROBLEM(COMMENT_REGION_UNCLOSED),
                     open->start_byte - s->source_base);
    core_free(opens);
    core_free(items);
    return false;
  }

  core_free(opens);
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
  bool pending_block_newline;
  bool pending_layout_block;

  int32_t bracket_depth;
  bool line_start;
  bool continued_line;
  bool line_has_token;
  bool sent_eof;
  bool outside;


  const scan_lex_directive_t *in_directive;
  size_t *fence_probe_skip_until;
  core_list_t(lex_heredoc_queue_t) heredoc_queue;
  bool heredoc_queue_ready;

  size_t skip_from;
  size_t skip_to;
  scan_problem_t status_diagnostic;
  const lex_recovery_t *pending_recovery;
  scan_problem_t pending_recovery_message;
  scan_problem_t recovery_diagnostic;
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
  core_string_t analysis_error = {};
  bool analyzed = scan_lex_analyze_spec(spec, g, &analysis_error);
  lex_compile_error_set(analysis_error);
  if (!analyzed) {
    return nullptr;
  }

  scan_lex_t *lex = core_alloc(scan_lex_t);

  lex->spec = spec;
  lex->g = g;
  lex->refs = 1;
  lex->term_nodes = core_list_new(lex_term_node_t);
  lex->term_edges = core_list_new(lex_term_edge_t);
  lex->string_nodes = core_list_new(lex_term_node_t);
  lex->string_edges = core_list_new(lex_term_edge_t);
  lex->number_nodes = core_list_new(lex_term_node_t);
  lex->number_edges = core_list_new(lex_term_edge_t);
  lex->number_suffix_nodes = core_list_new(lex_term_node_t);
  lex->number_suffix_edges = core_list_new(lex_term_edge_t);
  lex->number_suffix_roots = core_list_new(int32_t);
  core_list_push(lex->term_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  core_list_push(lex->string_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  core_list_push(lex->number_nodes, ((lex_term_node_t){.edge = -1, .tid = 0}));
  compile_ascii_sets(lex);
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
  if (lex->number_form_count) {
    lex->number_sign_not_after = core_alloc_array(lex_tid_set_t,
                                                   lex->number_form_count);
    for (size_t i = 0; i < lex->number_form_count; i++) {
      for (const core_string_t *name = spec->number_forms[i].sign_not_after;
           name && name->data; name++) {
        int64_t tid = parse_terminal_id(g, *name);
        if (!tid && name->u8_bytes == 1) {
          tid = (unsigned char)name->data[0];
        }
        if (tid) {
          tid_set_add(&lex->number_sign_not_after[i], (int32_t)tid);
        }
      }
    }
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

  core_free(lex->recoveries);
  scan_unicode_profile_free(lex->identifier_profile);

  core_free(lex->codepoint_terminals.slots);

  core_free(lex->ascii_sets);

  core_list_free(lex->term_after);
  core_list_free(lex->term_nodes);
  core_list_free(lex->term_edges);
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
  for (size_t i = 0; lex->number_sign_not_after
                     && i < lex->number_form_count; i++) {
    core_free(lex->number_sign_not_after[i].slots);
  }
  core_free(lex->number_sign_not_after);
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
  core_free(session->fence_probe_skip_until);
  core_free(session->status_diagnostic.detail.data);
  core_free(session->pending_recovery_message.detail.data);
  core_free(session->recovery_diagnostic.detail.data);
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
  if (lex->fence_probe_skip_until) {
    memset(lex->fence_probe_skip_until, 0,
           lex->program->fence_count * sizeof(*lex->fence_probe_skip_until));
  }
  for (size_t i = 0; i < core_list_len(lex->heredoc_queue); i++) {
    core_free(lex->heredoc_queue.data[i].tag.data);
  }
  lex->heredoc_queue.len = 0;
  lex->heredoc_queue_ready = false;
  core_free(lex->status_diagnostic.detail.data);
  lex->status_diagnostic = (scan_problem_t){0};
  core_free(lex->pending_recovery_message.detail.data);
  lex->pending_recovery_message = (scan_problem_t){0};
  core_free(lex->recovery_diagnostic.detail.data);
  lex->recovery_diagnostic = (scan_problem_t){0};
  free_pending_recovery_token(lex->pending_recovery_token);
  lex->pending_recovery_token = nullptr;
  lex->pending_recovery = nullptr;
  lex->pending_recovery_start = 0;
  lex->pending_recovery_end = 0;
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
                            uint32_t level, size_t content_start) {
  size_t fill_bytes = f->fill.u8_bytes * (size_t)level;
  size_t pattern_len =
      f->body_close.u8_bytes + fill_bytes + f->outer_close.u8_bytes;
  char *pattern = core_alloc_size(1, pattern_len);
  size_t *prefix = core_alloc_array(size_t, pattern_len);
  char *out = pattern;

  memcpy(out, f->body_close.data, f->body_close.u8_bytes);
  out += f->body_close.u8_bytes;
  for (uint32_t i = 0; i < level; i++) {
    memcpy(out, f->fill.data, f->fill.u8_bytes);
    out += f->fill.u8_bytes;
  }
  memcpy(out, f->outer_close.data, f->outer_close.u8_bytes);

  for (size_t i = 1, matched = 0; i < pattern_len; i++) {
    while (matched && pattern[i] != pattern[matched]) {
      matched = prefix[matched - 1];
    }
    if (pattern[i] == pattern[matched]) {
      matched++;
    }
    prefix[i] = matched;
  }

  size_t search = content_start;
  size_t matched = 0;
  size_t close_cursor = 0;
  bool closed = false;

  while (search < s->input.u8_bytes) {
    char byte = s->input.data[search];

    while (matched && byte != pattern[matched]) {
      matched = prefix[matched - 1];
    }
    if (byte == pattern[matched]) {
      matched++;
    }
    search++;

    if (matched == pattern_len) {
      close_cursor = search - matched;
      bool has_pad = false;
      size_t trimmed = fence_trim_pad_before(s, f, close_cursor, &has_pad);
      if (f->pad != SCAN_LEX_FENCE_PAD_REQUIRED || has_pad) {
        close_cursor = trimmed;
        closed = true;
        break;
      }
      matched = prefix[matched - 1];
    }
  }
  core_free(pattern);
  core_free(prefix);

  while (s->cursor < search) {
    scan_advance(s);
  }

  if (closed) {
    emit_named(s, f->tok, PARSE_TOK_STRING_LIT);

    parse_token_info_t *tok = scan_prev_token(s);

    if (tok) {
      if (close_cursor < content_start) {
        content_start = close_cursor;
      }
      parse_token_set_content_span(tok, s->source_base + content_start,
                                   s->source_base + close_cursor);
    }

    return true;
  }

  scan_set_incomplete(s, SCAN_PROBLEM(FENCE_UNTERMINATED),
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
      record_failed_fence_probe(run, s, f);
      s->cursor = start;
      s->line = start_line;
      s->column = start_col;
      continue;
    }

    if (!at_fence_body_open(s, f) && f->label &&
        id_start_here(lex, scan_peek(s, 0))) {
      scan_advance(s);

      while (!scan_at_eof(s) && id_cont_here(lex, scan_peek(s, 0))) {
        scan_advance(s);
      }
    }

    if (!at_fence_body_open(s, f)) {
      if (scan_at_eof(s) || partial_at_end(s, f->body_open)) {
        scan_set_incomplete(
            s, SCAN_PROBLEM(FENCE_OPENER_INCOMPLETE), s->mark);

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

    return scan_fence_body(s, f, level, content_start);
  }

  return false;
}

static void previous_token_invalid(parse_scanner_t *s, scan_problem_t problem) {
  parse_token_info_t *token = scan_prev_token(s);
  size_t start = token && parse_token_start_byte(token) >= s->source_base
                     ? parse_token_start_byte(token) - s->source_base
                     : s->mark;

  scan_set_invalid(s, problem, start);
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

static void emit_named(parse_scanner_t *s, core_string_t name,
                       int32_t fallback) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;

  scan_emit_marked(s, tid_for(lex->g, name, fallback));
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
      scan_set_invalid(outer, diagnostic.problem,
                       content_start + diagnostic.start_byte);
      outer->status_end_byte =
          outer->source_base + content_start + diagnostic.end_byte;
      outer->status_lexer =
          diagnostic.lexer.data ? diagnostic.lexer : program->spec->name;
    } else {
      scan_set_invalid(outer, SCAN_PROBLEM(EMBED_INVALID),
                       content_start);
      outer->status_lexer = program->spec->name;
    }
  }

  scan_token_stream_free(stream);
  return status == SCAN_STATUS_COMPLETE;
}

static bool scan_embed_token(parse_scanner_t *s,
                             const scan_lex_embed_t *embed) {
  scan_lex_session_t *run = s->user_state;
  size_t content_start = s->cursor;
  size_t search = s->cursor;
  size_t matched = 0;
  size_t close_at = 0;
  bool closed = false;
  size_t *prefix = core_alloc_array(size_t, embed->close.u8_bytes);

  prefix[0] = 0;
  for (size_t i = 1, at = 0; i < embed->close.u8_bytes; i++) {
    while (at && embed->close.data[i] != embed->close.data[at]) {
      at = prefix[at - 1];
    }
    if (embed->close.data[i] == embed->close.data[at]) {
      at++;
    }
    prefix[i] = at;
  }

  while (search < s->input.u8_bytes) {
    char byte = s->input.data[search];

    while (matched && byte != embed->close.data[matched]) {
      matched = prefix[matched - 1];
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
  core_free(prefix);

  while (s->cursor < search) {
    scan_advance(s);
  }

  if (!closed) {
    scan_set_incomplete(s, SCAN_PROBLEM(EMBED_UNTERMINATED),
                        s->mark);
    return false;
  }

  if (!validate_embedded_body(s, run, embed, content_start, close_at)) {
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
      continue;
    }

    if (!hit || e->open.u8_bytes > hit->open.u8_bytes) {
      hit = e;
    }
  }

  if (!hit) {
    return INLINE_EMBED_NONE;
  }

  size_t open_at = s->cursor;

  scan_mark(s);
  scan_advance_bytes(s, hit->open.u8_bytes);

  if (!hit->tokens) {
    return scan_embed_token(s, hit) ? INLINE_EMBED_EMITTED : INLINE_EMBED_STOP;
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
  if (run->indents.len) {
    run->indents.len--;
  }
  uint32_t depth = (uint32_t)run->indents.len;
  bool in_block = run->layout_blocks.len > 0;
  while (run->layout_blocks.len &&
         depth <
             (uint32_t)run->layout_blocks.data[run->layout_blocks.len - 1]) {
    run->layout_blocks.len--;
    run->pending_block_newline = lex->spec->dedent_newline &&
                                 lex->tid_newline != PARSE_TOK_IGNORED;
  }
  run->pending_dedents--;

  scan_mark(s);
  scan_emit_marked(s, lex->tid_dedent);
  parse_token_info_t *token = scan_prev_token(s);
  if (token) {
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

  if (scan_at_eof(s) || partial_at_end(s, embed->close)) {
    scan_set_incomplete(s, SCAN_PROBLEM(EMBED_UNTERMINATED),
                        run->embed_start);
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
                           size_t *delimiter_depth) {
  size_t scalars = 0;
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
        scan_set_invalid(s, SCAN_PROBLEM(STRING_INVALID_ESCAPE),
                         s->mark);
      }

      scalars++;

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
      scalars++;
      continue;
    }

    if (at_str(s, str->closer)) {
      scan_advance_bytes(s, str->closer.u8_bytes);

      if (*delimiter_depth) {
        (*delimiter_depth)--;
        scalars++;
        continue;
      }

      if ((str->flags & SCAN_LEX_STR_ONE_SCALAR) && scalars != 1) {
        scan_set_invalid(
            s,
            SCAN_PROBLEM(CHARACTER_LENGTH),
            s->mark);
      }

      return BODY_CLOSED;
    }

    if (!multi && core_codepoint_is_line_terminator(scan_peek(s, 0))) {
      return BODY_INVALID_NEWLINE;
    }

    scan_advance(s);
    scalars++;
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

  if (!best) {
    return false;
  }

  scan_mark(s);
  scan_advance_bytes(s, best->opener.u8_bytes);

  size_t delimiter_depth = 0;
  body_stop stop = scan_body(s, best, &delimiter_depth);

  if (stop == BODY_HOLE) {
    emit_named(s, best->tok, PARSE_TOK_STRING_LIT);
    push_mode(run, best, delimiter_depth);

    return true;
  }

  if (stop == BODY_UNTERMINATED) {
    scan_set_incomplete(s, SCAN_PROBLEM(STRING_UNTERMINATED),
                        s->mark);

    return false;
  }

  emit_named(s, best->tok, PARSE_TOK_STRING_LIT);

  if (stop == BODY_INVALID_NEWLINE) {
    previous_token_invalid(s,
                           SCAN_PROBLEM(STRING_NEWLINE));
  }

  return true;
}

static bool resume_string(parse_scanner_t *s, bool mark) {
  scan_lex_session_t *run = s->user_state;
  lex_mode_t *m = &run->modes.data[run->modes.len - 1];

  if (mark) {
    scan_mark(s);
  }

  body_stop stop = scan_body(s, m->str, &m->delimiter_depth);

  if (stop == BODY_HOLE) {
    emit_named(s, m->str->tok_mid, PARSE_TOK_STRING_LIT);
    m->in_hole = true;
    m->at_open = (m->str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0;

    return true;
  }

  if (stop == BODY_UNTERMINATED) {
    scan_set_incomplete(s, SCAN_PROBLEM(STRING_UNTERMINATED),
                        s->mark);

    return false;
  }

  emit_named(s, m->str->tok_end, PARSE_TOK_STRING_LIT);
  run->modes.len--;

  if (stop == BODY_INVALID_NEWLINE) {
    previous_token_invalid(s,
                           SCAN_PROBLEM(STRING_NEWLINE));
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
    }
  }
  if (!matched) return REGION_NONE;
  if (run->line_has_token) {
    scan_set_invalid(s,
        SCAN_PROBLEM(REGION_ANCHOR_NOT_ALONE),
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
  if (scan_at_eof(s) || core_codepoint_is_line_terminator(scan_peek(s, 0))) {
    scan_set_invalid(s, SCAN_PROBLEM(REGION_ANCHOR_MALFORMED),
                     s->mark);
    return REGION_STOP;
  }
  size_t name_end = s->cursor;
  scan_advance_bytes(s, matched->terminator.u8_bytes);
  while (!scan_at_eof(s) && !core_codepoint_is_line_terminator(scan_peek(s, 0))) {
    if (!is_line_whitespace(scan_peek(s, 0))) {
      scan_set_invalid(s,
          SCAN_PROBLEM(REGION_ANCHOR_NOT_ALONE),
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
        core_free(filled_close.data);
        scan_set_incomplete(s, SCAN_PROBLEM(BLOCK_COMMENT_UNTERMINATED),
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

static bool number_digit(const scan_lex_t *lex, core_codepoint_t c,
                         core_string_t digits) {
  return digits.data ? in_set(lex, digits, c) : unicode_cp_is_ascii_digit(c);
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
                                                    size_t at) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  uint32_t node = 0;
  int32_t best = lex->number_nodes.data[0].tid;
  size_t cursor = at;

  while (cursor < s->input.u8_bytes) {
    uint32_t child = trie_child(&((scan_lex_t *)lex)->number_nodes,
                                &((scan_lex_t *)lex)->number_edges,
                                node, (uint8_t)s->input.data[cursor], false);
    if (child == UINT32_MAX) {
      break;
    }
    node = child;
    cursor++;
    if (lex->number_nodes.data[node].tid) {
      best = lex->number_nodes.data[node].tid;
    }
  }

  return best ? &lex->spec->number_forms[best - 1] : nullptr;
}

static const scan_lex_suffix_t *
number_form_suffix(parse_scanner_t *s, const scan_lex_number_form_t *form) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  size_t form_index = (size_t)(form - lex->spec->number_forms);
  uint32_t node = (uint32_t)lex->number_suffix_roots.data[form_index];
  int32_t best = 0;
  size_t at = s->cursor;

  while (at < s->input.u8_bytes) {
    uint32_t child = trie_child(&((scan_lex_t *)lex)->number_suffix_nodes,
                                &((scan_lex_t *)lex)->number_suffix_edges,
                                node, (uint8_t)s->input.data[at], false);
    if (child == UINT32_MAX) {
      break;
    }
    node = child;
    at++;
    if (lex->number_suffix_nodes.data[node].tid) {
      best = lex->number_suffix_nodes.data[node].tid;
    }
  }

  return best ? &form->suffixes[best - 1] : nullptr;
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

  if (signed_start) {
    parse_token_info_t *previous = scan_prev_token(s);
    size_t form_index = (size_t)(plain - lex->spec->number_forms);

    signed_start = !previous
        || !tid_set_contains(&lex->number_sign_not_after[form_index],
                             parse_token_tid(previous));
  }

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

  const scan_lex_number_form_t *form = plain;
  size_t digits = 0;
  size_t exp_digits = 0;
  bool after_digit = false;
  bool need_digit = false;
  bool malformed = false;
  bool is_float = false;
  bool point = false;

  if (scan_peek(s, 0) == '.') {
    is_float = true;
    point = true;
    scan_advance(s);
  } else {
    form = number_form_at(s, s->cursor);
    if (!form) {
      s->cursor = s->mark;
      s->line = s->mark_line;
      s->column = s->mark_col;
      return false;
    }
    scan_advance_bytes(s, form->prefix.u8_bytes);
    if (form->prefix.u8_bytes &&
        (form->flags & SCAN_LEX_NUM_SEPARATOR_AFTER_PREFIX)) {
      after_digit = true;
    }
    number_scan_digits(s, form->digits, form->separator, &digits,
                       &after_digit, &need_digit, &malformed);
    if (need_digit) {
      malformed = true;
    }
    if ((form->flags & SCAN_LEX_NUM_POINT) && scan_peek(s, 0) == '.') {
      bool next_digit = number_digit(lex, scan_peek(s, 1), form->digits);
      bool trailing = (form->flags & SCAN_LEX_NUM_TRAILING_POINT) &&
                      scan_peek(s, 1) != '.' &&
                      !id_start_here(lex, scan_peek(s, 1));
      if (next_digit || trailing) {
        is_float = true;
        point = true;
        scan_advance(s);
        after_digit = false;
        need_digit = false;
      }
    }
  }

  if (point) {
    number_scan_digits(s, form->digits, form->separator, &digits,
                       &after_digit, &need_digit, &malformed);
    if (need_digit) {
      malformed = true;
    }
  }

  if (in_set(lex, form->exponent, scan_peek(s, 0))) {
    is_float = true;
    scan_advance(s);
    if (scan_peek(s, 0) == '+' || scan_peek(s, 0) == '-') {
      scan_advance(s);
    }
    after_digit = false;
    need_digit = false;
    number_scan_digits(s, form->exponent_digits, form->separator,
                       &exp_digits, &after_digit, &need_digit, &malformed);
    if (!exp_digits || need_digit) {
      malformed = true;
    }
  } else if (point &&
             (form->flags & SCAN_LEX_NUM_EXPONENT_REQUIRED_FOR_POINT)) {
    malformed = true;
  }

  const scan_lex_suffix_t *suffix = number_form_suffix(s, form);
  if (suffix) {
    scan_advance_bytes(s, suffix->suffix.u8_bytes);
  }
  core_string_t tok = suffix ? suffix->tok
      : is_float ? form->tok_float : form->tok_int;
  int32_t fallback = is_float ? PARSE_TOK_FLOAT : PARSE_TOK_INTEGER;

  if (!digits) {
    malformed = true;
  }
  if (suffix && suffix->forms &&
      !(suffix->forms &
        (is_float ? SCAN_LEX_SUFFIX_FLOAT : SCAN_LEX_SUFFIX_INTEGER))) {
    malformed = true;
  }
  if (id_cont_here(lex, scan_peek(s, 0))) {
    malformed = true;
  }

  emit_named(s, tok, fallback);
  if (malformed) {
    previous_token_invalid(s, SCAN_PROBLEM(NUMBER_MALFORMED));
  }
  return true;
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
      if (at + g->prefix.u8_bytes <= s->input.u8_bytes
          && !memcmp(s->input.data + at, g->prefix.data, g->prefix.u8_bytes))
        continue;
    }

    if (!best || g->prefix.u8_bytes > best->prefix.u8_bytes) {
      best = g;
    }
  }

  if (!best) {
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

  if (attach) {
    parse_token_info_t *previous = scan_prev_token(s);
    if (!previous
        || core_option_is_set(parse_token_postfix_modifier(previous))) {
      scan_set_invalid(s, SCAN_PROBLEM(POSTFIX_INVALID),
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

static body_stop scan_regex_body(parse_scanner_t *s, const scan_lex_regex_t *r) {
  bool in_class = false;

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
      in_class = true;
      scan_advance(s);

      continue;
    }

    if (k == ']') {
      in_class = false;
      scan_advance(s);

      continue;
    }

    if (!in_class && at_str(s, r->closer)) {
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

    body_stop stop = scan_regex_body(s, r);

    if (stop != BODY_CLOSED) {
      if (stop == BODY_UNTERMINATED) {
        scan_set_incomplete(
            s, SCAN_PROBLEM(REGEX_UNTERMINATED), s->mark);

        return false;
      }

      emit_named(s, r->tok, PARSE_TOK_STRING_LIT);
      previous_token_invalid(
          s, SCAN_PROBLEM(REGEX_NEWLINE));

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
                         uint8_t quote_kind, size_t body, size_t body_end,
                         size_t consumed_end) {
  scan_lex_session_t *run = s->user_state;
  core_string_t name = heredoc_token(h, quote_kind);
  core_string_t value = heredoc_body_value(s, h, body, body_end);

  run->skip_from = body;
  run->skip_to = consumed_end;
  scan_emit_owned(s, tid_for(run->program->g, name, PARSE_TOK_STRING_LIT),
                  core_option_set(core_string_t, value));

  parse_token_info_t *tok = scan_prev_token(s);
  if (tok) {
    parse_token_set_content_span(tok, s->source_base + body,
                                 s->source_base + body_end);
  }
}

static bool scan_heredoc_lines(parse_scanner_t *s, const scan_lex_heredoc_t *h,
                               uint8_t quote_kind, size_t tag_a, size_t tag_b,
                               size_t body) {
  size_t scan = body;
  size_t matched = 0;
  bool at_indent = h->terminator_indent;
  bool rejected = false;
  size_t line_start = body;
  size_t tag_len = tag_b - tag_a;

  while (scan < s->input.u8_bytes) {
    uint8_t byte = (uint8_t)s->input.data[scan];
    if (byte == '\n') {
      if (!rejected && matched == tag_len) {
        size_t end = scan + 1;
        emit_heredoc(s, h, quote_kind, body, line_start, end);
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

  if (!rejected && matched == tag_len) {
    emit_heredoc(s, h, quote_kind, body, line_start, scan);
    return true;
  }

  scan_set_incomplete(s, SCAN_PROBLEM(HEREDOC_UNTERMINATED), s->mark);
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

static bool scan_heredoc(parse_scanner_t *s, const scan_lex_heredoc_t *h) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_heredoc_header_t *header = &h->header;
  size_t scan = s->cursor + h->opener.u8_bytes;

  while (header->leading_space && scan < s->input.u8_bytes &&
         (s->input.data[scan] == ' ' || s->input.data[scan] == '\t')) {
    scan++;
  }

  uint8_t quote = scan < s->input.u8_bytes ? (uint8_t)s->input.data[scan] : 0;
  uint8_t quote_kind = SCAN_LEX_HEREDOC_UNQUOTED;

  if (quote == '\'' || quote == '"') {
    quote_kind = quote == '\'' ? SCAN_LEX_HEREDOC_SINGLE_QUOTED
                               : SCAN_LEX_HEREDOC_DOUBLE_QUOTED;
    if (!(header->quotes & quote_kind)) {
      scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_QUOTED_NOT_PERMITTED),
                       scan);
      return false;
    }
    scan++;
  } else {
    quote = 0;
    if (!(header->quotes & SCAN_LEX_HEREDOC_UNQUOTED)) {
      scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_MUST_BE_QUOTED), scan);
      return false;
    }
  }

  size_t tag_start = scan;
  size_t tag_end;

  if (quote) {
    while (scan < s->input.u8_bytes &&
           (uint8_t)s->input.data[scan] != quote &&
           s->input.data[scan] != '\n') {
      scan++;
    }
    if (scan == s->input.u8_bytes || s->input.data[scan] == '\n') {
      scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_UNTERMINATED),
                       tag_start - 1);
      return false;
    }
    tag_end = scan;
    scan++;
  } else {
    uint32_t next = (uint32_t)scan;
    int32_t cp =
        unicode_utf8_decode(s->input.data, (uint32_t)s->input.u8_bytes, &next);
    if (cp < 0 || (!unicode_is_id_start((core_codepoint_t)cp) &&
                   !(cp < 128 && heredoc_tag_extra(header->tag_start_extra,
                                                   (uint8_t)cp)))) {
      scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_INVALID), scan);
      return false;
    }
    scan = next;
    while (scan < s->input.u8_bytes) {
      next = (uint32_t)scan;
      cp = unicode_utf8_decode(s->input.data, (uint32_t)s->input.u8_bytes,
                               &next);
      if (cp < 0) {
        scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_INVALID_UTF8), scan);
        return false;
      }
      if (!unicode_is_id_continue((core_codepoint_t)cp) &&
          !(cp < 128 &&
            heredoc_tag_extra(header->tag_continue_extra, (uint8_t)cp))) {
        break;
      }
      scan = next;
    }
    tag_end = scan;
  }

  if (tag_end == tag_start) {
    scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_EMPTY), scan);
    return false;
  }
  size_t token_end = scan;

  if (h->queued) {
    core_string_t tag = core_string_from_raw(s->input.data + tag_start,
                                             (int64_t)(tag_end - tag_start));
    scan_advance_bytes(s, token_end - s->cursor);
    emit_named(s, heredoc_token(h, quote_kind), PARSE_TOK_STRING_LIT);

    parse_token_info_t *token = scan_prev_token(s);
    core_list_push(run->heredoc_queue,
                   ((lex_heredoc_queue_t){
                       .recipe = h,
                       .tag = tag,
                       .token_index = token ? parse_token_index(token) : -1,
                       .quote_kind = quote_kind,
                   }));
    return true;
  }

  while (scan < s->input.u8_bytes && s->input.data[scan] != '\n') {
    if (!header->trailing_text && s->input.data[scan] != ' ' &&
        s->input.data[scan] != '\t') {
      scan_set_invalid(s, SCAN_PROBLEM(HEREDOC_TAG_TRAILING_TEXT), scan);
      return false;
    }
    scan++;
  }

  scan_advance_bytes(s, token_end - s->cursor);
  size_t body = scan < s->input.u8_bytes ? scan + 1 : scan;
  return scan_heredoc_lines(s, h, quote_kind, tag_start, tag_end, body);
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
    return scan_heredoc(s, h);
  }

  return false;
}

static bool try_terminal_min(parse_scanner_t *s, size_t min_len) {
  const scan_lex_t *lex = ((scan_lex_session_t *)s->user_state)->program;
  uint32_t node = 0;
  size_t consumed = 0;
  size_t best_len = 0;
  int32_t best_tid = 0;

  scan_mark(s);
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
      core_codepoint_t next = scan_peek(s, (int32_t)consumed);

      if (next > 0 && id_cont_here(lex, next)) {
        continue;
      }
    }

    best_len = consumed;
    best_tid = candidate->tid;
  }

  if (best_tid) {
    scan_advance_bytes(s, best_len);
    scan_emit_marked(s, best_tid);

    return true;
  }

  return false;
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

  if (!result.diagnosed) {
    return;
  }
  if (s->status == SCAN_STATUS_INVALID) {
    core_free(result.problem.detail.data);
    return;
  }
  core_free(run->status_diagnostic.detail.data);
  run->status_diagnostic = result.problem;
  size_t local_start = parse_token_start_byte(token) - s->source_base
                     + result.diagnostic_start;
  scan_set_invalid(s, run->status_diagnostic, local_start);
  s->status_end_byte = parse_token_start_byte(token)
                     + result.diagnostic_end;
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

  scan_emit_marked(s, upper ? lex->tid_ident_upper : lex->tid_ident);
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

static indent_line_kind_t indent_line_kind(parse_scanner_t *s,
                                           const scan_lex_t *lex, size_t at) {
  if (at >= s->input.u8_bytes) {
    return INDENT_LINE_BLANK;
  }

  if (s->input.data[at] == '\n' || s->input.data[at] == '\r') {
    return INDENT_LINE_BLANK;
  }

  for (const scan_lex_comment_t *c = lex->spec->comments; c && c->opener.data;
       c++) {
    if (c->kind != SCAN_LEX_LINE || c->tok.data) {
      continue;
    }

    if (s->input.u8_bytes - at >= c->opener.u8_bytes &&
        !memcmp(s->input.data + at, c->opener.data, c->opener.u8_bytes)) {
      return INDENT_LINE_COMMENT;
    }
  }

  return INDENT_LINE_CONTENT;
}

static void invalid_indent(parse_scanner_t *s, scan_problem_t problem,
                           size_t start, size_t end) {
  scan_set_invalid(s, problem, start);
  s->status_end_byte = s->source_base + end;
}

static layout_result_t do_layout(parse_scanner_t *s) {
  scan_lex_session_t *run = s->user_state;
  const scan_lex_t *lex = run->program;
  const scan_lex_spec_t *spec = lex->spec;

  if (!(spec->layout & SCAN_LEX_LAYOUT_INDENT) || !run->line_start) {
    return LAYOUT_NONE;
  }

  indent_measure_t m = measure_indent(
      s, spec->indent_tab_width,
      (indent_measure_t){.end = s->cursor, .first_tab = SIZE_MAX});
  indent_line_kind_t kind = indent_line_kind(s, lex, m.end);

  bool ignore = (kind == INDENT_LINE_BLANK &&
                 spec->indent_blank_lines == SCAN_LEX_INDENT_IGNORE) ||
                (kind == INDENT_LINE_COMMENT &&
                 spec->indent_comment_lines == SCAN_LEX_INDENT_IGNORE);

  if (ignore) {
    run->continued_line = false;
    return LAYOUT_NONE;
  }

  if (m.has_tab && spec->indent_tabs == SCAN_LEX_INDENT_REJECT) {
    invalid_indent(s, SCAN_PROBLEM(TABS_IN_INDENTATION),
                   m.first_tab, m.first_tab + 1);
    return LAYOUT_STOP;
  }

  if (m.has_tab && m.has_space &&
      spec->indent_mixed == SCAN_LEX_INDENT_REJECT) {
    invalid_indent(s, SCAN_PROBLEM(MIXED_INDENTATION),
                   s->cursor, m.end);
    return LAYOUT_STOP;
  }

  if (run->bracket_depth > 0 || run->continued_line) {
    run->continued_line = false;

    if (m.end > s->cursor && spec->indent_alignment == SCAN_LEX_INDENT_REJECT) {
      invalid_indent(s,
                     SCAN_PROBLEM(ALIGNMENT_INDENTATION),
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
                     SCAN_PROBLEM(DEDENT_MISMATCH),
                     s->cursor, m.end);
      return LAYOUT_STOP;
    }

    run->pending_dedents = (int32_t)(run->indents.len - keep);

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

    size_t body = s->cursor;
    size_t scan = s->cursor;
    size_t line_start = s->cursor;
    size_t matched = 0;
    bool at_indent = h->terminator_indent;
    bool rejected = false;
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

    if (!complete && !rejected && matched == queued->tag.u8_bytes) {
      body_end = line_start;
      consumed = scan;
      complete = true;
    }

    if (!complete) {
      scan_set_incomplete(s, SCAN_PROBLEM(HEREDOC_QUEUED_UNTERMINATED),
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
  }

  run->heredoc_queue_ready = false;
  return true;
}

static size_t recovery_boundary(parse_scanner_t *s,
                                const scan_lex_recovery_t *recipe) {
  size_t width = 0;

  for (const core_string_t *sync = recipe->synchronize; sync && sync->data;
       sync++) {
    if (sync->u8_bytes > width && at_str(s, *sync)) {
      width = sync->u8_bytes;
    }
  }
  return width;
}

static void finish_recovery(parse_scanner_t *s, scan_lex_session_t *run) {
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

  scan_problem_t message = run->pending_recovery_message;
  if (!s->had_recovery) {
    core_free(run->recovery_diagnostic.detail.data);
    run->recovery_diagnostic = run->pending_recovery_message;
    run->pending_recovery_message = (scan_problem_t){0};
    message = run->recovery_diagnostic;
  }

  size_t start = run->pending_recovery_start >= s->source_base
                     ? run->pending_recovery_start - s->source_base
                     : 0;
  size_t end = run->pending_recovery_end >= s->source_base
                   ? run->pending_recovery_end - s->source_base
                   : start;
  scan_set_recovered(s, message, start, end);

  core_free(run->pending_recovery_message.detail.data);
  run->pending_recovery_message = (scan_problem_t){0};
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
}

static void begin_recovery(parse_scanner_t *s, scan_lex_session_t *run) {
  parse_token_info_t *token = scan_prev_token(s);
  const lex_recovery_t *recovery =
      token ? recovery_for(run->program, parse_token_tid(token)) : nullptr;

  if (!recovery) {
    return;
  }

  run->pending_recovery = recovery;
  run->pending_recovery_token = token;
  if (s->stream && s->stream->token_count &&
      s->stream->tokens[s->stream->token_count - 1] == token) {
    s->stream->tokens[--s->stream->token_count] = nullptr;
  }
  run->pending_recovery_message = scan_problem_copy(s->status_problem);
  run->pending_recovery_start = s->status_start_byte;
  run->pending_recovery_end = s->status_end_byte;
  s->status = SCAN_STATUS_ACTIVE;
  s->status_problem = (scan_problem_t){0};
  s->status_lexer = (core_string_t){0};
  s->status_start_byte = 0;
  s->status_end_byte = 0;

  const scan_lex_recovery_t *recipe = recovery->recipe;
  while (recipe->synchronize && !scan_at_eof(s)) {
    size_t width = recovery_boundary(s, recipe);

    if (width) {
      if (recipe->consume) {
        scan_advance_bytes(s, width);
      }
      break;
    }
    scan_advance(s);
  }

  finish_recovery(s, run);
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
      scan_set_invalid(s, SCAN_PROBLEM(TABS_NOT_ALLOWED), start);
      return false;
    }

    if ((c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') &&
        lex->spec->whitespace == SCAN_LEX_WHITESPACE_SKIP) {
      scan_advance(s);

      continue;
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

    scan_mark(s);

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
    if (core_list_len(run->heredoc_queue)) {
      scan_set_incomplete(
          s, SCAN_PROBLEM(HEREDOC_QUEUED_NOT_BEGUN),
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
    if (s->recovery_enabled && lex->spec->fallback_recovers) {
      scan_set_recovered(s, SCAN_PROBLEM(UNEXPECTED_CHARACTER),
                         s->mark, s->cursor);
    } else {
      scan_set_invalid(s, SCAN_PROBLEM(UNEXPECTED_CHARACTER), s->mark);
    }
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

  bool emitted = scan_lex_scan_one(s);

  if (emitted && s->recovery_enabled && s->status == SCAN_STATUS_INVALID) {
    begin_recovery(s, run);
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
  core_string_t name;
} bidi_control_t;

static const bidi_control_t bidi_controls[] = {
#define BIDI_ENTRY(cp, name) {cp, CORE_STRING_STATIC(name)},
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
      scan_set_invalid(s, SCAN_PROBLEM_WITH(BIDI_CONTROL, control->name), at);
      s->cursor = cursor;
      return;
    }
    if (policy == SCAN_LEX_BIDI_WARN) {
      scan_add_warning(s,
          SCAN_PROBLEM_WITH(FENCE_BIDI_CONTROL, control->name), at,
          at + length);
    }
    at += length - 1;
  }
}
