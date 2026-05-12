#include <unistd.h>

#include "n00b.h"
#include "core/exit.h"

static bool exiting           = false;
static int  saved_exit_code   = 0;
bool        n00b_abort_signal = false;

[[noreturn]] void
n00b_abort(void)
{
    saved_exit_code   = 139;
    exiting           = true;
    n00b_abort_signal = true;
    abort();
}

bool
n00b_current_process_is_exiting(void)
{
    return exiting;
}
