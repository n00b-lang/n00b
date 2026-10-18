#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct Event {
    int code;
} Event;

typedef int (*Compare)(const void *a, const void *b);
typedef void (*Handler)(void *context, Event *event);

static Handler subscribed;
static void *subscribed_context;

int
sort_bytes(const uint8_t *items, size_t count, Compare compare)
{
    uint8_t *copy = malloc(count ? count : 1);
    for (size_t i = 0; i < count; i++) {
        copy[i] = items[i];
    }
    qsort(copy, count, 1, compare);
    int sorted = 1;
    for (size_t i = 1; i < count; i++) {
        sorted = sorted && copy[i - 1] >= copy[i];
    }
    free(copy);
    return sorted;
}

uint64_t
bus_subscribe(Handler handler, void *context)
{
    subscribed = handler;
    subscribed_context = context;
    return 1;
}

void
bus_publish(int code)
{
    Event event = {code};
    if (subscribed) {
        subscribed(subscribed_context, &event);
    }
}

void
bus_unsubscribe(uint64_t id)
{
    (void)id;
    subscribed = NULL;
    subscribed_context = NULL;
}
