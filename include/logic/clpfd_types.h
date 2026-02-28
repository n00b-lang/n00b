/**
 * @file clpfd_types.h
 * @brief Core types for the constraint satisfaction solver.
 *
 * Defines variable IDs, domain kinds, constraint kinds, and the
 * fundamental data structures for finite-domain constraint solving.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/string.h"

/**
 * @brief Constraint variable identifier.
 */
typedef int32_t n00b_csp_var_id_t;

/**
 * @brief Domain representation kind.
 */
typedef enum {
    N00B_CSP_DOM_INTERVAL, /**< Contiguous [lo, hi]. */
    N00B_CSP_DOM_BITSET,   /**< Small domain (<=64 values), bitmap. */
    N00B_CSP_DOM_SPARSE,   /**< Arbitrary sorted int array. */
    N00B_CSP_DOM_EMPTY,    /**< Empty domain (failure). */
} n00b_csp_dom_kind_t;

/**
 * @brief Finite domain (tagged union of interval/bitset/sparse).
 */
typedef struct {
    n00b_csp_dom_kind_t kind;
    union {
        struct {
            int64_t lo;
            int64_t hi;
        } interval;
        struct {
            int64_t  base; /**< Lowest possible value. */
            uint64_t bits; /**< Bitmask: bit i set => base+i is in domain. */
        } bitset;
        struct {
            int64_t *values; /**< Sorted array of values. */
            int32_t  count;  /**< Number of values. */
            int32_t  cap;    /**< Capacity of values array. */
        } sparse;
    };
} n00b_csp_domain_t;

/**
 * @brief Constraint kind.
 */
typedef enum {
    N00B_CSP_CON_EQ,       /**< X = Y */
    N00B_CSP_CON_EQ_CONST, /**< X = c */
    N00B_CSP_CON_NE,       /**< X != Y */
    N00B_CSP_CON_LT,       /**< X < Y */
    N00B_CSP_CON_LE,       /**< X <= Y */
    N00B_CSP_CON_IN,       /**< X in D */
    N00B_CSP_CON_LINEAR,   /**< a1*X1 + a2*X2 + ... = c */
    N00B_CSP_CON_ALLDIFF,  /**< all_different(X1, X2, ...) */
} n00b_csp_con_kind_t;

/**
 * @brief A constraint in the store.
 */
typedef struct {
    n00b_csp_con_kind_t  kind;
    n00b_csp_var_id_t   *vars;      /**< Variables involved. */
    int32_t              var_count;
    int64_t             *coeffs;    /**< LINEAR: coefficients (one per var). */
    int64_t              constant;  /**< LINEAR: RHS constant; EQ_CONST: value. */
    n00b_csp_domain_t    in_domain; /**< IN: target domain. */
    bool                 entailed;  /**< True if constraint permanently satisfied. */
    bool                 failed;    /**< True if constraint cannot be satisfied. */
} n00b_csp_constraint_t;

/**
 * @brief A constraint variable.
 */
typedef struct {
    n00b_csp_var_id_t  id;
    n00b_string_t     *name;   /**< Variable name. */
    n00b_csp_domain_t  domain;
    bool               ground; /**< True if domain is singleton. */
    int64_t            value;  /**< Value if ground. */
} n00b_csp_var_t;

