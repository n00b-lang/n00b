/*
 * thread_lifecycle.c — Thread lifecycle event tracking.
 *
 * Publishes thread start/death events on conduit topics using the n00b
 * thread system.  Deferred until the n00b thread lifecycle callback
 * infrastructure is available — this file provides stub implementations.
 */

#include "conduit/conduit.h"
#include "core/thread.h"
#include "core/platform.h"
#include <string.h>
#include <stdio.h>

// The full lifecycle tracking requires hooks into the n00b thread
// library that don't exist yet.  For now, provide the basic structure
// that higher layers (IO backends, managed FD) can build on.
//
// TODO: Wire into n00b thread lifecycle callbacks when available.
