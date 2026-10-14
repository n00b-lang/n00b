#pragma once

#include "../internal.h"

typedef enum : uint16_t {
#define WRIT_KIND(id, name) WRIT_N_##id,
#define WRIT_SYNTAX(nt, kind, form, name_form, name_child)
#include "ast_nodes.def"
#undef WRIT_KIND
#undef WRIT_SYNTAX
    WRIT_N_COUNT,
} writ_node_kind_t;

typedef struct {
    writ_node_kind_t kind;
    uint16_t         flags;
    writ_loc_t       loc;
    uint32_t         first_child;
    uint32_t         child_count;
    writ_sym_t       name;
    writ_sym_t       payload;
} writ_node_t;

core_list_decl(writ_node_t);

typedef struct {
    writ_source_kind_t kind;
    uint32_t           source;
    uint32_t           root;
    uint32_t           first_token;
    uint32_t           last_token;
} writ_unit_root_t;

core_list_decl(writ_unit_root_t);
core_list_decl(writ_loc_t);

struct writ_unit {
    writ_session_t                *session;
    core_list_t(writ_node_t)       nodes;
    core_list_t(uint32_t)          children;
    core_list_t(writ_unit_root_t)  roots;
    core_list_t(writ_loc_t)        tokens;
};

const writ_node_t *writ_node(const writ_unit_t *unit, uint32_t id);
uint32_t           writ_node_child(const writ_unit_t *unit, uint32_t id, uint32_t index);
uint32_t           writ_node_first_of(const writ_unit_t *unit, uint32_t id, writ_node_kind_t kind);
const char        *writ_node_kind_name(writ_node_kind_t kind);
void               writ_unit_release(void *unit);
writ_str_t         writ_syntax_audit(writ_session_t *session, bool candidates);
writ_status_t      writ_unit_read_file(writ_unit_t *unit, writ_str_t path, uint32_t *source_id);
