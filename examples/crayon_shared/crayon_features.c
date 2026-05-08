/**
 * @file crayon_features.m
 * @brief Feature extraction + auto-labeling for warehouse events.
 */

#ifdef __APPLE__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crayon_features.h"
#include "crayon_protocol.h"
#include "adt/dict_untyped.h"

// ----------------------------------------------------------------------------
// Tiny n00b JSON walking helpers — local; the public API in json.h gives
// constructors but no field-lookup convenience.
// ----------------------------------------------------------------------------

static n00b_json_node_t *
json_obj_get(n00b_json_node_t *obj, const char *key)
{
    if (!n00b_json_is_object(obj)) return NULL;
    bool  found = false;
    void *v     = _n00b_dict_untyped_get(obj->object, (void *)key, &found);
    return found ? (n00b_json_node_t *)v : NULL;
}

static const char *
json_str(n00b_json_node_t *n)
{
    return (n && n00b_json_is_string(n)) ? n->string : NULL;
}

static int64_t
json_int(n00b_json_node_t *n)
{
    return (n && n00b_json_is_int(n)) ? n->integer : 0;
}

// ----------------------------------------------------------------------------
// Path / argv helpers
// ----------------------------------------------------------------------------

static const char *
path_basename(const char *p)
{
    if (!p) return NULL;
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int
path_depth(const char *p)
{
    if (!p) return 0;
    int n = 0;
    for (const char *s = p; *s; s++) if (*s == '/') n++;
    return n;
}

static const char *
cwd_prefix_bucket(const char *cwd)
{
    if (!cwd) return "cwd:none";
    if (strncmp(cwd, "/Users/",        7) == 0) return "cwd:home";
    if (strncmp(cwd, "/tmp",           4) == 0) return "cwd:tmp";
    if (strncmp(cwd, "/private/tmp",  12) == 0) return "cwd:tmp";
    if (strncmp(cwd, "/var",           4) == 0) return "cwd:var";
    if (strncmp(cwd, "/Applications", 13) == 0) return "cwd:apps";
    if (strncmp(cwd, "/Library",       8) == 0) return "cwd:library";
    if (strncmp(cwd, "/opt",           4) == 0) return "cwd:opt";
    if (strncmp(cwd, "/usr",           4) == 0) return "cwd:usr";
    return "cwd:other";
}

static const char *
exe_prefix_bucket(const char *exe)
{
    if (!exe) return "exe:none";
    if (strncmp(exe, "/Applications", 13) == 0) return "exe:apps";
    if (strncmp(exe, "/usr/bin",       8) == 0) return "exe:usrbin";
    if (strncmp(exe, "/usr/local",    10) == 0) return "exe:usrlocal";
    if (strncmp(exe, "/opt/homebrew", 13) == 0) return "exe:brew";
    if (strncmp(exe, "/opt",           4) == 0) return "exe:opt";
    if (strncmp(exe, "/Users/",        7) == 0) return "exe:home";
    if (strncmp(exe, "/Library",       8) == 0) return "exe:library";
    if (strncmp(exe, "/System",        7) == 0) return "exe:system";
    if (strncmp(exe, "/bin",           4) == 0) return "exe:bin";
    return "exe:other";
}

static const char *
argv_count_bucket(size_t n)
{
    if (n == 0) return "argv:0";
    if (n <= 2) return "argv:1-2";
    if (n <= 5) return "argv:3-5";
    if (n <= 10) return "argv:6-10";
    return "argv:11+";
}

static const char *
exe_depth_bucket(int d)
{
    if (d <= 2) return "depth:1-2";
    if (d <= 4) return "depth:3-4";
    if (d <= 6) return "depth:5-6";
    return "depth:7+";
}

static const char *
ancestry_depth_bucket(size_t d)
{
    if (d <= 1) return "anc:1";
    if (d == 2) return "anc:2";
    if (d == 3) return "anc:3";
    if (d <= 5) return "anc:4-5";
    return "anc:6+";
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

crayon_features_t
crayon_features_register_rule_groups(n00b_ml_trainer_t *trainer)
{
    crayon_features_t s = {
        .lex  = n00b_ml_trainer_define_rule_group_cstr(trainer, "LEX",  1u << 14),
        .flow = n00b_ml_trainer_define_rule_group_cstr(trainer, "FLOW", 1u << 12),
        .geom = n00b_ml_trainer_define_rule_group_cstr(trainer, "GEOM", 256),
        .env  = n00b_ml_trainer_define_rule_group_cstr(trainer, "ENV",  1u << 10),
    };
    return s;
}

bool
crayon_features_project(n00b_ml_input_t         *input,
                        const crayon_features_t *ids,
                        n00b_json_node_t        *event)
{
    n00b_json_node_t *actor = json_obj_get(event, "actor");
    if (!actor) return false;

    // ------ LEX: exe basename, argv tokens, signing_id ------
    const char *exe       = json_str(json_obj_get(actor, "exe_path"));
    const char *exe_base  = path_basename(exe);
    if (exe_base && *exe_base) {
        n00b_ml_input_match_cstr(input, ids->lex, exe_base);
    }

    n00b_json_node_t *argv = json_obj_get(actor, "argv");
    size_t argc = 0;
    if (argv && n00b_json_is_array(argv)) {
        argc = n00b_list_len(argv->array);
        size_t cap = argc < 6 ? argc : 6;     // cap LEX matches per input
        for (size_t i = 0; i < cap; i++) {
            n00b_json_node_t *e = n00b_list_get(argv->array, i);
            const char       *s = json_str(e);
            if (s && *s) {
                // Take the basename if the token looks like a path so we
                // hash on the meaningful tail (e.g. argv[1] = path/to/foo.py
                // contributes "foo.py" as a rule match).
                const char *t = (strchr(s, '/') ? path_basename(s) : s);
                if (t && *t) {
                    n00b_ml_input_match_cstr(input, ids->lex, t);
                }
            }
        }
    }

    const char *sid = json_str(json_obj_get(actor, "signing_id"));
    if (sid) n00b_ml_input_match_cstr(input, ids->lex, sid);

    // ------ FLOW: kind + ancestry depth ------
    const char *kind = json_str(json_obj_get(event, "kind"));
    if (kind) n00b_ml_input_match_cstr(input, ids->flow, kind);

    n00b_json_node_t *anc = json_obj_get(actor, "ancestry");
    size_t anc_len = (anc && n00b_json_is_array(anc))
                         ? n00b_list_len(anc->array) : 0;
    n00b_ml_input_match_cstr(input, ids->flow, ancestry_depth_bucket(anc_len));

    // ------ GEOM: argv count, exe depth ------
    n00b_ml_input_match_cstr(input, ids->geom, argv_count_bucket(argc));
    n00b_ml_input_match_cstr(input, ids->geom, exe_depth_bucket(path_depth(exe)));

    // ------ ENV: signing_id, team_id, cwd / exe path buckets ------
    if (sid) n00b_ml_input_match_cstr(input, ids->env, sid);
    const char *tid = json_str(json_obj_get(actor, "team_id"));
    if (tid) n00b_ml_input_match_cstr(input, ids->env, tid);

    const char *cwd = json_str(json_obj_get(actor, "cwd"));
    n00b_ml_input_match_cstr(input, ids->env, cwd_prefix_bucket(cwd));
    n00b_ml_input_match_cstr(input, ids->env, exe_prefix_bucket(exe));

    return true;
}

bool
crayon_features_classification(n00b_json_node_t *event, uint64_t *out)
{
    n00b_json_node_t *actor = json_obj_get(event, "actor");
    if (!actor) return false;
    n00b_json_node_t *cls = json_obj_get(actor, "classification");
    if (!cls) return false;
    n00b_json_node_t *cats = json_obj_get(cls, "categories");
    if (!cats || !n00b_json_is_int(cats)) return false;
    *out = (uint64_t)cats->integer;
    return true;
}

bool
crayon_features_dev_activity_label(uint64_t mask)
{
    return (mask & (CRAYON_CLASSIFY_BIT(CRAYON_CLASSIFY_CATEGORY_AI)
                    | CRAYON_CLASSIFY_BIT(CRAYON_CLASSIFY_CATEGORY_EDITOR)))
           != 0;
}

#endif // __APPLE__
