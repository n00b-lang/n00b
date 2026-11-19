#include "n00bb_rules.h"

#include <pthread.h>

#include "lib/alloc.h"
#include "parse/grammar.h"

_Atomic(const n00bb_rule_table_t *) n00bb_rule_table_latest;

static const core_string_t rule_names[] = {
#define N00BB_RULE_NAME_(id, name) CORE_STRING_STATIC(name),
    N00BB_RULE_NAMES(N00BB_RULE_NAME_)
#undef N00BB_RULE_NAME_
};

// Every table built, newest first: a table is never freed, since a reader
// may still hold it, and versions rarely change (a grammar is loaded once).
typedef struct built_table_t {
    struct built_table_t *next;
    n00bb_rule_table_t *table;
} built_table_t;

static pthread_mutex_t tables_lock = PTHREAD_MUTEX_INITIALIZER;
static built_table_t *tables;

static n00bb_rule_table_t *
build(parse_grammar_t *grammar, uint64_t version)
{
    int64_t ids[N00BB_RULE_COUNT - 1];
    size_t count = 0;
    for (size_t i = 0; i < N00BB_RULE_COUNT - 1; i++) {
        parse_nonterm_t *nt = parse_find_nonterm(grammar, rule_names[i]);
        ids[i] = nt ? parse_nonterm_id(nt) : -1;
        if (ids[i] >= 0 && (size_t)ids[i] + 1 > count)
            count = (size_t)ids[i] + 1;
    }
    n00bb_rule_table_t *table = core_alloc_flex(n00bb_rule_table_t, uint16_t,
                                                count);
    table->version = version;
    table->count = count;
    // Names are unique within a grammar, so no id is named twice.
    for (size_t i = 0; i < N00BB_RULE_COUNT - 1; i++)
        if (ids[i] >= 0) table->rules[ids[i]] = (uint16_t)(i + 1);
    return table;
}

const n00bb_rule_table_t *
n00bb_rule_table_for(parse_grammar_t *grammar)
{
    static const n00bb_rule_table_t empty = {0};
    if (!grammar) return &empty;
    uint64_t version = parse_grammar_version(grammar);
    pthread_mutex_lock(&tables_lock);
    n00bb_rule_table_t *table = nullptr;
    for (built_table_t *at = tables; at && !table; at = at->next)
        if (at->table->version == version) table = at->table;
    if (!table) {
        table = build(grammar, version);
        built_table_t *entry = core_alloc(built_table_t);
        entry->table = table;
        entry->next = tables;
        tables = entry;
    }
    atomic_store_explicit(&n00bb_rule_table_latest, table,
                          memory_order_release);
    pthread_mutex_unlock(&tables_lock);
    return table;
}
