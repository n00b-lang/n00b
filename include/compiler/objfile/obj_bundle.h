/**
 * @file obj_bundle.h
 * @brief Format-neutral object bundle construction and manifest codec API.
 *
 * Object bundles are canonical, carrier-independent collections of artifacts,
 * execution mappings, and policy records. ELF, Mach-O, and PE carriers are
 * backend concerns layered below this API.
 *
 * Related modules:
 * - @ref result.h for structured result payloads.
 * - @ref buffer.h for artifact and manifest byte payloads.
 * - @ref types.h for object-file format enums.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "n00b.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "compiler/objfile/types.h"

#define N00B_OBJ_BUNDLE_MANIFEST_MAGIC_LEN 8u
#define N00B_OBJ_BUNDLE_POLICY_MAGIC_LEN   8u
#define N00B_OBJ_BUNDLE_MANIFEST_MAJOR     1u
#define N00B_OBJ_BUNDLE_MANIFEST_MINOR     0u
#define N00B_OBJ_BUNDLE_CONTENT_ID_LEN     32u
#define N00B_OBJ_BUNDLE_DIGEST_LEN         32u
#define N00B_OBJ_BUNDLE_ARTIFACT_ID_NONE   UINT64_MAX
#define N00B_OBJ_BUNDLE_POLICY_ID_NONE     UINT64_MAX

extern const uint8_t
    N00B_OBJ_BUNDLE_MANIFEST_MAGIC[N00B_OBJ_BUNDLE_MANIFEST_MAGIC_LEN];
extern const uint8_t
    N00B_OBJ_BUNDLE_POLICY_MAGIC[N00B_OBJ_BUNDLE_POLICY_MAGIC_LEN];

typedef struct n00b_obj_bundle n00b_obj_bundle_t;
typedef struct n00b_obj_bundle_artifact n00b_obj_bundle_artifact_t;
typedef struct n00b_obj_bundle_error n00b_obj_bundle_error_t;

typedef enum {
    N00B_OBJ_BUNDLE_ARTIFACT_FILE,
    N00B_OBJ_BUNDLE_ARTIFACT_EXECUTABLE,
    N00B_OBJ_BUNDLE_ARTIFACT_DIRECTORY,
    N00B_OBJ_BUNDLE_ARTIFACT_METADATA,
    N00B_OBJ_BUNDLE_ARTIFACT_OPAQUE,
} n00b_obj_bundle_artifact_kind_t;

typedef enum {
    N00B_OBJ_BUNDLE_CARRIER_AUTO,
    N00B_OBJ_BUNDLE_CARRIER_METADATA,
    N00B_OBJ_BUNDLE_CARRIER_LOADABLE,
    N00B_OBJ_BUNDLE_CARRIER_SPLIT,
} n00b_obj_bundle_carrier_t;

typedef enum {
    N00B_OBJ_BUNDLE_REJECT_EXISTING,
    N00B_OBJ_BUNDLE_REPLACE_EXISTING,
} n00b_obj_bundle_replace_policy_t;

typedef enum {
    N00B_OBJ_BUNDLE_POLICY_ENFORCE,
    N00B_OBJ_BUNDLE_POLICY_VALIDATE_ONLY,
} n00b_obj_bundle_policy_mode_t;

typedef enum {
    N00B_OBJ_BUNDLE_POLICY_KIND_NONE,
    N00B_OBJ_BUNDLE_POLICY_KIND_BUILTIN_DEFAULT,
    N00B_OBJ_BUNDLE_POLICY_KIND_DECLARATIVE_V1,
} n00b_obj_bundle_policy_kind_t;

typedef enum {
    N00B_OBJ_BUNDLE_POLICY_SCOPE_NONE       = 0,
    N00B_OBJ_BUNDLE_POLICY_SCOPE_EXTRACTION = 1u << 0,
    N00B_OBJ_BUNDLE_POLICY_SCOPE_EXECUTION  = 1u << 1,
    N00B_OBJ_BUNDLE_POLICY_SCOPE_BOTH =
        N00B_OBJ_BUNDLE_POLICY_SCOPE_EXTRACTION
        | N00B_OBJ_BUNDLE_POLICY_SCOPE_EXECUTION,
} n00b_obj_bundle_policy_scope_t;

typedef enum {
    N00B_OBJ_BUNDLE_POLICY_F_REQUIRED         = 1u << 0,
    N00B_OBJ_BUNDLE_POLICY_F_OPTIONAL         = 1u << 1,
    N00B_OBJ_BUNDLE_POLICY_F_FALLBACK_ALLOWED = 1u << 2,
} n00b_obj_bundle_policy_flags_t;

typedef enum {
    N00B_OBJ_BUNDLE_ERR_OK                     = 0,
    N00B_OBJ_BUNDLE_ERR_INVALID_ARGUMENT       = -3701,
    N00B_OBJ_BUNDLE_ERR_INVALID_LOGICAL_PATH   = -3702,
    N00B_OBJ_BUNDLE_ERR_DUPLICATE_LOGICAL_PATH = -3703,
    N00B_OBJ_BUNDLE_ERR_DUPLICATE_SELECTOR     = -3704,
    N00B_OBJ_BUNDLE_ERR_MULTIPLE_DEFAULT_EXEC  = -3705,
    N00B_OBJ_BUNDLE_ERR_MISSING_TARGET         = -3706,
    N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_VERSION    = -3707,
    N00B_OBJ_BUNDLE_ERR_UNSUPPORTED_FEATURE    = -3708,
    N00B_OBJ_BUNDLE_ERR_MALFORMED_MANIFEST     = -3709,
    N00B_OBJ_BUNDLE_ERR_NON_CANONICAL_MANIFEST = -3710,
    N00B_OBJ_BUNDLE_ERR_OUT_OF_BOUNDS          = -3711,
    N00B_OBJ_BUNDLE_ERR_DIGEST_MISMATCH        = -3712,
    N00B_OBJ_BUNDLE_ERR_BUILD                  = -3713,
    N00B_OBJ_BUNDLE_ERR_DUPLICATE_POLICY_ID    = -3714,
} n00b_obj_bundle_error_code_t;

/**
 * @post On success, returns an empty mutable object bundle.
 * @post The returned bundle and its owned data use @c allocator.
 * @kw allocator Optional allocator for the returned bundle; default `nullptr`.
 */
extern n00b_result_t(n00b_obj_bundle_t *)
n00b_obj_bundle_new() _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @pre @p bundle is non-null and mutable.
 * @pre @p logical_path is non-null and valid UTF-8.
 * @pre @p bytes is non-null for file, executable, and opaque artifacts.
 * @post On success, @p bundle contains one artifact at the normalized path.
 * @post On error, @p bundle is unchanged.
 * @kw kind Artifact kind; default `N00B_OBJ_BUNDLE_ARTIFACT_FILE`.
 * @kw role Optional role string; default `nullptr`.
 * @kw mode Intended filesystem mode for extraction; default `0`.
 * @kw flags Required/optional artifact flags; default `0`.
 */
extern n00b_result_t(bool)
n00b_obj_bundle_add_artifact(n00b_obj_bundle_t *bundle,
                             n00b_string_t     *logical_path,
                             const n00b_buffer_t *bytes) _kargs {
    n00b_obj_bundle_artifact_kind_t kind  = N00B_OBJ_BUNDLE_ARTIFACT_FILE;
    n00b_string_t                  *role  = nullptr;
    uint32_t                        mode  = 0;
    uint64_t                        flags = 0;
};

/**
 * @pre @p bundle is non-null and mutable.
 * @pre @p logical_path names an existing executable-compatible artifact.
 * @post On success, @p logical_path is the bundle's only default executable.
 * @post On error, @p bundle is unchanged.
 */
extern n00b_result_t(bool)
n00b_obj_bundle_set_default_exec(n00b_obj_bundle_t *bundle,
                                 n00b_string_t     *logical_path);

/**
 * @pre @p bundle is non-null and mutable.
 * @pre @p selector is non-null.
 * @pre @p target_path names an existing executable-compatible artifact.
 * @post On success, @p selector maps to @p target_path.
 * @post Duplicate selectors are rejected and leave @p bundle unchanged.
 */
extern n00b_result_t(bool)
n00b_obj_bundle_add_exec_mapping(n00b_obj_bundle_t *bundle,
                                 n00b_string_t     *selector,
                                 n00b_string_t     *target_path);

/**
 * @pre @p bundle is non-null and mutable.
 * @post On success, @p bundle contains one policy record with @p policy_id.
 * @post On error, @p bundle is unchanged.
 * @kw flags Policy compatibility flags; default
 *      `N00B_OBJ_BUNDLE_POLICY_F_REQUIRED`.
 * @kw priority Selection priority among policies for the same scope; default
 *      `0`.
 * @kw payload Optional policy payload bytes; default `nullptr`.
 * @kw fallback_policy_id Lower-priority fallback policy or no-fallback
 *      sentinel; default `N00B_OBJ_BUNDLE_POLICY_ID_NONE`.
 */
extern n00b_result_t(bool)
n00b_obj_bundle_add_policy(n00b_obj_bundle_t             *bundle,
                           uint64_t                      policy_id,
                           n00b_obj_bundle_policy_kind_t  kind,
                           n00b_obj_bundle_policy_scope_t scope) _kargs {
    uint64_t       flags              = N00B_OBJ_BUNDLE_POLICY_F_REQUIRED;
    uint64_t       priority           = 0;
    const n00b_buffer_t *payload      = nullptr;
    uint64_t       fallback_policy_id = N00B_OBJ_BUNDLE_POLICY_ID_NONE;
};

/**
 * @pre @p bundle is non-null.
 * @post On success, all bundle model invariants hold.
 * @post Validation has no side effects.
 * @kw strict Reject non-canonical but otherwise interpretable input when true;
 *      default `true`.
 */
extern n00b_result_t(bool)
n00b_obj_bundle_validate(n00b_obj_bundle_t *bundle) _kargs {
    bool strict = true;
};

/**
 * @pre @p bundle is non-null and valid.
 * @post On success, returns canonical object-bundle bytes independent of any
 *       object-file carrier.
 * @post The returned buffer is independent of @p bundle mutation after return.
 * @kw allocator Optional allocator for the returned buffer; default `nullptr`.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_obj_bundle_encode(n00b_obj_bundle_t *bundle) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @pre @p bundle_bytes is non-null.
 * @post On success, returns a fully valid object bundle.
 * @post On error, no partially decoded bundle escapes.
 * @kw strict Reject non-canonical manifests when true; default `true`.
 * @kw allocator Optional allocator for the returned bundle; default `nullptr`.
 */
extern n00b_result_t(n00b_obj_bundle_t *)
n00b_obj_bundle_decode(n00b_buffer_t *bundle_bytes) _kargs {
    bool              strict    = true;
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @pre @p error is non-null.
 * @return Stable object-bundle error code carried by @p error.
 */
extern n00b_obj_bundle_error_code_t
n00b_obj_bundle_error_code(n00b_obj_bundle_error_t *error);

/**
 * @return Human-readable description for any object-bundle error code.
 */
extern n00b_string_t *
n00b_obj_bundle_err_str(n00b_err_t err);

/**
 * @pre @p error is non-null.
 * @return Error message when one was attached, otherwise none.
 */
extern n00b_option_t(n00b_string_t *)
n00b_obj_bundle_error_message(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Object format context when the error is carrier-specific,
 *         otherwise none.
 */
extern n00b_option_t(n00b_format_t)
n00b_obj_bundle_error_format(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Carrier-placement context when known, otherwise none.
 */
extern n00b_option_t(n00b_obj_bundle_carrier_t)
n00b_obj_bundle_error_carrier(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Logical bundle path associated with the error, otherwise none.
 */
extern n00b_option_t(n00b_string_t *)
n00b_obj_bundle_error_logical_path(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Artifact ID associated with the error, otherwise none.
 */
extern n00b_option_t(uint64_t)
n00b_obj_bundle_error_artifact_id(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Policy kind associated with the error, otherwise none.
 */
extern n00b_option_t(n00b_obj_bundle_policy_kind_t)
n00b_obj_bundle_error_policy_kind(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Policy scope associated with the error, otherwise none.
 */
extern n00b_option_t(n00b_obj_bundle_policy_scope_t)
n00b_obj_bundle_error_policy_scope(n00b_obj_bundle_error_t *error);

/**
 * @pre @p error is non-null.
 * @return Backend-specific detail value associated with the error, otherwise
 *         none.
 */
extern n00b_option_t(int64_t)
n00b_obj_bundle_error_detail(n00b_obj_bundle_error_t *error);
