#include "private/th/internal.h"
#include "lib/dict.h"

#include <stdalign.h>
#include <string.h>

#include "lib/alloc.h"

core_list_decl(th_member);

#define TH_ENV_ARCHIVE_SCHEMA UINT64_C(0x7468656e76617204)
#define TH_ENV_ARCHIVE_ROOT_TYPE UINT64_C(0x7468656e762e726f)
#define TH_TYPE_ARCHIVE_BASE UINT64_C(0x7468747970650000)

static uint64_t environment_archive_abi(void) {
  return core_persist_arena_native_abi_tag(TH_ENV_ARCHIVE_SCHEMA);
}

static uint64_t type_archive_id(th_kind kind) {
  return TH_TYPE_ARCHIVE_BASE | (uint64_t)kind + 1;
}

static _Thread_local core_dict_t *saving_var_indices;

static uint64_t saved_var_index(uint64_t index) {
  if (!saving_var_indices) return index;
  bool found = false;
  void *slot = core_dict_get(saving_var_indices, (void *)(uintptr_t)(index + 1),
                             &found);
  if (found) return (uint64_t)(uintptr_t)slot;
  uint64_t next = core_dict_len(saving_var_indices);
  core_dict_put(saving_var_indices, (void *)(uintptr_t)(index + 1),
                (void *)(uintptr_t)next);
  return next;
}

static bool write_count(core_object_writer_t *writer, size_t count) {
  return core_object_writer_u64(writer, count);
}

static bool read_count(core_object_reader_t *reader, size_t *count) {
  uint64_t value;
  if (!count || !core_object_reader_u64(reader, &value) || value > SIZE_MAX)
    return false;
  *count = (size_t)value;
  return true;
}

static bool write_type_ref(core_object_writer_t *writer, const th_type *type) {
  return core_object_writer_ref(writer,
      type ? type_archive_id(type->kind) : 0, type);
}

static bool read_type_ref(core_object_reader_t *reader, th_type **type) {
  void *value = nullptr;
  if (!core_object_reader_ref(reader, 0, &value))
    return false;
  *type = value;
  return true;
}

static bool write_string_arg(core_object_writer_t *writer,
                             core_string_t value) {
  return write_count(writer, value.u8_bytes) &&
         core_object_writer_bytes(writer, value.data, value.u8_bytes);
}

static bool save_constraint_scope(core_object_writer_t *writer,
                                  const th_constraint *constraint) {
  return core_object_writer_u32(writer, constraint->has_scope) &&
         (!constraint->has_scope ||
          (core_object_writer_string(writer, constraint->scope_module) &&
           core_object_writer_string(writer, constraint->scope_package)));
}

static bool read_constraint_scope(core_object_reader_t *reader,
                                  th_constraint *constraint) {
  uint32_t has_scope;
  if (!core_object_reader_u32(reader, &has_scope) || has_scope > 1)
    return false;
  constraint->has_scope = has_scope;
  if (!constraint->has_scope) return true;
  return core_object_reader_string(reader, &constraint->scope_module) &&
         core_object_reader_string(reader, &constraint->scope_package);
}

static bool read_string_arg(core_object_reader_t *reader,
                            core_string_t *out) {
  size_t size;
  if (!read_count(reader, &size))
    return false;
  char *copy = core_object_reader_alloc(reader, size ? size : 1,
                                        alignof(char));
  if (!copy || !core_object_reader_bytes(reader, copy, size))
    return false;
  *out = core_string_borrow(copy, size);
  return true;
}

static bool save_constraint(core_object_writer_t *writer,
                            const th_constraint *constraint) {
  if (!core_object_writer_u32(writer, constraint->kind))
    return false;
  switch (constraint->kind) {
  case TH_CON_KIND:
    return core_object_writer_u32(writer, constraint->type_kind);
  case TH_CON_UNIFIES:
  case TH_CON_PROMOTES:
  case TH_CON_EXCLUDES:
    return write_type_ref(writer, constraint->target);
  case TH_CON_ONE_OF:
    if (!write_count(writer, constraint->one_of.n)) return false;
    for (size_t i = 0; i < constraint->one_of.n; i++)
      if (!write_type_ref(writer, constraint->one_of.types[i])) return false;
    return true;
  case TH_CON_IMPLEMENTS:
    return core_object_writer_string(writer, constraint->iface) &&
           save_constraint_scope(writer, constraint);
  case TH_CON_HAS_FIELD:
    return core_object_writer_string(writer, constraint->field.name) &&
           write_type_ref(writer, constraint->field.type) &&
           core_object_writer_u32(writer, constraint->field.auto_deref);
  case TH_CON_HAS_PARAM:
    return core_object_writer_u64(writer, constraint->param.index) &&
           write_type_ref(writer, constraint->param.type);
  case TH_CON_METHOD:
    return core_object_writer_string(writer, constraint->method.name) &&
           write_type_ref(writer, constraint->method.result) &&
           write_type_ref(writer, constraint->method.call) &&
           save_constraint_scope(writer, constraint) &&
           core_object_writer_string(writer, constraint->method.in_place);
  case TH_CON_INTEGER:
    return core_object_writer_u32(writer, constraint->integer.flags);
  }
  return false;
}

static bool read_constraint(core_object_reader_t *reader,
                            th_constraint *constraint) {
  memset(constraint, 0, sizeof(*constraint));
  uint32_t kind;
  if (!core_object_reader_u32(reader, &kind) || kind > TH_CON_INTEGER)
    return false;
  constraint->kind = (th_con_kind)kind;
  switch (constraint->kind) {
  case TH_CON_KIND: {
    uint32_t type_kind;
    if (!core_object_reader_u32(reader, &type_kind) || type_kind > TH_TRAIT)
      return false;
    constraint->type_kind = (th_kind)type_kind;
    return true;
  }
  case TH_CON_UNIFIES:
  case TH_CON_PROMOTES:
  case TH_CON_EXCLUDES:
    return read_type_ref(reader, &constraint->target);
  case TH_CON_ONE_OF:
    if (!read_count(reader, &constraint->one_of.n) ||
        constraint->one_of.n > SIZE_MAX / sizeof(th_type *))
      return false;
    if (constraint->one_of.n > SIZE_MAX / sizeof(th_type *)) return false;
    constraint->one_of.types = constraint->one_of.n
        ? core_object_reader_alloc(reader,
              constraint->one_of.n * sizeof(th_type *), alignof(th_type *))
        : nullptr;
    if (constraint->one_of.n && !constraint->one_of.types) return false;
    for (size_t i = 0; i < constraint->one_of.n; i++)
      if (!read_type_ref(reader, &constraint->one_of.types[i])) return false;
    return true;
  case TH_CON_IMPLEMENTS:
    return core_object_reader_string(reader, &constraint->iface) &&
           read_constraint_scope(reader, constraint);
  case TH_CON_HAS_FIELD:
    if (!core_object_reader_string(reader, &constraint->field.name) ||
        !read_type_ref(reader, &constraint->field.type)) return false;
    uint32_t auto_deref;
    if (!core_object_reader_u32(reader, &auto_deref) || auto_deref > 1)
      return false;
    constraint->field.auto_deref = auto_deref;
    return true;
  case TH_CON_HAS_PARAM:
    return core_object_reader_u64(reader, &constraint->param.index) &&
           read_type_ref(reader, &constraint->param.type);
  case TH_CON_METHOD:
    return core_object_reader_string(reader, &constraint->method.name) &&
           constraint->method.name.data &&
           read_type_ref(reader, &constraint->method.result) &&
           constraint->method.result &&
           read_type_ref(reader, &constraint->method.call) &&
           read_constraint_scope(reader, constraint) &&
           core_object_reader_string(reader, &constraint->method.in_place);
  case TH_CON_INTEGER: {
    uint32_t flags = 0;
    if (!core_object_reader_u32(reader, &flags) || flags > UINT8_MAX)
      return false;
    constraint->integer.flags = (uint8_t)flags;
    constraint->integer.owner = 0;
    return true;
  }
  }
  return false;
}

static bool save_type(core_object_writer_t *writer, const void *object,
                      void *user_data) {
  const th_type *type = object;
  th_kind kind = (th_kind)(uintptr_t)user_data;
  if (!type || type->kind != kind)
    return false;
  switch (kind) {
  case TH_NAMED:
    if (!core_object_writer_string(writer, type->named.name) ||
        !write_count(writer, type->named.name.u8_bytes) ||
        !core_object_writer_u32(writer, type->named.is_variant) ||
        !write_count(writer, type->named.nargs)) return false;
    for (size_t i = 0; i < type->named.nargs; i++) {
      const th_arg *arg = &type->named.args[i];
      if (!core_object_writer_u32(writer, arg->kind)) return false;
      switch (arg->kind) {
      case TH_ARG_TYPE:
        if (!write_type_ref(writer, arg->type)) return false;
        break;
      case TH_ARG_UINT:
        if (!core_object_writer_u64(writer, arg->u)) return false;
        break;
      case TH_ARG_SINT:
        if (!core_object_writer_u64(writer, (uint64_t)arg->s)) return false;
        break;
      case TH_ARG_STR:
        if (!write_string_arg(writer, arg->str)) return false;
        break;
      }
    }
    return true;
  case TH_BITINT:
    return core_object_writer_u64(writer, type->bitint.width) &&
           core_object_writer_u32(writer, type->bitint.flavor);
  case TH_INDIRECTION:
    return core_object_writer_u32(writer, type->ind.attrs) &&
           write_type_ref(writer, type->ind.referent);
  case TH_SEQUENCE:
    return core_object_writer_u32(writer, type->seq.has_extent) &&
           core_object_writer_u64(writer, type->seq.extent) &&
           write_type_ref(writer, type->seq.elem);
  case TH_RECORD:
    if (!write_count(writer, type->record.n)) return false;
    for (size_t i = 0; i < type->record.n; i++)
      if (!core_object_writer_string(writer, type->record.members[i].name) ||
          !write_type_ref(writer, type->record.members[i].type)) return false;
    return true;
  case TH_TRAIT:
    if (!core_object_writer_string(writer, type->trait.name) ||
        !write_count(writer, type->trait.n)) return false;
    for (size_t i = 0; i < type->trait.n; i++)
      if (!core_object_writer_string(writer, type->trait.methods[i].name) ||
          !write_type_ref(writer, type->trait.methods[i].type)) return false;
    if (!write_count(writer, type->trait.nr)) return false;
    for (size_t i = 0; i < type->trait.nr; i++) {
      const th_trait_requirement *requirement =
          &type->trait.requirements[i];
      if (!core_object_writer_u32(writer, requirement->kind) ||
          !write_type_ref(writer, requirement->subject) ||
          !write_type_ref(writer, requirement->target) ||
          !core_object_writer_string(writer, requirement->iface)) return false;
    }
    return true;
  case TH_FUNCTION:
    if (!write_type_ref(writer, type->fn.ret) ||
        !write_type_ref(writer, type->fn.error) ||
        !write_count(writer, type->fn.np)) return false;
    for (size_t i = 0; i < type->fn.np; i++)
      if (!core_object_writer_u32(writer, type->fn.params[i].mode) ||
          !write_type_ref(writer, type->fn.params[i].type)) return false;
    if (!core_object_writer_u32(writer, type->fn.rest != nullptr)) return false;
    if (type->fn.rest &&
        (!core_object_writer_u32(writer, type->fn.rest->mode) ||
         !write_type_ref(writer, type->fn.rest->type))) return false;
    if (!write_count(writer, type->fn.nkw)) return false;
    for (size_t i = 0; i < type->fn.nkw; i++)
      if (!core_object_writer_string(writer, type->fn.kw[i].name) ||
          !core_object_writer_u32(writer, type->fn.kw[i].mode) ||
          !write_type_ref(writer, type->fn.kw[i].type)) return false;
    return true;
  case TH_BINDER:
    if (!core_object_writer_u64(writer, type->binder.arity) ||
        !write_type_ref(writer, type->binder.body) ||
        !core_object_writer_u32(writer, type->binder.qvars != nullptr))
      return false;
    if (type->binder.qvars)
      for (uint64_t i = 0; i < type->binder.arity; i++)
        if (!write_type_ref(writer, type->binder.qvars[i])) return false;
    return true;
  case TH_BOUND:
    return core_object_writer_u64(writer, type->bound.distance) &&
           core_object_writer_u64(writer, type->bound.position);
  case TH_SUM:
    if (!core_object_writer_u32(writer, type->sum.comb) ||
        !write_count(writer, type->sum.n)) return false;
    for (size_t i = 0; i < type->sum.n; i++)
      if (!write_type_ref(writer, type->sum.arms[i])) return false;
    return true;
  case TH_VAR:
    if (!core_object_writer_u64(writer, saved_var_index(type->var.index)) ||
        !core_object_writer_u32(writer, type->var.has_index) ||
        !write_type_ref(writer, type->var.ref) ||
        !core_object_writer_string(writer, type->var.name) ||
        !write_count(writer, type->var.cons.len)) return false;
    for (size_t i = 0; i < type->var.cons.len; i++)
      if (!save_constraint(writer, &type->var.cons.data[i])) return false;
    return true;
  }
  return false;
}

static void *type_allocate(core_persist_arena_t *arena, uint32_t version,
                           core_object_reader_t *payload, void *user_data) {
  (void)payload;
  if (version != 1)
    return nullptr;
  th_type *type = core_persist_arena_alloc_transient(
      arena, sizeof(*type), alignof(th_type), nullptr);
  if (type) type->kind = (th_kind)(uintptr_t)user_data;
  return type;
}

static void *read_array(core_object_reader_t *reader, size_t count,
                        size_t item_size, size_t alignment) {
  if (!count)
    return nullptr;
  if (item_size && count > SIZE_MAX / item_size)
    return nullptr;
  return core_object_reader_alloc(reader, count * item_size, alignment);
}

static bool read_mode(core_object_reader_t *reader, th_mode *mode) {
  uint32_t value;
  if (!core_object_reader_u32(reader, &value) ||
      !th_mode_is_valid((th_mode)value))
    return false;
  *mode = (th_mode)value;
  return true;
}

static bool restore_type(core_object_reader_t *reader, void *object,
                         void *user_data) {
  th_type *type = object;
  th_kind kind = (th_kind)(uintptr_t)user_data;
  if (!type || kind > TH_TRAIT || type->kind != kind)
    return false;
  uint32_t flag, value;
  size_t count;
  switch (kind) {
  case TH_NAMED:
    if (!core_object_reader_string(reader, &type->named.name) ||
        !read_count(reader, &count) ||
        !core_object_reader_u32(reader, &flag) || flag > 1 ||
        !read_count(reader, &type->named.nargs)) return false;
    type->named.is_variant = flag;
    if (!type->named.name.data || type->named.name.u8_bytes != count ||
        type->named.nargs > SIZE_MAX / sizeof(th_arg)) return false;
    type->named.args = read_array(reader, type->named.nargs, sizeof(th_arg),
                                  alignof(th_arg));
    if (type->named.nargs && !type->named.args) return false;
    for (size_t i = 0; i < type->named.nargs; i++) {
      th_arg *arg = &type->named.args[i];
      if (!core_object_reader_u32(reader, &value) || value > TH_ARG_STR)
        return false;
      arg->kind = (th_argkind)value;
      switch (arg->kind) {
      case TH_ARG_TYPE:
        if (!read_type_ref(reader, &arg->type)) return false;
        break;
      case TH_ARG_UINT:
        if (!core_object_reader_u64(reader, &arg->u)) return false;
        break;
      case TH_ARG_SINT: {
        uint64_t signed_bits;
        if (!core_object_reader_u64(reader, &signed_bits)) return false;
        memcpy(&arg->s, &signed_bits, sizeof(arg->s));
        break;
      }
      case TH_ARG_STR:
        if (!read_string_arg(reader, &arg->str)) return false;
        break;
      }
    }
    return true;
  case TH_BITINT:
    if (!core_object_reader_u64(reader, &type->bitint.width) ||
        !core_object_reader_u32(reader, &flag) || flag > TH_BITINT_BITS)
        return false;
    type->bitint.flavor = (th_bitint_flavor)flag;
    return true;
  case TH_INDIRECTION:
    if (!core_object_reader_u32(reader, &value) || value > UINT8_MAX ||
        !read_type_ref(reader, &type->ind.referent)) return false;
    type->ind.attrs = (uint8_t)value;
    return type->ind.referent != nullptr;
  case TH_SEQUENCE:
    if (!core_object_reader_u32(reader, &flag) || flag > 1 ||
        !core_object_reader_u64(reader, &type->seq.extent) ||
        !read_type_ref(reader, &type->seq.elem)) return false;
    type->seq.has_extent = flag;
    return type->seq.elem != nullptr;
  case TH_RECORD:
    if (!read_count(reader, &type->record.n) ||
        type->record.n > SIZE_MAX / sizeof(th_member)) return false;
    type->record.members = read_array(reader, type->record.n,
        sizeof(th_member), alignof(th_member));
    if (type->record.n && !type->record.members) return false;
    for (size_t i = 0; i < type->record.n; i++)
      if (!core_object_reader_string(reader,
              &type->record.members[i].name) ||
          !type->record.members[i].name.data ||
          !read_type_ref(reader, &type->record.members[i].type) ||
          !type->record.members[i].type) return false;
    return true;
  case TH_TRAIT:
    if (!core_object_reader_string(reader, &type->trait.name) ||
        !type->trait.name.data ||
        !read_count(reader, &type->trait.n) ||
        type->trait.n > SIZE_MAX / sizeof(th_member)) return false;
    if (!th_name_syntax_valid(type->trait.name))
      return false;
    type->trait.methods = read_array(reader, type->trait.n,
        sizeof(th_member), alignof(th_member));
    if (type->trait.n && !type->trait.methods) return false;
    for (size_t i = 0; i < type->trait.n; i++)
      if (!core_object_reader_string(reader,
              &type->trait.methods[i].name) ||
          !type->trait.methods[i].name.data ||
          !read_type_ref(reader, &type->trait.methods[i].type) ||
          !type->trait.methods[i].type) return false;
    if (!read_count(reader, &type->trait.nr) ||
        type->trait.nr > SIZE_MAX / sizeof(th_trait_requirement))
      return false;
    type->trait.requirements = read_array(reader, type->trait.nr,
        sizeof(th_trait_requirement), alignof(th_trait_requirement));
    if (type->trait.nr && !type->trait.requirements) return false;
    for (size_t i = 0; i < type->trait.nr; i++) {
      th_trait_requirement *requirement = &type->trait.requirements[i];
      uint32_t kind;
      if (!core_object_reader_u32(reader, &kind) ||
          !read_type_ref(reader, &requirement->subject) ||
          !read_type_ref(reader, &requirement->target) ||
          !core_object_reader_string(reader, &requirement->iface) ||
          !requirement->subject) return false;
      requirement->kind = (th_con_kind)kind;
      if (kind == TH_CON_EXCLUDES) {
        if (!requirement->target || requirement->iface.data) return false;
      }
      else if (kind == TH_CON_IMPLEMENTS) {
        if (requirement->target || !requirement->iface.data ||
            !th_name_syntax_valid(requirement->iface)) return false;
      }
      else return false;
    }
    return true;
  case TH_FUNCTION:
    if (!read_type_ref(reader, &type->fn.ret) || !type->fn.ret ||
        !read_type_ref(reader, &type->fn.error) ||
        !read_count(reader, &type->fn.np) ||
        type->fn.np > SIZE_MAX / sizeof(th_param)) return false;
    type->fn.params = read_array(reader, type->fn.np, sizeof(th_param),
                                 alignof(th_param));
    if (type->fn.np && !type->fn.params) return false;
    for (size_t i = 0; i < type->fn.np; i++)
      if (!read_mode(reader, &type->fn.params[i].mode) ||
          !read_type_ref(reader, &type->fn.params[i].type) ||
          !type->fn.params[i].type) return false;
    if (!core_object_reader_u32(reader, &flag) || flag > 1) return false;
    if (flag) {
      type->fn.rest = read_array(reader, 1, sizeof(th_param), alignof(th_param));
      if (!type->fn.rest || !read_mode(reader, &type->fn.rest->mode) ||
          !read_type_ref(reader, &type->fn.rest->type) ||
          !type->fn.rest->type) return false;
    }
    if (!read_count(reader, &type->fn.nkw) ||
        type->fn.nkw > SIZE_MAX / sizeof(th_kwparam)) return false;
    type->fn.kw = read_array(reader, type->fn.nkw, sizeof(th_kwparam),
                             alignof(th_kwparam));
    if (type->fn.nkw && !type->fn.kw) return false;
    for (size_t i = 0; i < type->fn.nkw; i++)
      if (!core_object_reader_string(reader, &type->fn.kw[i].name) ||
          !type->fn.kw[i].name.data || !read_mode(reader, &type->fn.kw[i].mode) ||
          !read_type_ref(reader, &type->fn.kw[i].type) ||
          !type->fn.kw[i].type) return false;
    return true;
  case TH_BINDER:
    if (!core_object_reader_u64(reader, &type->binder.arity) ||
        type->binder.arity > SIZE_MAX / sizeof(th_type *) ||
        !read_type_ref(reader, &type->binder.body) || !type->binder.body ||
        !core_object_reader_u32(reader, &flag) || flag > 1)
      return false;
    if (flag) {
      type->binder.qvars = read_array(reader, (size_t)type->binder.arity,
          sizeof(th_type *), alignof(th_type *));
      if (type->binder.arity && !type->binder.qvars) return false;
      for (uint64_t i = 0; i < type->binder.arity; i++)
        if (!read_type_ref(reader, &type->binder.qvars[i]) ||
            !type->binder.qvars[i]) return false;
    }
    return true;
  case TH_BOUND:
    return core_object_reader_u64(reader, &type->bound.distance) &&
           core_object_reader_u64(reader, &type->bound.position);
  case TH_SUM:
    if (!core_object_reader_u32(reader, &value) || value > TH_INTERSECTION ||
        !read_count(reader, &type->sum.n) ||
        type->sum.n > SIZE_MAX / sizeof(th_type *)) return false;
    type->sum.comb = (th_comb)value;
    type->sum.arms = read_array(reader, type->sum.n, sizeof(th_type *),
                                alignof(th_type *));
    if (type->sum.n && !type->sum.arms) return false;
    for (size_t i = 0; i < type->sum.n; i++)
      if (!read_type_ref(reader, &type->sum.arms[i]) || !type->sum.arms[i])
        return false;
    return true;
  case TH_VAR:
    if (!core_object_reader_u64(reader, &type->var.index) ||
        !core_object_reader_u32(reader, &flag) || flag > 1 ||
        !read_type_ref(reader, &type->var.ref) ||
        !core_object_reader_string(reader, &type->var.name) ||
        !read_count(reader, &count) || count > SIZE_MAX / sizeof(th_constraint))
      return false;
    type->var.has_index = flag;
    type->var.cons.data = read_array(reader, count, sizeof(th_constraint),
                                     alignof(th_constraint));
    type->var.cons.len = type->var.cons.cap = count;
    type->var.cons_borrowed = true;
    if (count && !type->var.cons.data) return false;
    for (size_t i = 0; i < count; i++)
      if (!read_constraint(reader, &type->var.cons.data[i])) return false;
    return true;
  }
  return false;
}

static bool save_environment(core_object_writer_t *writer, const void *object,
                             void *user_data) {
  (void)user_data;
  const th_env *env = object;
  if (!env || !env->frozen ||
      !write_count(writer, env->type_origins.len)) return false;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    const th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (!core_object_writer_string(writer, origin->name) ||
        !core_object_writer_string(writer, origin->module) ||
        !core_object_writer_string(writer, origin->package) ||
        !core_object_writer_u32(writer, origin->visibility) ||
        !core_object_writer_u32(writer, origin->shape_kind) ||
        !write_type_ref(writer, origin->shape)) return false;
  }
  if (!write_count(writer, env->traits.len)) return false;
  for (size_t i = 0; i < env->traits.len; i++) {
    const th_env_trait_t *trait = &env->traits.data[i];
    if (!core_object_writer_string(writer, trait->name) ||
        !core_object_writer_string(writer, trait->module) ||
        !core_object_writer_string(writer, trait->package) ||
        !core_object_writer_u32(writer, trait->visibility) ||
        !write_type_ref(writer, trait->scheme) ||
        !write_type_ref(writer, trait->errors) ||
        !write_type_ref(writer, trait->contracts)) return false;
  }
  if (!write_count(writer, env->functions.len)) return false;
  for (size_t i = 0; i < env->functions.len; i++) {
    const th_env_function_t *function = &env->functions.data[i];
    if (!core_object_writer_string(writer, function->qualified_name) ||
        !core_object_writer_string(writer, function->root_name) ||
        !core_object_writer_string(writer, function->module) ||
        !core_object_writer_string(writer, function->package) ||
        !core_object_writer_u32(writer, function->visibility) ||
        !write_type_ref(writer, function->scheme) ||
        !write_type_ref(writer, function->error_type) ||
        !write_type_ref(writer, function->contract) ||
        !core_object_writer_u64(writer, function->required_params))
      return false;
  }
  if (!write_count(writer, env->values.len)) return false;
  for (size_t i = 0; i < env->values.len; i++) {
    const th_env_value_t *value = &env->values.data[i];
    if (!core_object_writer_string(writer, value->qualified_name) ||
        !core_object_writer_string(writer, value->root_name) ||
        !core_object_writer_string(writer, value->module) ||
        !core_object_writer_string(writer, value->package) ||
        !core_object_writer_u32(writer, value->visibility) ||
        !write_type_ref(writer, value->type)) return false;
  }
  if (!write_count(writer, env->metadata.len)) return false;
  for (size_t i = 0; i < env->metadata.len; i++) {
    const th_env_metadata_record_t *metadata = &env->metadata.data[i];
    if (!core_object_writer_u32(writer, metadata->owner_kind) ||
        !core_object_writer_string(writer, metadata->owner_name) ||
        !core_object_writer_string(writer, metadata->key) ||
        !core_object_writer_u32(writer, metadata->version) ||
        !write_count(writer, metadata->size) ||
        !core_object_writer_bytes(writer, metadata->data, metadata->size))
      return false;
  }
  if (!write_count(writer, env->intrinsic_types.len)) return false;
  for (size_t i = 0; i < env->intrinsic_types.len; i++) {
    const th_env_intrinsic_type_t *fact = &env->intrinsic_types.data[i];
    if (!core_object_writer_string(writer, fact->trait_name) ||
        !write_type_ref(writer, fact->receiver_pattern)) return false;
  }
  return true;
}

static void *environment_allocate(core_persist_arena_t *arena,
                                  uint32_t version,
                                  core_object_reader_t *payload,
                                  void *user_data) {
  (void)payload;
  (void)user_data;
  if (version != 1) return nullptr;
  th_env *temporary = th_env_new();
  th_env *mapped = core_persist_arena_alloc_transient(
      arena, sizeof(*mapped), alignof(th_env), nullptr);
  if (!mapped) {
    th_env_free(temporary);
    return nullptr;
  }
  *mapped = *temporary;
  core_free(temporary);
  return mapped;
}

static bool restore_environment(core_object_reader_t *reader, void *object,
                                void *user_data) {
  (void)user_data;
  th_env *env = object;
  size_t count;
  if (!read_count(reader, &count) || count > SIZE_MAX / sizeof(th_env_type_origin_t))
    return false;
  for (size_t i = 0; i < count; i++) {
    th_env_type_origin_t origin = {0};
    uint32_t visibility = TH_ENV_PUBLIC;
    uint32_t shape_kind = TH_ENV_TYPE_SHAPE_NONE;
    if (!core_object_reader_string(reader, &origin.name) ||
        !core_object_reader_string(reader, &origin.module) ||
        !core_object_reader_string(reader, &origin.package) ||
        !core_object_reader_u32(reader, &visibility) ||
        visibility > TH_ENV_PUBLIC ||
        !core_object_reader_u32(reader, &shape_kind) ||
        shape_kind > TH_ENV_TYPE_SHAPE_BITSET ||
        !read_type_ref(reader, &origin.shape) ||
        ((shape_kind == TH_ENV_TYPE_SHAPE_NONE) != (origin.shape == nullptr)))
      return false;
    origin.visibility = (th_env_visibility_t)visibility;
    origin.shape_kind = (th_env_type_shape_kind_t)shape_kind;
    core_list_push(env->type_origins, origin);
  }
  if (!read_count(reader, &count) || count > SIZE_MAX / sizeof(th_env_trait_t))
    return false;
  for (size_t i = 0; i < count; i++) {
    th_env_trait_t trait = {0};
    uint32_t visibility;
    if (!core_object_reader_string(reader, &trait.name) ||
        !core_object_reader_string(reader, &trait.module) ||
        !core_object_reader_string(reader, &trait.package) ||
        !core_object_reader_u32(reader, &visibility) ||
        visibility > TH_ENV_PUBLIC ||
        !read_type_ref(reader, &trait.scheme) ||
        !read_type_ref(reader, &trait.errors) ||
        !read_type_ref(reader, &trait.contracts)) return false;
    trait.visibility = (th_env_visibility_t)visibility;
    core_list_push(env->traits, trait);
  }
  if (!read_count(reader, &count) || count > SIZE_MAX / sizeof(th_env_function_t))
    return false;
  for (size_t i = 0; i < count; i++) {
    th_env_function_t function = {0};
    uint32_t visibility;
    if (!core_object_reader_string(reader,
            &function.qualified_name) ||
        !core_object_reader_string(reader, &function.root_name) ||
        !core_object_reader_string(reader, &function.module) ||
        !core_object_reader_string(reader, &function.package) ||
        !core_object_reader_u32(reader, &visibility) ||
        visibility > TH_ENV_PUBLIC ||
        !read_type_ref(reader, &function.scheme) ||
        !read_type_ref(reader, &function.error_type) ||
        !read_type_ref(reader, &function.contract)) return false;
    uint64_t required;
    if (!core_object_reader_u64(reader, &required)) return false;
    function.required_params = (size_t)required;
    function.visibility = (th_env_visibility_t)visibility;
    function.order = i;
    core_list_push(env->functions, function);
  }
  if (!read_count(reader, &count) ||
      count > SIZE_MAX / sizeof(th_env_value_t)) return false;
  for (size_t i = 0; i < count; i++) {
    th_env_value_t value = {0};
    uint32_t visibility;
    if (!core_object_reader_string(reader, &value.qualified_name) ||
        !core_object_reader_string(reader, &value.root_name) ||
        !core_object_reader_string(reader, &value.module) ||
        !core_object_reader_string(reader, &value.package) ||
        !core_object_reader_u32(reader, &visibility) ||
        visibility > TH_ENV_PUBLIC || !read_type_ref(reader, &value.type) ||
        !value.type) return false;
    value.visibility = (th_env_visibility_t)visibility;
    core_list_push(env->values, value);
  }
  if (!read_count(reader, &count) ||
      count > SIZE_MAX / sizeof(th_env_metadata_record_t)) return false;
  for (size_t i = 0; i < count; i++) {
    th_env_metadata_record_t metadata = {0};
    uint32_t owner_kind;
    if (!core_object_reader_u32(reader, &owner_kind) ||
        !core_object_reader_string(reader, &metadata.owner_name) ||
        !core_object_reader_string(reader, &metadata.key) ||
        !core_object_reader_u32(reader, &metadata.version) ||
        !read_count(reader, &metadata.size) ||
        owner_kind < TH_ENV_METADATA_TYPE ||
        owner_kind > TH_ENV_METADATA_ENVIRONMENT || !metadata.version)
      return false;
    metadata.owner_kind = (th_env_metadata_owner_t)owner_kind;
    if (metadata.size) {
      metadata.data = core_object_reader_alloc(reader, metadata.size,
                                               alignof(uint8_t));
      if (!metadata.data || !core_object_reader_bytes(
              reader, (void *)metadata.data, metadata.size)) return false;
    }
    core_list_push(env->metadata, metadata);
  }
  if (!read_count(reader, &count) ||
      count > SIZE_MAX / sizeof(th_env_intrinsic_type_t)) return false;
  for (size_t i = 0; i < count; i++) {
    th_env_intrinsic_type_t fact = {0};
    if (!core_object_reader_string(reader, &fact.trait_name) ||
        !fact.trait_name.data || !read_type_ref(reader, &fact.receiver_pattern) ||
        !fact.receiver_pattern) return false;
    core_list_push(env->intrinsic_types, fact);
  }
  return true;
}

static bool finalize_environment(void *object, void *user_data) {
  (void)user_data;
  th_env *env = object;
  core_list_t(th_env_type_origin_t) origins = env->type_origins;
  core_list_t(th_env_trait_t) traits = env->traits;
  core_list_t(th_env_function_t) functions = env->functions;
  core_list_t(th_env_value_t) values = env->values;
  core_list_t(th_env_intrinsic_type_t) intrinsic_types = env->intrinsic_types;
  core_list_t(th_env_metadata_record_t) metadata = env->metadata;
  env->type_origins = core_list_new(th_env_type_origin_t);
  env->traits = core_list_new(th_env_trait_t);
  env->functions = core_list_new(th_env_function_t);
  env->values = core_list_new(th_env_value_t);
  env->intrinsic_types = core_list_new(th_env_intrinsic_type_t);
  env->metadata = core_list_new(th_env_metadata_record_t);
  bool ok = true;
  for (size_t i = 0; ok && i < origins.len; i++) {
    th_env_type_origin_t *entry = &origins.data[i];
    ok = th_env_register_type(env, entry->name, entry->module,
                              entry->package, entry->visibility);
    if (ok && entry->shape_kind != TH_ENV_TYPE_SHAPE_NONE)
      ok = th_env_register_type_shape(env, entry->name, entry->shape_kind,
                                      entry->shape);
  }
  for (size_t i = 0; ok && i < traits.len; i++) {
    th_env_trait_t *entry = &traits.data[i];
    ok = th_env_register_trait_contracts(
        env, entry->name, entry->module, entry->package, entry->visibility,
        entry->contracts);
  }
  for (size_t i = 0; ok && i < functions.len; i++) {
    th_env_function_t *entry = &functions.data[i];
    ok = th_env_register_function_contract(
        env, entry->qualified_name, entry->root_name, entry->module,
        entry->package, entry->visibility, entry->contract)
        && th_env_set_function_required(env, entry->qualified_name,
                                        entry->required_params);
  }
  for (size_t i = 0; ok && i < values.len; i++) {
    th_env_value_t *entry = &values.data[i];
    ok = th_env_register_value(env, entry->qualified_name, entry->root_name,
        entry->module, entry->package, entry->visibility, entry->type);
  }
  for (size_t i = 0; ok && i < intrinsic_types.len; i++) {
    th_env_intrinsic_type_t *entry = &intrinsic_types.data[i];
    ok = th_env_register_intrinsic_type_pattern(
        env, entry->trait_name, entry->receiver_pattern);
  }
  for (size_t i = 0; ok && i < metadata.len; i++) {
    th_env_metadata_record_t *entry = &metadata.data[i];
    ok = th_env_register_metadata(env, entry->owner_kind, entry->owner_name,
                                  entry->key, entry->version, entry->data,
                                  entry->size);
  }
  core_list_free(origins);
  core_list_free(traits);
  core_list_free(functions);
  core_list_free(values);
  core_list_free(intrinsic_types);
  core_list_free(metadata);
  return ok && th_env_freeze(env);
}

static void destroy_environment(void *object, void *user_data) {
  (void)user_data;
  th_env_release_contents(object);
}

static bool register_environment_codecs(
    core_object_archive_registry_t *registry) {
  if (!core_object_archive_register(registry, (core_object_archive_type_t){
          .type_id = TH_ENV_ARCHIVE_ROOT_TYPE,
          .version = 1,
          .allocate = environment_allocate,
          .save = save_environment,
          .restore = restore_environment,
          .finalize = finalize_environment,
          .destroy = destroy_environment})) return false;
  for (uint32_t kind = 0; kind <= TH_TRAIT; kind++)
    if (!core_object_archive_register(registry, (core_object_archive_type_t){
            .type_id = type_archive_id((th_kind)kind),
            .version = 1,
            .allocate = type_allocate,
            .save = save_type,
            .restore = restore_type,
            .user_data = (void *)(uintptr_t)kind})) return false;
  return true;
}

bool th_env_save(const th_env *env, core_string_t path,
                 size_t mapping_capacity, core_object_archive_error_t *error) {
  if (!env || !env->frozen || !path.data) {
    if (error) *error = CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT;
    return false;
  }
  core_object_archive_registry_t *registry =
      core_object_archive_registry_new();
  bool registered = register_environment_codecs(registry);
  core_dict_t var_indices;
  core_dict_init(&var_indices, nullptr, nullptr);
  saving_var_indices = &var_indices;
  bool ok = registered && core_object_archive_save(
      path, mapping_capacity, environment_archive_abi(), registry,
      TH_ENV_ARCHIVE_ROOT_TYPE, env, error);
  saving_var_indices = nullptr;
  core_dict_free(&var_indices);
  if (!registered && error)
    *error = CORE_OBJECT_ARCHIVE_CALLBACK_FAILED;
  core_object_archive_registry_free(registry);
  return ok;
}

th_env *th_env_load(core_string_t path, core_object_archive_error_t *error) {
  if (!path.data) {
    if (error) *error = CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT;
    return nullptr;
  }
  core_object_archive_registry_t *registry =
      core_object_archive_registry_new();
  if (!register_environment_codecs(registry)) {
    core_object_archive_registry_free(registry);
    if (error) *error = CORE_OBJECT_ARCHIVE_CALLBACK_FAILED;
    return nullptr;
  }
  core_object_archive_t *archive = nullptr;
  bool loaded = core_object_archive_load(path, environment_archive_abi(), registry,
                                         &archive, error);
  core_object_archive_registry_free(registry);
  if (!loaded) return nullptr;
  uint64_t type_id;
  th_env *env = core_object_archive_root(archive, &type_id);
  if (!env || type_id != TH_ENV_ARCHIVE_ROOT_TYPE || !env->frozen) {
    core_object_archive_close(archive, nullptr);
    if (error) *error = CORE_OBJECT_ARCHIVE_BAD_FORMAT;
    return nullptr;
  }
  env->persisted_archive = archive;
  return env;
}
