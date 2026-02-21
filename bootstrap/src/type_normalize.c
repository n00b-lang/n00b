/**
 * @file type_normalize.c
 * @brief Type tree normalization for `typeid()` / `typestr()`.
 *
 * Normalizes the parse tree produced by `type_parse` into a canonical
 * form suitable for hashing.  Unordered elements (qualifier lists,
 * attribute lists) are sorted lexicographically; ordered elements
 * (parameters, struct members) are preserved in declaration order.
 * The result is a simplified tree that folds into a unique token
 * sequence, which is then hashed to produce a stable type identifier.
 */

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "base_alloc_shim.h"
#include "ncc_limits.h"

#include "st.h"
#include "types.h"
#include "parse.h"

#define SHA2_BLOCK_SIZE 64

// Qualifiers to remove during normalization.
// const and volatile are kept; restrict and _Atomic are dropped.
static const char *quals_to_drop[] = {"restrict", "_Atomic", nullptr};

static bool
in_list(const char *s, const char **list)
{
    for (int i = 0; list[i]; i++) {
        if (!strcmp(s, list[i])) {
            return true;
        }
    }
    return false;
}

// Initial capacity for child lists; grows as needed.
#define INITIAL_KIDS_CAPACITY 4

// Used when we convert binary blobs to unique identifiers; since
// there are only 63 valid chars, if we find the last two, we generate
// _a or _b when we need to encode.

// clang-format off
static const signed char b64_map[] = {
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',
    'z', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', -1, -2
};


static_assert(sizeof(b64_map) == 64);

// clang-format on

// Stable encoding IDs for NT types. These are used for type identity
// hashing and must NOT change across builds (unlike nt_type_t enum
// values which are auto-generated and may shift). Map from nt_type_t
// to a stable encoding integer; -1 means not a recognized type NT.
#define PASSTHROUGH_ENCODING_ID 0

static int
nt_encoding_id(nt_type_t nt)
{
    switch (nt) {
    case NT_type_name:
        return 1;
    case NT_specifier_qualifier_list:
        return 2;
    case NT_abstract_declarator:
        return 3;
    case NT_type_specifier_qualifier:
        return 4;
    case NT_attribute_specifier_sequence:
        return 5;
    case NT_pointer:
        return 6;
    case NT_direct_abstract_declarator:
        return 7;
    case NT_type_specifier:
        return 8;
    case NT_alignment_specifier:
        return 9;
    case NT_attribute_specifier:
        return 10;
    case NT_type_qualifier_list:
        return 11;
    case NT_array_abstract_declarator:
        return 12;
    case NT_function_abstract_declarator:
        return 13;
    case NT_atomic_type_specifier:
        return 14;
    case NT_struct_or_union_specifier:
        return 15;
    case NT_enum_specifier:
        return 16;
    case NT_typedef_name:
        return 17;
    case NT_typeof_specifier:
        return 18;
    case NT_attribute_list:
        return 19;
    case NT_parameter_type_list:
        return 20;
    case NT_member_declaration_list:
        return 21;
    case NT_enum_type_specifier:
        return 22;
    case NT_enumerator_list:
        return 23;
    case NT_typeof_specifier_argument:
        return 24;
    case NT_attribute:
        return 25;
    case NT_parameter_list:
        return 26;
    case NT_member_declaration:
        return 27;
    case NT_enumerator:
        return 28;
    case NT_attribute_token:
        return 29;
    case NT_attribute_argument_clause:
        return 30;
    case NT_parameter_declaration:
        return 31;
    case NT_member_declarator_list:
        return 32;
    case NT_static_assert_declaration:
        return 33;
    case NT_enumeration_constant:
        return 34;
    case NT_standard_attribute:
        return 35;
    case NT_attribute_prefixed_token:
        return 36;
    case NT_attribute_prefix:
        return 37;
    case NT_declaration_specifiers:
        return 38;
    case NT_declarator:
        return 39;
    case NT_member_declarator:
        return 40;
    case NT_declaration_specifier:
        return 41;
    case NT_direct_declarator:
        return 42;
    case NT_storage_class_specifier:
        return 43;
    case NT_function_specifier:
        return 44;
    case NT_array_declarator:
        return 45;
    case NT_function_declarator:
        return 46;
    default:
        return -1;
    }
}

#define NUM_NT_ENCODINGS 47

enum non_nt_codes {
    ID_OR_KEYWORD_CODE = 70,
    PUNCT_LPAREN,
    PUNCT_RPAREN,
    PUNCT_LBRACKET,
    PUNCT_RBRACKET,
    PUNCT_LBRACE,
    PUNCT_RBRACE,
    PUNCT_POINTER,
    PUNCT_COLON,
    PUNCT_ELIPSIS,
    PUNCT_COMMA,
    PUNCT_SEMI,
    PUNCT_EQ,
};

static_assert(ID_OR_KEYWORD_CODE > NUM_NT_ENCODINGS);

typedef struct norm_node_t norm_node_t;

struct norm_node_t {
    nt_type_t    nt_id;
    char        *value;
    tok_t       *leaf;
    norm_node_t *parent;
    char        *_private;
    ncc_list_t      *kids;
    int          num_kids;
};

typedef struct norm_ctx {
    ncc_buf_t   *input;
    tnode_t     *cur;
    norm_node_t *root;
} norm_ctx;

static norm_node_t *
norm_alloc(nt_type_t nt_id)
{
    norm_node_t *result = base_calloc(1, sizeof(norm_node_t));
    result->nt_id       = nt_id;
    result->kids        = ncc_list_alloc(INITIAL_KIDS_CAPACITY);
    result->num_kids    = 0;

    return result;
}

static void
add_sub(norm_node_t *parent, norm_node_t *kid)
{
    kid->parent = parent;

    // Grow the list if needed
    if (parent->num_kids >= parent->kids->nitems) {
        int     new_cap  = parent->kids->nitems * 2;
        ncc_list_t *new_kids = ncc_list_alloc(new_cap);
        for (int i = 0; i < parent->num_kids; i++) {
            new_kids->items[i] = parent->kids->items[i];
        }
        base_dealloc(parent->kids);
        parent->kids = new_kids;
    }

    parent->kids->items[parent->num_kids++] = kid;
}

typedef struct {
    tnode_t     *tn;
    norm_node_t *result;
    int          kid_index;
} simplify_frame_t;

// Iterative version of initial_simplify to handle deeply nested parse trees
// (e.g. 16K+ element initializer lists) without blowing the stack.
static norm_node_t *
initial_simplify(norm_ctx *ctx)
{
    int               cap   = 256;
    simplify_frame_t *stack = base_alloc(cap * sizeof(simplify_frame_t));
    int               sp    = 0;
    norm_node_t      *ret   = nullptr;

    tnode_t *start = ctx->cur;
    if (!start) {
        base_dealloc(stack);
        return nullptr;
    }

    // Skip single-child non-terminal chains
    while (start->num_kids == 1) {
        tnode_t *first = tnode_get_kid(start, 0);
        if (!first || first->tptr) {
            break;
        }
        start = first;
    }

    stack[sp++] = (simplify_frame_t){.tn = start, .result = nullptr, .kid_index = -1};

    while (sp > 0) {
        simplify_frame_t *f  = &stack[sp - 1];
        tnode_t          *tn = f->tn;

        // First visit
        if (f->kid_index == -1) {
            if (!tn) {
                ret = nullptr;
                sp--;
                goto got_child;
            }

            // Skip single-child non-terminal chains
            while (tn->num_kids == 1) {
                tnode_t *first = tnode_get_kid(tn, 0);
                if (!first || first->tptr) {
                    break;
                }
                tn    = first;
                f->tn = tn;
            }

            // Leaf
            if (!tn->num_kids) {
                if (!tn->id) {
                    ret = nullptr;
                }
                else {
                    assert(tn->tptr);
                    ret        = norm_alloc(tn->nt_id);
                    ret->value = extract(ctx->input, tn->tptr);
                    ret->leaf  = tn->tptr;
                }
                sp--;
                goto got_child;
            }

            f->result    = norm_alloc(tn->nt_id);
            f->kid_index = 0;
        }

        // All kids done?
        if (f->kid_index >= tn->num_kids) {
            norm_node_t *r = f->result;
            sp--;
            switch (r->num_kids) {
            case 0:
                assert(false);
            case 1:
                ret = r->kids->items[0];
                break;
            default:
                ret = r;
                break;
            }
            goto got_child;
        }

        // Push next kid
        {
            tnode_t *kid = tnode_get_kid(tn, f->kid_index++);
            if (sp >= cap) {
                cap *= 2;
                stack = base_realloc(stack, cap * sizeof(simplify_frame_t));
            }
            stack[sp++] = (simplify_frame_t){.tn = kid, .result = nullptr, .kid_index = -1};
            continue;
        }

got_child:
        if (sp > 0 && ret != nullptr) {
            add_sub(stack[sp - 1].result, ret);
        }
    }

    base_dealloc(stack);
    return ret;
}

// NTs that should be collapsed into their parent when they share the
// same nt_id (list flattening).
static bool
is_collapsible_list(nt_type_t nt)
{
    switch (nt) {
    case NT_specifier_qualifier_list:
    case NT_type_qualifier_list:
    case NT_attribute_specifier_sequence:
    case NT_attribute_list:
    case NT_parameter_list:
    case NT_enumerator_list:
    case NT_member_declaration_list:
    case NT_member_declarator_list:
        return true;
    default:
        return false;
    }
}

// Post-order collapse helper (applied after all children are visited).
static void
collapse_lists_postorder(norm_node_t *n)
{
    if (n->parent && is_collapsible_list(n->nt_id)
        && n->nt_id == n->parent->nt_id) {
        int pivot = -1;

        for (int i = 0; i < n->parent->num_kids; i++) {
            if (n->parent->kids->items[i] == n) {
                pivot = i;
                break;
            }
        }

        assert(pivot != -1);

        for (int i = pivot + 1; i < n->parent->num_kids; i++) {
            add_sub(n, n->parent->kids->items[i]);
        }

        n->parent->num_kids = pivot;

        for (int i = 0; i < n->num_kids; i++) {
            add_sub(n->parent, n->kids->items[i]);
        }
    }
}

typedef struct norm_frame_t {
    norm_node_t *node;
    int          child_idx;
} norm_frame_t;

static void
collapse_lists(norm_node_t *n)
{
    if (!n) {
        return;
    }

    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_frame_t *stk = base_alloc(cap * sizeof(norm_frame_t));
    if (!stk) {
        return;
    }

    // Post-order: push with child_idx starting at num_kids-1 (reverse order)
    stk[top++] = (norm_frame_t){.node = n, .child_idx = n->num_kids - 1};

    while (top > 0) {
        norm_frame_t *f = &stk[top - 1];

        if (f->child_idx >= 0) {
            norm_node_t *child = f->node->kids->items[f->child_idx];
            f->child_idx--;

            if (child && child->num_kids > 0) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_frame_t));
                    f   = &stk[top - 1];
                }
                stk[top++] = (norm_frame_t){.node = child, .child_idx = child->num_kids - 1};
            }
        }
        else {
            collapse_lists_postorder(f->node);
            top--;
        }
    }

    base_dealloc(stk);
}

static bool
is_attr_nt(nt_type_t nt)
{
    switch (nt) {
    case NT_attribute_specifier:
    case NT_attribute_specifier_sequence:
    case NT_attribute_list:
    case NT_attribute:
        return true;
    default:
        return false;
    }
}

static bool
is_qualifier_list_nt(nt_type_t nt)
{
    switch (nt) {
    case NT_type_qualifier_list:
    case NT_specifier_qualifier_list:
    case NT_pointer:
        return true;
    default:
        return false;
    }
}

static bool
is_collapsible_container(nt_type_t nt)
{
    switch (nt) {
    case NT_specifier_qualifier_list:
    case NT_type_qualifier_list:
    case NT_type_specifier_qualifier:
        return true;
    default:
        return false;
    }
}

// Post-order filter helper.
static void
filter_type_elements_postorder(norm_node_t *n)
{
    int write = 0;
    for (int i = 0; i < n->num_kids; i++) {
        norm_node_t *c = n->kids->items[i];
        if (is_attr_nt(c->nt_id)) {
            continue;
        }
        if (is_qualifier_list_nt(n->nt_id) && c->leaf && c->value && in_list(c->value, quals_to_drop)) {
            continue;
        }
        n->kids->items[write++] = c;
    }
    n->num_kids = write;
}

// Remove unwanted attributes and qualifiers from the tree (iterative post-order).
static void
filter_type_elements(norm_node_t *n)
{
    if (!n) {
        return;
    }

    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_frame_t *stk = base_alloc(cap * sizeof(norm_frame_t));
    if (!stk) {
        return;
    }

    stk[top++] = (norm_frame_t){.node = n, .child_idx = 0};

    while (top > 0) {
        norm_frame_t *f = &stk[top - 1];

        if (f->child_idx < f->node->num_kids) {
            norm_node_t *child = f->node->kids->items[f->child_idx];
            f->child_idx++;

            if (child && child->num_kids > 0) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_frame_t));
                    f   = &stk[top - 1];
                }
                stk[top++] = (norm_frame_t){.node = child, .child_idx = 0};
            }
        }
        else {
            filter_type_elements_postorder(f->node);
            top--;
        }
    }

    base_dealloc(stk);
}

// Check if a pointer node contains only stars (no qualifiers).
static bool
pointer_only_stars(norm_node_t *n)
{
    if (n->nt_id != NT_pointer) {
        return false;
    }
    for (int i = 0; i < n->num_kids; i++) {
        norm_node_t *c = n->kids->items[i];
        if (c->nt_id == NT_pointer) {
            continue;
        }
        if (c->leaf && c->value && !strcmp(c->value, "*")) {
            continue;
        }
        return false;
    }
    return true;
}

// Post-order collapse: after all children are collapsed, apply collapse logic.
static void
collapse_node_postorder(norm_node_t *n)
{
    int write = 0;
    for (int i = 0; i < n->num_kids; i++) {
        norm_node_t *c = n->kids->items[i];
        if (!c->leaf && c->num_kids == 0) {
            continue;
        }
        if (!c->leaf && c->num_kids == 1 && is_collapsible_container(c->nt_id)) {
            c         = c->kids->items[0];
            c->parent = n;
        }
        if (pointer_only_stars(c)) {
            for (int j = 0; j < c->num_kids; j++) {
                norm_node_t *pc         = c->kids->items[j];
                pc->parent              = n;
                n->kids->items[write++] = pc;
            }
            continue;
        }
        n->kids->items[write++] = c;
    }
    n->num_kids = write;
}

// Collapse empty nodes, single-child containers, and pointer-only-stars (iterative).
typedef struct collapse_frame_t {
    norm_node_t *node;
    int          child_idx;
} collapse_frame_t;

static norm_node_t *
collapse_filtered_tree(norm_node_t *n)
{
    if (!n) {
        return nullptr;
    }

    int               cap = NCC_CAP_LARGE;
    int               top = 0;
    collapse_frame_t *stk = base_alloc(cap * sizeof(collapse_frame_t));
    if (!stk) {
        return n;
    }

    stk[top++] = (collapse_frame_t){.node = n, .child_idx = 0};

    while (top > 0) {
        collapse_frame_t *f = &stk[top - 1];

        if (f->child_idx < f->node->num_kids) {
            norm_node_t *child = f->node->kids->items[f->child_idx];
            f->child_idx++;

            if (!child) {
                continue;
            }

            // If child has children, push it for traversal
            if (child->num_kids > 0) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(collapse_frame_t));
                    f   = &stk[top - 1];
                }
                stk[top++] = (collapse_frame_t){.node = child, .child_idx = 0};
            }
        }
        else {
            // All children visited — apply post-order collapse logic
            collapse_node_postorder(f->node);
            top--;
        }
    }

    base_dealloc(stk);

    // Final collapse of root if single-child container
    if (!n->leaf && n->num_kids == 1 && is_collapsible_container(n->nt_id)) {
        norm_node_t *child = n->kids->items[0];
        child->parent      = n->parent;
        return child;
    }
    return n;
}

static inline bool
is_qual_leaf(const norm_node_t *n)
{
    if (!n->leaf) {
        return false;
    }
    switch (n->leaf->type) {
    case TT_ID:
    case TT_KEYWORD:
        return true;
    default:
        return false;
    }
}

static int
cmp_attr_spec(const norm_node_t **left, const norm_node_t **right)
{
    const norm_node_t *kleft  = (*left)->kids->items[2];
    const norm_node_t *kright = (*right)->kids->items[2];

    if (!kleft || !kright) {
        return 0;
    }

    if (!kleft->leaf || !kright->leaf) {
        return 0;
    }

    return strcmp(kleft->value, kright->value);
}

static int
cmp_qualifier(const norm_node_t **left, const norm_node_t **right)
{
    const norm_node_t *l = *left;
    const norm_node_t *r = *right;

    if (!l || !r || !is_qual_leaf(l) || !is_qual_leaf(r)) {
        return 0;
    }

    if (l->leaf->type != r->leaf->type) {
        return 0;
    }

    return strcmp(l->value, r->value);
}

static int
cmp_alist(const norm_node_t **left, const norm_node_t **right)
{
    const norm_node_t *l = *left;
    const norm_node_t *r = *right;

    if (!l || !r) {
        return 0;
    }

    // Push commas to the back, not the front:
    if (l->value[0] == ',') {
        return 1;
    }
    if (r->value[0] == ',') {
        return -1;
    }

    return strcmp(l->value, r->value);
}

typedef int (*cmp_fn)(const void *, const void *);

static void
normalize_lists(norm_node_t *root)
{
    if (!root) {
        return;
    }

    // Pre-order iterative: sort at each node, then push children.
    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_node_t **stk = base_alloc(cap * sizeof(norm_node_t *));
    if (!stk) {
        return;
    }

    stk[top++] = root;

    while (top > 0) {
        norm_node_t *n    = stk[--top];
        bool         trim = false;
        cmp_fn       cmp  = nullptr;

        if (n->nt_id == NT_attribute_specifier_sequence) {
            cmp = (cmp_fn)cmp_attr_spec;
        }
        if (n->nt_id == NT_type_qualifier_list) {
            cmp = (cmp_fn)cmp_qualifier;
        }
        if (n->nt_id == NT_specifier_qualifier_list) {
            cmp = (cmp_fn)cmp_qualifier;
        }
        if (n->nt_id == NT_attribute_list) {
            cmp  = (cmp_fn)cmp_alist;
            trim = true;
        }

        if (cmp) {
            qsort(&n->kids->items[0],
                  n->num_kids,
                  sizeof(n->kids->items[0]),
                  cmp);

            if (trim) {
                while (n->num_kids) {
                    norm_node_t *sub = n->kids->items[n->num_kids - 1];
                    if (sub->value && !strcmp(sub->value, ",")) {
                        n->num_kids--;
                    }
                    else {
                        break;
                    }
                }
            }
        }

        for (int i = n->num_kids - 1; i >= 0; i--) {
            norm_node_t *kid = n->kids->items[i];
            if (kid) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_node_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
}

static const char *
nt_name_for_debug(nt_type_t nt)
{
    for (unsigned i = 0; i < NT_LOOKUP_SIZE; i++) {
        if (nt_lookup_table[i].id == nt) {
            return nt_lookup_table[i].name;
        }
    }
    return "(unknown)";
}

static void
print_normalized_type_tree(norm_node_t *n, int level)
{
    for (int i = 0; i < level; i++) {
        fprintf(stderr, " ");
    }
    if (!n) {
        return;
    }
    if (n->leaf) {
        fprintf(stderr, "◆ %s\n", n->value);
    }
    else {
        fprintf(stderr, "◆ %s\n", nt_name_for_debug(n->nt_id));
    }
    for (int i = 0; i < n->num_kids; i++) {
        print_normalized_type_tree(n->kids->items[i], level + 2);
    }
}

#define MAX_PREFIX 6

static char *
encode(int id, char *passed_value, int num_kids)
{
    int len = MAX_PREFIX + 1; // 1 for the null
    if (passed_value) {
        len += strlen(passed_value);
    }

    char *result = base_calloc(len, sizeof(char));

    if (passed_value) {
        snprintf(result, len, "_%d%s", id, passed_value);
    }
    else {
        if (num_kids > 0) {
            snprintf(result, len, "__%02d%d", id, num_kids);
        }
        else {
            snprintf(result, len, "_%02d", id);
        }
    }

    return result;
}

static char *
encode_non_nt(norm_node_t *n)
{
    assert(n->leaf);
    char *base         = n->value;
    int   code         = -1;
    char *passed_value = nullptr;

    // We can end up with some meaningless punctuation (given
    // redundency in the tree rep). For instance, commas and semicolons,
    // and some parens / brackets but not others.
    //
    // But, by this point, if we haven't trimmed it out, we're going
    // to encode it. Note that we do this using the first character
    // only. This isn't an issue, even though the grammar accepts both
    // ':' and "::" as separate operators, because there are not cases
    // where it can be ambiguous.
    //
    // Note also that we explicitly include assignments in an
    // enumeration. This maybe isn't the best idea, but it's fine for
    // now.

    switch (base[0]) {
    case '(':
        code = PUNCT_LPAREN;
        break;
    case ')':
        code = PUNCT_RPAREN;
        break;
    case '[':
        code = PUNCT_LBRACKET;
        break;
    case ']':
        code = PUNCT_RBRACKET;
        break;
    case '{':
        code = PUNCT_LBRACE;
        break;
    case '}':
        code = PUNCT_RBRACE;
        break;
    case '*':
        code = PUNCT_POINTER;
        break;
    case ':':
        code = PUNCT_COLON;
        break;
    case '.':
        code = PUNCT_ELIPSIS;
        break;
    case ',':
        code = PUNCT_COMMA;
        break;
    case ';':
        code = PUNCT_SEMI;
        break;
    case '=':
        code = PUNCT_EQ;
        break;
    default:
        if (isalpha(base[0]) || base[0] == '_') {
            code         = ID_OR_KEYWORD_CODE;
            passed_value = base;
            break;
        }
        else {
            assert(false); // We missed something, or otherwise have a bug.
        }
    }

    return encode(code, passed_value, -1);
}

static inline char *
encode_one_node(norm_node_t *n)
{
    // Leaf nodes (terminals) must be encoded with their actual value,
    // even if they have a recognized nt_id (e.g., NT_typedef_name).
    // Without this check, all typedef names hash to the same encoding.
    if (n->leaf) {
        return encode_non_nt(n);
    }

    int enc_id = nt_encoding_id(n->nt_id);

    if (enc_id >= 0) {
        return encode(enc_id, nullptr, n->num_kids);
    }

    return encode_non_nt(n);
}

static int
cache_encodings(norm_node_t *root, int so_far)
{
    if (!root) {
        return so_far;
    }

    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_node_t **stk = base_alloc(cap * sizeof(norm_node_t *));
    if (!stk) {
        return so_far;
    }

    stk[top++] = root;

    while (top > 0) {
        norm_node_t *n = stk[--top];
        n->_private    = encode_one_node(n);
        so_far += strlen(n->_private);

        for (int i = n->num_kids - 1; i >= 0; i--) {
            norm_node_t *kid = n->kids->items[i];
            if (kid) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_node_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
    return so_far;
}

static char *
populate_id(norm_node_t *root, char *p)
{
    if (!root) {
        return p;
    }

    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_node_t **stk = base_alloc(cap * sizeof(norm_node_t *));
    if (!stk) {
        return p;
    }

    stk[top++] = root;

    while (top > 0) {
        norm_node_t *n = stk[--top];
        int l = strlen(n->_private);
        memcpy(p, n->_private, l);
        p += l;

        for (int i = n->num_kids - 1; i >= 0; i--) {
            norm_node_t *kid = n->kids->items[i];
            if (kid) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_node_t *));
                }
                stk[top++] = kid;
            }
        }
    }

    base_dealloc(stk);
    return p;
}

norm_node_t *
normalize_tokens_to_type_tree(ncc_buf_t *input, tnode_t *troot)
{
    norm_ctx ctx = {
        .input = input,
        .cur   = troot,
        .root  = nullptr,
    };

    ctx.root = initial_simplify(&ctx);
    collapse_lists(ctx.root);
    filter_type_elements(ctx.root);
    ctx.root = collapse_filtered_tree(ctx.root);
    normalize_lists(ctx.root);

    return ctx.root;
}

static inline char *
encoding_long_from_normalized_type_tree(norm_node_t *node)
{
    // We're going to take the "easy" way out; for any node still in
    // the tree, we are going to represent it, instead of picking how
    // to skip. But, we don't need our ids to get too insanely long,
    // so for non-terminals we are going to map them to unique
    // integers. A somewhat future-proof way to do this is to number
    // the toplevel non-terminals, whether they currently can appear
    // in the graph or not.

    int   outlen = cache_encodings(node, 1); // 1 for the null.
    char *id     = base_calloc(outlen, sizeof(char));
    populate_id(node, id);

    return id;
}

static char *
map_one(int bits, char *p)
{
    assert(bits < 64 && bits >= -2);

    int c = b64_map[bits];

    if (c > 0) {
        *p++ = c;
    }
    else {
        *p++ = '_';
        *p++ = 'c' + c;
    }

    return p;
}

char *
encoding_from_normalized_type_tree(norm_node_t *node)
{
    char *long_str = encoding_long_from_normalized_type_tree(node);

    ncc_sha256_digest_t digest;

    ncc_sha256_hash(long_str, strlen(long_str), digest);

    uint8_t *dbytes = (uint8_t *)digest;

    // Tons of extra space in case everything hashes to 63 or 64.
    // We don't bother with the last 2 bytes either.
    char *res = base_calloc(96, sizeof(char));
    char *p   = res;
    *p++      = '_';
    *p++      = '_';

    for (int i = 0; i < 30;) {
        int c = dbytes[i++];
        int d = dbytes[i++];
        int e = dbytes[i++];
        int tmp;

        p   = map_one(c >> 2, p);
        tmp = (c & 0x3) << 4;
        tmp |= (d >> 4);
        p   = map_one(tmp, p);
        tmp = (d & 0x0f) << 2;
        tmp |= e >> 6;
        p = map_one(tmp, p);
        p = map_one(e & 0x3f, p);
    }
    *p = 0;

    return res;
}

// Emit one leaf node into the type string buffer.
static char *
emit_leaf_to_type_string(norm_node_t *n, char *start, char *p)
{
    norm_node_t *parent = n->parent;
    bool         idish  = n->leaf->type == TT_KEYWORD || n->leaf->type == TT_ID;

    if (!idish && p != start) {
        switch (n->value[0]) {
        case '(':
        case ',':
        case ')':
        case ']':
        case '}':
        case ';':
            if (p[-1] == ' ') {
                p--;
            }
            break;
        default:
            __builtin_unreachable();
        }
    }

    int vlen = strlen(n->value);
    memcpy(p, n->value, vlen);
    p += vlen;

    switch (n->value[0]) {
    case ',':
    case ':':
    case ';':
        *p++ = ' ';
        break;
    default:
        __builtin_unreachable();
    }

    if (parent && parent->nt_id == NT_attribute_list) {
        if (parent->kids->items[parent->num_kids - 1] != n) {
            *p++ = ',';
            *p++ = ' ';
        }
    }

    if (idish) {
        if (p != start && p[-1] != ' ') {
            *p++ = ' ';
        }
    }

    return p;
}

// Iterative pre-order type string builder.
static char *
node_to_type_string(norm_node_t *root, char *start, char *p)
{
    if (!root) {
        return p;
    }

    int           cap = NCC_CAP_LARGE;
    int           top = 0;
    norm_node_t **stk = base_alloc(cap * sizeof(norm_node_t *));
    if (!stk) {
        return p;
    }

    stk[top++] = root;

    while (top > 0) {
        norm_node_t *n = stk[--top];
        if (!n) {
            continue;
        }

        if (n->leaf) {
            p = emit_leaf_to_type_string(n, start, p);
        }
        else {
            for (int i = n->num_kids - 1; i >= 0; i--) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_node_t *));
                }
                stk[top++] = n->kids->items[i];
            }
        }
    }

    base_dealloc(stk);
    return p;
}

static int
approximate_output_len(norm_node_t *root)
{
    if (!root) {
        return 0;
    }

    int           result = 0;
    int           cap    = NCC_CAP_LARGE;
    int           top    = 0;
    norm_node_t **stk    = base_alloc(cap * sizeof(norm_node_t *));
    if (!stk) {
        return 0;
    }

    stk[top++] = root;

    while (top > 0) {
        norm_node_t *n = stk[--top];
        if (n->leaf) {
            result += strlen(n->value) * 2;
        }
        else {
            for (int i = n->num_kids - 1; i >= 0; i--) {
                if (top >= cap) {
                    cap *= 2;
                    stk = base_realloc(stk, cap * sizeof(norm_node_t *));
                }
                stk[top++] = n->kids->items[i];
            }
        }
    }

    base_dealloc(stk);
    return result;
}

char *
normalized_type_tree_to_string(norm_node_t *n)
{
    int sz = approximate_output_len(n);

    if (!sz) {
        return "";
    }

    char *res = base_calloc(sz, sizeof(char));
    char *end = node_to_type_string(n, res, res);

    if (end != res && end[-1] == ' ') {
        end[-1] = 0;
    }

    return res;
}

// Common C standard library typedef names to pre-register
static const char *standard_typedefs[] = {
    // stddef.h
    "size_t",
    "ptrdiff_t",
    "max_align_t",
    "nullptr_t",
    // stdint.h
    "int8_t",
    "int16_t",
    "int32_t",
    "int64_t",
    "uint8_t",
    "uint16_t",
    "uint32_t",
    "uint64_t",
    "intptr_t",
    "uintptr_t",
    "intmax_t",
    "uintmax_t",
    // wchar.h
    "wchar_t",
    "wint_t",
    // time.h
    "time_t",
    "clock_t",
    // stdio.h
    "FILE",
    // signal.h
    "sig_atomic_t",
    nullptr,
};

/**
 * @brief Initialize a symbol table with common C standard library typedefs.
 * @param st Symbol table to populate
 */
void
init_standard_typedefs(symtab_t *st)
{
    for (const char **name = standard_typedefs; *name; name++) {
        st_add_typedef(st, (char *)*name, nullptr, nullptr);
    }
}

norm_node_t *
type_to_normalized_type_tree_st(char *type_as_string, symtab_t *ext_st)
{
    int        len = strlen(type_as_string);
    ncc_buf_t *b   = (ncc_buf_t *)base_calloc(1, len + sizeof(ncc_buf_t) + 1);
    b->len         = len;
    memcpy(b->data, type_as_string, len);

    lex_t state;
    lex_init(&state, b, nullptr);
    lex(&state);

    // Use a symbol table pre-populated with standard typedefs.
    // If an external symbol table is provided, use it directly
    // (it should already contain standard typedefs plus user typedefs).
    symtab_t  local_st;
    symtab_t *st;

    if (ext_st) {
        st = ext_st;
    }
    else {
        st_init(&local_st);
        init_standard_typedefs(&local_st);
        st = &local_st;
    }

    int      pos   = 0;
    tnode_t *parse = parse_type_expression_st(&state, &pos, st);
    int      end;

    if (pos < state.num_toks) {
        end = state.toks[pos].offset;
    }
    else {
        end = len;
    }

    if (end != len) {
        fprintf(stderr, "[typeid-debug] parse FAILED for '%s': num_toks=%d pos=%d end=%d len=%d parse=%p\n", type_as_string, state.num_toks, pos, end, len, (void *)parse);
        for (int i = 0; i < state.num_toks; i++) {
            fprintf(stderr, "  tok[%d]: type=%d offset=%d len=%d text='%.*s'\n", i, state.toks[i].type, state.toks[i].offset, state.toks[i].len, state.toks[i].len, b->data + state.toks[i].offset);
        }
        if (!ext_st) {
            fprintf(stderr, "  st_is_typedef(st, \"%s\") = %d\n", type_as_string, st_is_typedef(st, type_as_string));
        }
        return nullptr;
    }

    return normalize_tokens_to_type_tree(b, parse);
}

norm_node_t *
type_to_normalized_type_tree(char *type_as_string)
{
    return type_to_normalized_type_tree_st(type_as_string, nullptr);
}

// ============================================================================
// Token-level normalization (matches pwz_type_normalize.c exactly)
//
// Both the bootstrap and main NCC must produce identical normalized C
// strings for the same input type, so they hash to the same identifier.
// ============================================================================

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} tn_strbuf_t;

static void
tn_strbuf_init(tn_strbuf_t *sb)
{
    sb->cap    = 256;
    sb->buf    = base_alloc(sb->cap);
    sb->len    = 0;
    sb->buf[0] = '\0';
}

static void
tn_strbuf_append(tn_strbuf_t *sb, const char *s, size_t slen)
{
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        sb->buf = base_realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void
tn_strbuf_emit_token(tn_strbuf_t *sb, const char *text)
{
    if (!text || !*text) {
        return;
    }
    // Add space between identifier-like tokens.
    if (sb->len > 0) {
        char last        = sb->buf[sb->len - 1];
        char first       = text[0];
        bool last_alnum  = isalnum((unsigned char)last) || last == '_';
        bool first_alnum = isalnum((unsigned char)first) || first == '_';
        if (last_alnum && first_alnum) {
            tn_strbuf_append(sb, " ", 1);
        }
    }
    tn_strbuf_append(sb, text, strlen(text));
}

static bool
tn_is_qualifier(const char *text)
{
    return strcmp(text, "const") == 0 || strcmp(text, "volatile") == 0 || strcmp(text, "restrict") == 0 || strcmp(text, "_Atomic") == 0;
}

static bool
tn_is_dropped_qualifier(const char *text)
{
    return strcmp(text, "restrict") == 0 || strcmp(text, "_Atomic") == 0;
}

static int
tn_cmp_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

// Normalize a type string using token-level approach.
// This produces the same canonical C string as pwz_type_normalize.c.
static char *
normalize_type_tokens(char *type_as_string)
{
    int        len = strlen(type_as_string);
    ncc_buf_t *b   = (ncc_buf_t *)base_calloc(1, len + sizeof(ncc_buf_t) + 1);
    b->len         = len;
    memcpy(b->data, type_as_string, len);

    lex_t state;
    lex_init(&state, b, nullptr);
    lex(&state);

    // Build an array of token text strings, skipping whitespace and comments.
    int          ntoks = 0;
    const char **texts = base_alloc(state.num_toks * sizeof(const char *));

    for (int i = 0; i < state.num_toks; i++) {
        tok_t *t = &state.toks[i];
        if (t->type == TT_WS || t->type == TT_COMMENT || t->type == TT_ERR) {
            continue;
        }
        texts[ntoks++] = extract(b, t);
    }

    // Qualifier accumulator.
    const char **quals  = base_alloc(ntoks * sizeof(const char *));
    int          nquals = 0;
    tn_strbuf_t  out;
    tn_strbuf_init(&out);

    int i = 0;
    while (i < ntoks) {
        const char *text = texts[i];
        if (!text) {
            i++;
            continue;
        }

        // Skip attribute blocks: [[ ... ]]
        if (strcmp(text, "[") == 0 && i + 1 < ntoks && texts[i + 1] && strcmp(texts[i + 1], "[") == 0) {
            int depth = 0;
            while (i < ntoks) {
                const char *tt = texts[i];
                if (tt && strcmp(tt, "[") == 0) {
                    depth++;
                }
                else if (tt && strcmp(tt, "]") == 0) {
                    depth--;
                    if (depth <= 0) {
                        i++;
                        break;
                    }
                }
                i++;
            }
            continue;
        }

        // Skip __attribute__((...)) style attributes.
        if (strcmp(text, "__attribute__") == 0 || strcmp(text, "__attribute") == 0) {
            i++;
            int depth = 0;
            while (i < ntoks) {
                const char *tt = texts[i];
                if (tt && strcmp(tt, "(") == 0) {
                    depth++;
                }
                else if (tt && strcmp(tt, ")") == 0) {
                    depth--;
                    if (depth <= 0) {
                        i++;
                        break;
                    }
                }
                i++;
            }
            continue;
        }

        if (tn_is_qualifier(text)) {
            if (!tn_is_dropped_qualifier(text)) {
                quals[nquals++] = text;
            }
            i++;
            continue;
        }

        // Non-qualifier token: flush sorted qualifiers first.
        if (nquals > 0) {
            qsort(quals, nquals, sizeof(const char *), tn_cmp_strings);
            for (int q = 0; q < nquals; q++) {
                tn_strbuf_emit_token(&out, quals[q]);
            }
            nquals = 0;
        }

        tn_strbuf_emit_token(&out, text);
        i++;
    }

    // Flush trailing qualifiers.
    if (nquals > 0) {
        qsort(quals, nquals, sizeof(const char *), tn_cmp_strings);
        for (int q = 0; q < nquals; q++) {
            tn_strbuf_emit_token(&out, quals[q]);
        }
    }

    // Clean up token strings.
    for (int j = 0; j < ntoks; j++) {
        base_dealloc((void *)texts[j]);
    }
    base_dealloc(texts);
    base_dealloc(quals);

    char *result = out.buf;
    return result;
}

static char *
hash_normalized_string(const char *normalized_str)
{
    ncc_sha256_digest_t digest;
    ncc_sha256_hash((void *)normalized_str, strlen(normalized_str), digest);

    uint8_t *dbytes = (uint8_t *)digest;

    char *res = base_calloc(96, sizeof(char));
    char *p   = res;
    *p++      = '_';
    *p++      = '_';

    for (int i = 0; i < 30;) {
        int c = dbytes[i++];
        int d = dbytes[i++];
        int e = dbytes[i++];
        int tmp;

        p   = map_one(c >> 2, p);
        tmp = (c & 0x3) << 4;
        tmp |= (d >> 4);
        p   = map_one(tmp, p);
        tmp = (d & 0x0f) << 2;
        tmp |= e >> 6;
        p = map_one(tmp, p);
        p = map_one(e & 0x3f, p);
    }
    *p = 0;

    return res;
}

char *
get_munged_identifier_st(char *type_as_string, symtab_t *st)
{
    (void)st;
    char *normalized = normalize_type_tokens(type_as_string);
    char *result     = hash_normalized_string(normalized);
    base_dealloc(normalized);
    return result;
}

char *
get_munged_identifier(char *type_as_string)
{
    return get_munged_identifier_st(type_as_string, nullptr);
}

uint64_t
get_type_hash_u64(char *type_as_string)
{
    char *normalized = normalize_type_tokens(type_as_string);

    ncc_sha256_digest_t digest;
    ncc_sha256_hash((void *)normalized, strlen(normalized), digest);
    base_dealloc(normalized);

    // Treat first 8 bytes of digest as a big-endian uint64_t.
    uint8_t *b = (uint8_t *)digest;
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | b[i];
    }
    return h;
}

char *
normalize_type(char *type_as_string)
{
    norm_node_t *n = type_to_normalized_type_tree(type_as_string);
    return normalized_type_tree_to_string(n);
}

static char **
load_tests(char *file_name, int *num_tests)
{
    ncc_buf_t *contents = ncc_buf_read_file_by_name(file_name);

    if (!contents || !contents->len) {
        printf("Could not read file: %s\n", file_name);
        exit(-1);
    }

    int   max_tests = 1;
    char *end       = contents->data + contents->len;
    char *p         = contents->data;

    while (p < end) {
        if (*p++ == '\n') {
            max_tests++;
        }
    }

    char **results = base_calloc(max_tests + 1, sizeof(char *));
    int    found   = 0;

    p = contents->data;

    while (p < end) {
        // Trim.
        while (p < end && (*p == '\n' || *p == ' ')) {
            p++;
        }

        char *start = p;
        while (p < end && *p != '\n') {
            p++;
        }
        int l = p - start;

        char *test = base_calloc(l + 1, sizeof(char));
        memcpy(test, start, l);
        char *trim_ptr = test + l;

        // Now trim spaces from the end.
        while (trim_ptr-- != test) {
            if (*trim_ptr == ' ') {
                *trim_ptr = 0;
                l--;
            }
            else {
                break;
            }
        }

        if (l) {
            results[found++] = test;
        }
    }

    *num_tests = found;

    return results;
}

void
type_parse_test(char *file_name, bool debug)
{
    int    num_tests;
    int    fails = 0;
    char **tests = load_tests(file_name, &num_tests);

    for (int i = 0; i < num_tests; i++) {
        printf("\nRunning test: %02d:  %s\n", i + 1, tests[i]);
        norm_node_t *root = type_to_normalized_type_tree(tests[i]);

        if (!root) {
            printf("----  [FAIL]  ----\n");
            fails++;
            // show_test_failure(tests[i]);
            continue;
        }

        printf("++++ SUCCESS! ++++\n");

        if (debug) {
            printf("Normalized tree:\n");
            print_normalized_type_tree(root, 0);
            printf("Reconstructed type from tree: %s\n",
                   normalized_type_tree_to_string(root));
            printf("Pre-hash encoding: %s\n",
                   encoding_long_from_normalized_type_tree(root));
        }

        printf("Encoded ID: %s\n", encoding_from_normalized_type_tree(root));
    }

    printf("%d fails out of %d cases.\n", fails, num_tests);
}
