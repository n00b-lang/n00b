#ifndef IR_SUMMARY_INTERNAL_H
#define IR_SUMMARY_INTERNAL_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/layout.h"
#include "ir/operands.h"
#include "ir/pdg_memory.h"
#include "ir/summary.h"
#include "ir/text.h"
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

core_list_decl(ir_sfield_t);
core_list_decl(ir_splace_t);
core_list_decl(ir_strap_site_t);
core_list_decl(ir_switness_t);
core_list_decl(ir_sheap_t);
core_list_decl(uint8_t);
typedef ir_func_t *summary_func_ptr_t;
core_list_decl(summary_func_ptr_t);
typedef const ir_instr_t *summary_instr_ptr_t;
core_list_decl(summary_instr_ptr_t);
typedef ir_summary_t *summary_ptr_t;
core_list_decl(summary_ptr_t);
core_list_decl(uint64_t);
typedef core_list_t(uint64_t) flow_bits_t;
typedef ir_spath_t *spath_ptr_t;
core_list_decl(spath_ptr_t);

typedef struct {
    ir_spath_t *to;
    flow_bits_t data;
    flow_bits_t control;
    flow_bits_t alias;
    flow_bits_t copy;
} flow_dest_t;
core_list_decl(flow_dest_t);

typedef struct {
    core_list_t(spath_ptr_t) sources;
    core_dict_t              source_index;
    core_list_t(flow_dest_t) dests;
    core_dict_t              dest_index;
    const ir_func_t         *function;
} flow_table_t;

typedef struct {
    const ir_func_t *function;
    uint64_t         content;
    uint64_t         points_to;
} summary_dep_t;
core_list_decl(summary_dep_t);

struct ir_summary_t {
    const ir_func_t           *function;
    uint32_t                   flags;
    uint32_t                   own_flags;
    uint8_t                    own_traps;
    uint32_t                   callback_params;
    bool                       deferring;
    bool                       results_known;
    bool                       returns_external;
    uint8_t                    pointer_result;
    flow_table_t               flows[2];
    uint8_t                    traps;
    core_list_t(ir_strap_site_t) trap_sites;
    core_list_t(ir_switness_t) witnesses;
    core_list_t(ir_sheap_t)    heaps;
    core_list_t(ir_sfield_t)   fields;
    core_list_t(core_string_t) effects;
    core_list_t(ir_spath_t)    paths;
    core_list_t(summary_dep_t) deps;
    uint64_t                   identity;
    uint64_t                   content;
    bool                       hashed;
    bool                       pinned;
    bool                       reads_registry;
    bool                       proof_free;
    bool                       bookkept;
    bool                       explained;
};

struct ir_summaries_t {
    const ir_summary_hooks_t *hooks;
    ir_summary_hooks_t        hook_values;
    core_list_t(summary_ptr_t) owned;
    core_dict_t               by_function;
    core_dict_t               type_memos;
    core_dict_t               engines;
    bool                      escapes_only;
    bool                      reusable;
    bool                      registry_changed;
    bool                      proofs_changed;
    ir_summaries_t           *previous;
    ir_summary_t             *recording;
    core_dict_t               adopted;
};

typedef ir_spath_list_t path_list_t;

enum { SPATH_INLINE = IR_SPATH_ROOT };

typedef struct {
    ir_summaries_t                *summaries;
    core_list_t(summary_func_ptr_t) targets;
    bool                           unknown;
} call_targets_t;

typedef struct pt_t pt_t;

typedef struct {
    ir_summaries_t    *summaries;
    ir_func_t         *f;
    pt_t              *pt;
    const ir_splace_t *target;
    const ir_spath_t  *leaf;
    core_dict_t       *path_places;
    core_dict_t       *stores;
    core_dict_t       *effects;
    ir_splace_t        effects_place;
} flow_query_t;

typedef struct {
    ir_summaries_t   *summaries;
    ir_func_t *const *members;
    size_t            count;
} recursion_t;

enum { PT_EXTERNAL = UINT32_MAX };

typedef struct {
    ir_spath_t path;
    uint32_t   site;
} pt_loc_t;

typedef struct stored_t stored_t;

typedef struct {
    pt_loc_t  loc;
    stored_t *entry;
} pt_frame_t;
core_list_decl(pt_frame_t);

typedef enum : uint8_t {
    PT_SAME,
    PT_COPY,
    PT_DERIVED,
    PT_CONTROL,
} pt_flow_kind_t;

typedef enum : uint8_t {
    PT_ADDRESS,
    PT_CONTENTS,
} pt_kind_t;

typedef struct {
    uint32_t             depth;
    bool                 deep;
    pt_kind_t            kind;
    pt_flow_kind_t       flow;
    const core_string_t *steps;
    pt_loc_t             loc;
    const ir_instr_t    *origin;
} pt_item_t;
core_list_decl(pt_item_t);
typedef core_list_t(pt_item_t) pt_set_t;
core_list_decl(pt_set_t);
typedef const pt_set_t *pt_set_ptr_t;
core_list_decl(pt_set_ptr_t);

typedef struct {
    pt_item_t                        item;
    core_list_t(summary_instr_ptr_t) origins;
} pt_alias_t;
core_list_decl(pt_alias_t);
typedef core_list_t(pt_alias_t) alias_list_t;

typedef struct {
    const void       *key;
    th_type          *type;
    bool              elements;
    pt_set_t          contents;
    alias_list_t      aliases;
    const ir_local_t *local;
    const ir_value_t *value;
    const ir_instr_t *call;
    bool              node;
    uint32_t          origin;
} pt_site_t;
core_list_decl(pt_site_t);

typedef struct {
    uint32_t   site;
    ir_spath_t path;
    bool       weak;
    pt_set_t   items;
} pt_write_t;
core_list_decl(pt_write_t);
typedef core_list_t(pt_write_t) pt_writes_t;

typedef enum : uint8_t {
    FLOW_KILL_NONE,
    FLOW_KILL_CELL,
    FLOW_KILL_SITE,
} flow_kill_t;

typedef struct {
    const ir_instr_t *instr;
    uint32_t          record;
    uint32_t          site;
    core_string_t     cell;
    flow_kill_t       kill;
    bool              member;
    bool              pointers;
} flow_def_t;
core_list_decl(flow_def_t);

typedef struct {
    core_dict_t             writes;
    core_dict_t             positions;
    core_dict_t             pending;
    core_dict_t             released;
    core_list_t(flow_def_t) defs;
    core_dict_t             at_instr;
    core_dict_t             covered;
    core_dict_t             visited;
    core_list_t(uint32_t)   walk;
    core_list_t(uint32_t)   reached;
    uint64_t                mark;
    ir_pdg_memory_t        *memory;
    ir_rd_t                *rd;
    bool                    sourced;
    bool                    stale;
    pt_set_t                items;
} pt_flow_t;

typedef struct {
    call_targets_t             targets;
    core_list_t(summary_ptr_t) callees;
    bool                       known;
} pt_callees_t;
core_list_decl(pt_callees_t);

typedef struct {
    core_dict_t pointer_types;
    core_dict_t members;
    core_dict_t definitions;
    core_dict_t fields;
    core_dict_t slots;
} type_memo_t;


typedef struct {
    pt_loc_t at;
    uint32_t site;
} pt_external_write_t;
core_list_decl(pt_external_write_t);
typedef core_list_t(pt_external_write_t) pt_external_writes_t;
core_list_decl(pt_external_writes_t);

typedef struct {
    ir_splace_t place;
    bool        load;
} pt_access_t;
core_list_decl(pt_access_t);

typedef struct {
    core_list_t(ir_spath_t) cells;
    core_dict_t             gens;
    uint64_t               *in;
    uint64_t               *state;
    uint32_t                words;
    bool                    built;
} pt_entry_t;

struct pt_t {
    ir_summaries_t        *summaries;
    const ir_func_t       *f;
    core_dict_t            value_index;
    core_list_t(pt_set_t)  values;
    core_dict_t            site_index;
    core_list_t(pt_site_t) sites;
    type_memo_t           *types;
    core_dict_t            load_uses;
    ir_reach_t            *reach;
    core_list_t(uint32_t)  aliased;
    core_dict_t            alias_roots;
    core_dict_t            changed;
    core_dict_t            mapped;
    core_dict_t            stored;
    core_dict_t            contents_memo;
    core_dict_t            aliased_memo;
    core_dict_t            widened_sets;
    uint64_t               cuts;
    uint64_t               delta_since;
    core_dict_t            valued;
    bool                   positional;
    core_dict_t            external_index;
    core_dict_t            site_sources;
    flow_table_t           source_names;
    core_dict_t            source_maps;
    core_list_t(uint64_t)  source_stack;
    uint32_t               source_order;
    core_dict_t            flow_nodes;
    core_dict_t            heap_sites;
    core_dict_t            copy_ins;
    pt_set_t               published;
    core_dict_t            escaped;
    bool                   escaped_built;
    core_dict_t            flow_groups;
    core_dict_t            value_groups;
    core_list_t(pt_external_writes_t) external_writes;
    core_list_t(uint32_t)  described;
    core_dict_t            call_index;
    core_list_t(pt_callees_t) calls;
    pt_set_t               scratch[3];
    flow_bits_t            crossed;
    uint32_t               crossed_known;
    pt_set_t               seen;
    core_list_t(pt_frame_t) reading;
    core_dict_t            resolved;
    core_list_t(uint64_t) *resolving;
    uint64_t               generation;
    ir_places_t           *places;
    core_list_t(pt_access_t) context;
    bool                   contextual;
    const ir_instr_t      *at;
    pt_flow_t              flow;
    core_list_t(summary_instr_ptr_t) order;
    core_dict_t            readers;
    bool                  *dirty;
    uint32_t               reader;
    bool                   pending;
    bool                   value_flow;
    uint32_t               nodes;
    ir_pdg_t              *pdg;
    const ir_points_to_hooks_t *hooks;
    ir_switness_t               unknown;
    uint64_t                    unknown_at;
    pt_entry_t                  entry;
};

enum { PT_NO_READER = UINT32_MAX, PT_EXTERNAL_WRITES_KEY = 2 };

struct stored_t {
    pt_loc_t loc;
    uint64_t generation;
    pt_set_t items;
    bool     partial;
    bool     busy;
};

typedef struct {
    core_list_t(uint64_t) depends;
    uint64_t              generation;
    const ir_instr_t     *at;
    uint32_t              reader;
    bool                  computed;
    bool                  positional;
} pt_memo_t;

typedef struct {
    pt_loc_t  loc;
    pt_set_t  items;
    pt_memo_t memo;
} resolved_t;

typedef struct {
    pt_loc_t          loc;
    const ir_instr_t *at;
} content_key_t;

typedef struct {
    pt_item_t item;
    uint64_t  generation;
} pt_logged_t;
core_list_decl(pt_logged_t);

typedef struct {
    content_key_t            key;
    pt_set_t                 items;
    const pt_set_t          *widened;
    core_list_t(pt_logged_t) log;
    pt_memo_t                memo;
} content_memo_t;

typedef struct {
    pt_loc_t loc;
    uint64_t signature;
} aliased_key_t;

typedef struct {
    aliased_key_t         key;
    core_list_t(uint64_t) visible;
    pt_set_t              items;
    pt_memo_t             memo;
} aliased_memo_t;

typedef struct {
    pt_loc_t  loc;
    uint8_t   mode;
    pt_set_t  items;
    pt_memo_t memo;
} valued_t;

typedef struct {
    const ir_instr_t *call;
    uintptr_t         origin;
} heap_site_key_t;

typedef struct {
    const ir_instr_t   *call;
    const ir_summary_t *callee;
    uintptr_t           table;
    uintptr_t           source;
} mapped_key_t;

typedef struct {
    mapped_key_t key;
    pt_set_t     items;
    pt_set_t     added;
    pt_memo_t    memo;
    uint32_t     version;
    bool         grew;
} mapped_entry_t;

typedef struct {
    core_list_t(uint64_t) spans;
    core_list_t(uint32_t) pool;
} source_map_t;

typedef struct {
    pt_loc_t    loc;
    flow_bits_t data;
    flow_bits_t control;
    uint32_t    order;
    uint32_t    low;
    bool        open;
    bool        done;
} site_sources_t;

static bool same_flows(const flow_table_t *a, const flow_table_t *b);

static th_type *pointee_type(ir_module_t *module, th_type *type);
static bool pointer_class(ir_module_t *module, th_type *type);
static bool generic_type(th_type *type);
static th_type *defined_type(const ir_func_t *f, th_type *type);
static uint32_t inline_depth(const ir_func_t *f, th_type *type,
                             bool sequence_root, const ir_spath_t *at,
                             bool *known);
static th_type *end_type(ir_module_t *module, th_type *type,
                         const ir_spath_t *path);
static bool refers_beyond(ir_module_t *module, th_type *type);
static th_type *root_object_type(const ir_func_t *f, const ir_spath_t *path,
                                 const pt_t *pt);

static bool resolved_eq(const void *a, const void *b);

typedef struct {
    core_list_t(uint64_t) *resolving;
    bool                   positional;
    uint64_t               delta_since;
} pt_memo_frame_t;

static bool memo_stale(pt_t *pt, const pt_memo_t *memo);
static void memo_begin(pt_t *pt, pt_memo_t *memo, pt_memo_frame_t *frame);
static void memo_end(pt_t *pt, pt_memo_t *memo, const pt_memo_frame_t *frame);
static void memo_use(pt_t *pt, pt_memo_t *memo);

static void record_set(pt_t *pt, ir_summary_t *summary,
                       const pt_set_t *addresses, uint8_t flags,
                       th_type *carried);
static void record_stores(pt_t *pt, ir_summary_t *summary,
                          const pt_set_t *addresses, const pt_set_t *values);
static bool copy_in_param(const pt_t *pt, uint32_t site, ir_spath_t *own);

static pt_t *function_engine(ir_summaries_t *summaries, ir_func_t *f);
static const flow_bits_t *crossings(pt_t *pt, const pt_loc_t *loc);
static bool engine_escapes(ir_summaries_t *summaries, const ir_func_t *f,
                           const ir_value_t *address);

#endif
