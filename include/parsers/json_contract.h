/**
 * @file json_contract.h
 * @brief Small JSON request contract helpers.
 *
 * This is intentionally not JSON Schema. It is a narrow helper layer for
 * endpoint code that needs predictable field extraction and useful diagnostics.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "adt/list.h"
#include "core/string.h"
#include "parsers/json.h"

typedef struct n00b_json_contract n00b_json_contract_t;

typedef enum {
    N00B_JSON_CONTRACT_ANY,
    N00B_JSON_CONTRACT_NULL,
    N00B_JSON_CONTRACT_BOOL,
    N00B_JSON_CONTRACT_INT,
    N00B_JSON_CONTRACT_DOUBLE,
    N00B_JSON_CONTRACT_NUMBER,
    N00B_JSON_CONTRACT_STRING,
    N00B_JSON_CONTRACT_ARRAY,
    N00B_JSON_CONTRACT_OBJECT,
} n00b_json_contract_type_t;

extern n00b_json_contract_t *
n00b_json_contract_new()
    _kargs { n00b_allocator_t *allocator = nullptr; };

extern void
n00b_json_contract_reset(n00b_json_contract_t *contract);

extern bool
n00b_json_contract_ok(n00b_json_contract_t *contract);

extern size_t
n00b_json_contract_error_count(n00b_json_contract_t *contract);

extern n00b_string_t *
n00b_json_contract_error(n00b_json_contract_t *contract, size_t index);

extern n00b_string_t *
n00b_json_contract_summary(n00b_json_contract_t *contract);

extern void
n00b_json_contract_add_error(n00b_json_contract_t *contract,
                             n00b_string_t        *path,
                             n00b_string_t        *message);

extern bool
n00b_json_contract_validate(n00b_json_contract_t      *contract,
                            n00b_json_node_t          *value,
                            n00b_string_t             *path,
                            n00b_json_contract_type_t  type)
    _kargs { bool nullable = false; };

extern n00b_json_node_t *
n00b_json_contract_required(n00b_json_contract_t      *contract,
                            n00b_json_node_t          *object,
                            n00b_string_t             *field,
                            n00b_json_contract_type_t  type)
    _kargs {
        bool           nullable = false;
        n00b_string_t *path     = nullptr;
    };

extern n00b_json_node_t *
n00b_json_contract_optional(n00b_json_contract_t      *contract,
                            n00b_json_node_t          *object,
                            n00b_string_t             *field,
                            n00b_json_contract_type_t  type,
                            n00b_json_node_t          *default_value)
    _kargs {
        bool           nullable = false;
        n00b_string_t *path     = nullptr;
    };

extern n00b_json_node_t *
n00b_json_contract_required_enum(n00b_json_contract_t        *contract,
                                 n00b_json_node_t            *object,
                                 n00b_string_t               *field,
                                 n00b_list_t(n00b_string_t *) *allowed)
    _kargs {
        bool           nullable = false;
        n00b_string_t *path     = nullptr;
    };

extern n00b_json_node_t *
n00b_json_contract_optional_enum(n00b_json_contract_t        *contract,
                                 n00b_json_node_t            *object,
                                 n00b_string_t               *field,
                                 n00b_list_t(n00b_string_t *) *allowed,
                                 n00b_json_node_t            *default_value)
    _kargs {
        bool           nullable = false;
        n00b_string_t *path     = nullptr;
    };
