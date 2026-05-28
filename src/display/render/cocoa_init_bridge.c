/*
 * Thin bridge for calling n00b_init()/n00b_shutdown() from ObjC code.
 *
 * n00b_init() uses _kargs, so it must be called from ncc-compiled code
 * that can generate the kargs struct.  This file is compiled through ncc
 * and exposes a plain C function the ObjC test can call.
 */
#include "n00b.h"
#include "core/runtime.h"
#include "display/render/cell.h"

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

/*
 * n00b_alloc_array() is an ncc macro that Apple clang cannot expand, so
 * the ObjC backend allocates its GC-managed staging cell buffer through
 * this bridge.  Allocating with the concrete element type here preserves
 * the GC scan map for the n00b_text_style_t* embedded in each rcell.
 */
n00b_rcell_t *
n00b_cocoa_bridge_alloc_rcells(n00b_isize_t count)
{
    return n00b_alloc_array(n00b_rcell_t, count);
}
