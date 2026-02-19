#include "unicode/collation.h"
#include "unicode/encoding.h"
#include "unicode/normalization.h"
#include "unicode/properties.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "internal/unicode/tables.h"
#include <string.h>

extern const uint16_t n00b_unicode_ducet_stage1[];
extern const uint32_t n00b_unicode_ducet_stage2[];
extern const uint16_t n00b_unicode_ce_data[];
extern const uint32_t n00b_unicode_ce_data_len;
extern const uint32_t n00b_unicode_contraction_count;

extern const uint32_t n00b_unicode_contr_keys[][4];
extern const uint32_t n00b_unicode_contr_key_lens[];
extern const uint32_t n00b_unicode_contr_ce_offsets[];
extern const uint16_t n00b_unicode_contr_ce_data[];

// ---------------------------------------------------------------------------
// Contraction lookup
// ---------------------------------------------------------------------------

// Find contraction entries starting with cp1.
// Returns the index of the first entry with keys[i][0] == cp1, or -1.
static int32_t
find_contraction_start(n00b_codepoint_t cp1)
{
    int32_t lo     = 0;
    int32_t hi     = (int32_t)n00b_unicode_contraction_count - 1;
    int32_t result = -1;

    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (n00b_unicode_contr_keys[mid][0] < cp1) {
            lo = mid + 1;
        }
        else if (n00b_unicode_contr_keys[mid][0] > cp1) {
            hi = mid - 1;
        }
        else {
            result = mid;
            hi     = mid - 1; // find first
        }
    }
    return result;
}

// Try to match a contraction starting at cps[idx] in the codepoint array.
// If matched, writes CEs to out and returns number of CEs written.
// matched_mask[i] is set to true for each input position consumed by the
// contraction. Returns 0 if no contraction matches.
static int
try_contraction(const n00b_codepoint_t *cps,
                const uint8_t          *cccs,
                uint32_t                num_cps,
                uint32_t                idx,
                uint16_t               *out,
                int                     max_ces,
                bool                   *matched_mask)
{
    if (n00b_unicode_contraction_count == 0)
        return 0;

    int32_t start = find_contraction_start(cps[idx]);
    if (start < 0)
        return 0;

    // Find the best (longest) matching contraction
    int      best_klen = 0;
    int32_t  best_ci   = -1;
    uint32_t best_positions[4]; // positions in input that matched key elements

    for (int32_t ci = start; ci < (int32_t)n00b_unicode_contraction_count
                             && n00b_unicode_contr_keys[ci][0] == cps[idx];
         ci++) {
        uint32_t klen = n00b_unicode_contr_key_lens[ci];
        if (klen < 2 || klen > 4)
            continue;

        // Try contiguous match first (most common case)
        uint32_t positions[4];
        positions[0]    = idx;
        bool contiguous = true;

        uint32_t ip = idx + 1;
        for (uint32_t k = 1; k < klen; k++) {
            if (ip < num_cps && cps[ip] == n00b_unicode_contr_keys[ci][k]) {
                positions[k] = ip;
                ip++;
            }
            else {
                contiguous = false;
                break;
            }
        }

        if (contiguous && (int)klen > best_klen) {
            best_klen = (int)klen;
            best_ci   = ci;
            for (uint32_t k = 0; k < klen; k++)
                best_positions[k] = positions[k];
            continue;
        }

        // Try discontiguous match (UCA S2.1.1):
        // First character must be a starter (CCC=0).
        // Match as many contiguous key elements as possible, then for remaining
        // elements, scan forward skipping combining marks whose CCC doesn't block.
        if (cccs[idx] != 0)
            continue;

        positions[0]       = idx;
        ip                 = idx + 1;
        uint32_t matched_k = 1;

        // Match contiguous prefix of key elements
        for (uint32_t k = 1; k < klen; k++) {
            if (ip < num_cps && cps[ip] == n00b_unicode_contr_keys[ci][k]) {
                positions[k] = ip;
                ip++;
                matched_k++;
            }
            else {
                break;
            }
        }

        if (matched_k == klen)
            continue; // already handled as contiguous

        // Now try discontiguous matching for remaining key elements
        uint32_t scan             = ip;
        uint8_t  last_matched_ccc = 0;
        bool     discont_match    = true;

        for (uint32_t k = matched_k; k < klen; k++) {
            uint32_t target = n00b_unicode_contr_keys[ci][k];
            bool     found  = false;

            for (uint32_t s = scan; s < num_cps; s++) {
                if (cccs[s] == 0)
                    break; // hit a starter -- stop
                if (cps[s] == target) {
                    uint8_t this_ccc = cccs[s];
                    if (this_ccc <= last_matched_ccc)
                        break; // blocked
                    bool blocked = false;
                    for (uint32_t m = scan; m < s; m++) {
                        if (cccs[m] >= this_ccc) {
                            blocked = true;
                            break;
                        }
                    }
                    if (blocked)
                        break;
                    positions[k]     = s;
                    last_matched_ccc = this_ccc;
                    scan             = s + 1;
                    found            = true;
                    break;
                }
            }
            if (!found) {
                discont_match = false;
                break;
            }
        }

        if (discont_match && (int)klen > best_klen) {
            best_klen = (int)klen;
            best_ci   = ci;
            for (uint32_t k = 0; k < klen; k++)
                best_positions[k] = positions[k];
        }
    }

    if (best_ci < 0)
        return 0;

    // Mark consumed positions
    for (int k = 0; k < best_klen; k++) {
        matched_mask[best_positions[k]] = true;
    }

    // Read CEs from contraction data
    uint32_t ce_off  = n00b_unicode_contr_ce_offsets[best_ci];
    uint16_t count   = n00b_unicode_contr_ce_data[ce_off];
    int      written = 0;
    for (uint16_t i = 0; i < count && written < max_ces; i++) {
        uint32_t base        = ce_off + 1 + i * 3;
        out[written * 3]     = n00b_unicode_contr_ce_data[base];
        out[written * 3 + 1] = n00b_unicode_contr_ce_data[base + 1];
        out[written * 3 + 2] = n00b_unicode_contr_ce_data[base + 2];
        written++;
    }

    return written;
}

// ---------------------------------------------------------------------------
// Single codepoint CE lookup
// ---------------------------------------------------------------------------

// Get collation elements for a single codepoint
// Returns number of CEs written to out (each CE is 3 uint16_t: primary,
// secondary, tertiary)
static int
get_ces(n00b_codepoint_t cp, uint16_t *out, int max_ces)
{
    uint32_t offset
        = N00B_UNICODE_LOOKUP(n00b_unicode_ducet_stage1, n00b_unicode_ducet_stage2, cp);

    if (offset == 0xFFFFFFFF) {
        // Implicit weight for unassigned/CJK: derived from codepoint
        // UCA 10.1.3: AAAA = base + (cp >> 15), BBBB = (cp & 0x7FFF) | 0x8000
        uint16_t base;

        // @implicitweights ranges from allkeys.txt (must check before CJK)
        if ((cp >= 0x17000 && cp <= 0x18AFF) || // Tangut + Components
            (cp >= 0x18D00 && cp <= 0x18D7F)) { // Tangut Supplement
            base = 0xFB00;
        }
        else if (cp >= 0x1B170 && cp <= 0x1B2FF) { // Nushu
            base = 0xFB01;
        }
        else if (cp >= 0x18B00 && cp <= 0x18CFF) { // Khitan Small Script
            base = 0xFB02;
        }
        // CJK Unified Ideographs (Unified_Ideograph=Yes and in core blocks)
        else if (cp >= 0x4E00 && cp <= 0x9FFF) { // CJK Unified
            base = 0xFB40;
        }
        // CJK extensions and other Unified_Ideograph ranges
        else if ((cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Ext A
                 (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compat Ideographs
                 (cp >= 0x20000 && cp <= 0x2A6DF) || // CJK Ext B
                 (cp >= 0x2A700 && cp <= 0x2B739) || // CJK Ext C
                 (cp >= 0x2B740 && cp <= 0x2B81D) || // CJK Ext D
                 (cp >= 0x2B820 && cp <= 0x2CEA1) || // CJK Ext E
                 (cp >= 0x2CEB0 && cp <= 0x2EBE0) || // CJK Ext F
                 (cp >= 0x2EBF0 && cp <= 0x2F7FF) || // CJK Ext I (Unicode 16.0)
                 (cp >= 0x30000 && cp <= 0x3134A) || // CJK Ext G
                 (cp >= 0x31350 && cp <= 0x323AF)) { // CJK Ext H
            base = 0xFB80;
        }
        // Everything else (unassigned, surrogates, private use, etc.)
        else {
            base = 0xFBC0;
        }

        uint16_t aaaa = base + (uint16_t)(cp >> 15);
        uint16_t bbbb = (uint16_t)((cp & 0x7FFF) | 0x8000);

        if (max_ces >= 2) {
            out[0] = aaaa;
            out[1] = 0x0020;
            out[2] = 0x0002;
            out[3] = bbbb;
            out[4] = 0x0000;
            out[5] = 0x0000;
            return 2;
        }
        return 0;
    }

    if (offset >= n00b_unicode_ce_data_len)
        return 0;

    uint16_t count   = n00b_unicode_ce_data[offset];
    int      written = 0;
    for (uint16_t i = 0; i < count && written < max_ces; i++) {
        uint32_t base = offset + 1 + i * 3;
        if (base + 2 >= n00b_unicode_ce_data_len)
            break;
        out[written * 3]     = n00b_unicode_ce_data[base];
        out[written * 3 + 1] = n00b_unicode_ce_data[base + 1];
        out[written * 3 + 2] = n00b_unicode_ce_data[base + 2];
        written++;
    }
    return written;
}

// ---------------------------------------------------------------------------
// Sort key construction
// ---------------------------------------------------------------------------

static n00b_unicode_sort_key_t
sort_key_internal(const char *data, int64_t len)
{
    // Normalize to NFD first
    n00b_allocator_t *allocator = nullptr;
    n00b_ensure_allocator(allocator);
    n00b_string_t nfd = n00b_unicode_nfd_raw(allocator, data, len);

    // Decode all codepoints into an array
    // Upper bound: each byte could be a codepoint
    n00b_codepoint_t *cps
        = n00b_alloc_array(char, (uint32_t)nfd.u8_bytes * sizeof(n00b_codepoint_t));
    uint32_t num_cps = 0;
    uint32_t pos     = 0;
    while (pos < (uint32_t)nfd.u8_bytes) {
        int32_t cp = n00b_unicode_utf8_decode(nfd.data, (uint32_t)nfd.u8_bytes, &pos);
        if (cp < 0)
            break;
        cps[num_cps++] = (n00b_codepoint_t)cp;
    }

    // Pre-compute CCC for each codepoint
    uint8_t *cccs = n00b_alloc_array(char, num_cps);
    for (uint32_t j = 0; j < num_cps; j++) {
        cccs[j] = n00b_unicode_combining_class(cps[j]);
    }

    // Track which positions are consumed by contractions
    bool *matched = n00b_alloc_array(bool, num_cps);

    // Collect all CEs -- max 18 CEs per codepoint in DUCET
    int       ce_cap    = (int)num_cps * 18 + 36;
    uint16_t *ces       = n00b_alloc_array(char, (uint32_t)ce_cap * 3 * sizeof(uint16_t));
    int       total_ces = 0;

    for (uint32_t i = 0; i < num_cps; i++) {
        if (matched[i])
            continue; // already consumed by a contraction

        int remaining = ce_cap - total_ces;
        if (remaining <= 0)
            break;

        // Try contractions (contiguous for all chars, discontiguous for starters)
        int n = try_contraction(cps, cccs, num_cps, i, ces + total_ces * 3, remaining, matched);
        if (n > 0) {
            total_ces += n;
            continue;
        }

        total_ces += get_ces(cps[i], ces + total_ces * 3, remaining);
    }

    n00b_free(matched);
    n00b_free(cccs);
    n00b_free(cps);

    // Build sort key: primary weights + 0000 + secondary weights + 0000 +
    // tertiary weights Each weight is uint16_t -> encode as big-endian bytes
    uint32_t key_cap = (uint32_t)(total_ces * 6 + 4);
    uint8_t *key     = n00b_alloc_array(char, key_cap);
    uint32_t key_len = 0;

    // Primary weights
    for (int j = 0; j < total_ces; j++) {
        uint16_t p = ces[j * 3];
        if (p != 0) {
            key[key_len++] = (uint8_t)(p >> 8);
            key[key_len++] = (uint8_t)(p & 0xFF);
        }
    }
    key[key_len++] = 0;
    key[key_len++] = 0;

    // Secondary weights
    for (int j = 0; j < total_ces; j++) {
        uint16_t sec = ces[j * 3 + 1];
        if (sec != 0) {
            key[key_len++] = (uint8_t)(sec >> 8);
            key[key_len++] = (uint8_t)(sec & 0xFF);
        }
    }
    key[key_len++] = 0;
    key[key_len++] = 0;

    // Tertiary weights
    for (int j = 0; j < total_ces; j++) {
        uint16_t ter = ces[j * 3 + 2];
        if (ter != 0) {
            key[key_len++] = (uint8_t)(ter >> 8);
            key[key_len++] = (uint8_t)(ter & 0xFF);
        }
    }

    n00b_free(ces);

    return (n00b_unicode_sort_key_t){.data = key, .len = key_len};
}

n00b_unicode_sort_key_t
n00b_unicode_sort_key(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator;
    return sort_key_internal(s.data, s.u8_bytes);
}

static int
collate_internal(const char *a, int64_t a_len, const char *b, int64_t b_len)
{
    n00b_unicode_sort_key_t ka = sort_key_internal(a, a_len);
    n00b_unicode_sort_key_t kb = sort_key_internal(b, b_len);

    uint32_t min_len = ka.len < kb.len ? ka.len : kb.len;
    int      result  = memcmp(ka.data, kb.data, min_len);
    if (result == 0) {
        if (ka.len < kb.len)
            result = -1;
        else if (ka.len > kb.len)
            result = 1;
    }

    n00b_unicode_sort_key_free(&ka);
    n00b_unicode_sort_key_free(&kb);
    return result;
}

int
n00b_unicode_collate(n00b_string_t a, n00b_string_t b)
{
    return collate_internal(a.data, a.u8_bytes, b.data, b.u8_bytes);
}

void
n00b_unicode_sort_key_free(n00b_unicode_sort_key_t *key)
{
    if (key && key->data) {
        n00b_free(key->data);
        key->data = nullptr;
        key->len  = 0;
    }
}
