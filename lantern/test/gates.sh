#!/bin/sh
set -u

root="$1"
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

g="$(hits '\bth_print\b' "$src/*.c $src/*.h $inc/ir.h")"
[ -z "$g" ] || fail "TYPE-04 the IR decodes types from their printed form" "$g"

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

if [ "$fails" -eq 0 ]; then
    echo "gates: all passed"
fi

exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
