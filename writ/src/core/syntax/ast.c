#include "syntax.h"

static const char *const kind_names[] = {
#define WRIT_KIND(id, name) name,
#define WRIT_SYNTAX(nt, kind, form, name_form, name_child)
#include "ast_nodes.def"
#undef WRIT_KIND
#undef WRIT_SYNTAX
};

const char *
writ_node_kind_name(writ_node_kind_t kind)
{
    return kind < WRIT_N_COUNT ? kind_names[kind] : "?";
}

const writ_node_t *
writ_node(const writ_unit_t *unit, uint32_t id)
{
    return id && id < unit->nodes.len ? &unit->nodes.data[id] : &unit->nodes.data[0];
}

uint32_t
writ_node_child(const writ_unit_t *unit, uint32_t id, uint32_t index)
{
    const writ_node_t *node = writ_node(unit, id);
    return index < node->child_count ? unit->children.data[node->first_child + index] : 0;
}

uint32_t
writ_node_first_of(const writ_unit_t *unit, uint32_t id, writ_node_kind_t kind)
{
    const writ_node_t *node = writ_node(unit, id);
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t child = unit->children.data[node->first_child + i];
        if (unit->nodes.data[child].kind == kind) {
            return child;
        }
    }
    return 0;
}

writ_unit_t *
writ_unit_new(writ_session_t *session)
{
    if (!session) {
        return nullptr;
    }
    writ_unit_t *unit = writ_alloc(session, sizeof(writ_unit_t));
    *unit = (writ_unit_t){
        .session  = session,
        .nodes    = core_list_new(writ_node_t),
        .children = core_list_new(uint32_t),
        .roots    = core_list_new(writ_unit_root_t),
        .tokens   = core_list_new(writ_loc_t),
        .files    = core_list_new(uint32_t),
        .contract_dirs = core_list_new(uint32_t),
    };
    core_list_push(unit->nodes, ((writ_node_t){0}));
    writ_on_session_free(session, writ_unit_release, unit);
    return unit;
}

void
writ_unit_release(void *object)
{
    writ_unit_t *unit = object;
    core_list_free(unit->nodes);
    core_list_free(unit->children);
    core_list_free(unit->roots);
    core_list_free(unit->tokens);
    core_list_free(unit->files);
    core_list_free(unit->contract_dirs);
}

writ_status_t
writ_unit_add_contract_dir(writ_unit_t *unit, writ_str_t dir)
{
    if (!unit || !dir.length) {
        return WRIT_INVALID_ARGUMENT;
    }
    core_list_push(unit->contract_dirs, writ_intern_str(unit->session, dir));
    return WRIT_OK;
}

size_t
writ_unit_file_count(const writ_unit_t *unit)
{
    return unit ? unit->files.len : 0;
}

writ_str_t
writ_unit_file_at(const writ_unit_t *unit, size_t index)
{
    return unit && index < unit->files.len ? writ_sym_str(unit->session, unit->files.data[index])
                                           : (writ_str_t){nullptr, 0};
}

uint32_t
writ_unit_next_item(const writ_unit_t *unit, writ_node_kind_t kind, uint32_t after)
{
    bool passed = !after;
    for (size_t i = 0; i < unit->roots.len; i++) {
        uint32_t root = unit->roots.data[i].root;
        for (uint32_t c = 0; c < writ_node(unit, root)->child_count; c++) {
            uint32_t item = writ_node_child(unit, root, c);
            if (passed && writ_node(unit, item)->kind == kind) {
                return item;
            }
            passed = passed || item == after;
        }
    }
    return 0;
}

static void
dump_node(const writ_unit_t *unit, writ_emit_t *emit, uint32_t id)
{
    const writ_node_t *node = writ_node(unit, id);
    writ_str_t name = writ_sym_str(unit->session, node->name);
    writ_str_t payload = writ_sym_str(unit->session, node->payload);
    writ_emit_line(emit, "%s%s%.*s%s%.*s", writ_node_kind_name(node->kind), name.length ? " " : "",
                   (int)name.length, name.data, payload.length ? " " : "", (int)payload.length, payload.data);
    writ_emit_indent(emit, 1);
    for (uint32_t i = 0; i < node->child_count; i++) {
        dump_node(unit, emit, writ_node_child(unit, id, i));
    }
    writ_emit_indent(emit, -1);
}

writ_status_t
writ_unit_dump(const writ_unit_t *unit, writ_str_t *rendered)
{
    if (!unit || !rendered) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_emit_t emit;
    writ_emit_init(&emit);
    for (size_t i = 0; i < unit->roots.len; i++) {
        dump_node(unit, &emit, unit->roots.data[i].root);
    }
    *rendered = writ_emit_finish(&emit, unit->session);
    return WRIT_OK;
}
