#include "text/regex/parse.h"
#include "text/regex/node.h"
#include "text/regex/charset.h"
#include "core/alloc.h"
#include "core/string.h"
#include "text/unicode/encoding.h"
#include "text/unicode/casemap.h"
#include "text/unicode/properties.h"

#include <limits.h>

// ============================================================================
// Parser state
// ============================================================================

typedef struct {
    n00b_regex_builder_t *builder;
    n00b_regex_solver_t  *solver;
    const char           *src;
    uint32_t              len;
    uint32_t              pos;
    bool                  case_insensitive;
    bool                  multiline;
    bool                  dot_all;
    n00b_regex_parse_error_t error;
} parse_ctx_t;

static inline bool
at_end(parse_ctx_t *p)
{
    return p->pos >= p->len;
}

static inline n00b_codepoint_t
peek_cp(parse_ctx_t *p)
{
    if (at_end(p)) return 0;
    uint32_t saved = p->pos;
    int32_t  cp    = n00b_unicode_utf8_decode(p->src, p->len, &p->pos);
    p->pos         = saved;
    return (cp < 0) ? 0 : (n00b_codepoint_t)cp;
}

static inline n00b_codepoint_t
next_cp(parse_ctx_t *p)
{
    if (at_end(p)) return 0;
    int32_t cp = n00b_unicode_utf8_decode(p->src, p->len, &p->pos);
    return (cp < 0) ? 0 : (n00b_codepoint_t)cp;
}

static inline bool
match_char(parse_ctx_t *p, char c)
{
    if (at_end(p) || p->src[p->pos] != c) return false;
    p->pos++;
    return true;
}

// Forward declarations
static uint32_t parse_regex(parse_ctx_t *p);
static uint32_t parse_sequence(parse_ctx_t *p);
static uint32_t parse_quantified(parse_ctx_t *p);
static uint32_t parse_atom(parse_ctx_t *p);
static uint32_t parse_charclass(parse_ctx_t *p);
static n00b_regex_charset_t parse_escape_class(parse_ctx_t *p);

// ============================================================================
// Character set helpers
// ============================================================================

/** Build charset for a single codepoint, case-folding if needed. */
static n00b_regex_charset_t
charset_for_cp(parse_ctx_t *p, n00b_codepoint_t cp)
{
    n00b_regex_charset_t cs = n00b_regex_charset_single(p->solver, cp);
    if (p->case_insensitive) {
        n00b_codepoint_t folded = n00b_unicode_casefold_cp(cp);
        if (folded != cp) {
            cs = n00b_regex_charset_or(p->solver, cs,
                     n00b_regex_charset_single(p->solver, folded));
        }
        // Also add upper/lower variants
        n00b_codepoint_t upper = n00b_unicode_toupper_cp(cp);
        n00b_codepoint_t lower = n00b_unicode_tolower_cp(cp);
        if (upper != cp) {
            cs = n00b_regex_charset_or(p->solver, cs,
                     n00b_regex_charset_single(p->solver, upper));
        }
        if (lower != cp) {
            cs = n00b_regex_charset_or(p->solver, cs,
                     n00b_regex_charset_single(p->solver, lower));
        }
    }
    return cs;
}

/** \d = DecimalDigitNumber */
static n00b_regex_charset_t
charset_digit(parse_ctx_t *p)
{
    return n00b_regex_charset_from_gc(p->solver, N00B_UNICODE_GC_ND);
}

/** \w = Letter categories + ND + PC + underscore */
static n00b_regex_charset_t
charset_word(parse_ctx_t *p)
{
    n00b_regex_charset_t w = p->solver->false_id;
    n00b_unicode_gc_t word_cats[] = {
        N00B_UNICODE_GC_LU, N00B_UNICODE_GC_LL, N00B_UNICODE_GC_LT,
        N00B_UNICODE_GC_LM, N00B_UNICODE_GC_LO, N00B_UNICODE_GC_MN,
        N00B_UNICODE_GC_ND, N00B_UNICODE_GC_PC,
    };
    for (int i = 0; i < (int)(sizeof(word_cats) / sizeof(word_cats[0])); i++) {
        w = n00b_regex_charset_or(p->solver, w,
                n00b_regex_charset_from_gc(p->solver, word_cats[i]));
    }
    return w;
}

/** \s = whitespace */
static n00b_regex_charset_t
charset_space(parse_ctx_t *p)
{
    // Standard regex \s: space, tab, newline, CR, FF, VT
    n00b_regex_charset_t s = p->solver->false_id;
    n00b_codepoint_t     ws[] = {' ', '\t', '\n', '\r', '\f', '\v'};
    for (int i = 0; i < 6; i++) {
        s = n00b_regex_charset_or(p->solver, s,
                n00b_regex_charset_single(p->solver, ws[i]));
    }
    // Also add Unicode Zs category
    s = n00b_regex_charset_or(p->solver, s,
            n00b_regex_charset_from_gc(p->solver, N00B_UNICODE_GC_ZS));
    return s;
}

/** Dot: any codepoint, or any-except-newline if !dot_all. */
static n00b_regex_charset_t
charset_dot(parse_ctx_t *p)
{
    if (p->dot_all) {
        return p->solver->true_id;
    }
    // Everything except \n
    n00b_regex_charset_t nl = n00b_regex_charset_single(p->solver, '\n');
    return n00b_regex_charset_not(p->solver, nl);
}

// ============================================================================
// Parse Unicode property: \p{Name} / \P{Name}
// ============================================================================

static n00b_regex_charset_t
parse_unicode_property(parse_ctx_t *p, bool negated)
{
    if (!match_char(p, '{')) {
        p->error = N00B_RE_PARSE_BAD_PROPERTY;
        return p->solver->false_id;
    }

    // Read property name until '}'
    uint32_t start = p->pos;
    while (!at_end(p) && p->src[p->pos] != '}') {
        p->pos++;
    }
    uint32_t name_len = p->pos - start;
    if (!match_char(p, '}') || name_len == 0) {
        p->error = N00B_RE_PARSE_BAD_PROPERTY;
        return p->solver->false_id;
    }

    // Match general categories by their two-letter abbreviations.
    // We support the most common ones.
    char name[64] = {};
    uint32_t copy_len = name_len < 63 ? name_len : 63;
    memcpy(name, p->src + start, copy_len);

    typedef struct {
        const char       *abbrev;
        n00b_unicode_gc_t gc;
    } gc_entry_t;

    static const gc_entry_t gc_table[] = {
        {"Lu", N00B_UNICODE_GC_LU}, {"Ll", N00B_UNICODE_GC_LL},
        {"Lt", N00B_UNICODE_GC_LT}, {"Lm", N00B_UNICODE_GC_LM},
        {"Lo", N00B_UNICODE_GC_LO}, {"Mn", N00B_UNICODE_GC_MN},
        {"Mc", N00B_UNICODE_GC_MC}, {"Me", N00B_UNICODE_GC_ME},
        {"Nd", N00B_UNICODE_GC_ND}, {"Nl", N00B_UNICODE_GC_NL},
        {"No", N00B_UNICODE_GC_NO}, {"Pc", N00B_UNICODE_GC_PC},
        {"Pd", N00B_UNICODE_GC_PD}, {"Ps", N00B_UNICODE_GC_PS},
        {"Pe", N00B_UNICODE_GC_PE}, {"Pi", N00B_UNICODE_GC_PI},
        {"Pf", N00B_UNICODE_GC_PF}, {"Po", N00B_UNICODE_GC_PO},
        {"Sm", N00B_UNICODE_GC_SM}, {"Sc", N00B_UNICODE_GC_SC},
        {"Sk", N00B_UNICODE_GC_SK}, {"So", N00B_UNICODE_GC_SO},
        {"Zs", N00B_UNICODE_GC_ZS}, {"Zl", N00B_UNICODE_GC_ZL},
        {"Zp", N00B_UNICODE_GC_ZP}, {"Cc", N00B_UNICODE_GC_CC},
        {"Cf", N00B_UNICODE_GC_CF}, {"Cs", N00B_UNICODE_GC_CS},
        {"Co", N00B_UNICODE_GC_CO}, {"Cn", N00B_UNICODE_GC_CN},
    };

    // Try single-char super-categories (L, M, N, P, S, Z, C)
    n00b_regex_charset_t result = p->solver->false_id;
    bool                 matched = false;

    if (name_len == 1) {
        typedef struct { char ch; n00b_unicode_gc_t first; n00b_unicode_gc_t last; } super_t;
        static const super_t supers[] = {
            {'L', N00B_UNICODE_GC_LU, N00B_UNICODE_GC_LO},
            {'M', N00B_UNICODE_GC_MN, N00B_UNICODE_GC_ME},
            {'N', N00B_UNICODE_GC_ND, N00B_UNICODE_GC_NO},
            {'P', N00B_UNICODE_GC_PC, N00B_UNICODE_GC_PO},
            {'S', N00B_UNICODE_GC_SM, N00B_UNICODE_GC_SO},
            {'Z', N00B_UNICODE_GC_ZS, N00B_UNICODE_GC_ZP},
            {'C', N00B_UNICODE_GC_CC, N00B_UNICODE_GC_CN},
        };
        for (int i = 0; i < (int)(sizeof(supers)/sizeof(supers[0])); i++) {
            if (name[0] == supers[i].ch) {
                for (int g = supers[i].first; g <= supers[i].last; g++) {
                    result = n00b_regex_charset_or(p->solver, result,
                                 n00b_regex_charset_from_gc(p->solver, (n00b_unicode_gc_t)g));
                }
                matched = true;
                break;
            }
        }
    }

    if (!matched) {
        for (int i = 0; i < (int)(sizeof(gc_table)/sizeof(gc_table[0])); i++) {
            if (strcmp(name, gc_table[i].abbrev) == 0) {
                result  = n00b_regex_charset_from_gc(p->solver, gc_table[i].gc);
                matched = true;
                break;
            }
        }
    }

    if (!matched) {
        p->error = N00B_RE_PARSE_BAD_PROPERTY;
        return p->solver->false_id;
    }

    return negated ? n00b_regex_charset_not(p->solver, result) : result;
}

// ============================================================================
// Escape sequences
// ============================================================================

/** Parse an escape sequence and return the charset it represents. */
static n00b_regex_charset_t
parse_escape_class(parse_ctx_t *p)
{
    n00b_codepoint_t c = next_cp(p);
    switch (c) {
    case 'd': return charset_digit(p);
    case 'D': return n00b_regex_charset_not(p->solver, charset_digit(p));
    case 'w': return charset_word(p);
    case 'W': return n00b_regex_charset_not(p->solver, charset_word(p));
    case 's': return charset_space(p);
    case 'S': return n00b_regex_charset_not(p->solver, charset_space(p));
    case 'p': return parse_unicode_property(p, false);
    case 'P': return parse_unicode_property(p, true);
    case 'n': return charset_for_cp(p, '\n');
    case 'r': return charset_for_cp(p, '\r');
    case 't': return charset_for_cp(p, '\t');
    case 'f': return charset_for_cp(p, '\f');
    case 'v': return charset_for_cp(p, '\v');
    case 'a': return charset_for_cp(p, '\a');
    case 'x': {
        // \xHH
        uint32_t val = 0;
        for (int i = 0; i < 2 && !at_end(p); i++) {
            char ch = p->src[p->pos];
            if (ch >= '0' && ch <= '9') val = val * 16 + (ch - '0');
            else if (ch >= 'a' && ch <= 'f') val = val * 16 + 10 + (ch - 'a');
            else if (ch >= 'A' && ch <= 'F') val = val * 16 + 10 + (ch - 'A');
            else break;
            p->pos++;
        }
        return charset_for_cp(p, val);
    }
    case 'u': {
        // \uHHHH
        uint32_t val = 0;
        for (int i = 0; i < 4 && !at_end(p); i++) {
            char ch = p->src[p->pos];
            if (ch >= '0' && ch <= '9') val = val * 16 + (ch - '0');
            else if (ch >= 'a' && ch <= 'f') val = val * 16 + 10 + (ch - 'a');
            else if (ch >= 'A' && ch <= 'F') val = val * 16 + 10 + (ch - 'A');
            else break;
            p->pos++;
        }
        return charset_for_cp(p, val);
    }
    case 'U': {
        // \UHHHHHHHH
        uint32_t val = 0;
        for (int i = 0; i < 8 && !at_end(p); i++) {
            char ch = p->src[p->pos];
            if (ch >= '0' && ch <= '9') val = val * 16 + (ch - '0');
            else if (ch >= 'a' && ch <= 'f') val = val * 16 + 10 + (ch - 'a');
            else if (ch >= 'A' && ch <= 'F') val = val * 16 + 10 + (ch - 'A');
            else break;
            p->pos++;
        }
        return charset_for_cp(p, val);
    }
    default:
        // Literal escaped character (e.g., \\, \., \*, etc.)
        return charset_for_cp(p, c);
    }
}

// ============================================================================
// Character class [...]
// ============================================================================

static uint32_t
parse_charclass(parse_ctx_t *p)
{
    bool negated = match_char(p, '^');
    n00b_regex_charset_t result = p->solver->false_id;

    while (!at_end(p) && p->src[p->pos] != ']') {
        n00b_regex_charset_t elem;

        if (p->src[p->pos] == '\\') {
            p->pos++;
            elem = parse_escape_class(p);
        }
        else {
            n00b_codepoint_t c = next_cp(p);
            elem = charset_for_cp(p, c);

            // Check for range: c-d
            if (!at_end(p) && p->src[p->pos] == '-' && p->pos + 1 < p->len
                && p->src[p->pos + 1] != ']') {
                p->pos++; // skip '-'
                n00b_codepoint_t end;
                if (p->src[p->pos] == '\\') {
                    p->pos++;
                    // For ranges with escapes, just get the codepoint
                    end = next_cp(p);
                }
                else {
                    end = next_cp(p);
                }
                elem = n00b_regex_charset_range(p->solver, c, end);
                if (p->case_insensitive) {
                    // Expand each codepoint in the range for case insensitivity
                    for (n00b_codepoint_t cp = c; cp <= end; cp++) {
                        n00b_codepoint_t f = n00b_unicode_casefold_cp(cp);
                        if (f != cp) {
                            elem = n00b_regex_charset_or(p->solver, elem,
                                       n00b_regex_charset_single(p->solver, f));
                        }
                    }
                }
            }
        }

        result = n00b_regex_charset_or(p->solver, result, elem);

        if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;
    }

    if (!match_char(p, ']')) {
        p->error = N00B_RE_PARSE_BAD_CHARCLASS;
        return N00B_RE_ID_NOTHING;
    }

    if (negated) {
        result = n00b_regex_charset_not(p->solver, result);
    }

    return n00b_regex_mk_singleton(p->builder, result);
}

// ============================================================================
// Word boundary \b / \B
// ============================================================================

/**
 * Build \b or \B as lookaround intersections, matching resharp's approach.
 *
 * \b = (wordLeft & nonWordRight) | (nonWordLeft & wordRight)
 * \B = (wordLeft & wordRight) | (nonWordLeft & nonWordRight)
 *
 * where:
 *   wordLeft     = lookbehind(Or(BEGIN, \w))
 *   nonWordLeft  = lookbehind(Or(BEGIN, \W))
 *   wordRight    = lookahead(Or(END, \w))
 *   nonWordRight = lookahead(Or(END, \W))
 */
static uint32_t
parse_word_boundary(parse_ctx_t *p, bool is_boundary)
{
    n00b_regex_builder_t *b = p->builder;
    n00b_regex_charset_t word_cs    = charset_word(p);
    n00b_regex_charset_t nonword_cs = n00b_regex_charset_not(p->solver, word_cs);
    uint32_t word_node    = n00b_regex_mk_singleton(b, word_cs);
    uint32_t nonword_node = n00b_regex_mk_singleton(b, nonword_cs);

    // wordLeft = lookbehind(Or(BEGIN, \w))
    uint32_t wl_body = n00b_regex_mk_or2(b, N00B_RE_ID_BEGIN, word_node);
    uint32_t word_left = n00b_regex_mk_lookaround(b, wl_body, true, 0, nullptr, 0);

    // nonWordLeft = lookbehind(Or(BEGIN, \W))
    uint32_t nwl_body = n00b_regex_mk_or2(b, N00B_RE_ID_BEGIN, nonword_node);
    uint32_t nonword_left = n00b_regex_mk_lookaround(b, nwl_body, true, 0, nullptr, 0);

    // wordRight = lookahead(Or(END, \w))
    uint32_t wr_body = n00b_regex_mk_or2(b, N00B_RE_ID_END, word_node);
    uint32_t word_right = n00b_regex_mk_lookaround(b, wr_body, false, 0, nullptr, 0);

    // nonWordRight = lookahead(Or(END, \W))
    uint32_t nwr_body = n00b_regex_mk_or2(b, N00B_RE_ID_END, nonword_node);
    uint32_t nonword_right = n00b_regex_mk_lookaround(b, nwr_body, false, 0, nullptr, 0);

    if (is_boundary) {
        // \b = (wordLeft & nonWordRight) | (nonWordLeft & wordRight)
        uint32_t and1_children[] = { word_left, nonword_right };
        uint32_t and1 = n00b_regex_mk_and(b, and1_children, 2);
        uint32_t and2_children[] = { nonword_left, word_right };
        uint32_t and2 = n00b_regex_mk_and(b, and2_children, 2);
        return n00b_regex_mk_or2(b, and1, and2);
    }
    else {
        // \B = (wordLeft & wordRight) | (nonWordLeft & nonWordRight)
        uint32_t and1_children[] = { word_left, word_right };
        uint32_t and1 = n00b_regex_mk_and(b, and1_children, 2);
        uint32_t and2_children[] = { nonword_left, nonword_right };
        uint32_t and2 = n00b_regex_mk_and(b, and2_children, 2);
        return n00b_regex_mk_or2(b, and1, and2);
    }
}

// ============================================================================
// Atoms
// ============================================================================

static uint32_t
parse_group(parse_ctx_t *p)
{
    // Already consumed '('
    // Check for group modifiers
    if (match_char(p, '?')) {
        if (match_char(p, ':')) {
            // Non-capturing group
            uint32_t inner = parse_regex(p);
            if (!match_char(p, ')')) {
                p->error = N00B_RE_PARSE_UNBALANCED;
            }
            return inner;
        }
        else if (match_char(p, '=')) {
            // Positive lookahead
            uint32_t body = parse_regex(p);
            if (!match_char(p, ')')) {
                p->error = N00B_RE_PARSE_UNBALANCED;
            }
            // Wrap: (?=body) => lookaround(body_.*,  forward)
            uint32_t wrapped = n00b_regex_mk_concat(p->builder, body, N00B_RE_ID_DOTSTAR);
            return n00b_regex_mk_lookaround(p->builder, wrapped, false, 0, nullptr, 0);
        }
        else if (match_char(p, '!')) {
            // Negative lookahead
            uint32_t body = parse_regex(p);
            if (!match_char(p, ')')) {
                p->error = N00B_RE_PARSE_UNBALANCED;
            }
            uint32_t wrapped = n00b_regex_mk_concat(p->builder, body, N00B_RE_ID_DOTSTAR);
            auto neg_r = n00b_regex_mk_not(p->builder, wrapped);
            if (n00b_result_is_err(neg_r)) {
                p->error = (n00b_regex_parse_error_t)n00b_result_get_err(neg_r);
                return N00B_RE_ID_NOTHING;
            }
            return n00b_regex_mk_lookaround(p->builder, n00b_result_get(neg_r), false, 0, nullptr, 0);
        }
        else if (match_char(p, '<')) {
            if (match_char(p, '=')) {
                // Positive lookbehind
                uint32_t body = parse_regex(p);
                if (!match_char(p, ')')) {
                    p->error = N00B_RE_PARSE_UNBALANCED;
                }
                uint32_t wrapped = n00b_regex_mk_concat(p->builder, N00B_RE_ID_DOTSTAR, body);
                return n00b_regex_mk_lookaround(p->builder, wrapped, true, 0, nullptr, 0);
            }
            else if (match_char(p, '!')) {
                // Negative lookbehind
                uint32_t body = parse_regex(p);
                if (!match_char(p, ')')) {
                    p->error = N00B_RE_PARSE_UNBALANCED;
                }
                uint32_t wrapped = n00b_regex_mk_concat(p->builder, N00B_RE_ID_DOTSTAR, body);
                auto neg_r2 = n00b_regex_mk_not(p->builder, wrapped);
                if (n00b_result_is_err(neg_r2)) {
                    p->error = (n00b_regex_parse_error_t)n00b_result_get_err(neg_r2);
                    return N00B_RE_ID_NOTHING;
                }
                return n00b_regex_mk_lookaround(p->builder, n00b_result_get(neg_r2), true, 0, nullptr, 0);
            }
            else {
                p->error = N00B_RE_PARSE_BAD_ESCAPE;
                return N00B_RE_ID_NOTHING;
            }
        }
        else {
            p->error = N00B_RE_PARSE_BAD_ESCAPE;
            return N00B_RE_ID_NOTHING;
        }
    }
    else {
        // Plain group (treated as non-capturing — no capture groups)
        uint32_t inner = parse_regex(p);
        if (!match_char(p, ')')) {
            p->error = N00B_RE_PARSE_UNBALANCED;
        }
        return inner;
    }
}

static uint32_t
parse_atom(parse_ctx_t *p)
{
    if (at_end(p)) return N00B_RE_ID_EPSILON;

    n00b_codepoint_t c = peek_cp(p);

    switch (c) {
    case '|':
    case ')':
    case '&':
        return N00B_RE_ID_EPSILON; // End of sequence

    case '.':
        next_cp(p);
        return n00b_regex_mk_singleton(p->builder, charset_dot(p));

    case '_':
        next_cp(p);
        return N00B_RE_ID_ANY; // Universal wildcard

    case '^':
        next_cp(p);
        if (p->multiline) {
            // ^ as lookaround for begin-of-line
            uint32_t body = n00b_regex_mk_or2(p->builder,
                                N00B_RE_ID_BEGIN,
                                n00b_regex_mk_singleton(p->builder,
                                    n00b_regex_charset_single(p->solver, '\n')));
            return n00b_regex_mk_lookaround(p->builder, body, true, 0, nullptr, 0);
        }
        return N00B_RE_ID_BEGIN;

    case '$':
        next_cp(p);
        if (p->multiline) {
            uint32_t body = n00b_regex_mk_or2(p->builder,
                                N00B_RE_ID_END,
                                n00b_regex_mk_singleton(p->builder,
                                    n00b_regex_charset_single(p->solver, '\n')));
            return n00b_regex_mk_lookaround(p->builder, body, false, 0, nullptr, 0);
        }
        return N00B_RE_ID_END;

    case '[':
        next_cp(p);
        return parse_charclass(p);

    case '(':
        next_cp(p);
        return parse_group(p);

    case '~':
        next_cp(p);
        if (match_char(p, '(')) {
            // Complement: ~(regex)
            uint32_t inner = parse_regex(p);
            if (!match_char(p, ')')) {
                p->error = N00B_RE_PARSE_UNBALANCED;
            }
            auto not_r = n00b_regex_mk_not(p->builder, inner);
            if (n00b_result_is_err(not_r)) {
                p->error = (n00b_regex_parse_error_t)n00b_result_get_err(not_r);
                return N00B_RE_ID_NOTHING;
            }
            return n00b_result_get(not_r);
        }
        // Literal ~ if not followed by (
        return n00b_regex_mk_singleton(p->builder, charset_for_cp(p, '~'));

    case '\\':
        next_cp(p); // consume backslash
        {
            // Check for anchors and zero-width assertions
            n00b_codepoint_t esc = peek_cp(p);
            if (esc == 'A') {
                next_cp(p);
                return N00B_RE_ID_BEGIN;
            }
            if (esc == 'z') {
                next_cp(p);
                return N00B_RE_ID_END;
            }
            if (esc == 'b' || esc == 'B') {
                next_cp(p);
                return parse_word_boundary(p, esc == 'b');
            }
            // Otherwise it's a class or literal escape
            n00b_regex_charset_t cs = parse_escape_class(p);
            if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;
            return n00b_regex_mk_singleton(p->builder, cs);
        }

    default:
        next_cp(p);
        return n00b_regex_mk_singleton(p->builder, charset_for_cp(p, c));
    }
}

// ============================================================================
// Quantifiers
// ============================================================================

static int32_t
parse_int(parse_ctx_t *p)
{
    int32_t val = 0;
    while (!at_end(p) && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
        val = val * 10 + (p->src[p->pos] - '0');
        p->pos++;
    }
    return val;
}

static uint32_t
parse_quantified(parse_ctx_t *p)
{
    uint32_t node = parse_atom(p);
    if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;

    while (!at_end(p)) {
        n00b_codepoint_t c = peek_cp(p);
        int32_t lo, hi;

        switch (c) {
        case '*':
            next_cp(p);
            lo = 0; hi = INT32_MAX;
            break;
        case '+':
            next_cp(p);
            lo = 1; hi = INT32_MAX;
            break;
        case '?':
            next_cp(p);
            lo = 0; hi = 1;
            break;
        case '{':
            next_cp(p);
            lo = parse_int(p);
            if (match_char(p, ',')) {
                if (!at_end(p) && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') {
                    hi = parse_int(p);
                }
                else {
                    hi = INT32_MAX;
                }
            }
            else {
                hi = lo;
            }
            if (!match_char(p, '}')) {
                p->error = N00B_RE_PARSE_BAD_QUANTIFIER;
                return N00B_RE_ID_NOTHING;
            }
            break;
        default:
            return node;
        }

        // Lazy modifier '?' — we ignore it since we always do leftmost-longest
        if (!at_end(p) && peek_cp(p) == '?') {
            next_cp(p);
        }

        node = n00b_regex_mk_loop(p->builder, node, lo, hi);
    }

    return node;
}

// ============================================================================
// Sequence and alternation
// ============================================================================

static uint32_t
parse_sequence(parse_ctx_t *p)
{
    uint32_t result = N00B_RE_ID_EPSILON;

    while (!at_end(p)) {
        n00b_codepoint_t c = peek_cp(p);
        if (c == '|' || c == ')' || c == '&') break;

        uint32_t q = parse_quantified(p);
        if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;

        result = n00b_regex_mk_concat(p->builder, result, q);
    }

    return result;
}

static uint32_t
parse_regex(parse_ctx_t *p)
{
    uint32_t result = parse_sequence(p);
    if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;

    // Handle alternation '|'
    while (!at_end(p) && peek_cp(p) == '|') {
        next_cp(p); // consume '|'
        uint32_t alt = parse_sequence(p);
        if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;
        result = n00b_regex_mk_or2(p->builder, result, alt);
    }

    // Handle intersection '&' (extension operator)
    while (!at_end(p) && peek_cp(p) == '&') {
        next_cp(p); // consume '&'
        uint32_t alt = parse_sequence(p);
        if (p->error != N00B_RE_PARSE_OK) return N00B_RE_ID_NOTHING;
        uint32_t children[2] = {result, alt};
        result = n00b_regex_mk_and(p->builder, children, 2);
    }

    return result;
}

// ============================================================================
// Public API
// ============================================================================

n00b_result_t(uint32_t)
n00b_regex_parse(n00b_regex_builder_t *builder,
                 n00b_string_t        *pattern,
                 bool                  case_insensitive,
                 bool                  multiline,
                 bool                  dot_all)
{
    parse_ctx_t p = {
        .builder          = builder,
        .solver           = builder->solver,
        .src              = pattern->data,
        .len              = (uint32_t)pattern->u8_bytes,
        .pos              = 0,
        .case_insensitive = case_insensitive,
        .multiline        = multiline,
        .dot_all          = dot_all,
        .error            = N00B_RE_PARSE_OK,
    };

    uint32_t root = parse_regex(&p);

    if (p.error != N00B_RE_PARSE_OK) {
        return n00b_result_err(uint32_t, (n00b_err_t)p.error);
    }

    if (!at_end(&p)) {
        return n00b_result_err(uint32_t, (n00b_err_t)N00B_RE_PARSE_UNBALANCED);
    }

    return n00b_result_ok(uint32_t, root);
}
