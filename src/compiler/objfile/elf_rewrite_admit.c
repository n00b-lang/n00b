#include "compiler/objfile/elf_rewrite_admit.h"

#include "compiler/objfile/elf_layout.h"
#include "text/strings/string_ops.h"

#define N00B_ELF_SHT_CRASHOVERRIDE_GUARD 0xc001u

typedef struct phtab_range {
    bool     present;
    uint64_t start;
    uint64_t end;
    uint64_t size;
} phtab_range_t;

static bool
checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (UINT64_MAX - a < b) {
        return false;
    }

    *out = a + b;
    return true;
}

static bool
checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static uint64_t
effective_alignment(uint64_t requested)
{
    if (requested == 0) {
        return 1;
    }

    return requested;
}

static n00b_err_t
layout_err_to_admit_err(n00b_err_t err)
{
    if (err == N00B_ELF_LAYOUT_ERR_OVERFLOW) {
        return N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW;
    }

    return N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE;
}

static bool
policy_has(n00b_elf_rewrite_admit_policy_t policy,
           n00b_elf_rewrite_admit_policy_flag_t flag)
{
    return (policy.flags & (uint64_t)flag) != 0;
}

static bool
policy_allows_append_after_overlay(n00b_elf_rewrite_admit_policy_t policy)
{
    return policy_has(policy, N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY)
        && policy_has(policy, N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY);
}

static n00b_elf_rewrite_admit_result_t
make_result(n00b_elf_layout_t                         *layout,
            n00b_elf_rewrite_admit_metadata_request_t *request,
            uint64_t                                   alignment,
            n00b_elf_rewrite_admit_outcome_t           outcome,
            n00b_elf_rewrite_admit_rejection_reason_t  reason,
            n00b_option_t(n00b_elf_rewrite_admit_placement_t) placement)
{
    return (n00b_elf_rewrite_admit_result_t){
        .outcome             = outcome,
        .rejection_reason    = reason,
        .placement           = placement,
        .file_size           = layout->file_size,
        .effective_alignment = alignment,
        .policy              = request->policy,
    };
}

static n00b_result_t(n00b_elf_rewrite_admit_result_t)
accepted(n00b_elf_layout_t                         *layout,
         n00b_elf_rewrite_admit_metadata_request_t *request,
         uint64_t                                   alignment,
         n00b_elf_rewrite_admit_placement_t         placement)
{
    n00b_elf_rewrite_admit_result_t result = make_result(
        layout,
        request,
        alignment,
        N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED,
        N00B_ELF_REWRITE_ADMIT_REJECT_NONE,
        n00b_option_set(n00b_elf_rewrite_admit_placement_t, placement));

    return n00b_result_ok(n00b_elf_rewrite_admit_result_t, result);
}

static n00b_result_t(n00b_elf_rewrite_admit_result_t)
rejected(n00b_elf_layout_t                         *layout,
         n00b_elf_rewrite_admit_metadata_request_t *request,
         uint64_t                                   alignment,
         n00b_elf_rewrite_admit_rejection_reason_t  reason)
{
    n00b_elf_rewrite_admit_result_t result = make_result(
        layout,
        request,
        alignment,
        N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED,
        reason,
        n00b_option_none(n00b_elf_rewrite_admit_placement_t));

    return n00b_result_ok(n00b_elf_rewrite_admit_result_t, result);
}

static bool
section_name_is_reserved(n00b_string_t *name)
{
    if (n00b_unicode_str_eq(name, r".chalk.mark")) {
        return true;
    }

    if (n00b_unicode_str_eq(name, r".chalk.free")) {
        return true;
    }

    return n00b_unicode_str_starts_with(name, r".0c001.");
}

static bool
target_has_reserved_section(n00b_elf_binary_t *bin)
{
    for (uint32_t i = 0; i < bin->num_sections; i++) {
        n00b_elf_section_t *sec = &bin->sections[i];

        if (sec->type == N00B_ELF_SHT_CRASHOVERRIDE_GUARD) {
            return true;
        }

        if (sec->name != nullptr && section_name_is_reserved(sec->name)) {
            return true;
        }
    }

    return false;
}

static bool
request_is_metadata_only(n00b_elf_rewrite_admit_metadata_request_t *request)
{
    if (request->section_flags != 0) {
        return false;
    }

    switch (request->section_type) {
    case SHT_PROGBITS:
    case SHT_NOTE:
        return true;
    default:
        return false;
    }
}

static n00b_result_t(phtab_range_t)
get_phtab_range(n00b_elf_binary_t *bin)
{
    phtab_range_t range = {};

    if (bin->header.phoff == 0 || bin->header.phnum == 0
        || bin->header.phentsize == 0) {
        return n00b_result_ok(phtab_range_t, range);
    }

    if (!checked_mul_u64(bin->header.phentsize, bin->header.phnum,
                         &range.size)) {
        return n00b_result_err(phtab_range_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
    }

    if (!checked_add_u64(bin->header.phoff, range.size, &range.end)) {
        return n00b_result_err(phtab_range_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
    }

    range.present = range.size != 0;
    range.start   = bin->header.phoff;
    return n00b_result_ok(phtab_range_t, range);
}

static n00b_result_t(n00b_option_t(uint64_t))
load_vaddr_for_file_range(n00b_elf_binary_t *bin,
                          uint64_t           start,
                          uint64_t           end)
{
    for (uint32_t i = 0; i < bin->num_segments; i++) {
        n00b_elf_segment_t *seg = &bin->segments[i];

        if (seg->type != PT_LOAD || start < seg->offset) {
            continue;
        }

        uint64_t seg_file_end;
        if (!checked_add_u64(seg->offset, seg->filesz, &seg_file_end)) {
            return n00b_result_err(n00b_option_t(uint64_t),
                                   N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
        }

        if (end > seg_file_end) {
            continue;
        }

        uint64_t delta = start - seg->offset;
        uint64_t vaddr;
        if (!checked_add_u64(seg->vaddr, delta, &vaddr)) {
            return n00b_result_err(n00b_option_t(uint64_t),
                                   N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
        }

        return n00b_result_ok(n00b_option_t(uint64_t),
                              n00b_option_set(uint64_t, vaddr));
    }

    return n00b_result_ok(n00b_option_t(uint64_t),
                          n00b_option_none(uint64_t));
}

static n00b_result_t(n00b_elf_rewrite_admit_rejection_reason_t)
check_pt_phdr(n00b_elf_binary_t        *bin,
              phtab_range_t             phtab,
              n00b_option_t(uint64_t)   phtab_vaddr)
{
    bool saw_pt_phdr = false;

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        n00b_elf_segment_t *seg = &bin->segments[i];

        if (seg->type != PT_PHDR) {
            continue;
        }

        if (saw_pt_phdr || !phtab.present
            || !n00b_option_is_set(phtab_vaddr)
            || seg->offset != phtab.start
            || seg->filesz != phtab.size
            || seg->memsz != phtab.size
            || seg->vaddr != n00b_option_get(phtab_vaddr)) {
            return n00b_result_ok(
                n00b_elf_rewrite_admit_rejection_reason_t,
                N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT);
        }

        saw_pt_phdr = true;
    }

    if (!saw_pt_phdr) {
        return n00b_result_ok(n00b_elf_rewrite_admit_rejection_reason_t,
                              N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_MISSING);
    }

    return n00b_result_ok(n00b_elf_rewrite_admit_rejection_reason_t,
                          N00B_ELF_REWRITE_ADMIT_REJECT_NONE);
}

static n00b_result_t(n00b_elf_rewrite_admit_rejection_reason_t)
check_entrypoint(n00b_elf_binary_t *bin)
{
    if (bin->header.entry == 0) {
        return n00b_result_ok(n00b_elf_rewrite_admit_rejection_reason_t,
                              N00B_ELF_REWRITE_ADMIT_REJECT_NONE);
    }

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        n00b_elf_segment_t *seg = &bin->segments[i];

        if (seg->type != PT_LOAD || (seg->flags & PF_X) == 0
            || bin->header.entry < seg->vaddr) {
            continue;
        }

        uint64_t mem_end;
        if (!checked_add_u64(seg->vaddr, seg->memsz, &mem_end)) {
            return n00b_result_err(n00b_elf_rewrite_admit_rejection_reason_t,
                                   N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
        }

        if (bin->header.entry >= mem_end) {
            continue;
        }

        uint64_t delta = bin->header.entry - seg->vaddr;
        if (delta >= seg->filesz) {
            return n00b_result_ok(
                n00b_elf_rewrite_admit_rejection_reason_t,
                N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY);
        }

        return n00b_result_ok(n00b_elf_rewrite_admit_rejection_reason_t,
                              N00B_ELF_REWRITE_ADMIT_REJECT_NONE);
    }

    return n00b_result_ok(n00b_elf_rewrite_admit_rejection_reason_t,
                          N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD);
}

static n00b_result_t(n00b_elf_rewrite_admit_rejection_reason_t)
check_loader_preservation(n00b_elf_binary_t *bin)
{
    auto phtab_result = get_phtab_range(bin);
    if (n00b_result_is_err(phtab_result)) {
        return n00b_result_err(n00b_elf_rewrite_admit_rejection_reason_t,
                               n00b_result_get_err(phtab_result));
    }

    phtab_range_t phtab = n00b_result_get(phtab_result);
    n00b_option_t(uint64_t) phtab_vaddr = n00b_option_none(uint64_t);

    if (phtab.present) {
        auto vaddr_result = load_vaddr_for_file_range(bin,
                                                      phtab.start,
                                                      phtab.end);
        if (n00b_result_is_err(vaddr_result)) {
            return n00b_result_err(n00b_elf_rewrite_admit_rejection_reason_t,
                                   n00b_result_get_err(vaddr_result));
        }

        phtab_vaddr = n00b_result_get(vaddr_result);
        if (!n00b_option_is_set(phtab_vaddr)) {
            return n00b_result_ok(
                n00b_elf_rewrite_admit_rejection_reason_t,
                N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD);
        }
    }

    auto phdr_reason = check_pt_phdr(bin, phtab, phtab_vaddr);
    if (n00b_result_is_err(phdr_reason)
        || n00b_result_get(phdr_reason)
               != N00B_ELF_REWRITE_ADMIT_REJECT_NONE) {
        return phdr_reason;
    }

    return check_entrypoint(bin);
}

static bool
collision_has_overlay(n00b_elf_layout_collision_t *collision)
{
    for (uint64_t i = 0; i < collision->interval_count; i++) {
        if (collision->intervals[i].kind == N00B_ELF_LAYOUT_INTERVAL_OVERLAY) {
            return true;
        }
    }

    return false;
}

static n00b_elf_rewrite_admit_placement_t
placement_from_range(n00b_elf_rewrite_admit_placement_kind_t kind,
                     uint64_t                                start,
                     uint64_t                                end,
                     uint64_t                                payload_size,
                     uint64_t                                alignment)
{
    return (n00b_elf_rewrite_admit_placement_t){
        .kind           = kind,
        .file_offset    = start,
        .file_end       = end,
        .payload_size   = payload_size,
        .file_alignment = alignment,
    };
}

static n00b_result_t(n00b_elf_rewrite_admit_result_t)
admit_preferred_placement(n00b_elf_binary_t                         *bin,
                          n00b_elf_layout_t                         *layout,
                          n00b_elf_rewrite_admit_metadata_request_t *request,
                          uint64_t                                   alignment,
                          n00b_allocator_t                          *allocator)
{
    uint64_t start = n00b_option_get(request->preferred_file_offset);
    if (start % alignment != 0) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT);
    }

    uint64_t end;
    if (!checked_add_u64(start, request->payload_size, &end)) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW);
    }

    auto collision_result = n00b_elf_layout_file_collision(layout,
                                                           start,
                                                           end,
                                                           .allocator = allocator);
    if (n00b_result_is_err(collision_result)) {
        return n00b_result_err(
            n00b_elf_rewrite_admit_result_t,
            layout_err_to_admit_err(n00b_result_get_err(collision_result)));
    }

    n00b_elf_layout_collision_t collision = n00b_result_get(collision_result);
    if (collision.interval_count != 0) {
        if (collision_has_overlay(&collision)) {
            return rejected(
                layout,
                request,
                alignment,
                N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);
        }

        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_FILE_COLLISION);
    }

    auto gap_result = n00b_elf_layout_find_file_gap(layout,
                                                    start,
                                                    end,
                                                    request->payload_size,
                                                    alignment);
    if (n00b_result_is_err(gap_result)) {
        return n00b_result_err(
            n00b_elf_rewrite_admit_result_t,
            layout_err_to_admit_err(n00b_result_get_err(gap_result)));
    }

    n00b_option_t(n00b_elf_layout_gap_t) gap_opt = n00b_result_get(gap_result);
    if (!n00b_option_is_set(gap_opt)) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT);
    }

    n00b_elf_layout_gap_t gap = n00b_option_get(gap_opt);
    if (gap.start != start || gap.end < end) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT);
    }

    switch (gap.kind) {
    case N00B_ELF_LAYOUT_GAP_ZERO_PADDING:
        return accepted(
            layout,
            request,
            alignment,
            placement_from_range(
                N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP,
                start,
                end,
                request->payload_size,
                alignment));
    case N00B_ELF_LAYOUT_GAP_UNKNOWN_NONZERO:
        return rejected(
            layout,
            request,
            alignment,
            N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES);
    case N00B_ELF_LAYOUT_GAP_OVERLAY:
        return rejected(
            layout,
            request,
            alignment,
            N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);
    case N00B_ELF_LAYOUT_GAP_EOF_TAIL:
        if (bin->overlay != nullptr) {
            if (!policy_allows_append_after_overlay(request->policy)) {
                return rejected(
                    layout,
                    request,
                    alignment,
                    N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);
            }

            return accepted(
                layout,
                request,
                alignment,
                placement_from_range(
                    N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY,
                    gap.start,
                    gap.end,
                    request->payload_size,
                    alignment));
        }

        return accepted(
            layout,
            request,
            alignment,
            placement_from_range(
                N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL,
                gap.start,
                gap.end,
                request->payload_size,
                alignment));
    case N00B_ELF_LAYOUT_GAP_VADDR_UNMAPPED:
    default:
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT);
    }
}

static n00b_result_t(n00b_elf_rewrite_admit_result_t)
admit_eof_placement(n00b_elf_binary_t                         *bin,
                    n00b_elf_layout_t                         *layout,
                    n00b_elf_rewrite_admit_metadata_request_t *request,
                    uint64_t                                   alignment)
{
    if (bin->overlay != nullptr
        && !policy_allows_append_after_overlay(request->policy)) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);
    }

    auto tail = n00b_elf_layout_eof_tail_gap(layout,
                                             request->payload_size,
                                             alignment);
    if (n00b_result_is_err(tail)) {
        return n00b_result_err(
            n00b_elf_rewrite_admit_result_t,
            layout_err_to_admit_err(n00b_result_get_err(tail)));
    }

    n00b_option_t(n00b_elf_layout_gap_t) gap_opt = n00b_result_get(tail);
    if (!n00b_option_is_set(gap_opt)) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE);
    }

    n00b_elf_layout_gap_t gap = n00b_option_get(gap_opt);
    n00b_elf_rewrite_admit_placement_kind_t kind =
        bin->overlay == nullptr
            ? N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL
            : N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY;

    return accepted(layout,
                    request,
                    alignment,
                    placement_from_range(kind,
                                         gap.start,
                                         gap.end,
                                         request->payload_size,
                                         alignment));
}

n00b_result_t(n00b_elf_rewrite_admit_result_t)
n00b_elf_rewrite_admit_metadata_insert(
    n00b_elf_binary_t                         *bin,
    n00b_elf_rewrite_admit_metadata_request_t *request) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bin == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_NULL_BINARY);
    }

    if (request == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_NULL_REQUEST);
    }

    if (request->section_name == nullptr) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_NULL_SECTION_NAME);
    }

    if (request->payload_size == 0) {
        return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                               N00B_ELF_REWRITE_ADMIT_ERR_ZERO_PAYLOAD_SIZE);
    }

    uint64_t alignment = effective_alignment(request->file_alignment);

    auto layout_result = n00b_elf_layout_build(bin, .allocator = allocator);
    if (n00b_result_is_err(layout_result)) {
        return n00b_result_err(
            n00b_elf_rewrite_admit_result_t,
            layout_err_to_admit_err(n00b_result_get_err(layout_result)));
    }

    n00b_elf_layout_t *layout = n00b_result_get(layout_result);

    if (section_name_is_reserved(request->section_name)
        || target_has_reserved_section(bin)) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);
    }

    if (!request_is_metadata_only(request)) {
        return rejected(layout,
                        request,
                        alignment,
                        N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA);
    }

    if (policy_has(request->policy,
                   N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION)) {
        auto loader_reason = check_loader_preservation(bin);
        if (n00b_result_is_err(loader_reason)) {
            return n00b_result_err(n00b_elf_rewrite_admit_result_t,
                                   n00b_result_get_err(loader_reason));
        }

        n00b_elf_rewrite_admit_rejection_reason_t reason =
            n00b_result_get(loader_reason);
        if (reason != N00B_ELF_REWRITE_ADMIT_REJECT_NONE) {
            return rejected(layout, request, alignment, reason);
        }
    }

    if (n00b_option_is_set(request->preferred_file_offset)) {
        return admit_preferred_placement(bin,
                                         layout,
                                         request,
                                         alignment,
                                         allocator);
    }

    return admit_eof_placement(bin, layout, request, alignment);
}

n00b_string_t *
n00b_elf_rewrite_admit_err_str(n00b_err_t err)
{
    switch (err) {
    case N00B_ELF_REWRITE_ADMIT_ERR_NULL_BINARY:
        return r"ELF rewrite admission: null binary";
    case N00B_ELF_REWRITE_ADMIT_ERR_NULL_REQUEST:
        return r"ELF rewrite admission: null request";
    case N00B_ELF_REWRITE_ADMIT_ERR_NULL_SECTION_NAME:
        return r"ELF rewrite admission: null section name";
    case N00B_ELF_REWRITE_ADMIT_ERR_ZERO_PAYLOAD_SIZE:
        return r"ELF rewrite admission: zero payload size";
    case N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE:
        return r"ELF rewrite admission: layout substrate failure";
    case N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW:
        return r"ELF rewrite admission: arithmetic overflow";
    default:
        return r"ELF rewrite admission: unknown error code";
    }
}

n00b_string_t *
n00b_elf_rewrite_admit_policy_flag_str(
    n00b_elf_rewrite_admit_policy_flag_t flag)
{
    switch (flag) {
    case N00B_ELF_REWRITE_ADMIT_POLICY_NONE:
        return r"none";
    case N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION:
        return r"strict-loader-preservation";
    case N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY:
        return r"preserve-overlay";
    case N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY:
        return r"append-after-overlay";
    default:
        return r"unknown-elf-rewrite-admit-policy-flag";
    }
}

n00b_string_t *
n00b_elf_rewrite_admit_outcome_str(
    n00b_elf_rewrite_admit_outcome_t outcome)
{
    switch (outcome) {
    case N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED:
        return r"accepted";
    case N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED:
        return r"rejected";
    default:
        return r"unknown-elf-rewrite-admit-outcome";
    }
}

n00b_string_t *
n00b_elf_rewrite_admit_placement_kind_str(
    n00b_elf_rewrite_admit_placement_kind_t kind)
{
    switch (kind) {
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE:
        return r"none";
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL:
        return r"eof-tail";
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP:
        return r"file-gap";
    case N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY:
        return r"after-overlay";
    default:
        return r"unknown-elf-rewrite-admit-placement-kind";
    }
}

n00b_string_t *
n00b_elf_rewrite_admit_rejection_reason_str(
    n00b_elf_rewrite_admit_rejection_reason_t reason)
{
    switch (reason) {
    case N00B_ELF_REWRITE_ADMIT_REJECT_NONE:
        return r"none";
    case N00B_ELF_REWRITE_ADMIT_REJECT_NOT_YET_CHECKED:
        return r"not-yet-checked";
    case N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME:
        return r"reserved-section-name";
    case N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA:
        return r"section-not-metadata";
    case N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT:
        return r"no-safe-placement";
    case N00B_ELF_REWRITE_ADMIT_REJECT_FILE_COLLISION:
        return r"file-collision";
    case N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES:
        return r"unknown-nonzero-bytes";
    case N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY:
        return r"overlay-policy";
    case N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD:
        return r"phtab-outside-load";
    case N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_MISSING:
        return r"pt-phdr-missing";
    case N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT:
        return r"pt-phdr-inconsistent";
    case N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD:
        return r"entry-outside-load";
    case N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY:
        return r"entry-memory-only";
    case N00B_ELF_REWRITE_ADMIT_REJECT_LOADER_PRESERVATION:
        return r"loader-preservation";
    default:
        return r"unknown-elf-rewrite-admit-rejection-reason";
    }
}
