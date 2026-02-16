// Symbol Table Implementation
//
// Uses the ncc_dict hash table with string keys to store symbols.
// Supports nested scopes with shadowing - when a name is redefined in
// an inner scope, the outer definition is preserved and restored when
// the inner scope is popped.
//
// Distinguishes between declarations (which introduce names)
// and definitions (which create entities).

#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include "st.h"

static sym_entry_t *
sym_alloc(char *name, sym_kind_t kind, int depth)
{
    sym_entry_t *sym = base_calloc(1, sizeof(sym_entry_t));
    if (!sym) {
        return nullptr;
    }
    sym->name        = name;
    sym->kind        = kind;
    sym->scope_depth = depth;
    return sym;
}

static bool
sym_add_loc(sym_loc_t **arr, int *count, int *capacity, tnode_t *node, norm_node_t *type)
{
    if (*count >= *capacity) {
        int        new_cap = *capacity == 0 ? SYM_DECL_INITIAL_CAPACITY : *capacity * 2;
        sym_loc_t *new_arr = base_realloc(*arr, new_cap * sizeof(sym_loc_t));
        if (!new_arr) {
            return false;
        }
        *arr      = new_arr;
        *capacity = new_cap;
    }

    (*arr)[*count].node = node;
    (*arr)[*count].type = type;
    (*count)++;
    return true;
}

#define sym_add_decl(sym, node, type) \
    sym_add_loc(&(sym)->decls, &(sym)->num_decls, &(sym)->decl_capacity, node, type)

#define sym_add_redef(sym, node, type) \
    sym_add_loc(&(sym)->redefs, &(sym)->num_redefs, &(sym)->redef_capacity, node, type)

// Allocate a new scope
static scope_t *
scope_alloc(scope_t *parent, int depth, tnode_t *scope_node)
{
    scope_t *scope = base_calloc(1, sizeof(scope_t));
    if (scope) {
        scope->parent         = parent;
        scope->depth          = depth;
        scope->scope_node     = scope_node;
        scope->first_in_scope = nullptr;
    }
    return scope;
}

void
st_init(symtab_t *st)
{
    ncc_dict_init(&st->symbols, 64, ncc_hash_cstring);
    ncc_dict_init(&st->tags, 64, ncc_hash_cstring);
    st->depth          = 0;
    st->current_scope  = scope_alloc(nullptr, 0, nullptr);
    st->package_name   = nullptr;
    st->package_prefix = nullptr;
}

void
st_push_scope(symtab_t *st, tnode_t *scope_node)
{
    st->depth++;
    scope_t *new_scope = scope_alloc(st->current_scope, st->depth, scope_node);
    st->current_scope  = new_scope;
}

void
st_pop_scope(symtab_t *st)
{
    if (!st->current_scope) {
        return;
    }

    scope_t *scope = st->current_scope;

    // Remove all symbols added in this scope
    for (sym_entry_t *sym = scope->first_in_scope; sym; sym = sym->next_in_scope) {
        // Tags go in a separate namespace
        ncc_dict_t *dict = (sym->kind == SYM_TAG) ? &st->tags : &st->symbols;

        if (sym->shadowed) {
            // Restore the shadowed definition
            ncc_dict_put(dict, sym->name, sym->shadowed);
        }
        else {
            // No shadowed definition, remove from table
            ncc_dict_remove(dict, sym->name);
        }
    }

    // Pop the scope
    st->current_scope = scope->parent;
    st->depth         = scope->parent ? scope->parent->depth : 0;
}

tnode_t *
st_get_scope_node(symtab_t *st)
{
    if (!st->current_scope) {
        return nullptr;
    }
    return st->current_scope->scope_node;
}

int
st_get_scope_depth(symtab_t *st)
{
    return st->depth;
}

// Internal: add or update a symbol
static bool
st_add_symbol(symtab_t *st, char *name, sym_kind_t kind, sym_decl_type_t decl_type, tnode_t *node, norm_node_t *norm_type)
{
    // Tags (struct/union/enum names) go in a separate namespace
    ncc_dict_t *dict = (kind == SYM_TAG) ? &st->tags : &st->symbols;

    bool         found;
    sym_entry_t *existing = ncc_dict_get(dict, name, &found);

    // If symbol exists at current scope depth, update it
    if (found && existing && existing->scope_depth == st->depth) {
        // Check for kind mismatch
        if (existing->kind != kind) {
            return false; // Conflicting declaration
        }

        if (decl_type == SYM_DEFINITION) {
            // Setting definition
            if (existing->def.node != nullptr) {
                // Already defined - capture as redefinition for error reporting
                if (!sym_add_redef(existing, node, norm_type)) {
                    return false; // Allocation failure
                }
            }
            else {
                existing->def.node = node;
                existing->def.type = norm_type;
            }
        }
        else {
            // Adding declaration
            if (!sym_add_decl(existing, node, norm_type)) {
                return false; // Allocation failure
            }
        }
        return true;
    }

    // Create new symbol entry
    sym_entry_t *sym = sym_alloc(name, kind, st->depth);
    if (!sym) {
        return false;
    }

    // Set definition or add declaration
    if (decl_type == SYM_DEFINITION) {
        sym->def.node = node;
        sym->def.type = norm_type;
    }
    else {
        if (!sym_add_decl(sym, node, norm_type)) {
            return false;
        }
    }

    // If there's an existing definition at outer scope, shadow it
    if (found && existing) {
        sym->shadowed = existing;
    }

    // Add to the dict (tags go in separate namespace)
    ncc_dict_put(dict, name, sym);

    // Add to the current scope's list
    sym->next_in_scope                = st->current_scope->first_in_scope;
    st->current_scope->first_in_scope = sym;

    return true;
}

bool
st_add_typedef(symtab_t *st, char *name, tnode_t *node, norm_node_t *norm_type)
{
    // Typedefs are always both declaration and definition
    // Add as definition (which also serves as the declaration)
    return st_add_symbol(st, name, SYM_TYPEDEF, SYM_DEFINITION, node, norm_type);
}

bool
st_add_variable(symtab_t *st, char *name, sym_decl_type_t decl_type, tnode_t *node, norm_node_t *norm_type)
{
    return st_add_symbol(st, name, SYM_VARIABLE, decl_type, node, norm_type);
}

bool
st_add_tag(symtab_t *st, char *name, sym_decl_type_t decl_type, tnode_t *node, norm_node_t *norm_type)
{
    return st_add_symbol(st, name, SYM_TAG, decl_type, node, norm_type);
}

bool
st_add_enum_const(symtab_t *st, char *name, tnode_t *node, norm_node_t *norm_type)
{
    // Enumeration constants are always definitions
    return st_add_symbol(st, name, SYM_ENUM_CONST, SYM_DEFINITION, node, norm_type);
}

sym_entry_t *
st_get_entry(symtab_t *st, char *name)
{
    if (!st || !name) {
        return nullptr;
    }
    // First check ordinary identifiers, then tags
    sym_entry_t *sym = ncc_dict_get(&st->symbols, name, nullptr);
    if (sym) {
        return sym;
    }
    return ncc_dict_get(&st->tags, name, nullptr);
}

sym_entry_t *
st_lookup_in_scope(scope_t *scope, char *name)
{
    if (!name) {
        return nullptr;
    }

    // Walk up the scope chain from the given scope to file scope
    for (scope_t *s = scope; s; s = s->parent) {
        // Search the symbols added in this scope
        for (sym_entry_t *sym = s->first_in_scope; sym; sym = sym->next_in_scope) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
        }
    }

    return nullptr;
}

bool
st_is_typedef(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym && sym->kind == SYM_TYPEDEF;
}

bool
st_is_defined(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym && sym->def.node;
}

bool
st_is_declared(symtab_t *st, char *name)
{
    return st_get_entry(st, name) != nullptr;
}

sym_kind_t
st_get_kind(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym ? sym->kind : SYM_VARIABLE;
}

tnode_t *
st_get_definition(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym ? sym->def.node : nullptr;
}

tnode_t **
st_get_declarations(symtab_t *st, char *name, int *count)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (count) {
        *count = sym ? sym->num_decls : 0;
    }
    if (!sym || sym->num_decls == 0) {
        return nullptr;
    }
    // Allocate array of node pointers extracted from sym_loc_t array
    tnode_t **nodes = base_calloc(sym->num_decls, sizeof(tnode_t *));
    if (!nodes) {
        return nullptr;
    }
    for (int i = 0; i < sym->num_decls; i++) {
        nodes[i] = sym->decls[i].node;
    }
    return nodes;
}

tnode_t *
st_get_first_declaration(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return (sym && sym->num_decls > 0) ? sym->decls[0].node : nullptr;
}

bool
st_has_redefinitions(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym && sym->num_redefs > 0;
}

tnode_t **
st_get_redefinitions(symtab_t *st, char *name, int *count)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (count) {
        *count = sym ? sym->num_redefs : 0;
    }
    if (!sym || sym->num_redefs == 0) {
        return nullptr;
    }
    // Allocate array of node pointers extracted from sym_loc_t array
    tnode_t **nodes = base_calloc(sym->num_redefs, sizeof(tnode_t *));
    if (!nodes) {
        return nullptr;
    }
    for (int i = 0; i < sym->num_redefs; i++) {
        nodes[i] = sym->redefs[i].node;
    }
    return nodes;
}

norm_node_t *
st_get_def_type(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    return sym ? sym->def.type : nullptr;
}

norm_node_t **
st_get_decl_types(symtab_t *st, char *name, int *count)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (count) {
        *count = sym ? sym->num_decls : 0;
    }
    if (!sym || sym->num_decls == 0) {
        return nullptr;
    }
    // Allocate array of type pointers extracted from sym_loc_t array
    norm_node_t **types = base_calloc(sym->num_decls, sizeof(norm_node_t *));
    if (!types) {
        return nullptr;
    }
    for (int i = 0; i < sym->num_decls; i++) {
        types[i] = sym->decls[i].type;
    }
    return types;
}

// =============================================================================
// Label Table Implementation
// =============================================================================

// Allocate a new label entry
static label_entry_t *
label_alloc(char *name)
{
    label_entry_t *label = base_calloc(1, sizeof(label_entry_t));
    if (label) {
        label->name = name;
    }
    return label;
}

// Add a reference to a label's refs array
static bool
label_add_ref(label_entry_t *label, tnode_t *node)
{
    if (!node) {
        return true;
    }

    if (label->num_refs >= label->ref_cap) {
        int       new_cap = label->ref_cap == 0 ? 4 : label->ref_cap * 2;
        tnode_t **new_arr = base_realloc(label->refs, new_cap * sizeof(tnode_t *));
        if (!new_arr) {
            return false;
        }
        label->refs    = new_arr;
        label->ref_cap = new_cap;
    }

    label->refs[label->num_refs++] = node;
    return true;
}

void
lt_init(label_table_t *lt)
{
    ncc_dict_init(&lt->labels, LABEL_INITIAL_CAPACITY, ncc_hash_cstring);
}

bool
lt_add_definition(label_table_t *lt, char *name, tnode_t *node)
{
    if (!lt || !name) {
        return true; // No-op if no label table or name
    }

    bool           found;
    label_entry_t *existing = ncc_dict_get(&lt->labels, name, &found);

    if (found && existing) {
        // Label already exists
        if (existing->def_node != nullptr) {
            // Already defined - duplicate label error
            return false;
        }
        // Was only referenced before, now define it
        existing->def_node = node;
        return true;
    }

    // Create new label entry
    label_entry_t *label = label_alloc(name);
    if (!label) {
        return false;
    }
    label->def_node = node;
    ncc_dict_put(&lt->labels, name, label);
    return true;
}

bool
lt_add_reference(label_table_t *lt, char *name, tnode_t *node)
{
    if (!lt || !name) {
        return true; // No-op if no label table or name
    }

    bool           found;
    label_entry_t *existing = ncc_dict_get(&lt->labels, name, &found);

    if (found && existing) {
        return label_add_ref(existing, node);
    }

    // Create new label entry (referenced before defined - forward goto)
    label_entry_t *label = label_alloc(name);
    if (!label) {
        return false;
    }
    if (!label_add_ref(label, node)) {
        return false;
    }
    ncc_dict_put(&lt->labels, name, label);
    return true;
}

bool
lt_is_defined(label_table_t *lt, char *name)
{
    label_entry_t *label = lt_get_entry(lt, name);
    return label && label->def_node;
}

tnode_t *
lt_get_definition(label_table_t *lt, char *name)
{
    label_entry_t *label = lt_get_entry(lt, name);
    return label ? label->def_node : nullptr;
}

tnode_t **
lt_get_references(label_table_t *lt, char *name, int *count)
{
    label_entry_t *label = lt_get_entry(lt, name);
    if (count) {
        *count = label ? label->num_refs : 0;
    }
    return label ? label->refs : nullptr;
}

label_entry_t *
lt_get_entry(label_table_t *lt, char *name)
{
    if (!lt || !name) {
        return nullptr;
    }
    return ncc_dict_get(&lt->labels, name, nullptr);
}

// ============================================================================
// Keyword Argument Functions
// ============================================================================

kw_info_t *
st_get_kw_info(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (!sym) {
        return nullptr;
    }
    return sym->kw_info;
}

bool
st_set_kw_info(symtab_t *st, char *name, kw_info_t *kw_info)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (!sym) {
        return false;
    }
    sym->kw_info = kw_info;
    return true;
}

// ============================================================================
// Variadic Argument Functions
// ============================================================================

vargs_info_t *
st_get_vargs_info(symtab_t *st, char *name)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (!sym) {
        return nullptr;
    }
    return sym->vargs_info;
}

bool
st_set_vargs_info(symtab_t *st, char *name, vargs_info_t *vargs_info)
{
    sym_entry_t *sym = st_get_entry(st, name);
    if (!sym) {
        return false;
    }
    sym->vargs_info = vargs_info;
    return true;
}
