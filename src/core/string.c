#include "core/string.h"
#include "core/alloc.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "internal/text/unicode/raw.h"
#include "text/strings/string_ops.h"
#include <string.h>

/* Thread-local scratch pool for transparent intermediate-string
 * allocation.  When a string-builder is called without an explicit
 * @c .allocator kwarg, no @ref n00b_with_allocator scope is active,
 * and no outer string-builder already set up a scratch, the builder
 * stands up a per-thread scratch pool, lets all inner allocations
 * go into it (via @ref n00b_with_allocator), and at the outermost
 * exit deep-copies the result string out into the runtime's default
 * allocator and tears down the scratch — confining every transient
 * format / concat / etc. allocation that would otherwise pollute
 * the GC default arena.
 *
 * Nested builder calls share the outer scratch (created==false in
 * the inner scope token) so a single event handler that does many
 * cformat / from_cstr calls only stands up one scratch.
 *
 * The pool struct lives in thread-local storage; pool_destroy on
 * scope exit munmaps the page table but leaves the struct
 * zeroed-by-pool_init-ready for the next outermost entry. */
static thread_local n00b_pool_t __n00b_string_scratch_storage;
static thread_local n00b_pool_t *__n00b_string_scratch_pool = nullptr;

n00b_string_scope_t
n00b_string_scope_enter(n00b_allocator_t **resolved)
{
    /* Explicit allocator wins. */
    if (*resolved != nullptr) {
        return (n00b_string_scope_t){.created = false};
    }
    /* Active @ref n00b_with_allocator scope wins next. */
    n00b_allocator_t *ovr = n00b_current_allocator();
    if (ovr != nullptr) {
        *resolved = ovr;
        return (n00b_string_scope_t){.created = false};
    }
    /* Nested builder call: share the outer scratch. */
    if (__n00b_string_scratch_pool != nullptr) {
        *resolved = (n00b_allocator_t *)__n00b_string_scratch_pool;
        return (n00b_string_scope_t){.created = false};
    }
    /* Skip the scratch dance entirely if the runtime isn't ready
     * yet (pre-init or mid-shutdown) — pool_init needs runtime
     * infrastructure, and the caller's @ref n00b_ensure_allocator
     * fallback will land them on the runtime default once it
     * exists.  This is also the right call for the pre-init
     * `static` n00b_string_t descriptors emitted by the ncc
     * static-image transform — they're built into the binary, not
     * dynamically allocated. */
    n00b_runtime_t *rt = n00b_get_runtime();
    if (rt == nullptr) {
        return (n00b_string_scope_t){.created = false};
    }
    /* Outermost: stand up a fresh scratch.  hidden + no external
     * metadata so the GC's metadata-pool walk doesn't pick it up
     * (the whole point is to keep this off the GC's plate). */
    n00b_pool_init(&__n00b_string_scratch_storage,
                   .hidden = true,
                   .name   = "n00b_string_scratch");
    __n00b_string_scratch_pool = &__n00b_string_scratch_storage;
    *resolved = (n00b_allocator_t *)__n00b_string_scratch_pool;
    return (n00b_string_scope_t){.created = true};
}

n00b_string_t *
n00b_string_scope_exit(n00b_string_scope_t *scope, n00b_string_t *result)
{
    if (!scope->created) {
        return result;
    }
    /* Copy the result OUT of the scratch into the runtime default
     * allocator before the scratch dies.  Use the lowest-level
     * @ref n00b_alloc_with_opts primitives so this copy does NOT
     * re-enter the string-builder API and recurse back through
     * @ref n00b_string_scope_enter (which would create another
     * scratch, copy again, and infinite-loop).  Layout matches
     * @c struct n00b_string_t verbatim — char *data, size_t
     * u8_bytes, size_t codepoints, void *styling.  The .styling
     * pointer is shallow-copied: it lives outside the scratch
     * already (styles are managed by the styling subsystem). */
    n00b_string_t *durable = result;
    if (result != nullptr) {
        /* IMPORTANT: durable must NOT be no_scan. n00b_string_t has
         * pointer fields (.data, .styling) that the copying GC needs
         * to update when their backing buffers are forwarded into
         * to-space. The earlier @c .no_scan=true flag here caused
         * post-collection dangling pointers, surfacing as SIGSEGV in
         * @c wax_text_has_value reading freed bytes via the stale
         * @c .data pointer. The char-buffer beneath is still no_scan
         * — that's correct, it holds raw bytes, no pointers. */
        durable = n00b_alloc_with_opts(
            n00b_string_t,
            &(n00b_alloc_opts_t){.allocator = nullptr});
        durable->u8_bytes   = result->u8_bytes;
        durable->codepoints = result->codepoints;
        durable->styling    = result->styling;
        if (result->data != nullptr && result->u8_bytes > 0) {
            char *bytes = n00b_alloc_array_with_opts(
                char,
                result->u8_bytes + 1,
                &(n00b_alloc_opts_t){.allocator = nullptr,
                                     .no_scan   = true});
            memcpy(bytes, result->data, result->u8_bytes);
            bytes[result->u8_bytes] = '\0';
            durable->data           = bytes;
        }
        else {
            /* Empty-string case: result->data was either a static
             * literal (e.g. "" from @ref n00b_string_empty) or a
             * scratch-pool allocation. Static is safe to point at
             * directly; scratch would be freed below. Resolve to a
             * known-safe empty string via a fresh 1-byte allocation. */
            char *bytes = n00b_alloc_array_with_opts(
                char,
                1,
                &(n00b_alloc_opts_t){.allocator = nullptr,
                                     .no_scan   = true});
            bytes[0]      = '\0';
            durable->data = bytes;
        }
    }
    n00b_pool_t *pool          = __n00b_string_scratch_pool;
    __n00b_string_scratch_pool = nullptr;
    if (pool != nullptr) {
        n00b_allocator_destroy((n00b_allocator_t *)pool);
    }
    return durable;
}

void
n00b_string_init(n00b_string_t *self)
    _kargs {
        const char          *src       = nullptr;
        int64_t              byte_len  = -1;
        n00b_allocator_t    *allocator = nullptr;
        int64_t             *cp_count  = nullptr;
        n00b_gc_scan_kind_t  scan_kind = N00B_GC_SCAN_KIND_NONE;
        n00b_gc_scan_cb_t    scan_cb   = nullptr;
        void                *scan_user = nullptr;
    }
{
    (void)src;
    (void)byte_len;
    (void)allocator;
    (void)cp_count;
    (void)scan_kind;
    (void)scan_cb;
    (void)scan_user;

    n00b_ensure_allocator(kargs->allocator);

    int64_t len = kargs->byte_len;

    if (len < 0 && kargs->src) {
        const char *p = kargs->src;
        while (*p) {
            p++;
        }
        len = (int64_t)(p - kargs->src);
    }
    else if (len < 0) {
        len = 0;
    }

    self->data = n00b_alloc_array_with_opts(
        char,
        (size_t)len + 1,
        &(n00b_alloc_opts_t){
            .allocator = kargs->allocator,
            .scan_kind = kargs->scan_kind,
            .scan_cb   = kargs->scan_cb,
            .scan_user = kargs->scan_user,
        });

    if (len > 0 && kargs->src) {
        memcpy(self->data, kargs->src, (size_t)len);
    }
    self->data[len] = '\0';
    self->u8_bytes  = len;
    self->codepoints =
        n00b_unicode_utf8_count_codepoints_raw(kargs->src, (uint32_t)len);

    if (kargs->cp_count) {
        *kargs->cp_count = (int64_t)self->codepoints;
    }
}

n00b_string_t *
n00b_string_from_raw(const char *src, int64_t byte_len)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        int64_t          *cp_count  = nullptr;
    }
{
    (void)cp_count;
    n00b_allocator_t   *resolved_allocator = allocator;
    n00b_string_scope_t scope              = n00b_string_scope_enter(
        &resolved_allocator);

    n00b_ensure_allocator(resolved_allocator);

    n00b_string_t *result = n00b_alloc_with_opts(
        n00b_string_t,
        &(n00b_alloc_opts_t){.allocator = resolved_allocator},
        n00b_kargs(string,
                   .src       = src,
                   .byte_len  = byte_len,
                   .allocator = resolved_allocator,
                   .cp_count  = kargs->cp_count));
    return n00b_string_scope_exit(&scope, result);
}

n00b_string_t *
n00b_ncc_rstr(const char *src)
{
    return n00b_string_from_cstr(src);
}

n00b_string_t *
n00b_string_from_cstr(const char *src)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    (void)allocator;
    if (!src) {
        return n00b_string_empty(.allocator = kargs->allocator);
    }

    const char *p = src;
    while (*p) {
        p++;
    }
    int64_t byte_len = (int64_t)(p - src);

    return n00b_string_from_raw(src, byte_len, .allocator = kargs->allocator);
}

n00b_string_t *
n00b_string_empty()
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    (void)allocator;
    return n00b_string_from_raw("", 0, .allocator = kargs->allocator);
}
