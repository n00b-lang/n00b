// tokenizer_registry.c — Global tokenizer name → callback registry.

#include "parsers/tokenizer_registry.h"
#include "slay/c_tokenizer.h"
#include <string.h>

// Forward declarations for tokenizers defined in other translation units.
extern bool n00b_text_tokenize(n00b_scanner_t *s);
extern bool n00b_shell_tokenize(n00b_scanner_t *s);
extern bool n00b_lisp_tokenize(n00b_scanner_t *s);
extern bool n00b_json_tokenize(n00b_scanner_t *s);
extern bool n00b_lang_tokenize(n00b_scanner_t *s);

// ============================================================================
// Static registry (simple array — tokenizer count is small and fixed)
// ============================================================================

#define MAX_TOKENIZERS 32

typedef struct {
    const char    *name;
    n00b_scan_cb_t cb;
    bool           occupied;
} tokenizer_entry_t;

static tokenizer_entry_t registry[MAX_TOKENIZERS];
static int               registry_count = 0;

void
n00b_tokenizer_register(const char *name, n00b_scan_cb_t cb)
{
    // Check for existing entry with this name and update it.
    for (int i = 0; i < registry_count; i++) {
        if (registry[i].occupied && strcmp(registry[i].name, name) == 0) {
            registry[i].cb = cb;
            return;
        }
    }

    if (registry_count >= MAX_TOKENIZERS) {
        return;
    }

    registry[registry_count].name     = name;
    registry[registry_count].cb       = cb;
    registry[registry_count].occupied = true;
    registry_count++;
}

n00b_scan_cb_t
n00b_tokenizer_lookup(const char *name, bool *found)
{
    if (!name) {
        if (found) *found = false;
        return nullptr;
    }

    for (int i = 0; i < registry_count; i++) {
        if (registry[i].occupied && strcmp(registry[i].name, name) == 0) {
            if (found) *found = true;
            return registry[i].cb;
        }
    }

    if (found) *found = false;
    return nullptr;
}

void
n00b_tokenizers_init(void)
{
    n00b_tokenizer_register("text",      n00b_text_tokenize);
    n00b_tokenizer_register("character", nullptr);
    n00b_tokenizer_register("shell",     n00b_shell_tokenize);
    n00b_tokenizer_register("lisp",      n00b_lisp_tokenize);
    n00b_tokenizer_register("json",      n00b_json_tokenize);
    n00b_tokenizer_register("c",         n00b_c_tokenize);
    n00b_tokenizer_register("n00b",      n00b_lang_tokenize);
}
