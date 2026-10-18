#include "parse/ambiguity.h"
#include "parse/annotation.h"
#include "parse/bnf.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "private/parse/grammar.h"
#include "private/parse/pwz.h"
#include "scanner/token_stream.h"

#include <stdio.h>
#include <string.h>

core_list_decl(int64_t);

typedef core_list_t(int64_t) ambig_sequence_t;
core_list_decl(ambig_sequence_t);

const char *parse_ambiguity_kind_name(parse_ambiguity_kind_t kind) {
  switch (kind) {
  case PARSE_AMBIG_DUPLICATE:
    return "duplicate";
  case PARSE_AMBIG_FIRST_OVERLAP:
    return "first-overlap";
  case PARSE_AMBIG_NULLABLE_PREFIX:
    return "nullable-prefix";
  case PARSE_AMBIG_COMMON_PREFIX:
    return "common-prefix";
  case PARSE_AMBIG_CONFIRMED:
    return "confirmed";
  }

  return "unknown";
}

const char *parse_ambiguity_severity_name(parse_ambiguity_severity_t severity) {
  switch (severity) {
  case PARSE_AMBIG_INFO:
    return "info";
  case PARSE_AMBIG_WARNING:
    return "warning";
  case PARSE_AMBIG_ERROR:
    return "error";
  }

  return "unknown";
}

static void json_string(core_buffer_t *buffer, core_string_t value) {
  core_buffer_putc(buffer, '"');

  for (size_t i = 0; i < value.u8_bytes; i++) {
    unsigned char c = (unsigned char)value.data[i];

    switch (c) {
    case '"':
      core_buffer_puts(buffer, "\\\"");
      break;
    case '\\':
      core_buffer_puts(buffer, "\\\\");
      break;
    case '\n':
      core_buffer_puts(buffer, "\\n");
      break;
    case '\r':
      core_buffer_puts(buffer, "\\r");
      break;
    case '\t':
      core_buffer_puts(buffer, "\\t");
      break;
    default:
      if (c < 0x20) {
        core_buffer_printf(buffer, "\\u%04x", c);
      } else {
        core_buffer_putc(buffer, (char)c);
      }
      break;
    }
  }

  core_buffer_putc(buffer, '"');
}

core_string_t
parse_ambiguity_format_json(const parse_ambiguity_list_t *findings) {
  core_buffer_t buffer = {0};

  core_buffer_putc(&buffer, '[');

  if (findings) {
    for (size_t i = 0; i < findings->len; i++) {
      const parse_ambiguity_finding_t *finding = &findings->data[i];

      if (i) {
        core_buffer_putc(&buffer, ',');
      }

      core_buffer_printf(&buffer,
                         "{\"kind\":\"%s\",\"severity\":\"%s\","
                         "\"nonterminal\":%lld,\"nonterminal_name\":",
                         parse_ambiguity_kind_name(finding->kind),
                         parse_ambiguity_severity_name(finding->severity),
                         (long long)finding->nonterminal);
      json_string(&buffer, finding->nonterminal_name);
      core_buffer_printf(&buffer,
                         ",\"rule_a\":%d,"
                         "\"rule_b\":%d,\"witness_is_prefix\":%s,"
                         "\"source_origin_a\":",
                         finding->rule_a, finding->rule_b,
                         finding->witness_is_prefix ? "true" : "false");
      json_string(&buffer, finding->source_origin_a);
      core_buffer_puts(&buffer, ",\"source_origin_b\":");
      json_string(&buffer, finding->source_origin_b);
      core_buffer_puts(&buffer, ",\"estimated_forest_nodes\":");
      core_buffer_printf(&buffer,
                         "%u,\"estimated_score_calls\":%u,"
                         "\"score_tie_count\":%u,"
                         "\"parse_tree_count\":%u,\"witness\":",
                         finding->estimated_forest_nodes,
                         finding->estimated_score_calls,
                         finding->score_tie_count, finding->parse_tree_count);
      json_string(&buffer, finding->witness);
      core_buffer_puts(&buffer, ",\"derivations\":[");
      for (size_t j = 0; j < finding->derivations.len; j++) {
        if (j) {
          core_buffer_putc(&buffer, ',');
        }
        json_string(&buffer, finding->derivations.data[j]);
      }
      core_buffer_putc(&buffer, ']');
      core_buffer_puts(&buffer, ",\"explanation\":");
      json_string(&buffer, finding->explanation);
      core_buffer_puts(&buffer, ",\"prescription\":");
      json_string(&buffer, finding->prescription);
      core_buffer_putc(&buffer, '}');
    }
  }

  core_buffer_putc(&buffer, ']');
  core_string_t result =
      core_string_from_raw(buffer.data, (int64_t)buffer.byte_len);
  core_buffer_release(&buffer);
  return result;
}

static bool first_overlap_token(const parse_rule_t *a, const parse_rule_t *b,
                                int64_t *out_token) {
  if (a->first_has_any || b->first_has_any || !a->first_set || !b->first_set) {
    return false;
  }

  core_dict_t *small = a->first_set;
  core_dict_t *large = b->first_set;

  if (core_dict_len(small) > core_dict_len(large)) {
    small = b->first_set;
    large = a->first_set;
  }

  core_dict_foreach(small, item) {
    if (core_dict_contains(large, item->key)) {
      *out_token = (int64_t)(uintptr_t)item->key - 0x100;
      return true;
    }
  }

  return false;
}

static core_string_t witness_for_token(parse_grammar_t *g, int64_t token) {
  core_string_t name = parse_terminal_name(g, token);

  if (name.data) {
    return core_string_dup(name);
  }

  const char *builtin = nullptr;
  switch (token) {
  case PARSE_TOK_IDENTIFIER:
    builtin = "<identifier>";
    break;
  case PARSE_TOK_TYPEDEF_NAME:
    builtin = "<typedef-name>";
    break;
  case PARSE_TOK_INTEGER:
    builtin = "<integer>";
    break;
  case PARSE_TOK_FLOAT:
    builtin = "<float>";
    break;
  case PARSE_TOK_CHAR_LIT:
    builtin = "<char-literal>";
    break;
  case PARSE_TOK_STRING_LIT:
    builtin = "<string-literal>";
    break;
  default:
    break;
  }
  if (builtin) {
    return core_string_from_cstr(builtin);
  }

  char text[48];
  int n = snprintf(text, sizeof(text), "<terminal:%lld>", (long long)token);

  return n > 0 ? core_string_from_raw(text, n) : (core_string_t){0};
}

static core_string_t common_prefix_witness(const parse_rule_t *a,
                                           const parse_rule_t *b,
                                           uint32_t max_tokens) {
  if (max_tokens == 0) {
    max_tokens = 8;
  }

  char text[256];
  size_t used = 0;
  size_t limit =
      a->contents.len < b->contents.len ? a->contents.len : b->contents.len;

  if (limit > max_tokens) {
    limit = max_tokens;
  }

  for (size_t i = 0; i < limit; i++) {
    const parse_match_t *ma = &a->contents.data[i];
    const parse_match_t *mb = &b->contents.data[i];

    if (ma->kind != PARSE_MATCH_TERMINAL || mb->kind != PARSE_MATCH_TERMINAL ||
        ma->terminal_id != mb->terminal_id) {
      break;
    }

    int n = snprintf(text + used, sizeof(text) - used, "%s<terminal:%lld>",
                     used ? " " : "", (long long)ma->terminal_id);
    if (n <= 0 || (size_t)n >= sizeof(text) - used) {
      break;
    }
    used += (size_t)n;
  }

  return used ? core_string_from_raw(text, (int64_t)used) : (core_string_t){0};
}

/* FIRST sets and bounded derivations identify candidates; this validates a
 * concrete sequence against the actual parser before reporting ambiguity. */
static void append_derivation_tree(parse_grammar_t *grammar, parse_tree_t *tree,
                                   core_buffer_t *buffer, uint32_t depth) {
  if (!tree) {
    return;
  }
  if (parse_node_is_token(tree)) {
    parse_token_info_t *token = parse_node_token(tree);
    core_string_t label =
        witness_for_token(grammar,
                          token ? parse_token_tid(token) : PARSE_TOK_OTHER);
    core_buffer_printf(buffer, "%*s%s\n", (int)(depth * 2), "",
                       label.data ? label.data : "<terminal>");
    core_free(label.data);
    return;
  }

  parse_nt_node_t *node = &core_tree_node_value(tree);
  parse_rule_t *rule = parse_get_node_rule(grammar, node);
  core_buffer_printf(buffer, "%*s%s [rule %d]\n", (int)(depth * 2), "",
                     node->name.data ? node->name.data : "<anonymous>",
                     rule ? (int)(rule - grammar->rules.data) : -1);
  for (size_t i = 0; i < core_tree_num_children(tree); i++) {
    append_derivation_tree(grammar, core_tree_child(tree, i), buffer,
                           depth + 1);
  }
}

static core_string_t format_derivation_tree(parse_grammar_t *grammar,
                                            parse_tree_t *tree) {
  core_buffer_t *buffer = core_buffer_empty();
  append_derivation_tree(grammar, tree, buffer, 0);
  size_t length = buffer->byte_len;
  char *text = core_buffer_take(buffer);
  core_string_t result = core_string_from_raw(text, (int64_t)length);
  core_free(text);
  return result;
}

static bool confirm_terminal_sequence(
    parse_grammar_t *grammar, parse_nt_id_t nt_id,
    const core_list_t(int64_t) * sequence, uint32_t *out_tree_count,
    uint32_t *out_forest_nodes, uint32_t *out_score_calls,
    uint32_t *out_score_ties, core_list_t(core_string_t) * out_derivations,
    parse_pwz_disambig_fn disambiguator, void *disambiguator_ud) {
  if (out_derivations) {
    *out_derivations = core_list_new(core_string_t);
  }
  if (!grammar || !sequence || sequence->len > INT32_MAX) {
    return false;
  }

  parse_token_info_t **raw =
      sequence->len ? core_alloc_array(parse_token_info_t *, sequence->len)
                    : nullptr;
  for (size_t i = 0; i < sequence->len; i++) {
    parse_token_info_t *token = parse_token_new();
    parse_token_set_tid(token, (int32_t)sequence->data[i]);
    parse_token_set_index(token, (int32_t)i);
    parse_token_set_origin(token, PARSE_TOKEN_SYNTHETIC);
    raw[i] = token;
  }

  parse_token_stream_t *stream =
      scan_token_stream_from_array(raw, (int32_t)sequence->len);
  parse_pwz_parser_t *parser = parse_pwz_new_for_start(grammar, nt_id);
  if (parser && disambiguator) {
    parse_pwz_set_disambiguator(parser, disambiguator, disambiguator_ud);
  }
  bool parsed = parser && parse_pwz_parse(parser, stream);
  uint32_t tree_count = 0;

  if (parsed) {
    parse_tree_array_t trees = parse_pwz_get_trees(parser);
    tree_count = trees.len > UINT32_MAX ? UINT32_MAX : (uint32_t)trees.len;
    if (out_forest_nodes) {
      *out_forest_nodes = parser->all_exps.len > UINT32_MAX
                              ? UINT32_MAX
                              : (uint32_t)parser->all_exps.len;
    }
    if (out_score_calls) {
      *out_score_calls = parser->score_predicate_calls;
    }
    if (out_score_ties) {
      *out_score_ties = parser->score_tie_count;
    }
    if (out_derivations && tree_count > 1) {
      for (size_t i = 0; i < trees.len; i++) {
        core_list_push(*out_derivations,
                       format_derivation_tree(grammar, trees.data[i]));
      }
    }
  }

  if (out_tree_count) {
    *out_tree_count = tree_count;
  }
  bool ambiguous = parsed && tree_count > 1;
  parse_pwz_free(parser);
  scan_token_stream_free(stream);
  for (size_t i = 0; i < sequence->len; i++) {
    parse_token_free(raw[i]);
  }
  core_free(raw);
  if (!ambiguous && out_derivations) {
    for (size_t i = 0; i < out_derivations->len; i++) {
      core_free(out_derivations->data[i].data);
    }
    core_list_free(*out_derivations);
    *out_derivations = (core_list_t(core_string_t)){0};
  }
  return ambiguous;
}

static bool derive_nt(parse_grammar_t *g, parse_nt_id_t nt_id,
                      core_list_t(int64_t) * out, uint32_t max_tokens,
                      bool *visiting);

static void sequence_free(ambig_sequence_t *seq) {
  if (seq) {
    core_list_free(*seq);
  }
}

static ambig_sequence_t sequence_clone(const ambig_sequence_t *seq) {
  ambig_sequence_t result = core_list_new(int64_t);

  if (seq) {
    for (size_t i = 0; i < seq->len; i++) {
      core_list_push(result, seq->data[i]);
    }
  }

  return result;
}

static void sequence_list_free(core_list_t(ambig_sequence_t) * seqs) {
  if (!seqs) {
    return;
  }
  for (size_t i = 0; i < seqs->len; i++) {
    sequence_free(&seqs->data[i]);
  }
  core_list_free(*seqs);
}

static bool sequence_equal(const ambig_sequence_t *a,
                           const ambig_sequence_t *b) {
  return a && b && a->len == b->len &&
         (a->len == 0 ||
          memcmp(a->data, b->data, a->len * sizeof(int64_t)) == 0);
}

static bool sequence_list_contains(const core_list_t(ambig_sequence_t) * seqs,
                                   const ambig_sequence_t *candidate) {
  if (!seqs || !candidate) {
    return false;
  }
  for (size_t i = 0; i < seqs->len; i++) {
    if (sequence_equal(&seqs->data[i], candidate)) {
      return true;
    }
  }
  return false;
}

static void sequence_list_push_unique(core_list_t(ambig_sequence_t) * seqs,
                                      const ambig_sequence_t *candidate) {
  if (!seqs || !candidate || candidate->len == 0 ||
      sequence_list_contains(seqs, candidate)) {
    return;
  }
  core_list_push(*seqs, sequence_clone(candidate));
}

static void sequence_list_sort(core_list_t(ambig_sequence_t) * seqs) {
  if (!seqs) {
    return;
  }
  for (size_t i = 1; i < seqs->len; i++) {
    ambig_sequence_t key = seqs->data[i];
    size_t j = i;

    while (j > 0 && seqs->data[j - 1].len > key.len) {
      seqs->data[j] = seqs->data[j - 1];
      j--;
    }
    seqs->data[j] = key;
  }
}

static bool derive_nt_sequences(parse_grammar_t *g, parse_nt_id_t nt_id,
                                core_list_t(ambig_sequence_t) * out,
                                uint32_t max_tokens, uint8_t *visiting);

static bool append_match_sequences(parse_grammar_t *g,
                                   const parse_match_t *match,
                                   const ambig_sequence_t *prefix,
                                   core_list_t(ambig_sequence_t) * out,
                                   uint32_t max_tokens, uint8_t *visiting) {
  if (!match || !prefix || !out || prefix->len > max_tokens) {
    return false;
  }

  switch (match->kind) {
  case PARSE_MATCH_EMPTY:
    sequence_list_push_unique(out, prefix);
    return true;
  case PARSE_MATCH_TERMINAL: {
    if (prefix->len == max_tokens) {
      return false;
    }
    ambig_sequence_t seq = sequence_clone(prefix);
    core_list_push(seq, match->terminal_id);
    sequence_list_push_unique(out, &seq);
    sequence_free(&seq);
    return true;
  }
  case PARSE_MATCH_NT: {
    core_list_t(ambig_sequence_t) nested = core_list_new(ambig_sequence_t);
    bool ok = derive_nt_sequences(g, match->nt_id, &nested,
                                  max_tokens - (uint32_t)prefix->len, visiting);

    for (size_t i = 0; ok && i < nested.len; i++) {
      if (prefix->len + nested.data[i].len > max_tokens) {
        continue;
      }
      ambig_sequence_t seq = sequence_clone(prefix);
      for (size_t j = 0; j < nested.data[i].len; j++) {
        core_list_push(seq, nested.data[i].data[j]);
      }
      sequence_list_push_unique(out, &seq);
      sequence_free(&seq);
    }
    bool produced = out->len > 0;
    sequence_list_free(&nested);
    return ok && produced;
  }
  case PARSE_MATCH_GROUP: {
    parse_rule_group_t *group = match->group;
    if (!group || group->min == 0) {
      sequence_list_push_unique(out, prefix);
      return true;
    }
    core_list_t(ambig_sequence_t) nested = core_list_new(ambig_sequence_t);
    bool ok = derive_nt_sequences(g, group->contents_id, &nested,
                                  max_tokens - (uint32_t)prefix->len, visiting);

    for (size_t i = 0; ok && i < nested.len; i++) {
      if (prefix->len + nested.data[i].len > max_tokens) {
        continue;
      }
      ambig_sequence_t seq = sequence_clone(prefix);
      for (size_t j = 0; j < nested.data[i].len; j++) {
        core_list_push(seq, nested.data[i].data[j]);
      }
      sequence_list_push_unique(out, &seq);
      sequence_free(&seq);
    }
    bool produced = out->len > 0;
    sequence_list_free(&nested);
    return ok && produced;
  }
  case PARSE_MATCH_ANY:
  case PARSE_MATCH_CLASS:
  case PARSE_MATCH_SET:
    return false;
  }

  return false;
}

static bool derive_rule_sequences(parse_grammar_t *g, const parse_rule_t *rule,
                                  core_list_t(ambig_sequence_t) * out,
                                  uint32_t max_tokens, uint8_t *visiting) {
  core_list_t(ambig_sequence_t) current = core_list_new(ambig_sequence_t);
  ambig_sequence_t empty = core_list_new(int64_t);

  core_list_push(current, empty);

  for (size_t i = 0; rule && i < rule->contents.len; i++) {
    core_list_t(ambig_sequence_t) next = core_list_new(ambig_sequence_t);

    for (size_t j = 0; j < current.len; j++) {
      append_match_sequences(g, &rule->contents.data[i], &current.data[j],
                             &next, max_tokens, visiting);
    }

    sequence_list_free(&current);
    current = next;
    if (!current.len) {
      break;
    }
  }

  sequence_list_sort(&current);
  for (size_t i = 0; i < current.len; i++) {
    sequence_list_push_unique(out, &current.data[i]);
  }
  bool produced = current.len > 0;
  sequence_list_free(&current);
  return produced;
}

static bool derive_nt_sequences(parse_grammar_t *g, parse_nt_id_t nt_id,
                                core_list_t(ambig_sequence_t) * out,
                                uint32_t max_tokens, uint8_t *visiting) {
  if (nt_id < 0 || (size_t)nt_id >= g->nt_list.len || !visiting ||
      visiting[nt_id] >= 2) {
    return false;
  }

  parse_nonterm_t *nt = parse_get_nonterm(g, nt_id);
  if (!nt) {
    return false;
  }

  visiting[nt_id]++;
  for (size_t i = 0; i < nt->rule_ids.len; i++) {
    parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[i]);
    if (!rule) {
      continue;
    }
    derive_rule_sequences(g, rule, out, max_tokens, visiting);
  }
  visiting[nt_id]--;
  sequence_list_sort(out);
  return out->len > 0;
}

static bool derive_match(parse_grammar_t *g, const parse_match_t *match,
                         core_list_t(int64_t) * out, uint32_t max_tokens,
                         bool *visiting) {
  if (out->len > max_tokens) {
    return false;
  }

  switch (match->kind) {
  case PARSE_MATCH_EMPTY:
    return true;
  case PARSE_MATCH_TERMINAL:
    if (out->len == max_tokens) {
      return false;
    }
    core_list_push(*out, match->terminal_id);
    return true;
  case PARSE_MATCH_NT:
    return derive_nt(g, match->nt_id, out, max_tokens, visiting);
  case PARSE_MATCH_GROUP: {
    parse_rule_group_t *group = match->group;
    if (!group || group->min == 0) {
      return true;
    }
    return derive_nt(g, group->contents_id, out, max_tokens, visiting);
  }
  case PARSE_MATCH_ANY:
  case PARSE_MATCH_CLASS:
  case PARSE_MATCH_SET:
    return false;
  }

  return false;
}

static bool derive_nt(parse_grammar_t *g, parse_nt_id_t nt_id,
                      core_list_t(int64_t) * out, uint32_t max_tokens,
                      bool *visiting) {
  if (nt_id < 0 || (size_t)nt_id >= g->nt_list.len || visiting[nt_id]) {
    return false;
  }

  parse_nonterm_t *nt = parse_get_nonterm(g, nt_id);
  if (!nt) {
    return false;
  }

  visiting[nt_id] = true;
  bool found = false;
  size_t best_len = SIZE_MAX;
  core_list_t(int64_t) best = core_list_new(int64_t);

  for (size_t i = 0; i < nt->rule_ids.len; i++) {
    parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[i]);
    if (!rule) {
      continue;
    }

    core_list_t(int64_t) candidate = core_list_new(int64_t);
    bool ok = true;
    for (size_t j = 0; j < rule->contents.len && ok; j++) {
      ok = derive_match(g, &rule->contents.data[j], &candidate, max_tokens,
                        visiting);
    }

    if (ok && candidate.len < best_len) {
      core_list_clear(best);
      for (size_t j = 0; j < candidate.len; j++) {
        core_list_push(best, candidate.data[j]);
      }
      best_len = candidate.len;
      found = true;
    }
    core_list_free(candidate);
  }

  visiting[nt_id] = false;
  if (found) {
    for (size_t i = 0; i < best.len; i++) {
      core_list_push(*out, best.data[i]);
    }
  }
  core_list_free(best);
  return found;
}

static core_string_t sequence_witness(parse_grammar_t *g,
                                      const core_list_t(int64_t) * sequence) {
  char text[256];
  size_t used = 0;

  for (size_t i = 0; i < sequence->len; i++) {
    core_string_t label = witness_for_token(g, sequence->data[i]);
    int n =
        snprintf(text + used, sizeof(text) - used, "%s%.*s", used ? " " : "",
                 (int)label.u8_bytes, label.data ? label.data : "?");
    core_free(label.data);
    if (n <= 0 || (size_t)n >= sizeof(text) - used) {
      return (core_string_t){0};
    }
    used += (size_t)n;
  }

  return used ? core_string_from_raw(text, (int64_t)used) : (core_string_t){0};
}

static bool same_match(const parse_match_t *a, const parse_match_t *b) {
  if (a->kind != b->kind) {
    return false;
  }

  switch (a->kind) {
  case PARSE_MATCH_NT:
    return a->nt_id == b->nt_id;
  case PARSE_MATCH_TERMINAL:
    return a->terminal_id == b->terminal_id;
  case PARSE_MATCH_CLASS:
    return a->char_class == b->char_class;
  case PARSE_MATCH_EMPTY:
  case PARSE_MATCH_ANY:
    return true;
  case PARSE_MATCH_SET:
  case PARSE_MATCH_GROUP:
    return a->set_items == b->set_items;
  }

  return false;
}

static bool same_rule(const parse_rule_t *a, const parse_rule_t *b) {
  if (!a || !b || a->contents.len != b->contents.len) {
    return false;
  }

  for (size_t i = 0; i < a->contents.len; i++) {
    if (!same_match(&a->contents.data[i], &b->contents.data[i])) {
      return false;
    }
  }

  return true;
}

static bool common_terminal_prefix(const parse_rule_t *a, const parse_rule_t *b,
                                   int64_t *token) {
  if (!a || !b || a->contents.len == 0 || b->contents.len == 0 ||
      a->contents.data[0].kind != PARSE_MATCH_TERMINAL ||
      b->contents.data[0].kind != PARSE_MATCH_TERMINAL ||
      a->contents.data[0].terminal_id != b->contents.data[0].terminal_id) {
    return false;
  }

  *token = a->contents.data[0].terminal_id;
  return true;
}

static bool has_nullable_prefix(parse_grammar_t *g, const parse_rule_t *rule) {
  for (size_t i = 0; i < rule->contents.len; i++) {
    const parse_match_t *m = &rule->contents.data[i];

    if (m->kind == PARSE_MATCH_EMPTY) {
      continue;
    }
    if (m->kind == PARSE_MATCH_NT) {
      parse_nonterm_t *nt = parse_get_nonterm(g, m->nt_id);
      bool nullable = nt && nt->nullable;

      if (nt && !nullable) {
        for (size_t j = 0; j < nt->rule_ids.len; j++) {
          parse_rule_t *nested = parse_get_rule(g, nt->rule_ids.data[j]);
          if (nested && nested->contents.len == 1 &&
              nested->contents.data[0].kind == PARSE_MATCH_EMPTY) {
            nullable = true;
            break;
          }
        }
      }

      if (nullable) {
        return true;
      }
      return false;
    }
    if (m->kind == PARSE_MATCH_GROUP) {
      parse_rule_group_t *group = m->group;
      if (group && group->min == 0) {
        return true;
      }
      return false;
    }
    return false;
  }

  return true;
}

static bool rule_is_intentional(parse_grammar_t *g, const parse_rule_t *rule) {
  (void)g;
  for (size_t i = 0; i < rule->annotations.len; i++) {
    if (rule->annotations.data[i] &&
        rule->annotations.data[i]->kind == PARSE_ANNOT_DISAMBIG) {
      return true;
    }
  }

  return false;
}

static bool
rule_is_recursive_literal_delimiter_postfix(const parse_rule_t *rule,
                                            parse_nt_id_t nt_id) {
  if (!rule || rule->contents.len < 2) {
    return false;
  }
  const parse_match_t *first = &rule->contents.data[0];
  const parse_match_t *next = &rule->contents.data[1];

  return first->kind == PARSE_MATCH_NT && first->nt_id == nt_id &&
         next->kind == PARSE_MATCH_TERMINAL &&
         (next->terminal_id == '(' || next->terminal_id == '[');
}

static bool token_is_identifier(parse_grammar_t *g, int64_t token) {
  if (token == PARSE_TOK_IDENTIFIER) {
    return true;
  }
  core_string_t name = parse_terminal_name(g, token);
  return name.data && core_string_eq(name, CORE_STRING_STATIC("IDENTIFIER"));
}

static core_string_t rule_source_origin(const parse_rule_t *rule) {
  for (size_t i = 0; i < rule->annotations.len; i++) {
    parse_annotation_t *annotation = rule->annotations.data[i];
    if (annotation && annotation->source_origin.data) {
      return core_string_dup(annotation->source_origin);
    }
  }

  return (core_string_t){0};
}

static parse_grammar_t *clone_for_audit(const parse_grammar_t *source) {
  parse_grammar_t *copy = parse_grammar_new();
  parse_grammar_set_error_recovery(copy, false);

  for (size_t i = 0; i < source->named_terms.len; i++) {
    parse_register_terminal(copy, source->named_terms.data[i].value);
  }

  for (size_t i = 0; i < source->nt_list.len; i++) {
    parse_nonterm_t *src_nt = source->nt_list.data[i];
    parse_nonterm_t *dst_nt = parse_nonterm(copy, src_nt->name);
    dst_nt->group_nt = src_nt->group_nt;
  }

  for (size_t i = 0; i < source->rules.len; i++) {
    const parse_rule_t *src_rule = &source->rules.data[i];
    parse_rule_t *dst_rule =
        parse_add_rule_v(copy, src_rule->nt_id, (int)src_rule->contents.len,
                         src_rule->contents.data);
    if (!dst_rule) {
      continue;
    }

    dst_rule->cost = src_rule->cost;
    dst_rule->penalty_rule = src_rule->penalty_rule;
    dst_rule->disambig_cost = src_rule->disambig_cost;
    if (src_rule->disambig_predicate.data) {
      dst_rule->disambig_predicate =
          core_string_dup(src_rule->disambig_predicate);
    }

    for (size_t j = 0; j < src_rule->annotations.len; j++) {
      if (src_rule->annotations.data[j]) {
        parse_rule_annotate(dst_rule,
                            parse_annot_clone(src_rule->annotations.data[j]));
      }
    }
  }

  return copy;
}

static bool first_sets_overlap(const parse_rule_t *a, const parse_rule_t *b) {
  if (a->first_has_any || b->first_has_any) {
    return true;
  }

  if (!a->first_set || !b->first_set) {
    return false;
  }

  core_dict_t *small = a->first_set;
  core_dict_t *large = b->first_set;

  if (core_dict_len(small) > core_dict_len(large)) {
    small = b->first_set;
    large = a->first_set;
  }

  core_dict_foreach(small, item) {
    if (core_dict_contains(large, item->key)) {
      return true;
    }
  }

  return false;
}

static void add_finding(parse_ambiguity_list_t *out,
                        const parse_ambiguity_options_t *options,
                        parse_ambiguity_kind_t kind, parse_nt_id_t nt_id,
                        core_string_t nt_name, int32_t rule_a, int32_t rule_b,
                        core_string_t source_a, core_string_t source_b,
                        core_string_t witness, bool witness_is_prefix,
                        const char *explanation, const char *prescription) {
  if (options && options->max_findings && out->len >= options->max_findings) {
    return;
  }

  for (size_t i = 0; i < out->len; i++) {
    const parse_ambiguity_finding_t *old = &out->data[i];

    if (old->kind == kind && old->nonterminal == nt_id &&
        old->rule_a == rule_a && old->rule_b == rule_b) {
      return;
    }
  }

  parse_ambiguity_finding_t finding = {
      .kind = kind,
      .severity =
          (kind == PARSE_AMBIG_DUPLICATE || kind == PARSE_AMBIG_CONFIRMED)
              ? PARSE_AMBIG_ERROR
              : PARSE_AMBIG_WARNING,
      .nonterminal = nt_id,
      .nonterminal_name = nt_name,
      .rule_a = rule_a,
      .rule_b = rule_b,
      .source_origin_a = source_a,
      .source_origin_b = source_b,
      .witness = witness,
      .witness_is_prefix = witness_is_prefix,
      .explanation = core_string_from_cstr(explanation),
      .prescription = core_string_from_cstr(prescription),
  };

  core_list_push(*out, finding);
}

parse_ambiguity_list_t
parse_grammar_audit_ambiguity(parse_grammar_t *grammar,
                              const parse_ambiguity_options_t *options) {
  parse_ambiguity_list_t findings = core_list_new(parse_ambiguity_finding_t);

  if (!grammar) {
    return findings;
  }

  parse_grammar_t *work = clone_for_audit(grammar);
  parse_grammar_finalize(work);

  size_t states = 0;

  for (size_t ni = 0; ni < work->nt_list.len; ni++) {
    parse_nonterm_t *nt = work->nt_list.data[ni];

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
      int32_t rule_a_id = nt->rule_ids.data[i];
      parse_rule_t *rule_a = parse_get_rule(work, rule_a_id);

      if (!rule_a) {
        continue;
      }

      for (size_t j = i + 1; j < nt->rule_ids.len; j++) {
        if (options && options->max_states && states++ >= options->max_states) {
          parse_grammar_free(work);
          return findings;
        }

        int32_t rule_b_id = nt->rule_ids.data[j];
        parse_rule_t *rule_b = parse_get_rule(work, rule_b_id);

        if (!rule_b) {
          continue;
        }

        if (options && !options->include_intentional &&
            !options->disambiguator &&
            (rule_is_intentional(work, rule_a) ||
             rule_is_intentional(work, rule_b))) {
          continue;
        }

        if (same_rule(rule_a, rule_b)) {
          add_finding(&findings, options, PARSE_AMBIG_DUPLICATE, nt->id,
                      core_string_dup(nt->name), rule_a_id, rule_b_id,
                      rule_source_origin(rule_a), rule_source_origin(rule_b),
                      (core_string_t){0}, false,
                      "two alternatives have identical bodies",
                      "remove the duplicate alternative");
        } else if (first_sets_overlap(rule_a, rule_b)) {
          int64_t token = 0;
          bool has_token = false;
          bool witness_is_prefix = false;
          core_string_t witness = {0};
          parse_ambiguity_kind_t kind = PARSE_AMBIG_FIRST_OVERLAP;
          const char *explanation = "alternatives have overlapping FIRST sets";
          const char *prescription =
              "left-factor the shared prefix or add explicit precedence";

          uint32_t max_tokens = options && options->max_witness_tokens
                                    ? options->max_witness_tokens
                                    : 8;
          core_list_t(int64_t) sequence_a = core_list_new(int64_t);
          core_list_t(int64_t) sequence_b = core_list_new(int64_t);
          bool *visiting = core_alloc_array(bool, work->nt_list.len);
          bool derives_a = true;
          bool derives_b = true;
          bool exact_candidate = false;
          int64_t deep_overlap_token = 0;
          bool has_deep_overlap_token =
              first_overlap_token(rule_a, rule_b, &deep_overlap_token);

          for (size_t k = 0; k < rule_a->contents.len; k++) {
            if (!derive_match(work, &rule_a->contents.data[k], &sequence_a,
                              max_tokens, visiting)) {
              derives_a = false;
              break;
            }
          }
          memset(visiting, 0, work->nt_list.len * sizeof(bool));
          for (size_t k = 0; k < rule_b->contents.len; k++) {
            if (!derive_match(work, &rule_b->contents.data[k], &sequence_b,
                              max_tokens, visiting)) {
              derives_b = false;
              break;
            }
          }

          if (derives_a && derives_b && sequence_a.len == sequence_b.len &&
              memcmp(sequence_a.data, sequence_b.data,
                     sequence_a.len * sizeof(int64_t)) == 0) {
            exact_candidate = true;
            has_token = true;
            witness = sequence_witness(work, &sequence_a);
            if (has_nullable_prefix(work, rule_a) ||
                has_nullable_prefix(work, rule_b)) {
              kind = PARSE_AMBIG_NULLABLE_PREFIX;
              explanation =
                  "a nullable prefix derives the same bounded token sequence";
              prescription =
                  "make optionality explicit or separate the follow rule";
            } else {
              explanation =
                  "alternatives derive the same bounded token sequence";
              prescription = "remove the duplicate derivation or add explicit "
                             "disambiguation";
            }
          }

          if (!exact_candidate && has_deep_overlap_token &&
              token_is_identifier(work, deep_overlap_token) &&
              (rule_is_recursive_literal_delimiter_postfix(rule_a, nt->id) ||
               rule_is_recursive_literal_delimiter_postfix(rule_b, nt->id))) {
            core_list_t(ambig_sequence_t) sequences_a =
                core_list_new(ambig_sequence_t);
            core_list_t(ambig_sequence_t) sequences_b =
                core_list_new(ambig_sequence_t);
            uint8_t *sequence_visiting =
                core_alloc_array(uint8_t, work->nt_list.len);
            uint32_t deep_max_tokens = max_tokens < 4 ? max_tokens : 4;

            memset(sequence_visiting, 0, work->nt_list.len * sizeof(uint8_t));
            derive_rule_sequences(work, rule_a, &sequences_a, deep_max_tokens,
                                  sequence_visiting);
            memset(sequence_visiting, 0, work->nt_list.len * sizeof(uint8_t));
            derive_rule_sequences(work, rule_b, &sequences_b, deep_max_tokens,
                                  sequence_visiting);

            for (size_t ai = 0; !exact_candidate && ai < sequences_a.len;
                 ai++) {
              for (size_t bi = 0; bi < sequences_b.len; bi++) {
                if (!sequence_equal(&sequences_a.data[ai],
                                    &sequences_b.data[bi])) {
                  continue;
                }
                core_list_clear(sequence_a);
                core_list_clear(sequence_b);
                for (size_t si = 0; si < sequences_a.data[ai].len; si++) {
                  core_list_push(sequence_a, sequences_a.data[ai].data[si]);
                  core_list_push(sequence_b, sequences_a.data[ai].data[si]);
                }
                exact_candidate = true;
                has_token = true;
                core_free(witness.data);
                witness = sequence_witness(work, &sequence_a);
                explanation =
                    "alternatives derive the same bounded token sequence";
                prescription = "remove the duplicate derivation or add "
                               "explicit disambiguation";
                break;
              }
            }

            sequence_list_free(&sequences_a);
            sequence_list_free(&sequences_b);
            core_free(sequence_visiting);
          }

          if (exact_candidate) {
            /* Keep the complete derivation witness. */
          } else if (common_terminal_prefix(rule_a, rule_b, &token)) {
            has_token = true;
            kind = PARSE_AMBIG_COMMON_PREFIX;
            witness_is_prefix = true;
            witness = common_prefix_witness(
                rule_a, rule_b, options ? options->max_witness_tokens : 0);
            explanation = "alternatives begin with the same terminal";
            prescription = "left-factor the common prefix";
          } else if (has_nullable_prefix(work, rule_a) ||
                     has_nullable_prefix(work, rule_b)) {
            kind = PARSE_AMBIG_NULLABLE_PREFIX;
            explanation = "a nullable prefix overlaps another alternative";
            prescription =
                "make optionality explicit or separate the follow rule";
          } else if (first_overlap_token(rule_a, rule_b, &token)) {
            has_token = true;
            /* The FIRST-set witness is still useful even when
             * the overlap comes through a nonterminal. */
          }

          if (has_token && !witness.data) {
            witness = witness_for_token(work, token);
          }

          uint32_t confirmed_trees = 0;
          uint32_t confirmed_forest_nodes = 0;
          uint32_t confirmed_score_calls = 0;
          uint32_t confirmed_score_ties = 0;
          core_list_t(core_string_t) derivations = core_list_new(core_string_t);
          bool confirmed = exact_candidate &&
                           confirm_terminal_sequence(
                               work, nt->id, &sequence_a, &confirmed_trees,
                               &confirmed_forest_nodes, &confirmed_score_calls,
                               &confirmed_score_ties, &derivations,
                               options ? options->disambiguator : nullptr,
                               options ? options->disambiguator_ud : nullptr);

          if (confirmed) {
            kind = PARSE_AMBIG_CONFIRMED;
            explanation = "PWZ produces multiple parse trees for this witness";
            prescription =
                "remove one derivation or add explicit disambiguation";
          }

          if (!confirmed && (!options || !options->include_candidates)) {
            core_free(witness.data);
            for (size_t di = 0; di < derivations.len; di++) {
              core_free(derivations.data[di].data);
            }
            core_list_free(derivations);
            core_free(visiting);
            core_list_free(sequence_a);
            core_list_free(sequence_b);
            continue;
          }

          add_finding(&findings, options,
                      confirmed ? PARSE_AMBIG_CONFIRMED : kind, nt->id,
                      core_string_dup(nt->name), rule_a_id, rule_b_id,
                      rule_source_origin(rule_a), rule_source_origin(rule_b),
                      witness, witness_is_prefix, explanation, prescription);
          if (confirmed && findings.len) {
            parse_ambiguity_finding_t *finding =
                &findings.data[findings.len - 1];
            finding->parse_tree_count = confirmed_trees;
            finding->estimated_forest_nodes = confirmed_forest_nodes;
            finding->estimated_score_calls = confirmed_score_calls;
            finding->score_tie_count = confirmed_score_ties;
            finding->derivations = derivations;
          } else {
            core_list_free(derivations);
          }
          core_free(visiting);
          core_list_free(sequence_a);
          core_list_free(sequence_b);
        }
      }
    }
  }

  parse_grammar_free(work);
  return findings;
}

int parse_ambiguity_cli(int argc, char **argv, FILE *out, FILE *err) {
  bool json = false;
  bool candidates = false;
  int arg = 1;

  while (argc > arg && argv[arg][0] == '-') {
    if (strcmp(argv[arg], "--json") == 0) {
      json = true;
    } else if (strcmp(argv[arg], "--candidates") == 0) {
      candidates = true;
    } else {
      break;
    }
    arg++;
  }

  if (argc - arg != 2) {
    fprintf(err, "usage: %s [--json] [--candidates] GRAMMAR.bnf START_SYMBOL\n",
            argc > 0 ? argv[0] : "ambiguity-audit");
    return 2;
  }

  parse_grammar_t *grammar = parse_grammar_new();
  parse_grammar_set_error_recovery(grammar, false);

  if (!parse_bnf_load_file(
          argv[arg], core_string_borrow(argv[arg + 1], strlen(argv[arg + 1])),
          grammar)) {
    fprintf(err, "failed to load %s: %s\n", argv[arg], parse_bnf_error());
    parse_grammar_free(grammar);
    return 1;
  }

  parse_ambiguity_options_t options = {
      .max_witness_tokens = 8,
      .max_states = 10000,
      .max_findings = 1000,
      .include_candidates = candidates,
  };
  parse_ambiguity_list_t findings =
      parse_grammar_audit_ambiguity(grammar, &options);

  if (json) {
    core_string_t output = parse_ambiguity_format_json(&findings);
    fprintf(out, "%.*s\n", (int)output.u8_bytes, output.data);
    core_free(output.data);
  } else {
    for (size_t i = 0; i < findings.len; i++) {
      parse_ambiguity_finding_t *finding = &findings.data[i];
      fprintf(out,
              "%s: nonterminal %.*s (#%lld), rules %d and %d: %s\n"
              "  %s\n"
              "  suggestion: %s\n",
              parse_ambiguity_severity_name(finding->severity),
              (int)finding->nonterminal_name.u8_bytes,
              finding->nonterminal_name.data ? finding->nonterminal_name.data
                                             : "?",
              (long long)finding->nonterminal, finding->rule_a, finding->rule_b,
              parse_ambiguity_kind_name(finding->kind),
              finding->explanation.data, finding->prescription.data);
      if (finding->source_origin_a.data || finding->source_origin_b.data) {
        fprintf(out, "  sources: %.*s / %.*s\n",
                (int)finding->source_origin_a.u8_bytes,
                finding->source_origin_a.data ? finding->source_origin_a.data
                                              : "?",
                (int)finding->source_origin_b.u8_bytes,
                finding->source_origin_b.data ? finding->source_origin_b.data
                                              : "?");
      }
      if (finding->witness.data) {
        fprintf(out, "  %s: %.*s\n",
                finding->witness_is_prefix ? "prefix" : "witness",
                (int)finding->witness.u8_bytes, finding->witness.data);
      }
      if (finding->kind == PARSE_AMBIG_CONFIRMED) {
        fprintf(out, "  structural trees: %u; equal-score ties: %u\n",
                finding->parse_tree_count, finding->score_tie_count);
      }
      for (size_t j = 0; j < finding->derivations.len; j++) {
        fprintf(out, "  match %zu:\n%.*s\n", j + 1,
                (int)finding->derivations.data[j].u8_bytes,
                finding->derivations.data[j].data);
      }
    }
  }

  int result = findings.len ? 1 : 0;
  parse_ambiguity_list_free(&findings);
  parse_grammar_free(grammar);
  return result;
}

void parse_ambiguity_list_free(parse_ambiguity_list_t *findings) {
  if (!findings) {
    return;
  }

  for (size_t i = 0; i < findings->len; i++) {
    core_free(findings->data[i].nonterminal_name.data);
    core_free(findings->data[i].source_origin_a.data);
    core_free(findings->data[i].source_origin_b.data);
    core_free(findings->data[i].witness.data);
    for (size_t j = 0; j < findings->data[i].derivations.len; j++) {
      core_free(findings->data[i].derivations.data[j].data);
    }
    core_list_free(findings->data[i].derivations);
    core_free(findings->data[i].explanation.data);
    core_free(findings->data[i].prescription.data);
  }

  core_list_free(*findings);
}

bool parse_ambiguity_confirm_finding(parse_grammar_t *grammar,
                                     parse_ambiguity_finding_t *finding,
                                     parse_ambiguity_tokenize_fn tokenize,
                                     void *user_data) {
  if (!grammar || !finding || !tokenize) {
    return false;
  }

  parse_token_stream_t *stream = tokenize(grammar, finding, user_data);
  if (!stream) {
    return false;
  }

  parse_pwz_parser_t *parser =
      parse_pwz_new_for_start(grammar, finding->nonterminal);
  bool parsed = parser && parse_pwz_parse(parser, stream);

  if (parsed) {
    parse_tree_array_t trees = parse_pwz_get_trees(parser);
    size_t count = trees.len;

    finding->parse_tree_count =
        count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
    finding->estimated_forest_nodes = parser->all_exps.len > UINT32_MAX
                                          ? UINT32_MAX
                                          : (uint32_t)parser->all_exps.len;
    finding->estimated_score_calls = parser->score_predicate_calls;
    finding->score_tie_count = parser->score_tie_count;

    if (count > 1) {
      for (size_t i = 0; i < finding->derivations.len; i++) {
        core_free(finding->derivations.data[i].data);
      }
      core_list_free(finding->derivations);
      finding->derivations = core_list_new(core_string_t);
      for (size_t i = 0; i < trees.len; i++) {
        core_list_push(finding->derivations,
                       format_derivation_tree(grammar, trees.data[i]));
      }
      finding->kind = PARSE_AMBIG_CONFIRMED;
      finding->severity = PARSE_AMBIG_ERROR;
    } else {
      parsed = false;
    }
  }

  parse_pwz_free(parser);
  scan_token_stream_free(stream);
  return parsed;
}
