#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/gc_map.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/type_info.h"
#include "adt/dict_untyped.h"
#include "n00b/embed_ffi.h"

typedef struct test_default_static_policy_t {
    int value;
} test_default_static_policy_t;

#ifndef _WIN32
typedef struct n00b_subproc n00b_subproc_t;
#endif

typedef struct test_plain_static_policy_t {
    uint64_t value;
} test_plain_static_policy_t;

typedef struct test_forbidden_static_policy_t {
    uint64_t value;
} test_forbidden_static_policy_t;

typedef struct test_fixup_static_policy_t {
    void *ptr;
} test_fixup_static_policy_t;

typedef struct test_constructor_static_policy_t {
    void *ptr;
} test_constructor_static_policy_t;

static n00b_static_layout_info_t *
require_layout(uint64_t type_hash)
{
    auto layout_opt = n00b_type_static_layout(type_hash);
    assert(n00b_option_is_set(layout_opt));
    return n00b_option_get(layout_opt);
}

static void
test_builtin_static_layout_policies(void)
{
    n00b_static_layout_info_t *layout = require_layout(typehash(uint64_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_PLAIN);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(layout->reason == nullptr);
    assert(n00b_type_static_layout_allowed(typehash(uint64_t *)));

    layout = require_layout(typehash(n00b_string_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_PLAIN);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_ALL);
    assert(n00b_type_static_layout_allowed(typehash(n00b_string_t *)));

    layout = require_layout(typehash(n00b_buffer_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(layout->reason != nullptr);
    assert(strstr(layout->reason, "buffer") != nullptr);
    assert(n00b_type_static_layout_allowed(typehash(n00b_buffer_t *)));

    layout = require_layout(typehash(n00b_dict_untyped_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_DEFAULT_DENY);
    assert(layout->reason != nullptr);
    assert(strstr(layout->reason, "dictionary") != nullptr);
    assert(!n00b_type_static_layout_allowed(typehash(n00b_dict_untyped_t *)));

    layout = require_layout(typehash(n00b_ffi_module_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_TRANSIENT);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(layout->reason != nullptr);
    assert(strstr(layout->reason, "ffi") != nullptr);
    assert(!n00b_type_static_layout_allowed(typehash(n00b_ffi_module_t *)));

#ifndef _WIN32
    layout = require_layout(typehash(n00b_subproc_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_TRANSIENT);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(layout->reason != nullptr);
    assert(strstr(layout->reason, "subprocess") != nullptr);
    assert(!n00b_type_static_layout_allowed(typehash(n00b_subproc_t *)));
#endif

    printf("  [PASS] builtin static layout policies\n");
}

static void
test_default_deny_for_unannotated_registration(void)
{
    bool ok = N00B_TYPE_REGISTER(test_default_static_policy_t);
    assert(ok);

    n00b_static_layout_info_t *layout = require_layout(typehash(test_default_static_policy_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_DEFAULT_DENY);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_DEFAULT);
    assert(layout->reason == nullptr);
    assert(!n00b_type_static_layout_allowed(typehash(test_default_static_policy_t *)));

    printf("  [PASS] unannotated registration defaults to deny\n");
}

static void
test_explicit_allowed_policy_macros(void)
{
    bool ok = N00B_TYPE_REGISTER(test_plain_static_policy_t,
        N00B_TYPE_STATIC_PLAIN(N00B_GC_SCAN_KIND_NONE),
    );
    assert(ok);

    n00b_static_layout_info_t *layout = require_layout(typehash(test_plain_static_policy_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_PLAIN);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(n00b_type_static_layout_allowed(typehash(test_plain_static_policy_t *)));

    ok = N00B_TYPE_REGISTER(test_fixup_static_policy_t,
        N00B_TYPE_STATIC_FIXUP(N00B_GC_SCAN_KIND_CALLBACK, "runtime pointer fixup required"),
    );
    assert(ok);

    layout = require_layout(typehash(test_fixup_static_policy_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_FIXUP);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_CALLBACK);
    assert(strcmp(layout->reason, "runtime pointer fixup required") == 0);
    assert(n00b_type_static_layout_allowed(typehash(test_fixup_static_policy_t *)));

    ok = N00B_TYPE_REGISTER(test_constructor_static_policy_t,
        N00B_TYPE_STATIC_CONSTRUCTOR_IMAGE(N00B_GC_SCAN_KIND_ALL, "constructor image available"),
    );
    assert(ok);

    layout = require_layout(typehash(test_constructor_static_policy_t *));
    assert(layout->policy == N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE);
    assert(layout->scan_kind == N00B_GC_SCAN_KIND_ALL);
    assert(strcmp(layout->reason, "constructor image available") == 0);
    assert(n00b_type_static_layout_allowed(typehash(test_constructor_static_policy_t *)));

    printf("  [PASS] explicit allowed policy macros\n");
}

static void
test_denial_reason_is_deep_copied(void)
{
    char reason[64] = "capability state cannot be static";

    bool ok = n00b_type_register(
        typehash(test_forbidden_static_policy_t *),
        &(n00b_type_info_t){
            .name          = "test_forbidden_static_policy_t",
            .alloc_len     = sizeof(test_forbidden_static_policy_t),
            .static_layout = {
                .policy    = N00B_STATIC_LAYOUT_FORBIDDEN,
                .scan_kind = N00B_GC_SCAN_KIND_NONE,
                .reason    = reason,
            },
        });
    assert(ok);

    n00b_static_layout_info_t *layout = require_layout(typehash(test_forbidden_static_policy_t *));
    const char *copied_reason = layout->reason;
    assert(copied_reason != nullptr);
    assert(copied_reason != reason);
    assert(strcmp(copied_reason, "capability state cannot be static") == 0);

    strcpy(reason, "mutated local reason");
    assert(strcmp(copied_reason, "capability state cannot be static") == 0);
    assert(!n00b_type_static_layout_allowed(typehash(test_forbidden_static_policy_t *)));

    printf("  [PASS] denial reason deep copy\n");
}

static void
test_unregistered_and_policy_names(void)
{
    uint64_t missing_hash = UINT64_C(0xF00DD00DCAFE1234);
    assert(!n00b_option_is_set(n00b_type_static_layout(missing_hash)));
    assert(!n00b_type_static_layout_allowed(missing_hash));

    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_DEFAULT_DENY), "default-deny") == 0);
    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_FORBIDDEN), "forbidden") == 0);
    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_TRANSIENT), "transient") == 0);
    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_PLAIN), "plain") == 0);
    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_FIXUP), "fixup") == 0);
    assert(strcmp(n00b_static_layout_policy_name(N00B_STATIC_LAYOUT_CONSTRUCTOR_IMAGE), "constructor-image") == 0);
    assert(strcmp(n00b_static_layout_policy_name((n00b_static_layout_policy_t)255), "unknown") == 0);

    printf("  [PASS] unregistered lookup and policy names\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running type static-layout policy tests...\n");

    test_builtin_static_layout_policies();
    test_default_deny_for_unannotated_registration();
    test_explicit_allowed_policy_macros();
    test_denial_reason_is_deep_copied();
    test_unregistered_and_policy_names();

    printf("All type static-layout policy tests passed.\n");
    n00b_shutdown();
    return 0;
}
