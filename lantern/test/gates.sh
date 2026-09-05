#!/bin/sh
set -u

root="$1"
diag_bin="${2:-}"
text_bin="${3:-}"
typing_bin="${4:-}"
verify_bin="${5:-}"
layout_bin="${6:-}"
promote_bin="${7:-}"
structure_bin="${8:-}"
pdg_bin="${9:-}"
inc="$root/include/ir"
src="$root/src"
fails=0

fail() {
    echo "FAIL  $1"
    echo "$2"
    fails=$((fails + 1))
}

hits() {
    grep -nE "$1" $2 2>/dev/null | grep -v '^[^:]*:[0-9]*: *$'
}

g="$(hits 'typedef (struct|enum).*ir_(typekind|scalarkind)_t|struct ir_type_t' "$inc/ir.h $src/*.c $src/*.h")"
[ -z "$g" ] || fail "TYPE-01 a parallel type model exists" "$g"

g="$(grep -nE '\bth_type\b' "$inc/ir.h" | grep -vE 'th_type[[:space:]]*\*|typehash\.h')"
[ -z "$g" ] || fail "TYPE-01 types are held by value, not as th_type *" "$g"

g="$(hits '\bth_unify\b|\bth_instantiate\b|\bth_generalize\b|\bth_apply_subst\b|\bth_occurs\b' "$inc/ir.h $src/*.c $src/*.h")"
[ -z "$g" ] || fail "TYPE-07 the IR reaches a unifier" "$g"

g="$(grep -nE '\bth_print\b' $src/*.c $src/*.h "$inc/ir.h" 2>/dev/null \
     | grep -v '^[^:]*ir_print\.c:')"
[ -z "$g" ] || fail "TYPE-04 th_print is used outside the canonical printer" "$g"

g="$(hits 'str(cmp|ncmp|str)\([^)]*th_' "$src/*.c $src/*.h")"
[ -z "$g" ] || fail "TYPE-04 a type decision is made by string comparison" "$g"

g="$(grep -nE '(params|args|block_params)' "$inc/ir.h" | grep -iE 'block' )"
[ -z "$g" ] || fail "MOD-04/SSA-13 blocks appear to carry parameters" "$g"

g="$(awk '/^struct ir_(block_t|instr_t|local_t|global_t|func_t)/,/^};/' "$inc/ir.h" | grep -c 'ir_span_t')"
[ "$g" = "5" ] || fail "LOC-07 not every node kind carries a span (found $g of 5)" ""

g="$(grep -nE 'ir_span_t' "$inc/ir.h" | grep -E 'th_type|ir_type')"
[ -z "$g" ] || fail "LOC-07 a type carries a span" "$g"

g="$(hits 'memcmp\([^)]*ir_span_t|span *== *|== *[a-z_]*\.span|\.span *==' "$inc/ir.h $src/*.c")"
[ -z "$g" ] || fail "LOC-08 spans are compared for equality" "$g"

g="$(hits 'IR_OP_(GOTO|BREAK|CONTINUE|MIN|MAX|TYPEOF|ARRAY_LEN)\b' "$inc/ir.h $src/*.c $inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-30/OP-46 a deleted opcode is present" "$g"

g="$(grep -nE '"(goto|break|continue|min|max|typeof|kwargs_call|generic_call)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-30/OP-46 a deleted mnemonic is present" "$g"

g="$(grep -nE '"(lt_s|lt_u|shr_s|shr_u|add_checked|addu)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-21 signedness or overflow is spelled into a mnemonic" "$g"

g="$(grep -nE '"(and_then|or_else|andalso|orelse|logic_and|logic_or|land|lor|ternary|cond|short_circuit|select_lazy)"' \
     "$inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-36 a short-circuit opcode exists" "$g"

g="$(awk -F'"' '/^IR_OP\(/ && $4 ~ /[bd]/ && $5 !~ /TERMINATOR/ {print FILENAME": "$2}' \
     "$inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-36 a non-terminator takes a block operand, so an operand can go unevaluated" "$g"

while IFS='|' read -r row pat; do
    [ -n "$pat" ] || continue
    g="$(grep -nEi "$pat" "$inc/ir_ops.def" "$inc/ir.h" $src/*.c 2>/dev/null)"
    [ -z "$g" ] || fail "OP-47 a §7.16 opcode is back: $row" "$g"
done <<EOF
option, result, typecase|IR_(OPTION|RESULT|TYPECASE)_?[A-Z]*|"(option|result|typecase)[._a-z]*"
kwargs call|IR_KWARGS_CALL|"kwargs_call"
defer, with, once|IR_(DEFER|WITH|ONCE)\b|"(defer|with|once)"
gc register, deregister, scan|IR_GC_(REGISTER|DEREGISTER|SCAN)|"gc\.(register|deregister|scan)"
rpc call, blueprint instantiate|IR_(RPC_CALL|BLUEPRINT_INSTANTIATE)|"(rpc_call|blueprint_instantiate)"
generic call, dynamic|IR_GENERIC_CALL|IR_DYNAMIC_|"(generic_call|dynamic_[a-z]+)"
logic and, logic or, ternary|\b(LOGIC_AND|LOGIC_OR|IR_TERNARY)\b|"(logic_and|logic_or|ternary)"
cmp min, cmp max|\bCMP_(MIN|MAX)_?[A-Z]*|"(min|max)"
array len|IR_ARRAY_LEN|"array_len"
typeof|IR_TYPEOF\b|"typeof"
EOF

g="$(grep -lE '^IR_OP\(' "$inc"/*.def "$inc"/*.h $src/*.c 2>/dev/null | grep -v 'ir_ops\.def')"
[ -z "$g" ] || fail "SSA-02 a second opcode table exists beside ir_ops.def" "$g"

g="$(grep -nE 'IR_PROP_SSA' $src/ir_ops.c 2>/dev/null)"
[ -z "$g" ] || fail "SSA-02 the opcode table knows about the SSA property" "$g"

g="$(grep -nE '(if|while|\?|&&|\|\||[&|]) *[^;]*IR_PROP_SSA|IR_PROP_SSA *[&|=!]' \
     $src/ir_parse.c $src/ir_print.c 2>/dev/null)"
[ -z "$g" ] || fail "SSA-02 the parser or printer branches on the SSA property" "$g"

g="$(grep -nE 'ir_op(info)?_[a-z_]*\(.*(ir_props_t|bool ssa|is_ssa)' "$inc/ir.h")"
[ -z "$g" ] || fail "SSA-02 an opcode query takes the form as an argument" "$g"

g="$(grep -nE '"(mov|move|assign|set|store_local|read_local)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "OP-08 a second value-moving opcode exists beside copy" "$g"

g="$(grep -nE '^IR_OP\((FIELD|EXTRACT|INSERT|OFFSETOF),' "$inc/ir_ops.def" \
     | grep -vE '"(vn|vnv|tn)"')"
[ -z "$g" ] || fail "OP-17 a member is named by something other than a name" "$g"

g="$(grep -nE '\b(volatile|atomic)\b' "$inc/ir.h" $src/ir_profile.c 2>/dev/null)"
[ -z "$g" ] || fail "OP-10 an access modifier leaked into the type model" "$g"

g="$(grep -nE '"(wrap|trap|sat|mask|ordered|unordered|volatile|atomic|to_zero|nearest|floor|ceil)"' \
     $src/ir_parse.c $src/ir_print.c 2>/dev/null)"
[ -z "$g" ] || fail "TXT-08 a modifier is spelled in the parser or printer instead of the table" "$g"

g="$(grep -nE 'strcmp\(opname|strcmp\([a-z_]*, *"(add|sub|jump|branch|switch|call|phi)"' \
     $src/ir_parse.c 2>/dev/null)"
[ -z "$g" ] || fail "TXT-08 a mnemonic is matched by hand instead of through the table" "$g"

g="$(grep -nE '"IR-[EWF][0-9]{4}"' $src/*.c $inc/diag.h 2>/dev/null)"
[ -z "$g" ] || fail "DIAG-08 a diagnostic code is spelled outside the registry" "$g"

g="$(grep -nE 'ir_diag_(begin|begin_at|note)\(' "$inc/diag.h" | grep -E 'char \*code|const char \*c[^h]')"
[ -z "$g" ] || fail "DIAG-08 an emit entry point takes a code as a string" "$g"

g="$(grep -nE '\bir_pass_run *\(' $src/*.c 2>/dev/null | grep -v 'ir_pass\.c:')"
[ -z "$g" ] || fail "LOC-17 a pass is run without the location-monotonicity check" "$g"

g="$(grep -nE '\bIR_REGION_|ir_region_' $src/ir_parse.c 2>/dev/null)"
[ -z "$g" ] || fail "REG-07 the parser knows about regions, so there are two syntaxes" "$g"

g="$(grep -cE '^ir_module_t \*ir_module_parse' $src/ir_parse.c)"
[ "$g" = "1" ] || fail "REG-07 there is not exactly one way to parse a module (found $g)" ""

g="$(hits 'ir_cfg_|build_cfg|ir_pass_cfg|n_cfg_edges' "$src/*.c $inc/*.h")"
[ -z "$g" ] || fail "ANA-01 a CFG is being constructed rather than read off the blocks" "$g"

g="$(grep -cE 'ir_block_t \*ir_succ_of' "$src/ir_verify.c")"
[ "$g" = "1" ] || fail "ANA-01 successors come from somewhere other than the terminator (found $g)" ""

g="$(hits 'strcmp|strstr|strncmp' "$src/ir_pdg.c $src/ir_rd.c")"
[ -z "$g" ] || fail "ANA-03 dependence is being keyed by a name string" "$g"

plan="$root/local/docs/ir-plan.md"

if [ -f "$plan" ]; then
    g="$(grep -oE '^\| [A-Z]+-[0-9]+ \| §' "$plan" | cut -d'|' -f2 | tr -d ' ' \
         | sort | uniq -d)"
    [ -z "$g" ] || fail "PLAN one conformance id names two requirements" "$g"
fi

work="${TMPDIR:-/tmp}/ir-gates-$$"
mkdir -p "$work/golden"
produced="$work/produced-codes.txt"
: > "$produced"

if [ -n "$diag_bin" ] && [ -x "$diag_bin" ]; then
    cp "$root"/test/golden/* "$work/golden/" 2>/dev/null
    "$diag_bin" "$work/golden" >/dev/null 2>&1 \
        || fail "DIAG-10 the diagnostic test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-diag.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no diagnostic binary was given, so coverage cannot be checked" ""
fi

if [ -n "$typing_bin" ] && [ -x "$typing_bin" ]; then
    "$typing_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the typing test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-typing.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no typing binary was given, so coverage cannot be checked" ""
fi

if [ -n "$verify_bin" ] && [ -x "$verify_bin" ]; then
    "$verify_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the verifier test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-verify.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no verifier binary was given, so coverage cannot be checked" ""
fi

if [ -n "$layout_bin" ] && [ -x "$layout_bin" ]; then
    "$layout_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the layout test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-layout.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no layout binary was given, so coverage cannot be checked" ""
fi

if [ -n "$promote_bin" ] && [ -x "$promote_bin" ]; then
    "$promote_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the promote test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-promote.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no promote binary was given, so coverage cannot be checked" ""
fi

if [ -n "$structure_bin" ] && [ -x "$structure_bin" ]; then
    "$structure_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the structure test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-structure.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no structure binary was given, so coverage cannot be checked" ""
fi

if [ -n "$pdg_bin" ] && [ -x "$pdg_bin" ]; then
    "$pdg_bin" "$work" >/dev/null 2>&1 \
        || fail "DIAG-10 the pdg test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-pdg.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no pdg binary was given, so coverage cannot be checked" ""
fi

if [ -n "$text_bin" ] && [ -x "$text_bin" ]; then
    "$text_bin" "$work/golden" >/dev/null 2>&1 \
        || fail "DIAG-10 the text test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-text.txt" >> "$produced" 2>/dev/null
else
    fail "DIAG-10 no text binary was given, so coverage cannot be checked" ""
fi

pending=""

for id in $(grep -oE '^IR_DIAG\([A-Z_]+' "$inc/ir_diags.def" | cut -d'(' -f2); do
    code="$(grep -E "^IR_DIAG\($id," "$inc/ir_diags.def" \
            | sed -E 's/.*"(IR-[EWF][0-9]+)".*/\1/')"

    if grep -qE "IR_DIAG_$id\b" $src/*.c 2>/dev/null; then
        if [ ! -f "$produced" ] || ! grep -qx "$code" "$produced"; then
            fail "DIAG-10 $code is emitted by the library but no test produces it" ""
        fi
    else
        pending="$pending $code"
    fi
done

if [ -n "$pending" ]; then
    echo "DIAG-10 pending, declared but not yet emitted by any library code:$pending"
fi

rm -rf "$work"

if [ "$fails" -eq 0 ]; then
    echo "gates: all passed"
fi

exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
