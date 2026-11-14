#include "compiler_axioms.h"
#include "lib/dict.h"

#include <pthread.h>

#define NATIVE(name_, native_, effects_, reads_, writes_)                  \
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC(name_),        \
     .native = CORE_STRING_STATIC(native_), .effects = effects_,           \
     .reads = reads_, .writes = writes_, .returns = IR_SRETURN_NONE}
#define COLLECTION_FREE_NATIVE(name_, native_, effects_, reads_, writes_)  \
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC(name_),        \
     .native = CORE_STRING_STATIC(native_), .effects = effects_,           \
     .reads = reads_, .writes = writes_, .collection_free = true,         \
     .returns = IR_SRETURN_NONE}
#define RETURNING_NATIVE(name_, native_, collection_free_, returns_)       \
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC(name_),        \
     .native = CORE_STRING_STATIC(native_),                                \
     .collection_free = collection_free_, .returns = returns_}
#define EXTERNAL_NATIVE(name_, native_, effects_)                          \
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC(name_),        \
     .native = CORE_STRING_STATIC(native_), .effects = effects_,           \
     .returns = IR_SRETURN_FRESH_EXTERNAL}
#define FUNCTION(module_, name_, writes_, roles_)                          \
    {.key = N00BB_AXIOM_FUNCTION, .module = CORE_STRING_STATIC(module_),  \
     .name = CORE_STRING_STATIC(name_), .writes = writes_,                \
     .roles = roles_}
#define BINDING(name_, roles_)                                             \
    {.key = N00BB_AXIOM_BINDING, .name = CORE_STRING_STATIC(name_),       \
     .writes = 0x1, .roles = roles_}
#define NATIVE_INPUT (IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_INPUT)
#define NATIVE_OUTPUT (IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_OUTPUT)
#define NATIVE_BLOCKING_INPUT (NATIVE_INPUT | IR_SUMMARY_MAY_BLOCK)
#define NATIVE_BLOCKING_OUTPUT (NATIVE_OUTPUT | IR_SUMMARY_MAY_BLOCK)
#define NATIVE_ENTERS (IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE       \
                       | IR_SUMMARY_MAY_BLOCK | NATIVE_INPUT | NATIVE_OUTPUT)

static const n00bb_axiom_t axioms[] = {
    RETURNING_NATIVE("__bitptr", "n00bb_bitptr", true, IR_SRETURN_PARAM),
    NATIVE("__os_page_size", "n00b_os_page_size", 0, 0, 0),
    NATIVE("__gc_enter_blocking", "n00b_os_gc_enter_blocking",
           IR_SUMMARY_MAY_BLOCK, 0, 0),
    NATIVE("__gc_leave_blocking",
           "sys.internal.gc.alloc.leave_blocking_runtime",
           IR_SUMMARY_MAY_BLOCK, 0, 0),
    NATIVE("__os_physical_memory", "n00b_os_physical_memory",
           IR_SUMMARY_READS_ENV, 0, 0),
    // Input, not pure: errno means something only right after the call that
    // set it, so reads must not be merged or moved.
    NATIVE("__os_errno", "n00b_os_errno", NATIVE_INPUT, 0, 0),
    NATIVE("__comptime_freeze", "n00b_comptime_freeze", NATIVE_OUTPUT, 0, 0),
    NATIVE("__comptime_frozen", "n00b_comptime_frozen", 0, 0, 0),
    NATIVE("__os_futex_wait", "n00b_os_futex_wait", IR_SUMMARY_MAY_BLOCK,
           0x1, 0),
    NATIVE("__os_futex_wake_all", "n00b_os_futex_wake_all", 0, 0, 0),
    NATIVE("__write_bytes", "n00bb_runtime_write_bytes",
           NATIVE_BLOCKING_OUTPUT, 0x2, 0),
    NATIVE("__os_thread_fence", "n00b_os_thread_fence", 0, 0, 0),
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC("__os_thread_start"),
     .native = CORE_STRING_STATIC("n00b_os_thread_start"),
     .effects = IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
         | IR_SUMMARY_MAY_BLOCK | IR_SUMMARY_STARTS_THREADS,
     .entries = 0x1,
     .returns = IR_SRETURN_NONE},
    COLLECTION_FREE_NATIVE("__effect_query", "n00bb_effect_query", 0, 0, 0),
    COLLECTION_FREE_NATIVE("__function_has_effect",
                           "n00bb_function_has_effect", 0, 0, 0),
    COLLECTION_FREE_NATIVE("__function_reads", "n00bb_function_reads", 0, 0,
                           0),
    COLLECTION_FREE_NATIVE("__function_writes", "n00bb_function_writes", 0,
                           0, 0),
    COLLECTION_FREE_NATIVE("__unobserved", "n00bb_unobserved", 0, 0, 0),
    NATIVE("__os_thread_stack_high_self", "n00b_os_thread_stack_high_self",
           0, 0, 0),
    NATIVE("__os_stack_address", "n00b_os_stack_address", 0, 0, 0),
    NATIVE("__os_stack_red_zone_bytes", "n00b_os_stack_red_zone_bytes", 0,
           0, 0),
    NATIVE("__os_frame_pointer_chain", "n00b_os_frame_pointer_chain", 0, 0,
           0),
    NATIVE("__os_thread_control_self", "n00b_os_thread_control_self", 0, 0,
           0),
    NATIVE("__os_thread_capture_self", "n00b_os_thread_capture_self", 0, 0,
           0x1),
    NATIVE("__os_thread_suspend", "n00b_os_thread_suspend",
           IR_SUMMARY_MAY_BLOCK, 0, 0x2),
    NATIVE("__os_thread_resume", "n00b_os_thread_resume", 0, 0x2, 0),
    NATIVE("__os_now_ns", "n00b_os_now_ns", IR_SUMMARY_READS_CLOCK, 0, 0),
    NATIVE("__os_cpu_relax", "n00b_os_cpu_relax", 0, 0, 0),
    NATIVE("__report_failure", "n00bb_runtime_report_failure",
           IR_SUMMARY_MAY_TRAP | IR_SUMMARY_DIVERGES, 0, 0),
    RETURNING_NATIVE("__os_map_zeroed", "n00b_os_map_zeroed", false,
                     IR_SRETURN_FRESH_EXTERNAL),
    NATIVE("__os_unmap", "n00b_os_unmap", 0, 0, 0x1),
    NATIVE("__os_protect", "n00b_os_protect", 0, 0, 0x1),
    NATIVE("__abi_type", "n00b_builtin_abi_type", 0, 0, 0),
    NATIVE("__abi_field", "n00b_builtin_abi_field", 0, 0, 0),
    NATIVE("__abi_arm", "n00b_builtin_abi_arm", 0, 0, 0),
    NATIVE("__abi_physical_run", "n00b_builtin_abi_physical_run", 0, 0, 0),
    NATIVE("__abi_trace", "n00b_builtin_abi_trace", 0, 0, 0),
    NATIVE("__abi_trace_child", "n00b_builtin_abi_trace_child", 0, 0, 0),
    NATIVE("__abi_trace_arm", "n00b_builtin_abi_trace_arm", 0, 0, 0),
    NATIVE("__abi_tag_extractor", "n00b_builtin_abi_tag_extractor", 0, 0, 0),
    NATIVE("__abi_layout", "n00b_builtin_abi_layout", 0, 0, 0),
    NATIVE("__abi_frame_layout", "n00b_builtin_abi_frame_layout", 0, 0, 0),
    NATIVE("__abi_storage", "n00b_builtin_abi_storage", 0, 0, 0),
    NATIVE("__abi_tls_relocation", "n00b_builtin_abi_tls_relocation", 0, 0,
           0),
    NATIVE("__abi_symbol", "n00b_builtin_abi_symbol", 0, 0, 0),
    NATIVE("__abi_module", "n00b_builtin_abi_module", 0, 0, 0),
    NATIVE("__abi_baked_node", "n00b_builtin_abi_baked_node", 0, 0, 0),
    NATIVE("__abi_function", "n00b_builtin_abi_function", 0, 0, 0),
    NATIVE("__abi_type_for", "n00b_builtin_abi_type_for", 0, 0, 0),
    NATIVE("__abi_baked_address", "n00b_abi_baked_address", 0, 0, 0),
    NATIVE("__native_symbol_name", "n00b_builtin_native_symbol_name", 0, 0,
           0),
    NATIVE("__abi_native_frame_count", "n00b_builtin_abi_native_frame_count",
           0, 0, 0),
    NATIVE("__abi_native_frame", "n00b_builtin_abi_native_frame", 0, 0, 0),
    NATIVE("__abi_native_frame_root", "n00b_builtin_abi_native_frame_root",
           0, 0, 0),
    NATIVE("__abi_storage_count", "n00b_builtin_abi_storage_count", 0, 0, 0),
    NATIVE("__abi_storage_address", "n00b_builtin_abi_storage_address", 0, 0,
           0),
    NATIVE("__abi_baked_node_count", "n00b_builtin_abi_baked_node_count", 0,
           0, 0),
    NATIVE("__abi_baked_node_address", "n00b_builtin_abi_baked_node_address",
           0, 0, 0),
    NATIVE("__abi_string_backing_at", "n00b_builtin_abi_string_backing_at", 0,
           0x1, 0),
    NATIVE("__abi_range_index", "n00b_builtin_abi_range_index", 0, 0, 0),
    NATIVE("__abi_table_base", "n00b_builtin_abi_table_base", 0, 0, 0),
    NATIVE("__abi_table_count", "n00b_builtin_abi_table_count", 0, 0, 0),
    NATIVE("__abi_table_record_bytes", "n00b_builtin_abi_table_record_bytes",
           0, 0, 0),
    NATIVE("__abi_unary_type", "n00b_builtin_abi_unary_type", 0, 0, 0),
    NATIVE("__abi_type_specializes", "n00b_builtin_abi_type_specializes", 0,
           0, 0),
    NATIVE("__abi_type_argument_type", "n00b_builtin_abi_type_argument_type",
           0, 0, 0),
    NATIVE("__abi_named_type", "n00b_builtin_abi_named_type", 0, 0, 0),
    COLLECTION_FREE_NATIVE("__dispatch_arm", "n00b_builtin_dispatch_arm", 0, 0,
                           0),
    COLLECTION_FREE_NATIVE("__dispatch_record", "n00b_builtin_dispatch_record",
                           0, 0, 0),
    RETURNING_NATIVE("__box_of", "n00bb_box_of", false, IR_SRETURN_PARAM),
    NATIVE("__result_box", "n00bb_result_box", 0, 0, 0),
    RETURNING_NATIVE("__box_as", "n00bb_box_as", false, IR_SRETURN_UNKNOWN),
    NATIVE("__abi_bytes", "n00b_builtin_abi_bytes", 0, 0, 0),
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC("__gc_allocate_bytes"),
     .native = CORE_STRING_STATIC("n00b_gc_allocate_bytes"),
     .effects = IR_SUMMARY_MAY_TRAP | IR_SUMMARY_MAY_ALLOCATE
         | IR_SUMMARY_ALLOCATES_DIRECT,
     .returns = IR_SRETURN_FRESH},
    COLLECTION_FREE_NATIVE("__abi_ref_address",
                           "n00b_builtin_abi_ref_address", 0, 0, 0),
    COLLECTION_FREE_NATIVE("__load_le_u64", "n00bb_load_le_u64", 0, 0x1, 0),
    COLLECTION_FREE_NATIVE("__load_le_u32", "n00bb_load_le_u32", 0, 0x1, 0),
    NATIVE("__gc_invoke_finalizer", "n00b_gc_finalize",
           NATIVE_ENTERS | IR_SUMMARY_CALLS_FINALIZERS, 0, 0),
    NATIVE("__finalize_boxed", "n00bb_finalize_boxed",
           NATIVE_ENTERS | IR_SUMMARY_CALLS_FINALIZERS, 0, 0),
    NATIVE("__ref_address_boxed", "n00bb_ref_address_boxed",
           NATIVE_ENTERS, 0x2, 0),
    NATIVE("__ref_address_mut_boxed", "n00bb_ref_address_mut_boxed",
           NATIVE_ENTERS, 0x2, 0),
    NATIVE("__array_length", "__n00bb_array_length", 0, 0x1, 0),
    {.key = N00BB_AXIOM_NATIVE, .name = CORE_STRING_STATIC("__array_prefix"),
     .native = CORE_STRING_STATIC("__n00bb_array_prefix"),
     .effects = IR_SUMMARY_MAY_ALLOCATE,
     .reads = 0x1, .holds = 0x1, .returns = IR_SRETURN_FRESH},
    NATIVE("__array_clear", "__n00bb_array_clear", 0, 0, 0x1),
    NATIVE("__ffi_borrow", "n00b_ffi_borrow", 0, 0x1, 0),
    NATIVE("__ffi_release", "n00b_ffi_release", 0, 0, 0),
    NATIVE("__ffi_unwind_jump", "n00b_ffi_unwind_jump", IR_SUMMARY_DIVERGES,
           0, 0),
    FUNCTION("sys.internal.gc.alloc", "allocate_runtime_known_on_mutator", 0,
             N00BB_AXIOM_MUTATOR_ALLOCATOR),
    FUNCTION("sys.internal.gc.alloc", "allocate_array_on_mutator", 0,
             N00BB_AXIOM_MUTATOR_ALLOCATOR),
    FUNCTION("sys.internal.gc.alloc", "mutator_for_native", 0,
             N00BB_AXIOM_MUTATOR_GETTER),
    FUNCTION("builtin.containers.buffer", "write_lock", 0x1, 0),
    FUNCTION("sys.internal.locks", "mutex_lock", 0, N00BB_AXIOM_LOCK_ACQUIRE),
    FUNCTION("sys.internal.locks", "mutex_unlock", 0,
             N00BB_AXIOM_LOCK_RELEASE),
    FUNCTION("sys.internal.locks", "mutex_try_lock", 0,
             N00BB_AXIOM_LOCK_SYNC),
    FUNCTION("sys.internal.locks", "condition_lock", 0,
             N00BB_AXIOM_LOCK_ACQUIRE),
    FUNCTION("sys.internal.locks", "condition_unlock", 0,
             N00BB_AXIOM_LOCK_RELEASE),
    FUNCTION("sys.internal.locks", "condition_wait", 0,
             N00BB_AXIOM_LOCK_SYNC),
    FUNCTION("sys.internal.locks", "condition_notify", 0,
             N00BB_AXIOM_LOCK_SYNC),
    FUNCTION("sys.internal.locks", "rw_lock_init", 0, N00BB_AXIOM_LOCK_SYNC),
    FUNCTION("sys.internal.locks", "rw_read_lock", 0,
             N00BB_AXIOM_LOCK_ACQUIRE),
    FUNCTION("sys.internal.locks", "rw_write_lock", 0,
             N00BB_AXIOM_LOCK_ACQUIRE),
    FUNCTION("sys.internal.locks", "rw_try_write_lock", 0,
             N00BB_AXIOM_LOCK_SYNC),
    FUNCTION("sys.internal.locks", "rw_unlock", 0, N00BB_AXIOM_LOCK_RELEASE),
    BINDING("n00bb.atomic.store", 0),
    BINDING("n00bb.atomic.store_relaxed", 0),
    BINDING("n00bb.atomic.store_release", 0),
    BINDING("n00bb.atomic.exchange", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.exchange_acquire", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.exchange_relaxed", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.compare_exchange", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_or", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_or_release", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_or_relaxed", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_and", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_add", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_add_relaxed", N00BB_AXIOM_READS_TARGET),
    BINDING("n00bb.atomic.fetch_sub", N00BB_AXIOM_READS_TARGET),
};

#undef NATIVE
#undef COLLECTION_FREE_NATIVE
#undef RETURNING_NATIVE
#undef FUNCTION
#undef BINDING

static const size_t axiom_count = sizeof(axioms) / sizeof(axioms[0]);

static pthread_once_t index_once = PTHREAD_ONCE_INIT;
static core_dict_t    by_native;
static core_dict_t    by_native_name;
static core_dict_t    by_function;
static core_dict_t    by_builtin;
static const n00bb_axiom_t *bindings[sizeof(axioms) / sizeof(axioms[0])];
static size_t         binding_count;

static void
build_indexes(void)
{
    core_dict_init_string(&by_native);
    core_dict_init_string(&by_native_name);
    core_dict_init_string(&by_function);
    core_dict_init_string(&by_builtin);
    for (size_t i = 0; i < axiom_count; i++) {
        const n00bb_axiom_t *axiom = &axioms[i];
        switch (axiom->key) {
        case N00BB_AXIOM_NATIVE:
            core_dict_put(&by_native, &axiom->native, axiom);
            core_dict_put(&by_native_name, &axiom->name, axiom);
            break;
        case N00BB_AXIOM_FUNCTION:
            core_dict_put(&by_function, &axiom->name, axiom);
            break;
        case N00BB_AXIOM_BUILTIN:
            core_dict_put(&by_builtin, &axiom->name, axiom);
            break;
        case N00BB_AXIOM_BINDING:
            bindings[binding_count++] = axiom;
            break;
        }
    }
}

static const n00bb_axiom_t *
indexed(core_dict_t *index, const core_string_t *name)
{
    pthread_once(&index_once, build_indexes);
    bool found = false;
    const n00bb_axiom_t *axiom = core_dict_get(index, (void *)name, &found);
    return found ? axiom : nullptr;
}

const n00bb_axiom_t *
n00bb_axiom_of_function(const ir_func_t *function)
{
    if (!function) return nullptr;
    if (!function->n_blocks) {
        const n00bb_axiom_t *native = indexed(&by_native, &function->name);
        if (native) return native;
    }
    const n00bb_axiom_t *axiom = indexed(&by_function, &function->name);
    return axiom && function->owner
        && core_string_eq(axiom->module, ir_module_name(function->owner))
        ? axiom : nullptr;
}

const n00bb_axiom_t *
n00bb_axiom_of_native_name(core_string_t name)
{
    return indexed(&by_native_name, &name);
}

const n00bb_axiom_t *
n00bb_axiom_of_builtin(core_string_t name)
{
    return indexed(&by_builtin, &name);
}

const n00bb_axiom_t *const *
n00bb_axiom_bindings(size_t *count)
{
    pthread_once(&index_once, build_indexes);
    *count = binding_count;
    return bindings;
}

bool
n00bb_axiom_function_has(const ir_func_t *function, uint8_t role)
{
    const n00bb_axiom_t *axiom = n00bb_axiom_of_function(function);
    return axiom && (axiom->roles & role);
}

enum { NATIVE_PURE = 1, NATIVE_FOREIGN = 2 };

static pthread_rwlock_t generated_lock = PTHREAD_RWLOCK_INITIALIZER;
static core_dict_t      generated_natives;
static core_dict_t      foreign_names;
static bool             generated_ready;

static void
note_generated_native(core_string_t symbol, uintptr_t kind,
                      core_string_t name)
{
    if (!symbol.data) return;
    pthread_rwlock_wrlock(&generated_lock);
    if (!generated_ready) {
        core_dict_init_string(&generated_natives);
        core_dict_init_string(&foreign_names);
        generated_ready = true;
    }
    if (!core_dict_contains(&generated_natives, &symbol))
        core_dict_put(&generated_natives, core_dict_string_key_copy(symbol),
                      (void *)kind);
    if (name.u8_bytes && !core_dict_contains(&foreign_names, &symbol)) {
        core_string_t *kept = core_alloc_array(core_string_t, 1);
        if (kept) {
            *kept = core_string_dup(name);
            core_dict_put(&foreign_names, core_dict_string_key_copy(symbol),
                          kept);
        }
    }
    pthread_rwlock_unlock(&generated_lock);
}

static uintptr_t
generated_native(const ir_func_t *function)
{
    if (!function || function->n_blocks) return 0;
    pthread_rwlock_rdlock(&generated_lock);
    bool      found = false;
    uintptr_t kind  = generated_ready
        ? (uintptr_t)core_dict_get(&generated_natives,
                                   (void *)&function->name, &found)
        : 0;
    pthread_rwlock_unlock(&generated_lock);
    return found ? kind : 0;
}

void
n00bb_axiom_note_pure_native(core_string_t symbol)
{
    note_generated_native(symbol, NATIVE_PURE, (core_string_t){0});
}

void
n00bb_axiom_note_foreign_native(core_string_t symbol, core_string_t name,
                                uint32_t effects)
{
    note_generated_native(symbol, NATIVE_FOREIGN | ((uintptr_t)effects << 2),
                          name);
}

core_string_t
n00bb_axiom_foreign_name(const ir_func_t *function)
{
    if (!function) return (core_string_t){0};
    pthread_rwlock_rdlock(&generated_lock);
    const core_string_t *name = generated_ready
        ? core_dict_get(&foreign_names, (void *)&function->name, nullptr)
        : nullptr;
    pthread_rwlock_unlock(&generated_lock);
    return name ? *name : function->name;
}

bool
n00bb_axiom_pure_native(const ir_func_t *function)
{
    return generated_native(function) == NATIVE_PURE;
}

bool
n00bb_axiom_foreign_native(const ir_func_t *function, uint32_t *effects)
{
    uintptr_t kind = generated_native(function);
    if ((kind & 3) != NATIVE_FOREIGN) return false;
    *effects = (uint32_t)(kind >> 2);
    return true;
}
