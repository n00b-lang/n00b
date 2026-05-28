#pragma once

#ifndef _WIN32

#include <signal.h>

// Normalize waitid()/siginfo_t exit data into a waitpid()-style status word.
extern int n00b_conduit_proc_wait_status_from_siginfo(const siginfo_t *info);

#endif
