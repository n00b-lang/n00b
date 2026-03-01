/*
 * Thin bridge for calling n00b_init()/n00b_shutdown() from ObjC code.
 *
 * n00b_init() uses _kargs, so it must be called from ncc-compiled code
 * that can generate the kargs struct.  This file is compiled through ncc
 * and exposes a plain C function the ObjC test can call.
 */
#include "n00b.h"
#include "core/runtime.h"

void
n00b_cocoa_bridge_init(void *rt_buf, int argc, char *argv[])
{
    n00b_init((n00b_runtime_t *)rt_buf, argc, argv);
}

void
n00b_cocoa_bridge_shutdown(void)
{
    n00b_shutdown();
}
