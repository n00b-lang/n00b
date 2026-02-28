#include "n00b.h"
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/runtime.h"
#include "adt/dict_untyped.h"
#include "core/type_info.h"
#include "core/pool.h"

void
n00b_type_registry_init(void)
{
    n00b_runtime_t   *rt = n00b_get_runtime();
    n00b_allocator_t *sp = (n00b_allocator_t *)&rt->system_pool;

    rt->type_registry = n00b_alloc_with_opts(n00b_dict_untyped_t, &(n00b_alloc_opts_t){.allocator = sp});
    n00b_dict_untyped_init(rt->type_registry,
                           .allocator     = sp,
                           .hash          = n00b_hash_word,
                           .skip_obj_hash = true);

    n00b_register_builtin_types();
}

bool
n00b_type_register(uint64_t type_hash, const n00b_type_info_t *info)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    if (!rt || !rt->type_registry) {
        return false;
    }

    // Check if already registered.
    bool found;
    n00b_dict_untyped_get(rt->type_registry, type_hash, &found);
    if (found) {
        return false;
    }

    // Deep-copy the type info into the system pool.
    n00b_allocator_t *sp   = (n00b_allocator_t *)&rt->system_pool;
    n00b_type_info_t *copy = n00b_alloc_with_opts(n00b_type_info_t, &(n00b_alloc_opts_t){.allocator = sp});

    *copy = *info;

    // Intern the name string.
    if (info->name) {
        size_t len  = strlen(info->name);
        char  *name = n00b_alloc_array_with_opts(char, len + 1, &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(name, info->name, len + 1);
        copy->name = name;
    }

    // Intern literal_modifier if present.
    if (info->literal_modifier) {
        size_t len = strlen(info->literal_modifier);
        char  *mod = n00b_alloc_array_with_opts(char, len + 1, &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(mod, info->literal_modifier, len + 1);
        copy->literal_modifier = mod;
    }

    // ext_vtable starts as none.
    copy->ext_vtable = n00b_option_none(n00b_array_t(n00b_method_t) *);

    n00b_dict_untyped_put(rt->type_registry, type_hash, copy);
    return true;
}

n00b_option_t(n00b_type_info_t *)
n00b_type_lookup(uint64_t type_hash)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    if (!rt || !rt->type_registry) {
        return n00b_option_none(n00b_type_info_t *);
    }

    bool found;
    n00b_type_info_t *info = n00b_dict_untyped_get(rt->type_registry,
                                                    type_hash,
                                                    &found);
    return found ? n00b_option_set(n00b_type_info_t *, info)
                 : n00b_option_none(n00b_type_info_t *);
}

uint64_t
n00b_obj_typehash(void *obj)
{
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(obj);

    switch (ainfo.kind) {
    case n00b_alloc_oob:
        return ainfo.hdr.oob->tinfo;
    case n00b_alloc_inline:
        return ainfo.hdr.in_line->tinfo;
    default:
        return 0;
    }
}

n00b_option_t(n00b_type_info_t *)
n00b_type_info_for(void *obj)
{
    uint64_t hash = n00b_obj_typehash(obj);

    if (!hash) {
        return n00b_option_none(n00b_type_info_t *);
    }

    return n00b_type_lookup(hash);
}

bool
n00b_type_add_method(uint64_t type_hash, const n00b_method_t *method)
{
    auto info_opt = n00b_type_lookup(type_hash);

    if (!n00b_option_is_set(info_opt)) {
        return false;
    }

    n00b_type_info_t *info = n00b_option_get(info_opt);

    n00b_runtime_t   *rt = n00b_get_runtime();
    n00b_allocator_t *sp = (n00b_allocator_t *)&rt->system_pool;

    // Lazily create the extension vtable array.
    if (!n00b_option_is_set(info->ext_vtable)) {
        n00b_array_t(n00b_method_t) *arr = n00b_alloc_with_opts(n00b_array_t(n00b_method_t),
                                                                &(n00b_alloc_opts_t){.allocator = sp});
        *arr = n00b_array_new(n00b_method_t, 4, sp);
        info->ext_vtable = n00b_option_set(n00b_array_t(n00b_method_t) *, arr);
    }

    n00b_array_t(n00b_method_t) *methods = n00b_option_get(info->ext_vtable);

    // Copy the method into the extension vtable.
    n00b_method_t m = *method;

    // Intern the method name.
    if (method->name) {
        size_t len  = strlen(method->name);
        char  *name = n00b_alloc_array_with_opts(char, len + 1, &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(name, method->name, len + 1);
        m.name = name;
    }

    // Intern return_type.type_name.
    if (method->return_type.type_name) {
        size_t len  = strlen(method->return_type.type_name);
        char  *rtn  = n00b_alloc_array_with_opts(char, len + 1, &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(rtn, method->return_type.type_name, len + 1);
        m.return_type.type_name = rtn;
    }

    // Deep-copy params array data and intern param type_name strings.
    if (method->params.len > 0 && method->params.data) {
        size_t pcount = method->params.len;
        n00b_method_param_t *pdata = n00b_alloc_array_with_opts(n00b_method_param_t,
                                                                pcount,
                                                                &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(pdata, method->params.data, pcount * sizeof(n00b_method_param_t));

        for (size_t i = 0; i < pcount; i++) {
            if (pdata[i].type_name) {
                size_t len = strlen(pdata[i].type_name);
                char  *tn  = n00b_alloc_array_with_opts(char, len + 1, &(n00b_alloc_opts_t){.allocator = sp});
                memcpy(tn, pdata[i].type_name, len + 1);
                pdata[i].type_name = tn;
            }
        }

        m.params.data = pdata;
        m.params.len  = pcount;
        m.params.cap  = pcount;
    }

    // Append to the array.
    size_t idx = methods->len;
    if (idx >= methods->cap) {
        // Grow.
        size_t new_cap = methods->cap * 2;
        n00b_method_t *new_data = n00b_alloc_array_with_opts(n00b_method_t, new_cap,
                                                            &(n00b_alloc_opts_t){.allocator = sp});
        memcpy(new_data, methods->data, methods->len * sizeof(n00b_method_t));
        n00b_free(methods->data);
        methods->data = new_data;
        methods->cap  = new_cap;
    }
    methods->data[idx] = m;
    methods->len       = idx + 1;

    return true;
}
