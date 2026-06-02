/**
 * @file elf_rewrite_admit.h
 * @brief Strict ELF metadata-rewrite admission vocabulary.
 *
 * The admission layer consumes an already parsed ELF object and a
 * metadata-section insertion request. It is deliberately separate from
 * parsing: the parser stays lenient, and parse failures remain
 * `n00b_elf_parse()` errors rather than rewrite rejections.
 *
 * Admission is a read-only decision layer. It never mutates the parsed ELF
 * object, never emits replacement bytes, and never produces a patch plan.
 * The result is an admission verdict with placement facts. It is not a patch
 * plan, and no returned offset authorizes mutation by itself.
 */
#pragma once

#include <stdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "adt/option.h"
#include "adt/result.h"
#include "compiler/objfile/elf.h"

#define N00B_ELF_REWRITE_ADMIT_OK 0
#define N00B_ELF_REWRITE_ADMIT_ERR_NULL_BINARY       (-3501)
#define N00B_ELF_REWRITE_ADMIT_ERR_NULL_REQUEST      (-3502)
#define N00B_ELF_REWRITE_ADMIT_ERR_NULL_SECTION_NAME (-3503)
#define N00B_ELF_REWRITE_ADMIT_ERR_ZERO_PAYLOAD_SIZE (-3504)
#define N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE  (-3505)
#define N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW          (-3506)

typedef enum {
    N00B_ELF_REWRITE_ADMIT_POLICY_NONE = 0,
    N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION = 1u << 0,
    /** Preserve existing overlay bytes; does not by itself choose placement. */
    N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY           = 1u << 1,
    /** Permit EOF-tail placement after an overlay when preserve is also set. */
    N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY       = 1u << 2,
} n00b_elf_rewrite_admit_policy_flag_t;

typedef struct n00b_elf_rewrite_admit_policy {
    uint64_t flags;
} n00b_elf_rewrite_admit_policy_t;

/**
 * @brief Metadata-section insertion request.
 *
 * The request borrows `section_name`; callers retain ownership and only need
 * it to remain alive for the admission call. The payload bytes themselves are
 * not supplied, only their size. `file_alignment == 0` is normalized to byte
 * alignment.
 *
 * Phase 2 admits only non-loadable metadata sections: `SHT_PROGBITS` or
 * `SHT_NOTE` with `section_flags == 0`.
 *
 * `preferred_file_offset` is an admission-only candidate. When set, admission
 * classifies exactly that file range and either returns a placement fact or a
 * stable rejection reason. It does not request byte mutation and is not a
 * patch-plan operation.
 */
typedef struct n00b_elf_rewrite_admit_metadata_request {
    n00b_string_t                     *section_name;
    uint64_t                           payload_size;
    uint64_t                           file_alignment;
    uint32_t                           section_type;
    uint64_t                           section_flags;
    n00b_option_t(uint64_t)            preferred_file_offset;
    n00b_elf_rewrite_admit_policy_t    policy;
} n00b_elf_rewrite_admit_metadata_request_t;

typedef enum {
    N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED,
    N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED,
} n00b_elf_rewrite_admit_outcome_t;

typedef enum {
    N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE,
    N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL,
    N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP,
    N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY,
} n00b_elf_rewrite_admit_placement_kind_t;

typedef enum {
    N00B_ELF_REWRITE_ADMIT_REJECT_NONE,
    N00B_ELF_REWRITE_ADMIT_REJECT_NOT_YET_CHECKED,
    N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME,
    N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA,
    N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT,
    N00B_ELF_REWRITE_ADMIT_REJECT_FILE_COLLISION,
    N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES,
    N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY,
    N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD,
    N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_MISSING,
    N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT,
    N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD,
    N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY,
    N00B_ELF_REWRITE_ADMIT_REJECT_LOADER_PRESERVATION,
} n00b_elf_rewrite_admit_rejection_reason_t;

typedef struct n00b_elf_rewrite_admit_placement {
    n00b_elf_rewrite_admit_placement_kind_t kind;
    uint64_t                                file_offset;
    uint64_t                                file_end;
    uint64_t                                payload_size;
    uint64_t                                file_alignment;
} n00b_elf_rewrite_admit_placement_t;

typedef struct n00b_elf_rewrite_admit_result {
    n00b_elf_rewrite_admit_outcome_t             outcome;
    n00b_elf_rewrite_admit_rejection_reason_t    rejection_reason;
    n00b_option_t(n00b_elf_rewrite_admit_placement_t) placement;
    uint64_t                                     file_size;
    uint64_t                                     effective_alignment;
    n00b_elf_rewrite_admit_policy_t              policy;
} n00b_elf_rewrite_admit_result_t;

/**
 * @brief Admit a metadata-section insertion request.
 *
 * Valid metadata-only requests return an accepted verdict with concrete
 * placement facts. Unsafe rewrite cases return `Ok(rejected result)` with a
 * stable N00b-owned rejection reason; API and substrate failures return
 * `Err(N00B_ELF_REWRITE_ADMIT_ERR_*)`.
 *
 * This function does not perform byte mutation, table movement, patch
 * planning, loadable-segment insertion, or PHTAB adjustment planning.
 *
 * @param bin Parsed ELF object from @ref n00b_elf_parse.
 * @param request Metadata-section insertion request.
 * @kw allocator Defaults to `nullptr`, meaning the current runtime allocator.
 *               Used only for admission-owned layout substrate allocations.
 *
 * @return Ok(result) or Err(N00B_ELF_REWRITE_ADMIT_ERR_*).
 *
 * @pre `bin`, `request`, and `request->section_name` are non-null.
 * @pre `request->payload_size` is nonzero.
 * @post `bin` and its parsed sections, segments, stream, and overlay pointers
 *       are not modified.
 */
extern n00b_result_t(n00b_elf_rewrite_admit_result_t)
n00b_elf_rewrite_admit_metadata_insert(
    n00b_elf_binary_t                         *bin,
    n00b_elf_rewrite_admit_metadata_request_t *request) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Look up a human-readable string for an admission error code.
 *
 * @param err Error code returned by the ELF rewrite admission API.
 *
 * @return A process-lifetime string literal. Unknown codes return a stable
 *         fallback message.
 */
extern n00b_string_t *
n00b_elf_rewrite_admit_err_str(n00b_err_t err);

/**
 * @brief Look up a stable name for a rewrite admission policy flag.
 *
 * @param flag One `N00B_ELF_REWRITE_ADMIT_POLICY_*` flag.
 *
 * @return A process-lifetime string literal. Unknown values return a stable
 *         fallback message.
 */
extern n00b_string_t *
n00b_elf_rewrite_admit_policy_flag_str(
    n00b_elf_rewrite_admit_policy_flag_t flag);

/**
 * @brief Look up a stable name for an admission outcome.
 *
 * @param outcome Admission outcome value.
 *
 * @return A process-lifetime string literal. Unknown values return a stable
 *         fallback message.
 */
extern n00b_string_t *
n00b_elf_rewrite_admit_outcome_str(
    n00b_elf_rewrite_admit_outcome_t outcome);

/**
 * @brief Look up a stable name for a candidate placement kind.
 *
 * @param kind Placement kind value.
 *
 * @return A process-lifetime string literal. Unknown values return a stable
 *         fallback message.
 */
extern n00b_string_t *
n00b_elf_rewrite_admit_placement_kind_str(
    n00b_elf_rewrite_admit_placement_kind_t kind);

/**
 * @brief Look up a stable name for an admission rejection reason.
 *
 * @param reason Rejection reason value.
 *
 * @return A process-lifetime string literal. Unknown values return a stable
 *         fallback message.
 */
extern n00b_string_t *
n00b_elf_rewrite_admit_rejection_reason_str(
    n00b_elf_rewrite_admit_rejection_reason_t reason);
