/** @file core/time.c
 *  @brief Non-inline time helpers — currently just the ISO-8601 UTC
 *  formatter used by protocol envelopes (SKP ingestion `received_at`,
 *  AWS SigV4 `amz_date`, X.509 UTCTime) that need a deterministic
 *  textual stamp.
 *
 *  Inline helpers (timestamp capture, duration arithmetic, timespec
 *  comparison) stay in `include/core/time.h`.
 */

#include "n00b.h"
#include "core/time.h"
#include "core/string.h"

n00b_string_t *
n00b_iso8601_utc(n00b_duration_t *t)
{
    time_t    raw = t ? (time_t)t->tv_sec : (time_t)0;
    struct tm tm_utc;
    /* gmtime_r is POSIX-thread-safe and allocation-free; n00b has no
     * calendar-walk of its own, so this is the documented single
     * boundary for "broken-down UTC time" inside libn00b. */
    gmtime_r(&raw, &tm_utc);
    char buf[32];
    /* Buffer is conservatively sized for `YYYY-MM-DDTHH:MM:SSZ\0`
     * (21 bytes). The format string is fixed. */
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return n00b_string_from_raw(buf, (int64_t)n);
}
