fixture sideband
: > "$work/sideband/tests/sideband-allowlist"
printf 'static core_string_t k = CORE_STRING_STATIC("n00bb.planted");\n' > "$work/sideband/compiler/a.c"
expect fail "side-band: new annotation key" check sideband sideband
printf 'key\tn00bb.planted\tcarries the planted fact\n' > "$work/sideband/tests/sideband-allowlist"
expect pass "side-band: key with a reason" check sideband sideband
printf 'key\tn00bb.planted\t\n' > "$work/sideband/tests/sideband-allowlist"
expect fail "side-band: key without a reason" check sideband sideband
fixture sideband
: > "$work/sideband/tests/sideband-allowlist"
printf '#define PLANTED_KEY 1\n' > "$work/sideband/compiler/a.h"
expect fail "side-band: new _KEY macro" check sideband sideband
fixture sideband
: > "$work/sideband/tests/sideband-allowlist"
printf 'typedef struct {\n    uint32_t planted;\n} n00b_abi_planted_v1;\n' > "$work/sideband/abi/n00b-abi-v1.h"
expect fail "side-band: new ABI row field" check sideband sideband
fixture sideband
: > "$work/sideband/tests/sideband-allowlist"
printf 'static const n00bb_axiom_t axioms[] = {\n    NATIVE("__planted", "n00b_planted", 0, 0, 0),\n};\n' \
    > "$work/sideband/compiler/compiler_axioms.c"
expect fail "side-band: new native" check sideband sideband
