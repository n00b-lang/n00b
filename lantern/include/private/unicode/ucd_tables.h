#pragma once

#include "core.h"
#include "lib/string.h"

typedef struct {
    uint32_t first;
    uint32_t last;
} ucd_range_t;

typedef struct {
    uint32_t first;
    uint32_t last;
    uint8_t  value;
} ucd_valued_range_t;

typedef struct {
    uint32_t cp;
    uint32_t offset;
    uint8_t  length;
    bool     compat;
} ucd_mapping_t;

typedef struct {
    uint32_t first;
    uint32_t second;
    uint32_t composite;
} ucd_composition_t;

typedef struct {
    core_string_t long_name;
    core_string_t short_name;
} ucd_script_name_t;

extern const core_string_t ucd_unicode_version;
extern const ucd_range_t ucd_xid_start[];
extern const size_t ucd_xid_start_count;
extern const ucd_range_t ucd_xid_continue[];
extern const size_t ucd_xid_continue_count;
extern const ucd_range_t ucd_extended_pictographic[];
extern const size_t ucd_extended_pictographic_count;
extern const ucd_range_t ucd_wide[];
extern const size_t ucd_wide_count;
extern const ucd_range_t ucd_zero_width[];
extern const size_t ucd_zero_width_count;
extern const ucd_script_name_t ucd_script_names[];
extern const size_t ucd_script_names_count;
extern const ucd_valued_range_t ucd_scripts[];
extern const size_t ucd_scripts_count;
extern const ucd_valued_range_t ucd_combining_class[];
extern const size_t ucd_combining_class_count;
extern const uint32_t ucd_decompositions_pool[];
extern const ucd_mapping_t ucd_decompositions[];
extern const size_t ucd_decompositions_count;
extern const ucd_composition_t ucd_compositions[];
extern const size_t ucd_compositions_count;
extern const uint32_t ucd_confusables_pool[];
extern const ucd_mapping_t ucd_confusables[];
extern const size_t ucd_confusables_count;
