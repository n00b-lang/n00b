#include <stdint.h>

typedef int (*hook_fn)(void *context, int value);

static hook_fn installed;
static void *installed_context;

uint64_t
hook_subscribe(hook_fn hook, void *context)
{
    installed = hook;
    installed_context = context;
    return 7;
}

int
hook_fire(int value)
{
    return installed ? installed(installed_context, value) : -1;
}

uint64_t
hook_context(void)
{
    return (uint64_t)(uintptr_t)installed_context;
}

void
hook_unsubscribe(uint64_t id)
{
    if (id == 7) {
        installed = 0;
    }
}
