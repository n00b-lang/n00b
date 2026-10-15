#include <stdio.h>

#include "callbacks_writ.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static int
descending(const void *a, const void *b)
{
    return *(const uint8_t *)b - *(const uint8_t *)a;
}

static void
record(const void *context, const Event *event)
{
    *(int *)context = event->code;
}

int
main(void)
{
    const uint8_t items[] = {3, 9, 1, 7};
    int sorted = 0;
    expect(callbacks_sort_bytes(items, sizeof items, descending, &sorted) == callbacks_sort_bytes_ok && sorted == 1,
           "qsort calls the comparator");
    int seen = 0;
    uint64_t id = callbacks_bus_subscribe(record, &seen);
    expect(callbacks_bus_publish(42) == callbacks_bus_publish_ok && seen == 42, "the handler gets its context");
    expect(callbacks_bus_unsubscribe(id) == callbacks_bus_unsubscribe_ok, "unsubscribe");
    seen = 0;
    callbacks_bus_publish(7);
    expect(seen == 0, "no handler after unsubscribe");
    return failures != 0;
}
