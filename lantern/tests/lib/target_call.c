#include "lib/target_call.h"
#include "harness.h"

static bool
classifies(const core_target_desc_t *target,
           const core_target_aggregate_t *aggregate, bool is_return,
           core_target_transport_t transport,
           core_target_lane_t first, core_target_lane_t second)
{
    core_target_aggregate_class_t result = {0};
    if (!core_target_classify_aggregate(target, aggregate, false, is_return,
                                        &result)
        || result.transport != transport) return false;
    if (transport != CORE_TARGET_TRANSPORT_DIRECT)
        return result.lane_count == 0;
    return result.lane_count == (second == CORE_TARGET_LANE_NONE ? 1 : 2)
        && result.lanes[0] == first
        && (second == CORE_TARGET_LANE_NONE || result.lanes[1] == second);
}

int main(void)
{
    core_target_desc_t linux = {0}, macos = {0}, windows = {0};
    check("three target call profiles",
          core_target_profile(CORE_STRING_STATIC("x86_64-linux"), 0, &linux)
          && core_target_profile(CORE_STRING_STATIC("arm64-macos"), 0,
                                 &macos)
          && core_target_profile(CORE_STRING_STATIC("x86_64-windows"), 0,
                                 &windows));

    const core_target_call_atom_t pair_atoms[] = {
        {0, 64, 64, CORE_TARGET_ATOM_INTEGER},
        {64, 64, 64, CORE_TARGET_ATOM_INTEGER},
    };
    core_target_aggregate_t pair = {16, 8, pair_atoms, 2};
    check("two-integer record uses two SysV integer lanes",
          classifies(&linux, &pair, true, CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_INTEGER));
    check("two-integer record uses two Darwin integer lanes",
          classifies(&macos, &pair, true, CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_INTEGER));
    check("two-integer record returns indirectly on Windows",
          classifies(&windows, &pair, true, CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE)
          && classifies(&windows, &pair, false,
                        CORE_TARGET_TRANSPORT_INDIRECT,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t mixed_atoms[] = {
        {0, 64, 64, CORE_TARGET_ATOM_FLOAT},
        {64, 64, 64, CORE_TARGET_ATOM_INTEGER},
    };
    core_target_aggregate_t mixed = {16, 8, mixed_atoms, 2};
    check("mixed record uses float then integer on SysV",
          classifies(&linux, &mixed, true, CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_FLOAT, CORE_TARGET_LANE_INTEGER));
    check("mixed record uses integer lanes on Darwin",
          classifies(&macos, &mixed, true, CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_INTEGER));
    check("mixed record is indirect on Windows",
          classifies(&windows, &mixed, true, CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t outcome_atoms[] = {
        {0, 64, 64, CORE_TARGET_ATOM_INTEGER},
        {0, 32, 32, CORE_TARGET_ATOM_INTEGER},
        {64, 1, 1, CORE_TARGET_ATOM_INTEGER},
    };
    core_target_aggregate_t outcome = {16, 8, outcome_atoms, 3};
    check("tagged outcome keeps its second integer lane",
          classifies(&linux, &outcome, true,
                     CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_INTEGER)
          && classifies(&macos, &outcome, true,
                        CORE_TARGET_TRANSPORT_DIRECT,
                        CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_INTEGER)
          && classifies(&windows, &outcome, true,
                        CORE_TARGET_TRANSPORT_INDIRECT,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t hfa_atoms[] = {
        {64, 64, 64, CORE_TARGET_ATOM_FLOAT},
        {0, 64, 64, CORE_TARGET_ATOM_FLOAT},
    };
    core_target_aggregate_t hfa = {16, 8, hfa_atoms, 2};
    check("Darwin homogeneous float record ignores input leaf order",
          classifies(&macos, &hfa, false,
                     CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_FLOAT, CORE_TARGET_LANE_FLOAT));
    check("SysV homogeneous float record has two SSE lanes",
          classifies(&linux, &hfa, false,
                     CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_FLOAT, CORE_TARGET_LANE_FLOAT));
    check("Windows homogeneous float record is still indirect",
          classifies(&windows, &hfa, false,
                     CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t four_floats[] = {
        {0, 64, 64, CORE_TARGET_ATOM_FLOAT},
        {64, 64, 64, CORE_TARGET_ATOM_FLOAT},
        {128, 64, 64, CORE_TARGET_ATOM_FLOAT},
        {192, 64, 64, CORE_TARGET_ATOM_FLOAT},
    };
    core_target_aggregate_t hfa4 = {32, 8, four_floats, 4};
    core_target_aggregate_class_t hfa4_result = {0};
    check("four-double HFA stays in SIMD registers on Darwin",
          core_target_classify_aggregate(&macos, &hfa4, false, false,
                                         &hfa4_result)
          && hfa4_result.transport == CORE_TARGET_TRANSPORT_DIRECT
          && hfa4_result.lane_count == 4
          && hfa4_result.lanes[0] == CORE_TARGET_LANE_FLOAT
          && hfa4_result.lanes[1] == CORE_TARGET_LANE_FLOAT
          && hfa4_result.lanes[2] == CORE_TARGET_LANE_FLOAT
          && hfa4_result.lanes[3] == CORE_TARGET_LANE_FLOAT
          && classifies(&linux, &hfa4, false,
                        CORE_TARGET_TRANSPORT_STACK,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t large_atoms[] = {
        {0, 64, 64, CORE_TARGET_ATOM_INTEGER},
        {64, 64, 64, CORE_TARGET_ATOM_INTEGER},
        {128, 64, 64, CORE_TARGET_ATOM_INTEGER},
    };
    core_target_aggregate_t large = {24, 8, large_atoms, 3};
    check("large SysV parameter is stack-passed",
          classifies(&linux, &large, false, CORE_TARGET_TRANSPORT_STACK,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));
    check("large SysV result uses hidden result storage",
          classifies(&linux, &large, true, CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));
    check("large Darwin parameter and result are indirect",
          classifies(&macos, &large, false,
                     CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE)
          && classifies(&macos, &large, true,
                        CORE_TARGET_TRANSPORT_INDIRECT,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t unaligned_atom =
        {8, 64, 64, CORE_TARGET_ATOM_FLOAT};
    core_target_aggregate_t unaligned = {9, 1, &unaligned_atom, 1};
    check("unaligned SysV leaf is memory-classified",
          classifies(&linux, &unaligned, false,
                     CORE_TARGET_TRANSPORT_STACK,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));
    const core_target_call_atom_t bitfield_atom =
        {7, 9, 1, CORE_TARGET_ATOM_INTEGER};
    core_target_aggregate_t bitfield = {2, 1, &bitfield_atom, 1};
    check("integer bitfields may be unaligned within a register lane",
          classifies(&linux, &bitfield, false,
                     CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_NONE));

    const core_target_call_atom_t small_atom =
        {0, 24, 1, CORE_TARGET_ATOM_INTEGER};
    core_target_aggregate_t small = {3, 1, &small_atom, 1};
    check("three-byte Windows aggregate is indirect",
          classifies(&windows, &small, false,
                     CORE_TARGET_TRANSPORT_INDIRECT,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));
    small.size_bytes = 4;
    check("four-byte Windows aggregate uses an integer register",
          classifies(&windows, &small, false,
                     CORE_TARGET_TRANSPORT_DIRECT,
                     CORE_TARGET_LANE_INTEGER, CORE_TARGET_LANE_NONE));

    core_target_aggregate_t empty = {0, 1, nullptr, 0};
    check("zero-sized aggregate consumes no transport",
          classifies(&linux, &empty, false,
                     CORE_TARGET_TRANSPORT_IGNORE,
                     CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE)
          && classifies(&macos, &empty, false,
                        CORE_TARGET_TRANSPORT_IGNORE,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE)
          && classifies(&windows, &empty, false,
                        CORE_TARGET_TRANSPORT_IGNORE,
                        CORE_TARGET_LANE_NONE, CORE_TARGET_LANE_NONE));

    core_target_aggregate_class_t untouched = {
        .transport = CORE_TARGET_TRANSPORT_STACK,
        .lane_count = 3,
    };
    small.size_bytes = 2;
    check("out-of-range leaf is rejected without modifying output",
          !core_target_classify_aggregate(&linux, &small, false, false,
                                          &untouched)
          && untouched.transport == CORE_TARGET_TRANSPORT_STACK
          && untouched.lane_count == 3);
    core_target_desc_t other = linux;
    other.call_abi = CORE_TARGET_CALL_OTHER;
    check("unknown call ABI requests a custom classifier",
          !core_target_classify_aggregate(&other, &pair, false, false,
                                          &untouched));
    other.call_abi = (core_target_call_abi_t)99;
    check("invalid call ABI is rejected",
          !core_target_classify_aggregate(&other, &pair, false, false,
                                          &untouched)
          && !core_target_classify_aggregate(&other, &empty, false, false,
                                             &untouched));
    other.call_abi = (core_target_call_abi_t)-1;
    check("negative call ABI is rejected, including forced transport",
          !core_target_classify_aggregate(&other, &pair, true, false,
                                          &untouched));

    core_target_call_atom_t bad_kind = pair_atoms[0];
    bad_kind.kind = (core_target_atom_kind_t)-1;
    core_target_aggregate_t bad_leaf = {8, 8, &bad_kind, 1};
    check("negative atom kind is rejected",
          !core_target_classify_aggregate(&linux, &bad_leaf, false, false,
                                          &untouched));

    pair.alignment_bytes = 16;
    core_target_aggregate_class_t forced = {0};
    check("caller may require indirect transport for an over-aligned value",
          core_target_classify_aggregate(&linux, &pair, true, false, &forced)
          && forced.transport == CORE_TARGET_TRANSPORT_INDIRECT
          && core_target_classify_aggregate(&macos, &pair, true, true, &forced)
          && forced.transport == CORE_TARGET_TRANSPORT_INDIRECT);
    return report("lib-target-call");
}
