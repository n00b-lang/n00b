#include <stdint.h>

typedef int (*hook_fn)(void *context, int value);

static hook_fn installed;
static void *installed_context;

uint64_t
hook_subscribe(hook_fn hook, void *context)
{
    installed = hook;
    installed_context = context;
    return 1;
}

int
hook_fire(int value)
{
    return installed ? installed(installed_context, value) : -1;
}

void
hook_unsubscribe(uint64_t id)
{
    (void)id;
    installed = 0;
    installed_context = 0;
}
