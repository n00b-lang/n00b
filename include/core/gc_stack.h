/**
 * @file gc_stack.h
 * @brief Compiler-published exact stack roots for the GC.
 *
 * The default collector path remains conservative.  Code generated or
 * instrumented with stack maps can publish a per-thread frame chain so the
 * collector can scan only declared root slots at STW safepoints.
 */
#pragma once

#include "n00b.h"

// The public GC stack-map ABI is declared in n00b.h so compiler-generated
// code can include the umbrella header only. This header remains the stable
// internal include path for runtime code that works on stack maps directly.
