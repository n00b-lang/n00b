/** @file src/hostmeta/arn.c — Amazon Resource Name split / rebuild.
 *
 *  `arn:PARTITION:SERVICE:REGION:ACCOUNT:RESOURCE`
 *
 *  The resource field keeps every remaining `:`, because resource
 *  separators are service-specific (`function:name:version`,
 *  `log-group:g:log-stream:s`) and re-splitting them here would just
 *  force the callers to glue them back together.
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_aws.h"

#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"

n00b_option_t(n00b_hostmeta_arn_t *)
    n00b_hostmeta_arn_parse(n00b_string_t *s)
{
    if (s == nullptr || !n00b_unicode_str_starts_with(s, r"arn:")) {
        return n00b_option_none(n00b_hostmeta_arn_t *);
    }

    // Five separators bound the first five fields; everything after the
    // fifth belongs to the resource.
    size_t seps[5];
    int    found = 0;
    for (size_t i = 0; i < s->u8_bytes && found < 5; i++) {
        if (s->data[i] == ':') {
            seps[found++] = i;
        }
    }
    if (found < 5) {
        return n00b_option_none(n00b_hostmeta_arn_t *);
    }

    n00b_hostmeta_arn_t *arn = n00b_alloc(n00b_hostmeta_arn_t);

    arn->partition = n00b_string_from_raw(s->data + seps[0] + 1,
                                          (int64_t)(seps[1] - seps[0] - 1));
    arn->service   = n00b_string_from_raw(s->data + seps[1] + 1,
                                        (int64_t)(seps[2] - seps[1] - 1));
    arn->region    = n00b_string_from_raw(s->data + seps[2] + 1,
                                       (int64_t)(seps[3] - seps[2] - 1));
    arn->account   = n00b_string_from_raw(s->data + seps[3] + 1,
                                        (int64_t)(seps[4] - seps[3] - 1));
    arn->resource  = n00b_string_from_raw(s->data + seps[4] + 1,
                                         (int64_t)(s->u8_bytes - seps[4] - 1));

    return n00b_option_set(n00b_hostmeta_arn_t *, arn);
}

n00b_string_t *
n00b_hostmeta_arn_format(n00b_hostmeta_arn_t *arn)
{
    if (arn == nullptr) {
        return nullptr;
    }
    return n00b_cformat("arn:[|#|]:[|#|]:[|#|]:[|#|]:[|#|]",
                        arn->partition,
                        arn->service,
                        arn->region,
                        arn->account,
                        arn->resource);
}

n00b_hostmeta_arn_t *
n00b_hostmeta_arn_with(n00b_hostmeta_arn_t *arn) _kargs
{
    n00b_string_t *partition = nullptr;
    n00b_string_t *service   = nullptr;
    n00b_string_t *region    = nullptr;
    n00b_string_t *account   = nullptr;
    n00b_string_t *resource  = nullptr;
}
{
    if (arn == nullptr) {
        return nullptr;
    }

    n00b_hostmeta_arn_t *out = n00b_alloc(n00b_hostmeta_arn_t);

    out->partition = partition ? partition : arn->partition;
    out->service   = service ? service : arn->service;
    out->region    = region ? region : arn->region;
    out->account   = account ? account : arn->account;
    out->resource  = resource ? resource : arn->resource;

    return out;
}
