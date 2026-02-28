#include "text/unicode/bidi.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "internal/text/unicode/tables.h"
#include <string.h>
#include <assert.h>

extern const uint32_t n00b_unicode_bidi_bracket_index[][2];
extern const uint32_t n00b_unicode_bidi_bracket_index_len;
extern const uint32_t n00b_unicode_bidi_bracket_data[];

extern const uint32_t n00b_unicode_bidi_mirror_index[][2];
extern const uint32_t n00b_unicode_bidi_mirror_index_len;
extern const uint32_t n00b_unicode_bidi_mirror_data[];

// ---------------------------------------------------------------------------
// Bidi paragraph structure
// ---------------------------------------------------------------------------

struct n00b_unicode_bidi_para_s {
    n00b_codepoint_t *cps;        // codepoints
    uint8_t          *types;      // resolved bidi types
    uint8_t          *levels;     // embedding levels
    uint32_t          len;        // number of codepoints
    uint8_t           para_level; // paragraph embedding level
};

// ---------------------------------------------------------------------------
// UAX #9 implementation
// ---------------------------------------------------------------------------

// P2/P3: Determine paragraph level
static uint8_t
determine_paragraph_level(const n00b_codepoint_t *cps [[maybe_unused]],
                          const uint8_t          *types,
                          uint32_t                len)
{
    // Find first strong character (L, R, AL)
    for (uint32_t i = 0; i < len; i++) {
        switch (types[i]) {
        case N00B_UNICODE_BIDI_L:
            return 0;
        case N00B_UNICODE_BIDI_R:
        case N00B_UNICODE_BIDI_AL:
            return 1;
        // Skip isolate pairs
        case N00B_UNICODE_BIDI_LRI:
        case N00B_UNICODE_BIDI_RLI:
        case N00B_UNICODE_BIDI_FSI: {
            // Skip to matching PDI
            int depth = 1;
            for (uint32_t j = i + 1; j < len && depth > 0; j++) {
                if (types[j] == N00B_UNICODE_BIDI_LRI || types[j] == N00B_UNICODE_BIDI_RLI
                    || types[j] == N00B_UNICODE_BIDI_FSI) {
                    depth++;
                }
                else if (types[j] == N00B_UNICODE_BIDI_PDI) {
                    depth--;
                }
            }
        } break;
        default:
            break;
        }
    }
    return 0; // Default LTR
}

// X1-X8: Resolve explicit embedding levels
static void
resolve_explicit(uint8_t *types, uint8_t *levels, uint32_t len, uint8_t para_level)
{
    uint8_t stack_level[125];
    uint8_t stack_override[125];
    bool    stack_isolate[125];
    int     sp                 = 0;
    uint8_t cur_level          = para_level;
    uint8_t cur_override       = N00B_UNICODE_BIDI_ON; // no override
    int     overflow_isolate   = 0;
    int     overflow_embedding = 0;
    int     valid_isolate      = 0;

    stack_level[0]    = para_level;
    stack_override[0] = N00B_UNICODE_BIDI_ON;
    stack_isolate[0]  = false;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t type = types[i];

        // X2-X5: RLE, LRE, RLO, LRO
        if (type == N00B_UNICODE_BIDI_RLE || type == N00B_UNICODE_BIDI_LRE
            || type == N00B_UNICODE_BIDI_RLO || type == N00B_UNICODE_BIDI_LRO) {
            uint8_t new_level;
            if (type == N00B_UNICODE_BIDI_RLE || type == N00B_UNICODE_BIDI_RLO) {
                new_level = (cur_level + 1) | 1; // next odd
            }
            else {
                new_level = (cur_level + 2) & ~1; // next even
            }

            if (new_level <= 125 && overflow_isolate == 0 && overflow_embedding == 0) {
                sp++;
                stack_level[sp]    = cur_level;
                stack_override[sp] = cur_override;
                stack_isolate[sp]  = false;
                cur_level          = new_level;
                cur_override       = (type == N00B_UNICODE_BIDI_RLO) ? N00B_UNICODE_BIDI_R
                                   : (type == N00B_UNICODE_BIDI_LRO) ? N00B_UNICODE_BIDI_L
                                                                     : N00B_UNICODE_BIDI_ON;
            }
            else if (overflow_isolate == 0) {
                overflow_embedding++;
            }

            levels[i] = cur_level;
            types[i]  = N00B_UNICODE_BIDI_BN; // X9
            continue;
        }

        // X5a-X5c: RLI, LRI, FSI
        if (type == N00B_UNICODE_BIDI_RLI || type == N00B_UNICODE_BIDI_LRI
            || type == N00B_UNICODE_BIDI_FSI) {
            if (type == N00B_UNICODE_BIDI_FSI) {
                // P2 on substring to matching PDI to determine direction
                // Find matching PDI
                int     fsi_depth = 1;
                uint8_t fsi_dir   = N00B_UNICODE_BIDI_LRI; // default to LRI if no strong found
                for (uint32_t j = i + 1; j < len && fsi_depth > 0; j++) {
                    uint8_t jt = types[j];
                    if (jt == N00B_UNICODE_BIDI_LRI || jt == N00B_UNICODE_BIDI_RLI
                        || jt == N00B_UNICODE_BIDI_FSI) {
                        fsi_depth++;
                    }
                    else if (jt == N00B_UNICODE_BIDI_PDI) {
                        fsi_depth--;
                    }
                    else if (fsi_depth == 1) {
                        // Only consider characters at depth 1
                        if (jt == N00B_UNICODE_BIDI_L) {
                            fsi_dir = N00B_UNICODE_BIDI_LRI;
                            break;
                        }
                        if (jt == N00B_UNICODE_BIDI_R || jt == N00B_UNICODE_BIDI_AL) {
                            fsi_dir = N00B_UNICODE_BIDI_RLI;
                            break;
                        }
                    }
                }
                type = fsi_dir;
            }

            levels[i] = cur_level;
            if (cur_override != N00B_UNICODE_BIDI_ON) {
                types[i] = cur_override;
            }

            uint8_t new_level;
            if (type == N00B_UNICODE_BIDI_RLI) {
                new_level = (cur_level + 1) | 1;
            }
            else {
                new_level = (cur_level + 2) & ~1;
            }

            if (new_level <= 125 && overflow_isolate == 0 && overflow_embedding == 0) {
                sp++;
                stack_level[sp]    = cur_level;
                stack_override[sp] = cur_override;
                stack_isolate[sp]  = true;
                valid_isolate++;
                cur_level    = new_level;
                cur_override = N00B_UNICODE_BIDI_ON;
            }
            else {
                overflow_isolate++;
            }
            continue;
        }

        // X6a: PDI
        if (type == N00B_UNICODE_BIDI_PDI) {
            if (overflow_isolate > 0) {
                overflow_isolate--;
            }
            else if (valid_isolate > 0) {
                overflow_embedding = 0;
                while (sp > 0 && !stack_isolate[sp])
                    sp--;
                if (sp > 0) {
                    cur_level    = stack_level[sp];
                    cur_override = stack_override[sp];
                    sp--;
                }
                valid_isolate--;
            }
            levels[i] = cur_level;
            if (cur_override != N00B_UNICODE_BIDI_ON) {
                types[i] = cur_override;
            }
            continue;
        }

        // X7: PDF
        if (type == N00B_UNICODE_BIDI_PDF) {
            if (overflow_isolate > 0) {
                // do nothing
            }
            else if (overflow_embedding > 0) {
                overflow_embedding--;
            }
            else if (sp > 0 && !stack_isolate[sp]) {
                cur_level    = stack_level[sp];
                cur_override = stack_override[sp];
                sp--;
            }
            levels[i] = cur_level;
            types[i]  = N00B_UNICODE_BIDI_BN;
            continue;
        }

        // X6: All other characters
        levels[i] = cur_level;
        if (cur_override != N00B_UNICODE_BIDI_ON) {
            types[i] = cur_override;
        }
    }
}

// W1-W7: Resolve weak types
static void
resolve_weak(uint8_t *types, const uint8_t *levels, uint32_t len)
{
    uint8_t prev_strong = N00B_UNICODE_BIDI_ON;

    for (uint32_t i = 0; i < len; i++) {
        if (types[i] == N00B_UNICODE_BIDI_BN)
            continue;

        uint8_t type = types[i];

        // W1: NSM -> type of preceding non-BN
        if (type == N00B_UNICODE_BIDI_NSM) {
            if (i > 0) {
                uint32_t j = i - 1;
                while (j > 0 && types[j] == N00B_UNICODE_BIDI_BN)
                    j--;
                uint8_t prev = types[j];
                if (prev == N00B_UNICODE_BIDI_LRI || prev == N00B_UNICODE_BIDI_RLI
                    || prev == N00B_UNICODE_BIDI_FSI || prev == N00B_UNICODE_BIDI_PDI) {
                    types[i] = N00B_UNICODE_BIDI_ON;
                }
                else {
                    types[i] = prev;
                }
            }
            else {
                types[i] = (levels[i] % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;
            }
            type = types[i];
        }

        // W2: EN after AL -> AN
        if (type == N00B_UNICODE_BIDI_EN && prev_strong == N00B_UNICODE_BIDI_AL) {
            types[i] = N00B_UNICODE_BIDI_AN;
            type     = N00B_UNICODE_BIDI_AN;
        }

        // W3: AL -> R
        if (type == N00B_UNICODE_BIDI_AL) {
            types[i] = N00B_UNICODE_BIDI_R;
            type     = N00B_UNICODE_BIDI_R;
        }

        // Track strong types
        if (type == N00B_UNICODE_BIDI_L || type == N00B_UNICODE_BIDI_R
            || type == N00B_UNICODE_BIDI_AL) {
            prev_strong = type;
        }

        // W4: ES between EN -> EN, CS between same -> same
        if (i >= 2 && (type == N00B_UNICODE_BIDI_ES || type == N00B_UNICODE_BIDI_CS)) {
            uint8_t  prev_type = types[i - 1];
            // Look ahead
            uint32_t next      = i + 1;
            while (next < len && types[next] == N00B_UNICODE_BIDI_BN)
                next++;
            if (next < len) {
                uint8_t next_type = types[next];
                if (type == N00B_UNICODE_BIDI_ES && prev_type == N00B_UNICODE_BIDI_EN
                    && next_type == N00B_UNICODE_BIDI_EN) {
                    types[i] = N00B_UNICODE_BIDI_EN;
                }
                else if (type == N00B_UNICODE_BIDI_CS) {
                    if ((prev_type == N00B_UNICODE_BIDI_EN && next_type == N00B_UNICODE_BIDI_EN)
                        || (prev_type == N00B_UNICODE_BIDI_AN
                            && next_type == N00B_UNICODE_BIDI_AN)) {
                        types[i] = prev_type;
                    }
                }
            }
        }

        // W5: ET adjacent to EN -> EN
        if (type == N00B_UNICODE_BIDI_ET) {
            bool found_en = false;
            // Look back
            for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                if (types[j] == N00B_UNICODE_BIDI_BN || types[j] == N00B_UNICODE_BIDI_ET)
                    continue;
                if (types[j] == N00B_UNICODE_BIDI_EN)
                    found_en = true;
                break;
            }
            if (!found_en) {
                // Look forward
                for (uint32_t j = i + 1; j < len; j++) {
                    if (types[j] == N00B_UNICODE_BIDI_BN || types[j] == N00B_UNICODE_BIDI_ET)
                        continue;
                    if (types[j] == N00B_UNICODE_BIDI_EN)
                        found_en = true;
                    break;
                }
            }
            if (found_en)
                types[i] = N00B_UNICODE_BIDI_EN;
        }

        // W6: remaining ES, ET, CS -> ON
        if (types[i] == N00B_UNICODE_BIDI_ES || types[i] == N00B_UNICODE_BIDI_ET
            || types[i] == N00B_UNICODE_BIDI_CS) {
            types[i] = N00B_UNICODE_BIDI_ON;
        }

        // W7: EN after L context -> L
        if (types[i] == N00B_UNICODE_BIDI_EN) {
            uint8_t strong = (levels[i] % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;
            for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                if (types[j] == N00B_UNICODE_BIDI_L || types[j] == N00B_UNICODE_BIDI_R) {
                    strong = types[j];
                    break;
                }
            }
            if (strong == N00B_UNICODE_BIDI_L)
                types[i] = N00B_UNICODE_BIDI_L;
        }
    }
}

// Binary search for a bracket entry in the bracket_index table
static int32_t
find_bracket(n00b_codepoint_t cp)
{
    int32_t lo = 0;
    int32_t hi = (int32_t)n00b_unicode_bidi_bracket_index_len - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (n00b_unicode_bidi_bracket_index[mid][0] < cp)
            lo = mid + 1;
        else if (n00b_unicode_bidi_bracket_index[mid][0] > cp)
            hi = mid - 1;
        else
            return (int32_t)n00b_unicode_bidi_bracket_index[mid][1]; // offset into data
    }
    return -1;
}

// Get bracket paired character and type. Returns true if cp is a bracket.
static bool
get_bracket_info(n00b_codepoint_t cp, n00b_codepoint_t *paired, int *bracket_type)
{
    int32_t offset = find_bracket(cp);
    if (offset < 0)
        return false;
    uint32_t len = n00b_unicode_bidi_bracket_data[offset];
    if (len < 2)
        return false;
    *paired       = n00b_unicode_bidi_bracket_data[offset + 1];
    *bracket_type = (int)n00b_unicode_bidi_bracket_data[offset + 2]; // 1=open, 2=close
    return true;
}

// N0: Resolve paired brackets
// This must be called after W rules and before N1/N2.
// For each bracket pair, determine strong type from content and assign.
static void
resolve_brackets(const n00b_codepoint_t *cps,
                 uint8_t                *types,
                 const uint8_t          *levels,
                 uint32_t                len,
                 uint8_t                 para_level [[maybe_unused]])
{
// Find bracket pairs using a stack (max 63 per UBA)
#define MAX_BRACKET_STACK 63
    struct {
        uint32_t         pos;
        n00b_codepoint_t paired_cp;
    } stack[MAX_BRACKET_STACK];
    int stack_top = -1;

    // Collect pairs: (open_pos, close_pos)
    struct {
        uint32_t open;
        uint32_t close;
    } pairs[128];
    uint32_t npairs = 0;

    for (uint32_t i = 0; i < len && npairs < 128; i++) {
        if (types[i] == N00B_UNICODE_BIDI_BN)
            continue;

        n00b_codepoint_t paired;
        int              btype;
        if (!get_bracket_info(cps[i], &paired, &btype))
            continue;
        if (types[i] != N00B_UNICODE_BIDI_ON)
            continue;     // Only ON brackets participate

        if (btype == 1) { // opening bracket
            if (stack_top >= MAX_BRACKET_STACK - 1)
                break;    // overflow
            stack_top++;
            stack[stack_top].pos       = i;
            stack[stack_top].paired_cp = paired;
        }
        else if (btype == 2) { // closing bracket
            // Find matching opener on stack
            for (int j = stack_top; j >= 0; j--) {
                if (stack[j].paired_cp == cps[i] ||
                    // canonical equivalents: check both directions
                    stack[j].paired_cp == paired) {
                    // Wait, the paired_cp of the opener is the closer.
                    // stack[j].paired_cp should equal cps[i] (the closer).
                    // But we also need to handle canonical equivalents.
                }
                // Check if this opener pairs with this closer
                // The opener's paired_cp should be the close bracket
                if (stack[j].paired_cp == cps[i]) {
                    pairs[npairs].open  = stack[j].pos;
                    pairs[npairs].close = i;
                    npairs++;
                    stack_top = j - 1; // pop everything above and including j
                    break;
                }
            }
        }
    }

    // Sort pairs by opening position
    for (uint32_t i = 1; i < npairs; i++) {
        for (uint32_t j = i; j > 0 && pairs[j].open < pairs[j - 1].open; j--) {
            uint32_t to = pairs[j].open, tc = pairs[j].close;
            pairs[j].open      = pairs[j - 1].open;
            pairs[j].close     = pairs[j - 1].close;
            pairs[j - 1].open  = to;
            pairs[j - 1].close = tc;
        }
    }

    // For each pair, find the strong type inside and assign
    for (uint32_t p = 0; p < npairs; p++) {
        uint32_t open       = pairs[p].open;
        uint32_t close      = pairs[p].close;
        uint8_t  pair_level = levels[open];
        uint8_t  pair_dir   = (pair_level % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;
        uint8_t  opposite
            = (pair_dir == N00B_UNICODE_BIDI_L) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;

        // Scan inside for strong types
        bool found_same     = false;
        bool found_opposite = false;
        for (uint32_t k = open + 1; k < close; k++) {
            if (types[k] == N00B_UNICODE_BIDI_BN)
                continue;
            uint8_t strong = N00B_UNICODE_BIDI_ON;
            if (types[k] == N00B_UNICODE_BIDI_L)
                strong = N00B_UNICODE_BIDI_L;
            else if (types[k] == N00B_UNICODE_BIDI_R || types[k] == N00B_UNICODE_BIDI_AN
                     || types[k] == N00B_UNICODE_BIDI_EN)
                strong = N00B_UNICODE_BIDI_R;
            if (strong == N00B_UNICODE_BIDI_ON)
                continue;

            if (strong == pair_dir)
                found_same = true;
            if (strong == opposite)
                found_opposite = true;
        }

        uint8_t resolved_type = N00B_UNICODE_BIDI_ON; // default: leave as ON
        if (found_same) {
            resolved_type = pair_dir;
        }
        else if (found_opposite) {
            // Check context before the open bracket
            uint8_t context = pair_dir; // default to embedding direction
            for (int32_t k = (int32_t)open - 1; k >= 0; k--) {
                if (types[k] == N00B_UNICODE_BIDI_BN)
                    continue;
                if (types[k] == N00B_UNICODE_BIDI_L) {
                    context = N00B_UNICODE_BIDI_L;
                    break;
                }
                if (types[k] == N00B_UNICODE_BIDI_R || types[k] == N00B_UNICODE_BIDI_AN
                    || types[k] == N00B_UNICODE_BIDI_EN) {
                    context = N00B_UNICODE_BIDI_R;
                    break;
                }
            }
            resolved_type = context;
        }

        if (resolved_type != N00B_UNICODE_BIDI_ON) {
            types[open]  = resolved_type;
            types[close] = resolved_type;
            // Also resolve any NSMs following the brackets
            for (uint32_t k = open + 1; k < len; k++) {
                if (types[k] == N00B_UNICODE_BIDI_BN)
                    continue;
                if (types[k] == N00B_UNICODE_BIDI_NSM)
                    types[k] = resolved_type;
                else
                    break;
            }
            for (uint32_t k = close + 1; k < len; k++) {
                if (types[k] == N00B_UNICODE_BIDI_BN)
                    continue;
                if (types[k] == N00B_UNICODE_BIDI_NSM)
                    types[k] = resolved_type;
                else
                    break;
            }
        }
    }

#undef MAX_BRACKET_STACK
}

// N1/N2: Resolve neutral and isolate formatting types
static void
resolve_neutral(uint8_t *types, const uint8_t *levels, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (types[i] == N00B_UNICODE_BIDI_BN)
            continue;

        uint8_t type = types[i];
        if (type != N00B_UNICODE_BIDI_B && type != N00B_UNICODE_BIDI_S
            && type != N00B_UNICODE_BIDI_WS && type != N00B_UNICODE_BIDI_ON
            && type != N00B_UNICODE_BIDI_FSI && type != N00B_UNICODE_BIDI_LRI
            && type != N00B_UNICODE_BIDI_RLI && type != N00B_UNICODE_BIDI_PDI) {
            continue;
        }

        // Find the run of neutrals
        uint32_t start = i;
        while (i < len
               && (types[i] == N00B_UNICODE_BIDI_B || types[i] == N00B_UNICODE_BIDI_S
                   || types[i] == N00B_UNICODE_BIDI_WS || types[i] == N00B_UNICODE_BIDI_ON
                   || types[i] == N00B_UNICODE_BIDI_FSI || types[i] == N00B_UNICODE_BIDI_LRI
                   || types[i] == N00B_UNICODE_BIDI_RLI || types[i] == N00B_UNICODE_BIDI_PDI
                   || types[i] == N00B_UNICODE_BIDI_BN)) {
            i++;
        }
        uint32_t end = i;

        // N1: Find adjacent strong types
        uint8_t before = (levels[start] % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;
        for (int32_t j = (int32_t)start - 1; j >= 0; j--) {
            if (types[j] == N00B_UNICODE_BIDI_L || types[j] == N00B_UNICODE_BIDI_R
                || types[j] == N00B_UNICODE_BIDI_AN || types[j] == N00B_UNICODE_BIDI_EN) {
                before = (types[j] == N00B_UNICODE_BIDI_AN || types[j] == N00B_UNICODE_BIDI_EN)
                           ? N00B_UNICODE_BIDI_R
                           : types[j];
                break;
            }
        }

        uint8_t after = (levels[start] % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L;
        for (uint32_t j = end; j < len; j++) {
            if (types[j] == N00B_UNICODE_BIDI_L || types[j] == N00B_UNICODE_BIDI_R
                || types[j] == N00B_UNICODE_BIDI_AN || types[j] == N00B_UNICODE_BIDI_EN) {
                after = (types[j] == N00B_UNICODE_BIDI_AN || types[j] == N00B_UNICODE_BIDI_EN)
                          ? N00B_UNICODE_BIDI_R
                          : types[j];
                break;
            }
        }

        uint8_t resolved;
        if (before == after) {
            resolved = before; // N1
        }
        else {
            resolved = (levels[start] % 2) ? N00B_UNICODE_BIDI_R : N00B_UNICODE_BIDI_L; // N2
        }

        for (uint32_t j = start; j < end; j++) {
            if (types[j] != N00B_UNICODE_BIDI_BN) {
                types[j] = resolved;
            }
        }

        i = end - 1; // will be incremented by loop
    }
}

// I1/I2: Resolve implicit levels
static void
resolve_implicit(const uint8_t *types, uint8_t *levels, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (types[i] == N00B_UNICODE_BIDI_BN)
            continue;

        if (levels[i] % 2 == 0) {
            // I1: even level
            if (types[i] == N00B_UNICODE_BIDI_R)
                levels[i]++;
            else if (types[i] == N00B_UNICODE_BIDI_AN || types[i] == N00B_UNICODE_BIDI_EN)
                levels[i] += 2;
        }
        else {
            // I2: odd level
            if (types[i] == N00B_UNICODE_BIDI_L || types[i] == N00B_UNICODE_BIDI_EN
                || types[i] == N00B_UNICODE_BIDI_AN) {
                levels[i]++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static n00b_unicode_bidi_para_t *
bidi_open_internal(const char *data, int64_t len)
{
    n00b_unicode_bidi_para_t *p         = n00b_alloc(n00b_unicode_bidi_para_t);
    uint32_t                  num_bytes = (uint32_t)len;

    // Count codepoints for allocation
    n00b_string_t _s             = {.u8_bytes = num_bytes, .data = (char *)data};
    int64_t       num_codepoints = n00b_unicode_utf8_count_codepoints(_s);
    if (num_codepoints < 0)
        num_codepoints = 0;

    // Decode string to codepoints
    p->cps    = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(n00b_codepoint_t));
    p->types  = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(uint8_t));
    p->levels = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(uint8_t));
    p->len    = 0;

    uint32_t pos = 0;
    while (pos < num_bytes) {
        int32_t cp = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        if (cp < 0)
            break;
        p->cps[p->len]   = (n00b_codepoint_t)cp;
        p->types[p->len] = (uint8_t)n00b_unicode_bidi_class((n00b_codepoint_t)cp);
        p->len++;
    }

    // P2/P3
    p->para_level = determine_paragraph_level(p->cps, p->types, p->len);

    // X1-X8
    resolve_explicit(p->types, p->levels, p->len, p->para_level);

    // W1-W7
    resolve_weak(p->types, p->levels, p->len);

    // N0: bracket pairs
    resolve_brackets(p->cps, p->types, p->levels, p->len, p->para_level);

    // N1/N2
    resolve_neutral(p->types, p->levels, p->len);

    // I1/I2
    resolve_implicit(p->types, p->levels, p->len);

    // L1: Reset levels for S, B, and preceding WS/isolate markers
    {
        // Also reset trailing WS/isolate chars to paragraph level
        // First, handle S and B: set to para_level, and reset preceding WS
        for (uint32_t i = 0; i < p->len; i++) {
            uint8_t orig = (uint8_t)n00b_unicode_bidi_class(p->cps[i]);
            if (orig == N00B_UNICODE_BIDI_S || orig == N00B_UNICODE_BIDI_B) {
                p->levels[i] = p->para_level;
                // Reset preceding WS and isolate formatting
                for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                    uint8_t ot = (uint8_t)n00b_unicode_bidi_class(p->cps[j]);
                    if (ot == N00B_UNICODE_BIDI_WS || ot == N00B_UNICODE_BIDI_FSI
                        || ot == N00B_UNICODE_BIDI_LRI || ot == N00B_UNICODE_BIDI_RLI
                        || ot == N00B_UNICODE_BIDI_PDI || ot == N00B_UNICODE_BIDI_BN) {
                        p->levels[j] = p->para_level;
                    }
                    else {
                        break;
                    }
                }
            }
        }
        // Also reset trailing WS at end of string
        for (int32_t j = (int32_t)p->len - 1; j >= 0; j--) {
            uint8_t ot = (uint8_t)n00b_unicode_bidi_class(p->cps[j]);
            if (ot == N00B_UNICODE_BIDI_WS || ot == N00B_UNICODE_BIDI_FSI
                || ot == N00B_UNICODE_BIDI_LRI || ot == N00B_UNICODE_BIDI_RLI
                || ot == N00B_UNICODE_BIDI_PDI || ot == N00B_UNICODE_BIDI_S
                || ot == N00B_UNICODE_BIDI_B || ot == N00B_UNICODE_BIDI_BN) {
                p->levels[j] = p->para_level;
            }
            else {
                break;
            }
        }
    }

    return p;
}

n00b_unicode_bidi_para_t *
n00b_unicode_bidi_open(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator;
    return bidi_open_internal(s.data, s.u8_bytes);
}

uint8_t
n00b_unicode_bidi_paragraph_level(const n00b_unicode_bidi_para_t *p)
{
    assert(p);
    return p->para_level;
}

n00b_array_t(uint8_t)
n00b_unicode_bidi_levels(const n00b_unicode_bidi_para_t *p)
{
    assert(p);
    n00b_array_t(uint8_t) result = n00b_array_new(uint8_t, p->len);
    memcpy(result.data, p->levels, p->len * sizeof(uint8_t));
    result.len = p->len;
    return result;
}

n00b_array_t(int32_t)
n00b_unicode_bidi_reorder_visual(const n00b_unicode_bidi_para_t *p)
{
    assert(p);
    n00b_array_t(int32_t) result = n00b_array_new(int32_t, p->len);
    int32_t *visual_map = result.data;
    result.len          = p->len;

    // Initialize identity mapping
    for (uint32_t i = 0; i < p->len; i++) {
        visual_map[i] = (int32_t)i;
    }

    // L2: reverse sequences of characters at the same level
    // Find max level
    uint8_t max_level = 0;
    for (uint32_t i = 0; i < p->len; i++) {
        if (p->levels[i] > max_level)
            max_level = p->levels[i];
    }

    // Reverse from max down to 1
    for (uint8_t level = max_level; level >= 1; level--) {
        uint32_t i = 0;
        while (i < p->len) {
            if (p->levels[i] >= level) {
                uint32_t start = i;
                while (i < p->len && p->levels[i] >= level)
                    i++;
                // Reverse start..i-1
                uint32_t lo = start;
                uint32_t hi = i - 1;
                while (lo < hi) {
                    int32_t tmp    = visual_map[lo];
                    visual_map[lo] = visual_map[hi];
                    visual_map[hi] = tmp;
                    lo++;
                    hi--;
                }
            }
            else {
                i++;
            }
        }
    }

    return result;
}

void
n00b_unicode_bidi_free(n00b_unicode_bidi_para_t *p)
{
    if (p) {
        n00b_free(p->cps);
        n00b_free(p->types);
        n00b_free(p->levels);
        n00b_free(p);
    }
}
