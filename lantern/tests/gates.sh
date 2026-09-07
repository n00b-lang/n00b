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
tree_bin="${10:-}"
inc="$root/include/ir"
src="$root/src"
priv="$root/include/private"
fails=0

fail() {
    echo "FAIL  $1"
    echo "$2"
    fails=$((fails + 1))
}

hits() {
    grep -nE "$1" $2 2>/dev/null | grep -v '^[^:]*:[0-9]*: *$'
}

g="$(hits 'typedef (struct|enum).*ir_(typekind|scalarkind)_t|struct ir_type_t' "$inc/ir.h $src/ir/*.c $priv/ir/*.h")"
[ -z "$g" ] || fail "a parallel type model exists" "$g"

g="$(grep -nE '\bth_type\b' "$inc/ir.h" | grep -vE 'th_type[[:space:]]*\*|typehash\.h')"
[ -z "$g" ] || fail "types are held by value, not as th_type *" "$g"

g="$(hits '\bth_unify\b|\bth_instantiate\b|\bth_generalize\b|\bth_apply_subst\b|\bth_occurs\b' "$inc/ir.h $src/ir/*.c $priv/ir/*.h")"
[ -z "$g" ] || fail "the IR reaches a unifier" "$g"

g="$(grep -nE '\bth_print\b' $src/ir/*.c $priv/ir/*.h "$inc/ir.h" 2>/dev/null \
     | grep -v '^[^:]*src/ir/print\.c:')"
[ -z "$g" ] || fail "th_print is used outside the canonical printer" "$g"

g="$(hits 'str(cmp|ncmp|str)\([^)]*th_' "$src/ir/*.c $priv/ir/*.h")"
[ -z "$g" ] || fail "a type decision is made by string comparison" "$g"

g="$(grep -nE '(params|args|block_params)' "$inc/ir.h" | grep -iE 'block' )"
[ -z "$g" ] || fail "blocks appear to carry parameters" "$g"

g="$(awk '/^struct ir_(block_t|instr_t|local_t|global_t|func_t)/,/^};/' "$inc/ir.h" | grep -c 'ir_span_t')"
[ "$g" = "5" ] || fail "not every node kind carries a span (found $g of 5)" ""

g="$(grep -nE 'ir_span_t' "$inc/ir.h" | grep -E 'th_type|ir_type')"
[ -z "$g" ] || fail "a type carries a span" "$g"

g="$(hits 'memcmp\([^)]*ir_span_t|span *== *|== *[a-z_]*\.span|\.span *==' "$inc/ir.h $src/ir/*.c")"
[ -z "$g" ] || fail "spans are compared for equality" "$g"

g="$(hits 'IR_OP_(GOTO|BREAK|CONTINUE|MIN|MAX|TYPEOF|ARRAY_LEN)\b' "$inc/ir.h $src/ir/*.c $inc/ir_ops.def")"
[ -z "$g" ] || fail "a deleted opcode is present" "$g"

g="$(grep -nE '"(goto|break|continue|min|max|typeof|kwargs_call|generic_call)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "a deleted mnemonic is present" "$g"

g="$(grep -nE '"(lt_s|lt_u|shr_s|shr_u|add_checked|addu)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "signedness or overflow is spelled into a mnemonic" "$g"

g="$(grep -nE '"(and_then|or_else|andalso|orelse|logic_and|logic_or|land|lor|ternary|cond|short_circuit|select_lazy)"' \
     "$inc/ir_ops.def")"
[ -z "$g" ] || fail "a short-circuit opcode exists" "$g"

g="$(awk -F'"' '/^IR_OP\(/ && $4 ~ /[bd]/ && $5 !~ /TERMINATOR/ {print FILENAME": "$2}' \
     "$inc/ir_ops.def")"
[ -z "$g" ] || fail "a non-terminator takes a block operand, so an operand can go unevaluated" "$g"

while IFS='@' read -r row con mne; do
    [ -n "$con" ] || continue
    g="$(grep -nEi "$con" "$inc/ir_ops.def" "$inc/ir.h" $src/ir/*.c 2>/dev/null)"
    [ -z "$mne" ] || g="$g
$(grep -nEi "$mne" "$inc/ir_ops.def" 2>/dev/null)"
    g="$(printf '%s' "$g" | grep -v '^$')"
    [ -z "$g" ] || fail "a removed opcode is back: $row" "$g"
done <<EOF
option, result, typecase@IR_(OPTION|RESULT|TYPECASE)_?[A-Z]*@"(option|result|typecase)[._a-z]*"
kwargs call@IR_KWARGS_CALL@"kwargs_call"
defer, with, once@IR_(DEFER|WITH|ONCE)\b@"(defer|with|once)"
gc register, deregister, scan@IR_GC_(REGISTER|DEREGISTER|SCAN)@"gc\.(register|deregister|scan)"
rpc call, blueprint instantiate@IR_(RPC_CALL|BLUEPRINT_INSTANTIATE)@"(rpc_call|blueprint_instantiate)"
generic call, dynamic@IR_GENERIC_CALL|IR_DYNAMIC_@"(generic_call|dynamic_[a-z]+)"
logic and, logic or, ternary@\b(LOGIC_AND|LOGIC_OR|IR_TERNARY)\b@"(logic_and|logic_or|ternary)"
cmp min, cmp max@\bCMP_(MIN|MAX)_?[A-Z]*@"(min|max)"
array len@IR_ARRAY_LEN@"array_len"
typeof@IR_TYPEOF\b@"typeof"
EOF

g="$(grep -lE '^IR_OP\(' "$inc"/*.def "$inc"/*.h $src/ir/*.c 2>/dev/null | grep -v 'ir_ops\.def')"
[ -z "$g" ] || fail "a second opcode table exists beside ir_ops.def" "$g"

g="$(grep -nE 'IR_PROP_SSA' $src/ir/ops.c 2>/dev/null)"
[ -z "$g" ] || fail "the opcode table knows about the SSA property" "$g"

g="$(grep -nE '(if|while|\?|&&|\|\||[&|]) *[^;]*IR_PROP_SSA|IR_PROP_SSA *[&|=!]' \
     $src/ir/build.c $src/ir/print.c 2>/dev/null)"
[ -z "$g" ] || fail "the parser or printer branches on the SSA property" "$g"

g="$(grep -nE 'ir_op(info)?_[a-z_]*\(.*(ir_props_t|bool ssa|is_ssa)' "$inc/ir.h")"
[ -z "$g" ] || fail "an opcode query takes the form as an argument" "$g"

g="$(grep -nE '"(mov|move|assign|set|store_local|read_local)"' "$inc/ir_ops.def")"
[ -z "$g" ] || fail "a second value-moving opcode exists beside copy" "$g"

g="$(grep -nE '^IR_OP\((FIELD|EXTRACT|INSERT|OFFSETOF),' "$inc/ir_ops.def" \
     | grep -vE '"(vn|vnv|tn)"')"
[ -z "$g" ] || fail "a member is named by something other than a name" "$g"

g="$(grep -nE '\b(volatile|atomic)\b' "$inc/ir.h" $src/ir/profile.c 2>/dev/null)"
[ -z "$g" ] || fail "an access modifier leaked into the type model" "$g"

g="$(grep -nE '"(wrap|trap|sat|mask|ordered|unordered|volatile|atomic|to_zero|nearest|floor|ceil)"' \
     $src/ir/build.c $src/ir/print.c 2>/dev/null)"
[ -z "$g" ] || fail "a modifier is spelled in the parser or printer instead of the table" "$g"

g="$(grep -nE 'strcmp\(opname|strcmp\([a-z_]*, *"(add|sub|jump|branch|switch|call|phi)"' \
     $src/ir/build.c 2>/dev/null)"
[ -z "$g" ] || fail "a mnemonic is matched by hand instead of through the table" "$g"

g="$(grep -nE '"IR-[EWF][0-9]{4}"' $src/ir/*.c $inc/diag.h 2>/dev/null)"
[ -z "$g" ] || fail "a diagnostic code is spelled outside the registry" "$g"

g="$(grep -nE 'ir_diag_(begin|begin_at|note)\(' "$inc/diag.h" | grep -E 'char \*code|const char \*c[^h]')"
[ -z "$g" ] || fail "an emit entry point takes a code as a string" "$g"

g="$(grep -nE '\bIR_REGION_|ir_region_' $src/ir/build.c 2>/dev/null)"
[ -z "$g" ] || fail "the parser knows about regions, so there are two syntaxes" "$g"

g="$(grep -cE '^ir_module_t \*ir_module_parse' $src/ir/build.c)"
[ "$g" = "1" ] || fail "there is not exactly one way to parse a module (found $g)" ""

g="$(hits 'ir_cfg_|build_cfg|ir_pass_cfg|n_cfg_edges' "$src/ir/*.c $inc/*.h")"
[ -z "$g" ] || fail "a CFG is being constructed rather than read off the blocks" "$g"

g="$(grep -cE 'ir_block_t \*ir_succ_of' "$src/ir/doms.c")"
[ "$g" = "1" ] || fail "successors come from somewhere other than the terminator (found $g)" ""

g="$(hits 'strcmp|strstr|strncmp' "$src/ir/pdg.c $src/ir/rd.c")"
[ -z "$g" ] || fail "dependence is being keyed by a name string" "$g"

work="${TMPDIR:-/tmp}/ir-gates-$$"
mkdir -p "$work/golden"

IR_CODE_DUMP="$work"
export IR_CODE_DUMP
produced="$work/produced-codes.txt"
: > "$produced"

if [ -n "$diag_bin" ] && [ -x "$diag_bin" ]; then
    cp "$root"/tests/ir/golden/* "$work/golden/" 2>/dev/null
    "$diag_bin" "$work/golden" >/dev/null 2>&1 \
        || fail "the diagnostic test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-diag.txt" >> "$produced" 2>/dev/null
else
    fail "no diagnostic binary was given, so coverage cannot be checked" ""
fi

if [ -n "$typing_bin" ] && [ -x "$typing_bin" ]; then
    "$typing_bin" "$work" >/dev/null 2>&1 \
        || fail "the typing test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-typing.txt" >> "$produced" 2>/dev/null
else
    fail "no typing binary was given, so coverage cannot be checked" ""
fi

if [ -n "$verify_bin" ] && [ -x "$verify_bin" ]; then
    "$verify_bin" "$work" >/dev/null 2>&1 \
        || fail "the verifier test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-verify.txt" >> "$produced" 2>/dev/null
else
    fail "no verifier binary was given, so coverage cannot be checked" ""
fi

if [ -n "$layout_bin" ] && [ -x "$layout_bin" ]; then
    "$layout_bin" "$work" >/dev/null 2>&1 \
        || fail "the layout test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-layout.txt" >> "$produced" 2>/dev/null
else
    fail "no layout binary was given, so coverage cannot be checked" ""
fi

if [ -n "$promote_bin" ] && [ -x "$promote_bin" ]; then
    "$promote_bin" "$work" >/dev/null 2>&1 \
        || fail "the promote test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-promote.txt" >> "$produced" 2>/dev/null
else
    fail "no promote binary was given, so coverage cannot be checked" ""
fi

if [ -n "$structure_bin" ] && [ -x "$structure_bin" ]; then
    "$structure_bin" "$work" >/dev/null 2>&1 \
        || fail "the structure test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-structure.txt" >> "$produced" 2>/dev/null
else
    fail "no structure binary was given, so coverage cannot be checked" ""
fi

if [ -n "$pdg_bin" ] && [ -x "$pdg_bin" ]; then
    "$pdg_bin" "$work" >/dev/null 2>&1 \
        || fail "the pdg test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-pdg.txt" >> "$produced" 2>/dev/null
else
    fail "no pdg binary was given, so coverage cannot be checked" ""
fi

if [ -n "$tree_bin" ] && [ -x "$tree_bin" ]; then
    "$tree_bin" "$work" >/dev/null 2>&1 \
        || fail "the tree-pass test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-tree.txt" >> "$produced" 2>/dev/null
else
    fail "no tree-pass binary was given, so coverage cannot be checked" ""
fi

if [ -n "$text_bin" ] && [ -x "$text_bin" ]; then
    "$text_bin" "$work/golden" >/dev/null 2>&1 \
        || fail "the text test did not pass, so its code coverage is unknown" ""
    cat "$work/produced-codes-text.txt" >> "$produced" 2>/dev/null
else
    fail "no text binary was given, so coverage cannot be checked" ""
fi

for id in $(grep -oE '^IR_DIAG\([A-Z_]+' "$inc/ir_diags.def" | cut -d'(' -f2); do
    code="$(grep -E "^IR_DIAG\($id," "$inc/ir_diags.def" \
            | sed -E 's/.*"(IR-[EWF][0-9]+)".*/\1/')"

    if grep -qE "IR_DIAG_$id\b" $src/ir/*.c 2>/dev/null; then
        if [ ! -f "$produced" ] || ! grep -qx "$code" "$produced"; then
            fail "$code is emitted by the library but no test produces it" ""
        fi
    else
        fail "$code is declared but no library code can emit it" \
             "delete the code, or emit it"
    fi
done

for sev in $(grep -oE ', (ERROR|WARNING|NOTE|HELP|[A-Z]+), ' \
             "$inc/ir_diags.def" | tr -d ' ,' | sort -u); do
    grep -q "IR_SEV_$sev\b" "$inc/diag.h" \
        || fail "the diag table uses a severity that does not exist" \
                "IR_SEV_$sev"
done

for sev in $(grep -oE 'IR_SEV_[A-Z]+' "$inc/diag.h" | sort -u); do
    short="${sev#IR_SEV_}"

    grep -qE ", $short, " "$inc/ir_diags.def" && continue

    awk '/is_attachment\(ir_severity_t/,/^}/' "$src/ir/diag.c" \
        | grep -q "$sev\b" && continue

    fail "nothing can ever produce severity $sev" \
         "no code defaults to it and it is not an attachment severity"
done

rm -rf "$work"

pubhdrs="$(sed -n '/^ir_public_headers = files(/,/^)/p' "$root/src/meson.build" \
           | grep -oE "include/ir/[a-z_]+\.h")"

for h in $pubhdrs; do
    [ -f "$root/$h" ] || fail "an installed header does not exist: $h" ""
done

for h in $pubhdrs; do
    grep -oE '\bir_[a-z0-9_]+ *\(' "$root/$h" 2>/dev/null | tr -d ' (' \
        | grep -vE '_t$' | sort -u
done | sort -u > "${TMPDIR:-/tmp}/ir-pub-$$"

unused=""
while read -r fn; do
    [ -n "$fn" ] || continue
    grep -qE "\b$fn *\(" $src/ir/*.c $root/tests/*/*.c 2>/dev/null || unused="$unused $fn"
done < "${TMPDIR:-/tmp}/ir-pub-$$"
rm -f "${TMPDIR:-/tmp}/ir-pub-$$"

[ -z "$unused" ] \
    || fail "installed headers declare what nothing calls:$unused" ""

ann_h="$root/include/parse/annotation.h"
kinds="$(sed -n '/#define PARSE_ANNOT_KINDS/,/^$/p' "$ann_h" \
         | grep -oE 'X\([A-Z_]+' | sed 's/X(//')"

[ -n "$kinds" ] || fail "the annotation kind list could not be read" "$ann_h"

consumers="$(ls "$root"/src/*/*.c 2>/dev/null \
             | grep -vE '/(annotation|annot_args|annot_grammar|bnf|bnf_print)\.c$')"

at_build_time="PENALTY DISAMBIG TOKENIZER"

expected_unconsumed=""

unconsumed=""
for k in $kinds; do
    case " $at_build_time " in
        *" $k "*) continue ;;
    esac

    grep -qE "PARSE_ANNOT_$k\b" $consumers 2>/dev/null && continue

    unconsumed="$unconsumed $k"
done

for k in $unconsumed; do
    case " $expected_unconsumed " in
        *" $k "*) ;;
        *) fail "@$(echo "$k" | tr 'A-Z' 'a-z') is parsed but nothing consumes it" \
                "add a consumer, or add $k to expected_unconsumed in this gate" ;;
    esac
done

for k in $expected_unconsumed; do
    case " $unconsumed " in
        *" $k "*) ;;
        *) fail "$k now has a consumer" \
                "drop it from expected_unconsumed in this gate" ;;
    esac
done

g="$(grep -rnE '(^|[^_[:alnum:]])(calloc|malloc|realloc|free|strdup|strndup)\(' \
     "$root"/src "$root"/include 2>/dev/null \
     | grep -vE 'core_(alloc|realloc|free|strdup|strndup|buffer|arena)|_core_calloc' \
     | grep -vE 'lib/alloc\.h|th/nfc_utf8proc\.c')"
[ -z "$g" ] || fail "libc allocation outside the one allocator" "$g"

g="$(grep -rn 'core_realloc(' "$root"/src 2>/dev/null)"
[ -z "$g" ] || fail "a container grows by hand instead of through core_list_t" "$g"

g="$(grep -rnE 'core_alloc_array\(char, *[a-z_]*cap' \
     "$root"/src 2>/dev/null)"
[ -z "$g" ] || fail "a string is built by hand instead of through core_buffer_t" "$g"

g="$(grep -rnE '(^|[^:"*])//|^[[:space:]]*/\*' \
     --include='*.c' --include='*.h' --include='*.def' \
     --exclude-dir=samples --exclude-dir=format \
     "$root"/src "$root"/include "$root"/tests 2>/dev/null \
     | grep -vE ':[[:space:]]*\*[a-zA-Z_(]' \
     | grep -vE '"[^"]*(//|/\*)')"
[ -z "$g" ] || fail "a comment is present in the source" "$g"

missing=""
for h in $(ls "$inc"/*.h 2>/dev/null | xargs -n1 basename); do
    [ "$h" = "libir.h" ] && continue
    [ "$h" = "cli.h" ] && continue

    grep -q "ir/$h" "$inc/libir.h" || missing="$missing $h"
done
[ -z "$missing" ] || fail "libir.h is not the whole public API" \
    "not reachable through the umbrella header:$missing"

missing=""
for h in $(grep -oE 'ir/[a-z_]+\.h' "$inc/libir.h" | sort -u); do
    grep -q "include/$h'" "$src/meson.build" || missing="$missing $h"
done
[ -z "$missing" ] || fail "libir.h includes a header that is not installed" \
    "add to ir_public_headers in src/meson.build:$missing"

g="$(grep -rnE '^[[:space:]]*(#|//)' \
     "$root"/src/meson.build "$root"/tests/meson.build "$root"/meson.build \
     2>/dev/null)"
[ -z "$g" ] || fail "a comment is present in the build files" "$g"

g="$(grep -rnE '(^|[^[:alnum:]])_?[Nn][Cc][Cc]_[A-Za-z0-9_]' \
     "$root"/src "$root"/include "$root"/tests "$root"/grammars 2>/dev/null \
     | grep -v 'c_ncc\.bnf')"
[ -z "$g" ] || fail "the legacy ncc_ prefix is back" \
    "one prefix per module: core_ parse_ scan_ unicode_ ir_ th_$g"

if [ "$fails" -eq 0 ]; then
    echo "gates: all passed"
fi

exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
