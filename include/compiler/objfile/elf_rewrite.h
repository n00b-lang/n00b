/**
 * @file elf_rewrite.h
 * @brief Surgical ELF metadata-rewrite planning and apply API.
 *
 * This layer turns strict metadata-rewrite admission facts into an explicit
 * patch plan, then applies accepted plans to a new in-memory byte buffer.
 * Planning and application never mutate the parsed ELF object or its original
 * byte stream.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"
#include "adt/array.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_rewrite_admit.h"

#define N00B_ELF_REWRITE_OK 0
#define N00B_ELF_REWRITE_ERR_NULL_BINARY       (-3601)
#define N00B_ELF_REWRITE_ERR_NULL_REQUEST      (-3602)
#define N00B_ELF_REWRITE_ERR_NULL_SECTION_NAME (-3603)
#define N00B_ELF_REWRITE_ERR_NULL_PAYLOAD      (-3604)
#define N00B_ELF_REWRITE_ERR_ZERO_PAYLOAD_SIZE (-3605)
#define N00B_ELF_REWRITE_ERR_TARGET_PROFILE    (-3606)
#define N00B_ELF_REWRITE_ERR_ADMISSION         (-3607)
#define N00B_ELF_REWRITE_ERR_OVERFLOW          (-3608)
#define N00B_ELF_REWRITE_ERR_NULL_PLAN         (-3609)
#define N00B_ELF_REWRITE_ERR_PLAN_REJECTED     (-3610)
#define N00B_ELF_REWRITE_ERR_UNSUPPORTED_PLAN  (-3611)
#define N00B_ELF_REWRITE_ERR_APPLY             (-3612)
#define N00B_ELF_REWRITE_ERR_PARSE_AFTER_APPLY (-3613)
#define N00B_ELF_REWRITE_ERR_MARK_NOT_FOUND    (-3614)
#define N00B_ELF_REWRITE_ERR_TRUSTED_NAME      (-3615)

typedef enum {
    N00B_ELF_REWRITE_PLAN_ACCEPTED,
    N00B_ELF_REWRITE_PLAN_REJECTED,
} n00b_elf_rewrite_plan_outcome_t;

typedef enum {
    N00B_ELF_REWRITE_OPERATION_METADATA_INSERT,
    N00B_ELF_REWRITE_OPERATION_CHALK_MARK_DELETE,
    N00B_ELF_REWRITE_OPERATION_CHALK_MARK_REPLACE,
} n00b_elf_rewrite_operation_t;

typedef enum {
    N00B_ELF_REWRITE_REJECT_NONE,
    N00B_ELF_REWRITE_REJECT_TARGET_PROFILE,
    N00B_ELF_REWRITE_REJECT_ADMISSION,
    N00B_ELF_REWRITE_REJECT_TABLE_PLACEMENT,
    N00B_ELF_REWRITE_REJECT_SECTION_COUNT_PROMOTION,
    N00B_ELF_REWRITE_REJECT_OVERFLOW,
    N00B_ELF_REWRITE_REJECT_CHALK_MARK_NOT_FOUND,
    N00B_ELF_REWRITE_REJECT_CHALK_MARK_UNSUPPORTED,
    N00B_ELF_REWRITE_REJECT_TRUSTED_NAME,
} n00b_elf_rewrite_rejection_reason_t;

typedef enum {
    N00B_ELF_REWRITE_PROFILE_OK,
    N00B_ELF_REWRITE_PROFILE_INVALID_EHSIZE,
    N00B_ELF_REWRITE_PROFILE_INVALID_SHENTSIZE,
    N00B_ELF_REWRITE_PROFILE_INVALID_PHENTSIZE,
    N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_SHNLO,
    N00B_ELF_REWRITE_PROFILE_SHTAB_SIZE,
    N00B_ELF_REWRITE_PROFILE_INVALID_SHTAB_BOUNDS,
    N00B_ELF_REWRITE_PROFILE_ZERO_PHNUM,
    N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_PXNUM,
    N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB,
    N00B_ELF_REWRITE_PROFILE_INVALID_PHTAB_BOUNDS,
    N00B_ELF_REWRITE_PROFILE_NO_STRTAB,
    N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE_WRAP,
    N00B_ELF_REWRITE_PROFILE_UNSUPPORTED_STRTAB_SHNLO,
    N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_INDEX,
    N00B_ELF_REWRITE_PROFILE_INVALID_STRTAB_TYPE,
    N00B_ELF_REWRITE_PROFILE_STRTAB_SIZE,
    N00B_ELF_REWRITE_PROFILE_SECTION_NAME_INDEX,
} n00b_elf_rewrite_target_profile_reason_t;

typedef enum {
    N00B_ELF_REWRITE_TABLE_STRATEGY_NONE,
    N00B_ELF_REWRITE_TABLE_STRATEGY_IN_PLACE_SHSTRTAB_GROWTH,
    N00B_ELF_REWRITE_TABLE_STRATEGY_TERMINAL_TAIL_INCISION,
    N00B_ELF_REWRITE_TABLE_STRATEGY_EOF_REPLACEMENT,
    N00B_ELF_REWRITE_TABLE_STRATEGY_AFTER_OVERLAY_REPLACEMENT,
} n00b_elf_rewrite_table_strategy_t;

typedef enum {
    N00B_ELF_REWRITE_PATCH_ELF_HEADER,
    N00B_ELF_REWRITE_PATCH_PAYLOAD,
    N00B_ELF_REWRITE_PATCH_SECTION_NAME_STRTAB,
    N00B_ELF_REWRITE_PATCH_SECTION_HEADER_TABLE,
    N00B_ELF_REWRITE_PATCH_TABLE_TAIL,
    N00B_ELF_REWRITE_PATCH_APPENDED_TABLES,
    N00B_ELF_REWRITE_PATCH_STALE_PAYLOAD,
} n00b_elf_rewrite_patch_kind_t;

typedef struct n00b_elf_rewrite_metadata_request {
    n00b_string_t                  *section_name;
    n00b_buffer_t                  *payload;
    uint64_t                        file_alignment;
    uint32_t                        section_type;
    uint64_t                        section_flags;
    n00b_option_t(uint64_t)         preferred_file_offset;
    n00b_elf_rewrite_admit_policy_t policy;
} n00b_elf_rewrite_metadata_request_t;

typedef struct n00b_elf_rewrite_target_profile {
    n00b_elf_rewrite_target_profile_reason_t reason;
    int                                      packager_errcode;
    bool                                     shstrtab_requires_terminator;
    uint64_t                                 file_size;
    uint64_t                                 section_count;
    uint64_t                                 segment_count;
    uint64_t                                 shstrtab_offset;
    uint64_t                                 shstrtab_reported_size;
    uint64_t                                 shstrtab_complete_size;
} n00b_elf_rewrite_target_profile_t;

typedef struct n00b_elf_rewrite_patch {
    n00b_elf_rewrite_patch_kind_t kind;
    uint64_t                      file_offset;
    uint64_t                      file_end;
    uint64_t                      original_file_offset;
    uint64_t                      original_file_end;
} n00b_elf_rewrite_patch_t;

typedef struct n00b_elf_rewrite_plan {
    n00b_elf_rewrite_operation_t                 operation;
    n00b_elf_rewrite_plan_outcome_t             outcome;
    n00b_elf_rewrite_rejection_reason_t         rejection_reason;
    n00b_elf_rewrite_target_profile_t           target_profile;
    n00b_elf_rewrite_admit_result_t             admission;
    n00b_elf_rewrite_table_strategy_t           table_strategy;
    n00b_array_t(n00b_elf_rewrite_patch_t)      patches;
    n00b_string_t                              *section_name;
    n00b_buffer_t                              *payload;
    uint64_t                                    section_alignment;
    uint32_t                                    section_type;
    uint64_t                                    section_flags;
    uint64_t                                    file_size;
    uint64_t                                    original_section_count;
    uint64_t                                    new_section_count;
    uint64_t                                    removed_section_index;
    uint64_t                                    removed_payload_offset;
    uint64_t                                    removed_payload_end;
    uint16_t                                    new_shstrndx;
    uint64_t                                    payload_offset;
    uint64_t                                    payload_end;
} n00b_elf_rewrite_plan_t;

/**
 * @brief Plan insertion of a non-loadable ELF metadata section.
 *
 * The request borrows `section_name` and `payload`; callers retain ownership.
 * Accepted plans describe the changed file ranges but do not perform those
 * writes. Rejected plans use stable N00b-owned reasons and, where applicable,
 * target-profile facts that map to Brandon packager's `read-target` behavior.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Metadata-section insertion request.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *               Owns the returned plan and patch array.
 *
 * @return Ok(plan) for accepted or rejected plans, or Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `request`, `request->section_name`, and `request->payload` are non-null.
 * @pre `request->payload->byte_len` is nonzero.
 * @post `bin`, its stream, and its parsed section and segment arrays are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_metadata_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Plan trusted insertion of Chalk's exact `.chalk.mark` section.
 *
 * This is the narrow reserved-name affordance needed by Chalk. It admits only
 * an exact `.chalk.mark` request for non-loadable metadata insertion; any other
 * requested name is rejected. The ordinary
 * @ref n00b_elf_rewrite_plan_metadata_insert path continues to reject
 * `.chalk.mark`, arbitrary `.chalk.*`, `.0c001.*`, and targets with existing
 * reserved metadata sections.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Metadata-section insertion request whose name is exactly
 *                `.chalk.mark`.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(plan) for accepted or rejected plans, or Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `request`, `request->section_name`, and `request->payload` are non-null.
 * @pre `request->payload->byte_len` is nonzero.
 * @post `bin`, its stream, and its parsed section and segment arrays are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Plan surgical deletion of one live non-loadable `.chalk.mark`.
 *
 * Accepted plans append replacement section-name and section-header tables,
 * update the ELF header to point at them, and mark the old mark payload range
 * as `N00B_ELF_REWRITE_PATCH_STALE_PAYLOAD` so application can zero it.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(plan) for accepted or rejected plans, or Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `bin->stream`, and `bin->stream->buf` are non-null.
 * @post `bin`, its stream, and its parsed section and segment arrays are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_delete(
    n00b_elf_binary_t *bin) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Plan surgical replacement of one live non-loadable `.chalk.mark`.
 *
 * Replacement removes the old live section table entry, excludes the old
 * payload bytes by zeroing their range at apply time, and emits exactly one
 * live `.chalk.mark` entry for the supplied payload.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Replacement request whose section name is exactly `.chalk.mark`.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(plan) for accepted or rejected plans, or Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `request`, `request->section_name`, and `request->payload` are non-null.
 * @pre `request->payload->byte_len` is nonzero.
 * @post `bin`, its stream, and its parsed section and segment arrays are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_plan_t *)
n00b_elf_rewrite_plan_chalk_mark_replace(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Apply an accepted metadata-insert rewrite plan to bytes.
 *
 * The plan must come from @ref n00b_elf_rewrite_plan_metadata_insert or
 * @ref n00b_elf_rewrite_plan_chalk_mark_insert for the same parsed binary.
 * Accepted plans borrow the metadata name, payload, type, flags, and alignment
 * from the original request so this plan-first API does not need the request
 * again.
 *
 * Accepted plans may place metadata payload bytes at EOF, in an admitted
 * zero-padding file gap, or after a preserved overlay when policy permits
 * appending there. Overlay-bearing inputs are applied only for plans whose
 * table strategy appends replacement metadata tables after the overlay.
 *
 * @param bin Parsed ELF object whose stream supplies the original bytes.
 * @param plan Accepted metadata-insert plan.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *               Owns the returned output buffer.
 *
 * @return Ok(buffer) containing the rewritten ELF bytes, or
 *         Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `bin->stream`, `bin->stream->buf`, and `plan` are non-null.
 * @pre `plan->outcome == N00B_ELF_REWRITE_PLAN_ACCEPTED`.
 * @post `bin`, its stream buffer, and parsed arrays are not modified.
 * @post The returned buffer reparses successfully with @ref n00b_elf_parse.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_metadata_insert_plan(
    n00b_elf_binary_t         *bin,
    n00b_elf_rewrite_plan_t   *plan) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Apply an accepted Chalk mark delete or replace plan to bytes.
 *
 * @param bin Parsed ELF object whose stream supplies the original bytes.
 * @param plan Accepted plan from @ref n00b_elf_rewrite_plan_chalk_mark_delete
 *             or @ref n00b_elf_rewrite_plan_chalk_mark_replace.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(buffer) containing the rewritten ELF bytes, or
 *         Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @pre `bin`, `bin->stream`, `bin->stream->buf`, and `plan` are non-null.
 * @pre `plan->outcome == N00B_ELF_REWRITE_PLAN_ACCEPTED`.
 * @post The returned buffer reparses successfully with @ref n00b_elf_parse.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_plan(
    n00b_elf_binary_t       *bin,
    n00b_elf_rewrite_plan_t *plan) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Convenience wrapper that plans and applies a metadata insertion.
 *
 * This is a request-level shortcut over
 * @ref n00b_elf_rewrite_plan_metadata_insert and
 * @ref n00b_elf_rewrite_apply_metadata_insert_plan. Callers that need to
 * inspect planned changed ranges should use the plan-first API directly.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Metadata-section insertion request.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(buffer), Err(N00B_ELF_REWRITE_ERR_*), or
 *         Err(N00B_ELF_REWRITE_ERR_PLAN_REJECTED) when admission/planning
 *         returns a rejected plan.
 *
 * @post `bin`, its stream buffer, and parsed arrays are not modified.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_metadata_insert(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Convenience wrapper that plans and applies `.chalk.mark` deletion.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(buffer), Err(N00B_ELF_REWRITE_ERR_*), or
 *         Err(N00B_ELF_REWRITE_ERR_PLAN_REJECTED) when planning rejects.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_delete(
    n00b_elf_binary_t *bin) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Convenience wrapper that plans and applies `.chalk.mark` replacement.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Replacement request whose section name is exactly `.chalk.mark`.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *
 * @return Ok(buffer), Err(N00B_ELF_REWRITE_ERR_*), or
 *         Err(N00B_ELF_REWRITE_ERR_PLAN_REJECTED) when planning rejects.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_elf_rewrite_apply_chalk_mark_replace(
    n00b_elf_binary_t                   *bin,
    n00b_elf_rewrite_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Evaluate packager-compatible rewrite target semantics.
 *
 * This is stricter than parsing and inspects raw section-header bytes where
 * parsed names have already been normalized.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @return Ok(profile) or Err(N00B_ELF_REWRITE_ERR_*).
 *
 * @post `bin` and its stream position are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_target_profile_t)
n00b_elf_rewrite_target_profile(n00b_elf_binary_t *bin);

/**
 * @param err ELF rewrite error code.
 * @return Stable process-lifetime diagnostic string.
 */
extern n00b_string_t *n00b_elf_rewrite_err_str(n00b_err_t err);

/**
 * @param outcome Metadata rewrite plan outcome.
 * @return Stable process-lifetime enum-name string.
 */
extern n00b_string_t *n00b_elf_rewrite_plan_outcome_str(
    n00b_elf_rewrite_plan_outcome_t outcome);

/**
 * @param reason Metadata rewrite rejection reason.
 * @return Stable process-lifetime enum-name string.
 */
extern n00b_string_t *n00b_elf_rewrite_rejection_reason_str(
    n00b_elf_rewrite_rejection_reason_t reason);

/**
 * @param reason Target profile rejection reason.
 * @return Stable process-lifetime enum-name string.
 */
extern n00b_string_t *n00b_elf_rewrite_target_profile_reason_str(
    n00b_elf_rewrite_target_profile_reason_t reason);

/**
 * @param strategy Metadata table placement strategy.
 * @return Stable process-lifetime enum-name string.
 */
extern n00b_string_t *n00b_elf_rewrite_table_strategy_str(
    n00b_elf_rewrite_table_strategy_t strategy);

/**
 * @param kind Planned byte-range patch kind.
 * @return Stable process-lifetime enum-name string.
 */
extern n00b_string_t *n00b_elf_rewrite_patch_kind_str(
    n00b_elf_rewrite_patch_kind_t kind);
