#pragma once

#include <time.h> // IWYU pragma: keep
#include <sys/time.h>

#define N00B_USEC_PER_SEC 1000000
#define N00B_NSEC_PER_SEC 1000000000
#define N00B_MS_PER_SEC   1000
#define N00B_NS_PER_MS    1000000

static inline void
n00b_capture_timestamp(n00b_duration_t *output)
{
    clock_gettime(CLOCK_REALTIME, (struct timespec *)output);
    output->tv_nsec *= N00B_NS_PER_US;
}

static inline int64_t
n00b_ns_from_duration(n00b_duration_t *d)
{
    return d->tv_sec * N00B_NS_PER_SEC + d->tv_nsec;
}

static inline int64_t
n00b_ns_minus(n00b_duration_t *d1, n00b_duration_t *d2)
{
    int64_t ns1 = n00b_ns_from_duration(d1);
    int64_t ns2 = n00b_ns_from_duration(d2);

    return ns1 - ns2;
}

static inline int64_t
n00b_ns_timestamp(void)
{
    n00b_duration_t d;
    clock_gettime(CLOCK_MONOTONIC, (void *)&d);
    d.tv_nsec *= N00B_NS_PER_US;

    return n00b_ns_from_duration(&d);
}

static inline int64_t
n00b_us_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    return tv.tv_sec * N00B_USEC_PER_SEC * tv.tv_usec;
}

static inline bool
n00b_timestamp_gt(struct timespec *t1,
                  struct timespec *t2)
{
    if (t1->tv_sec > t2->tv_sec) {
        return true;
    }
    if (t1->tv_sec < t2->tv_sec) {
        return false;
    }

    return t1->tv_nsec > t2->tv_nsec;
}

static inline void
n00b_timestamp_diff(struct timespec *t1,
                    struct timespec *t2,
                    struct timespec *result)
{
    struct timespec *b, *l;

    if (n00b_timestamp_gt(t1, t2)) {
        b = t1;
        l = t2;
    }
    else {
        b = t2;
        l = t1;
    }
    result->tv_nsec = b->tv_nsec - l->tv_nsec;
    result->tv_sec  = b->tv_sec - l->tv_sec;

    if (result->tv_nsec < 0) {
        result->tv_nsec += N00B_NSEC_PER_SEC;
        result->tv_sec -= 1;
    }
}
