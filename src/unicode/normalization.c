#include "unicode/normalization.h"
#include "unicode/encoding.h"
#include "unicode/properties.h"
#include "internal/unicode/hangul.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include "internal/unicode/tables.h"
#include <string.h>

// External generated tables
extern const uint32_t n00b_unicode_canon_decomp_index[][2];
extern const uint32_t n00b_unicode_canon_decomp_index_len;
extern const uint32_t n00b_unicode_canon_decomp_data[];

extern const uint32_t n00b_unicode_compat_decomp_index[][2];
extern const uint32_t n00b_unicode_compat_decomp_index_len;
extern const uint32_t n00b_unicode_compat_decomp_data[];

extern const uint32_t n00b_unicode_comp_starter[];
extern const uint32_t n00b_unicode_comp_combining[];
extern const uint32_t n00b_unicode_comp_result[];

extern const uint16_t n00b_unicode_nqc_stage1[];
extern const uint8_t n00b_unicode_nqc_stage2[];

#define N00B_UNICODE_COMP_TABLE_SIZE 2048
#define N00B_UNICODE_COMP_EMPTY 0xFFFFFFFFU

// ---------------------------------------------------------------------------
// Decomposition helpers
// ---------------------------------------------------------------------------

// Get canonical decomposition for a codepoint.
// Returns count (0 if none), fills out[] (must have room for at least 4).
static int get_canon_decomp(n00b_codepoint_t cp, n00b_codepoint_t *out) {
  // Hangul algorithmic decomposition
  if (hangul_is_syllable(cp)) {
    return hangul_decompose(cp, out);
  }

  const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_canon_decomp_index,
                                                n00b_unicode_canon_decomp_index_len,
                                                n00b_unicode_canon_decomp_data, cp);

  if (!entry)
    return 0;

  uint32_t len = entry[0];
  for (uint32_t i = 0; i < len && i < 4; i++) {
    out[i] = entry[1 + i];
  }
  return (int)len;
}

// Get compatibility decomposition
static int get_compat_decomp(n00b_codepoint_t cp, n00b_codepoint_t *out) {
  // Try canonical first (canonical decomps are also compatibility decomps)
  int n = get_canon_decomp(cp, out);
  if (n > 0)
    return n;

  const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_compat_decomp_index,
                                                n00b_unicode_compat_decomp_index_len,
                                                n00b_unicode_compat_decomp_data, cp);

  if (!entry)
    return 0;

  uint32_t len = entry[0];
  for (uint32_t i = 0; i < len && i < 18; i++) {
    out[i] = entry[1 + i];
  }
  return (int)len;
}

// Canonical composition lookup
static n00b_codepoint_t compose_pair(n00b_codepoint_t a,
                                        n00b_codepoint_t b) {
  // Hangul algorithmic composition
  n00b_codepoint_t h = hangul_compose(a, b);
  if (h)
    return h;

  // Hash table lookup
  // Use same hash as the Python generator: ((starter << 21) | combining) % size
  uint64_t key = ((uint64_t)a << 21) | (uint64_t)b;
  uint32_t slot = (uint32_t)(key % N00B_UNICODE_COMP_TABLE_SIZE);

  for (uint32_t i = 0; i < N00B_UNICODE_COMP_TABLE_SIZE; i++) {
    uint32_t idx = (slot + i) % N00B_UNICODE_COMP_TABLE_SIZE;
    if (n00b_unicode_comp_starter[idx] == N00B_UNICODE_COMP_EMPTY)
      return 0;
    if (n00b_unicode_comp_starter[idx] == a && n00b_unicode_comp_combining[idx] == b) {
      return n00b_unicode_comp_result[idx];
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Buffer for codepoint sequences during normalization
// ---------------------------------------------------------------------------

typedef struct {
  n00b_codepoint_t *data;
  uint32_t len;
  uint32_t cap;
} cp_buf_t;

static void cp_buf_init(cp_buf_t *buf) {
  buf->cap = 64;
  buf->len = 0;
  buf->data = n00b_alloc_array(char, buf->cap * sizeof(n00b_codepoint_t));
}

static void cp_buf_push(cp_buf_t *buf, n00b_codepoint_t cp) {
  if (buf->len >= buf->cap) {
    buf->cap *= 2;
    {
      n00b_codepoint_t *new_data = n00b_alloc_array(char, buf->cap * sizeof(n00b_codepoint_t));
      memcpy(new_data, buf->data, buf->len * sizeof(n00b_codepoint_t));
      n00b_free(buf->data);
      buf->data = new_data;
    }
  }
  buf->data[buf->len++] = cp;
}

static void cp_buf_free(cp_buf_t *buf) { n00b_free(buf->data); }

// ---------------------------------------------------------------------------
// Full recursive decomposition into buffer
// ---------------------------------------------------------------------------

static void decompose_recursive(n00b_codepoint_t cp, cp_buf_t *buf,
                                bool compat) {
  n00b_codepoint_t decomp[18];
  int n;

  if (compat) {
    n = get_compat_decomp(cp, decomp);
  } else {
    n = get_canon_decomp(cp, decomp);
  }

  if (n == 0) {
    cp_buf_push(buf, cp);
    return;
  }

  for (int i = 0; i < n; i++) {
    decompose_recursive(decomp[i], buf, compat);
  }
}

// ---------------------------------------------------------------------------
// Canonical ordering: sort combining marks by CCC using stable insertion sort
// ---------------------------------------------------------------------------

static void canonical_order(n00b_codepoint_t *cps, uint32_t len) {
  for (uint32_t i = 1; i < len; i++) {
    n00b_codepoint_t cp = cps[i];
    uint8_t ccc = n00b_unicode_combining_class(cp);

    if (ccc == 0)
      continue; // starters don't move

    uint32_t j = i;
    while (j > 0) {
      uint8_t prev_ccc = n00b_unicode_combining_class(cps[j - 1]);
      if (prev_ccc <= ccc || prev_ccc == 0)
        break;
      cps[j] = cps[j - 1];
      j--;
    }
    cps[j] = cp;
  }
}

// ---------------------------------------------------------------------------
// Canonical composition on a decomposed+ordered buffer
// ---------------------------------------------------------------------------

static void canonical_compose(cp_buf_t *buf) {
  if (buf->len < 2)
    return;

  uint32_t starter_idx = 0;
  uint8_t last_ccc = 0;
  bool has_starter = (n00b_unicode_combining_class(buf->data[0]) == 0);

  if (!has_starter) {
    // If first character is not a starter, nothing to compose with
    starter_idx = 0;
    last_ccc = n00b_unicode_combining_class(buf->data[0]);
  }

  for (uint32_t i = 1; i < buf->len; i++) {
    n00b_codepoint_t cp = buf->data[i];
    uint8_t ccc = n00b_unicode_combining_class(cp);

    if (has_starter) {
      // D117: a combining character C is blocked from a starter S
      // if and only if there is some character B between S and C,
      // and either B is a starter (ccc=0) or ccc(B) >= ccc(C).
      //
      // When C has ccc=0 (is itself a starter), it can only compose
      // with S if there is NO intervening character (last_ccc must be 0).
      bool blocked = (ccc == 0) ? (last_ccc != 0) : (last_ccc >= ccc);

      if (!blocked) {
        n00b_codepoint_t composed = compose_pair(buf->data[starter_idx], cp);
        if (composed) {
          buf->data[starter_idx] = composed;
          // Remove cp by shifting
          for (uint32_t k = i; k + 1 < buf->len; k++) {
            buf->data[k] = buf->data[k + 1];
          }
          buf->len--;
          i--; // re-examine at same position
          // Don't update last_ccc -- it stays the same
          continue;
        }
      }
    }

    if (ccc == 0) {
      starter_idx = i;
      has_starter = true;
      last_ccc = 0;
    } else {
      last_ccc = ccc;
    }
  }
}

// ---------------------------------------------------------------------------
// Internal: decompose string to codepoint buffer
// ---------------------------------------------------------------------------

static cp_buf_t decompose_str(const char *data, int64_t len, bool compat) {
  cp_buf_t buf;
  cp_buf_init(&buf);

  uint32_t pos = 0;
  while (pos < (uint32_t)len) {
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (cp < 0)
      break;
    decompose_recursive((n00b_codepoint_t)cp, &buf, compat);
  }

  canonical_order(buf.data, buf.len);
  return buf;
}

// ---------------------------------------------------------------------------
// Convert codepoint buffer to n00b_string_t (allocator-allocated)
// ---------------------------------------------------------------------------

static n00b_string_t cp_buf_to_n00b_string(n00b_allocator_t *allocator, cp_buf_t *buf) {
  // Worst case: 4 bytes per codepoint
  char *out = n00b_alloc_array(char, buf->len * 4 + 1);
  uint32_t pos = 0;

  for (uint32_t i = 0; i < buf->len; i++) {
    pos += n00b_unicode_utf8_encode(buf->data[i], out + pos);
  }
  out[pos] = '\0';

  n00b_string_t result =
      n00b_string_from_raw(allocator, out, (int64_t)pos, (int64_t)buf->len);
  n00b_free(out);
  return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

n00b_string_t n00b_unicode_nfd_raw(n00b_allocator_t *allocator, const char *data,
                           int64_t len) {
  cp_buf_t buf = decompose_str(data, len, false);
  n00b_string_t result = cp_buf_to_n00b_string(allocator, &buf);
  cp_buf_free(&buf);
  return result;
}

n00b_string_t n00b_unicode_nfd(n00b_string_t s) _kargs { n00b_allocator_t *allocator = NULL; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_nfd_raw(allocator, s.data, s.u8_bytes);
}

n00b_string_t n00b_unicode_nfkd_raw(n00b_allocator_t *allocator, const char *data,
                            int64_t len) {
  cp_buf_t buf = decompose_str(data, len, true);
  n00b_string_t result = cp_buf_to_n00b_string(allocator, &buf);
  cp_buf_free(&buf);
  return result;
}

n00b_string_t n00b_unicode_nfkd(n00b_string_t s) _kargs { n00b_allocator_t *allocator = NULL; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_nfkd_raw(allocator, s.data, s.u8_bytes);
}

n00b_string_t n00b_unicode_nfc_raw(n00b_allocator_t *allocator, const char *data,
                           int64_t len) {
  cp_buf_t buf = decompose_str(data, len, false);
  canonical_compose(&buf);
  n00b_string_t result = cp_buf_to_n00b_string(allocator, &buf);
  cp_buf_free(&buf);
  return result;
}

n00b_string_t n00b_unicode_nfc(n00b_string_t s) _kargs { n00b_allocator_t *allocator = NULL; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_nfc_raw(allocator, s.data, s.u8_bytes);
}

n00b_string_t n00b_unicode_nfkc_raw(n00b_allocator_t *allocator, const char *data,
                            int64_t len) {
  cp_buf_t buf = decompose_str(data, len, true);
  canonical_compose(&buf);
  n00b_string_t result = cp_buf_to_n00b_string(allocator, &buf);
  cp_buf_free(&buf);
  return result;
}

n00b_string_t n00b_unicode_nfkc(n00b_string_t s) _kargs { n00b_allocator_t *allocator = NULL; }
{
  if (!allocator)
    allocator = nullptr;
  return n00b_unicode_nfkc_raw(allocator, s.data, s.u8_bytes);
}

// ---------------------------------------------------------------------------
// Quick check
// ---------------------------------------------------------------------------

// NFC_QC bits 0-1, NFD_QC bits 2-3
static inline uint8_t nqc_lookup(n00b_codepoint_t cp) {
  if (cp >= 0x110000)
    return 0;
  return N00B_UNICODE_LOOKUP(n00b_unicode_nqc_stage1, n00b_unicode_nqc_stage2, cp);
}

bool n00b_unicode_is_nfc_raw(const char *data, int64_t len) {
  uint32_t pos = 0;
  uint8_t last_ccc = 0;

  while (pos < (uint32_t)len) {
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (cp < 0)
      return false;

    uint8_t nqc = nqc_lookup((n00b_codepoint_t)cp);
    uint8_t nfc_qc = nqc & 0x03; // NFC_QC: 0=Y, 1=M, 2=N

    if (nfc_qc == 2)
      return false; // No -> definitely not NFC

    uint8_t ccc = n00b_unicode_combining_class((n00b_codepoint_t)cp);
    if (ccc != 0 && last_ccc > ccc)
      return false; // not in canonical order

    if (nfc_qc == 1)
      return false; // Maybe -> need full check (conservative)

    last_ccc = ccc;
  }

  return true;
}

bool n00b_unicode_is_nfc(n00b_string_t s) {
  return n00b_unicode_is_nfc_raw(s.data, s.u8_bytes);
}

bool n00b_unicode_is_nfd_raw(const char *data, int64_t len) {
  uint32_t pos = 0;
  uint8_t last_ccc = 0;

  while (pos < (uint32_t)len) {
    int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
    if (cp < 0)
      return false;

    uint8_t nqc = nqc_lookup((n00b_codepoint_t)cp);
    uint8_t nfd_qc = (nqc >> 2) & 0x03;

    if (nfd_qc == 2)
      return false; // has canonical decomposition

    uint8_t ccc = n00b_unicode_combining_class((n00b_codepoint_t)cp);
    if (ccc != 0 && last_ccc > ccc)
      return false;

    last_ccc = ccc;
  }

  return true;
}

bool n00b_unicode_is_nfd(n00b_string_t s) {
  return n00b_unicode_is_nfd_raw(s.data, s.u8_bytes);
}

// ---------------------------------------------------------------------------
// Streaming normalizer
// ---------------------------------------------------------------------------

struct n00b_unicode_normalizer_s {
  n00b_unicode_norm_form_t form;
  cp_buf_t pending;  // accumulated codepoints not yet flushed
  cp_buf_t output;   // normalized output ready for reading
  uint32_t read_pos; // position in output
};

n00b_unicode_normalizer_t *
n00b_unicode_normalizer_new(n00b_unicode_norm_form_t form) _kargs {
  n00b_allocator_t *allocator = nullptr;
}
{
  (void)allocator; // accepted for API consistency, but use global allocator
                   // internally
  n00b_unicode_normalizer_t *n = n00b_alloc(n00b_unicode_normalizer_t);
  n->form = form;
  cp_buf_init(&n->pending);
  cp_buf_init(&n->output);
  return n;
}

// Process pending buffer: decompose, order, optionally compose, push to output
static void normalizer_process(n00b_unicode_normalizer_t *n) {
  if (n->pending.len == 0)
    return;

  bool compat = (n->form == N00B_UNICODE_NFKC || n->form == N00B_UNICODE_NFKD);

  // Decompose all pending codepoints
  cp_buf_t decomposed;
  cp_buf_init(&decomposed);
  for (uint32_t i = 0; i < n->pending.len; i++) {
    decompose_recursive(n->pending.data[i], &decomposed, compat);
  }
  n->pending.len = 0;

  canonical_order(decomposed.data, decomposed.len);

  if (n->form == N00B_UNICODE_NFC || n->form == N00B_UNICODE_NFKC) {
    canonical_compose(&decomposed);
  }

  for (uint32_t i = 0; i < decomposed.len; i++) {
    cp_buf_push(&n->output, decomposed.data[i]);
  }
  cp_buf_free(&decomposed);
}

void n00b_unicode_normalizer_feed(n00b_unicode_normalizer_t *n, n00b_codepoint_t cp) {
  uint8_t ccc = n00b_unicode_combining_class(cp);

  // If this is a starter and we have pending data, process first
  if (ccc == 0 && n->pending.len > 0) {
    normalizer_process(n);
  }

  cp_buf_push(&n->pending, cp);
}

size_t n00b_unicode_normalizer_read(n00b_unicode_normalizer_t *n,
                               n00b_codepoint_t *out, size_t max) {
  size_t count = 0;
  while (count < max && n->read_pos < n->output.len) {
    out[count++] = n->output.data[n->read_pos++];
  }

  // Compact if fully consumed
  if (n->read_pos >= n->output.len) {
    n->output.len = 0;
    n->read_pos = 0;
  }

  return count;
}

size_t n00b_unicode_normalizer_flush(n00b_unicode_normalizer_t *n,
                                n00b_codepoint_t *out, size_t max) {
  normalizer_process(n);
  return n00b_unicode_normalizer_read(n, out, max);
}

void n00b_unicode_normalizer_free(n00b_unicode_normalizer_t *n) {
  if (n) {
    cp_buf_free(&n->pending);
    cp_buf_free(&n->output);
    n00b_free(n);
  }
}
