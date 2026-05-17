/**
 * @file ml_io.c
 * @brief Versioned binary save/load for `(rules, model)` and trainer
 *        state.
 *
 * The blob is little-endian and fixed-layout.  Both supported targets
 * (aarch64, x86_64) are LE; we memcpy raw bytes rather than
 * encode/decode each field for speed.
 *
 * Layout — base:
 *
 *     offset  size  field
 *     ------  ----  -----
 *      0       4    magic           = 'N','M','L','B'
 *      4       2    version
 *      6       2    flags           (bit 0 = HAS_DELTA, bit 1 = HAS_TRAINER_STATE)
 *      8       4    num_groups
 *     12       4    total_size      (sum of group sizes)
 *     16       4    reserved
 *     20       4    bias            (IEEE 754 little-endian)
 *     24      ...   group_table     (per group: u32 size, u32 name_len, name)
 *     ...     4×T   weights[]       (T = total_size)
 *
 * Optional tails (in this order):
 *   - correction tail (HAS_DELTA): u32 correction_bias, 4×T weights
 *   - trainer-state tail (HAS_TRAINER_STATE): see blob_format.h
 */

#include "ml/ml.h"
#include "internal/ml/blob_format.h"

// ----------------------------------------------------------------------------
// Error → string
// ----------------------------------------------------------------------------

const char *
n00b_ml_err_str(n00b_ml_err_t err)
{
    switch (err) {
    case N00B_ML_ERR_BAD_MAGIC:   return "bad magic (not an n00b ML blob)";
    case N00B_ML_ERR_BAD_VERSION: return "unsupported blob format version";
    case N00B_ML_ERR_TRUNCATED:   return "blob ended mid-record (truncated)";
    case N00B_ML_ERR_BAD_HEADER:  return "blob header is internally inconsistent";
    case N00B_ML_ERR_NOT_TRAINER: return "blob has no trainer-state tail";
    default:                      return "unknown ml error";
    }
}

// ----------------------------------------------------------------------------
// Internal: build / parse the base blob
// ----------------------------------------------------------------------------

// Length of just the base portion (no optional tails).
size_t
_n00b_ml_base_blob_length(const n00b_ml_rules_t *rules,
                          const n00b_ml_model_t *model)
{
    size_t off = sizeof(n00b_ml_blob_header_t);
    size_t ng  = n00b_list_len(rules->groups);
    for (size_t i = 0; i < ng; i++) {
        n00b_ml_rule_group_t g = n00b_list_get(rules->groups, i);
        off += 8 + (size_t)g.name->u8_bytes;
    }
    off += (size_t)model->weights.length * sizeof(float);
    return off;
}

n00b_buffer_t *
n00b_ml_save(const n00b_ml_rules_t *rules,
             const n00b_ml_model_t *model)
{
    uint32_t num_groups = (uint32_t)n00b_list_len(rules->groups);

    size_t table_bytes = 0;
    for (uint32_t i = 0; i < num_groups; i++) {
        n00b_ml_rule_group_t g = n00b_list_get(rules->groups, i);
        table_bytes += 8 + (size_t)g.name->u8_bytes;
    }

    size_t weights_bytes = (size_t)model->weights.length * sizeof(float);
    size_t total_bytes   = sizeof(n00b_ml_blob_header_t)
                         + table_bytes + weights_bytes;

    n00b_buffer_t *buf = n00b_buffer_empty();
    n00b_buffer_resize(buf, total_bytes);
    char *p = buf->data;

    n00b_ml_blob_header_t hdr = {
        .magic         = N00B_ML_BLOB_MAGIC,
        .version       = N00B_ML_BLOB_VERSION,
        .flags         = 0,
        .num_subspaces = num_groups,
        .total_size    = rules->total_size,
        .reserved      = 0,
        .bias          = model->bias,
    };
    memcpy(p, &hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (uint32_t i = 0; i < num_groups; i++) {
        n00b_ml_rule_group_t g    = n00b_list_get(rules->groups, i);
        uint32_t             sz   = g.size;
        uint32_t             nlen = (uint32_t)g.name->u8_bytes;
        memcpy(p, &sz,   4); p += 4;
        memcpy(p, &nlen, 4); p += 4;
        if (nlen > 0) {
            memcpy(p, g.name->data, nlen);
            p += nlen;
        }
    }

    memcpy(p, model->weights.data, weights_bytes);
    return buf;
}

// Internal: parse just the base portion.  Returns true on success
// and fills out_rules/out_model.
bool
_n00b_ml_load_base(n00b_buffer_t    *blob,
                   n00b_ml_rules_t **out_rules,
                   n00b_ml_model_t **out_model,
                   uint16_t         *out_flags,
                   n00b_ml_err_t    *out_err)
{
    const char *p   = blob->data;
    size_t      rem = blob->byte_len;

    if (rem < sizeof(n00b_ml_blob_header_t)) {
        *out_err = N00B_ML_ERR_TRUNCATED;
        return false;
    }

    n00b_ml_blob_header_t hdr;
    memcpy(&hdr, p, sizeof(hdr));
    p   += sizeof(hdr);
    rem -= sizeof(hdr);

    if (hdr.magic != N00B_ML_BLOB_MAGIC) {
        *out_err = N00B_ML_ERR_BAD_MAGIC;
        return false;
    }
    if (hdr.version != N00B_ML_BLOB_VERSION) {
        *out_err = N00B_ML_ERR_BAD_VERSION;
        return false;
    }

    n00b_ml_rules_t *rules = n00b_ml_rules_new();
    uint64_t accum = 0;
    for (uint32_t i = 0; i < hdr.num_subspaces; i++) {
        if (rem < 8) {
            *out_err = N00B_ML_ERR_TRUNCATED;
            return false;
        }
        uint32_t sz, nlen;
        memcpy(&sz,   p,     4);
        memcpy(&nlen, p + 4, 4);
        p   += 8;
        rem -= 8;
        if (rem < nlen) {
            *out_err = N00B_ML_ERR_TRUNCATED;
            return false;
        }
        n00b_string_t *name = n00b_string_from_raw((char *)p, (int64_t)nlen);
        p   += nlen;
        rem -= nlen;
        n00b_ml_define_rule_group(rules, name, sz);
        accum += sz;
    }

    if (accum != hdr.total_size || rules->total_size != hdr.total_size) {
        *out_err = N00B_ML_ERR_BAD_HEADER;
        return false;
    }

    size_t weights_bytes = (size_t)hdr.total_size * sizeof(float);
    if (rem < weights_bytes) {
        *out_err = N00B_ML_ERR_TRUNCATED;
        return false;
    }

    n00b_ml_model_t *model = n00b_ml_model_new(hdr.total_size);
    model->bias            = hdr.bias;
    memcpy(model->weights.data, p, weights_bytes);

    *out_rules = rules;
    *out_model = model;
    *out_flags = hdr.flags;
    return true;
}

// ----------------------------------------------------------------------------
// Scorer save / load
// ----------------------------------------------------------------------------

n00b_buffer_t *
n00b_ml_scorer_save(n00b_ml_scorer_t *scorer)
{
    return n00b_ml_save(scorer->rules, scorer->model);
}

n00b_result_t(n00b_ml_scorer_t *)
n00b_ml_scorer_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; }
{
    n00b_ml_rules_t *rules;
    n00b_ml_model_t *model;
    uint16_t         flags;
    n00b_ml_err_t    err;
    if (!_n00b_ml_load_base(blob, &rules, &model, &flags, &err)) {
        return n00b_result_err(n00b_ml_scorer_t *, err);
    }
    return n00b_result_ok(n00b_ml_scorer_t *,
                          n00b_ml_scorer_new(.rules   = rules,
                                             .model   = model,
                                             .no_lock = no_lock));
}

// ----------------------------------------------------------------------------
// Trainer save / load
// ----------------------------------------------------------------------------

static bool
locate_trainer_state(const n00b_buffer_t   *blob,
                     const n00b_ml_rules_t *rules,
                     const n00b_ml_model_t *model,
                     uint16_t               flags,
                     size_t                *out_off)
{
    if (!(flags & N00B_ML_BLOB_FLAG_HAS_TRAINER_STATE)) return false;
    size_t off = _n00b_ml_base_blob_length(rules, model);
    if (flags & N00B_ML_BLOB_FLAG_HAS_DELTA) {
        off += sizeof(float)  // correction bias
             + (size_t)model->weights.length * sizeof(float);
    }
    if (blob->byte_len < off + sizeof(n00b_ml_blob_trainer_state_t)) {
        return false;
    }
    *out_off = off;
    return true;
}

n00b_buffer_t *
n00b_ml_trainer_save(n00b_ml_trainer_t *trainer)
{
    if (!trainer->sealed) {
        n00b_ml_trainer_seal(trainer);
    }
    n00b_data_read_lock(trainer->lock);

    n00b_buffer_t *blob = n00b_ml_save(trainer->rules, trainer->model);

    uint16_t flags;
    memcpy(&flags, blob->data + 6, 2);
    flags |= N00B_ML_BLOB_FLAG_HAS_TRAINER_STATE;
    memcpy(blob->data + 6, &flags, 2);

    n00b_ml_blob_trainer_state_t st = {
        .loss          = 0,                       // logistic only
        .learning_rate = trainer->learning_rate,
        .l2            = trainer->weight_decay,   // wire field name preserved
        .reserved      = 0,
        .observations  = trainer->observations,
    };

    size_t old_len = blob->byte_len;
    n00b_buffer_resize(blob, old_len + sizeof(st));
    memcpy(blob->data + old_len, &st, sizeof(st));

    n00b_data_unlock(trainer->lock);
    return blob;
}

n00b_result_t(n00b_ml_trainer_t *)
n00b_ml_trainer_load(n00b_buffer_t *blob)
    _kargs { bool no_lock = false; }
{
    n00b_ml_rules_t *rules;
    n00b_ml_model_t *model;
    uint16_t         flags;
    n00b_ml_err_t    err;
    if (!_n00b_ml_load_base(blob, &rules, &model, &flags, &err)) {
        return n00b_result_err(n00b_ml_trainer_t *, err);
    }

    if (!(flags & N00B_ML_BLOB_FLAG_HAS_TRAINER_STATE)) {
        return n00b_result_err(n00b_ml_trainer_t *, N00B_ML_ERR_NOT_TRAINER);
    }

    size_t off;
    if (!locate_trainer_state(blob, rules, model, flags, &off)) {
        return n00b_result_err(n00b_ml_trainer_t *, N00B_ML_ERR_TRUNCATED);
    }
    n00b_ml_blob_trainer_state_t st;
    memcpy(&st, blob->data + off, sizeof(st));

    n00b_ml_trainer_t *t = n00b_alloc(n00b_ml_trainer_t);
    t->rules         = rules;
    t->model         = model;
    t->learning_rate = st.learning_rate;
    t->weight_decay  = st.l2;
    t->observations  = st.observations;
    t->sealed        = true;
    t->lock          = no_lock ? nullptr : n00b_data_lock_new();

    return n00b_result_ok(n00b_ml_trainer_t *, t);
}
