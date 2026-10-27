#!/usr/bin/env bash
set -u

results="${TEST_RESULTS:?}"
tier="${TEST_TIER:-}"
budget_file=tests/budget
baseline_file=tests/times.baseline
ok=0
skipped=0
failed=()
problems=()

budget() {
    awk -v t="$1" -v k="$2" '$1 == t && $2 == k { print $3 }' "$budget_file"
}

for status in "$results"/*/*.status; do
    [ -e "$status" ] || continue
    name="${status#"$results"/}"
    name="${name%.status}"
    case "$(cat "$status")" in
        OK) ok=$((ok + 1)) ;;
        SKIP) skipped=$((skipped + 1)) ;;
        *) failed+=("$name") ;;
    esac
done

: > "$results/times"
: > "$results/cpu"
for time in "$results"/*/*.time; do
    [ -e "$time" ] || continue
    name="${time#"$results"/}"
    name="${name%.time}"
    printf '%s %s\n' "$name" "$(cat "$time")" >> "$results/times"
    if [ -f "$results/$name.cpu" ]; then
        printf '%s %s\n' "$name" "$(cat "$results/$name.cpu")" >> "$results/cpu"
    fi
done
sort -o "$results/times" "$results/times"
sort -o "$results/cpu" "$results/cpu"

for name in "${failed[@]+"${failed[@]}"}"; do
    printf '\n==== %s (%s) ====\n' "$name" "$(cat "$results/$name.status")"
    tail -40 "$results/$name.log"
done

if [ -n "$tier" ] && [ -f "$results/started" ]; then
    wall=$(awk -v s="$(cat "$results/started")" \
        -v n="$(python3 -c 'import time; print(f"{time.time():.2f}")')" \
        'BEGIN { printf "%.2f", n - s }')
    limit=$(budget "$tier" wall)
    scaled=$(awk -v b="$limit" \
        -v s="${N00B_WALL_BUDGET_SCALE:-${N00B_TEST_BUDGET_SCALE:-1}}" \
        'BEGIN { printf "%.2f", b * s }')
    if [ -n "$limit" ] \
        && [ "$(awk -v w="$wall" -v b="$scaled" 'BEGIN { print (w > b) }')" = 1 ]; then
        problems+=("the $tier tier took ${wall}s, over its ${scaled}s budget in $budget_file; slowest:")
        while read -r line; do
            problems+=("  $line")
        done < <(sort -k2 -rn "$results/times" | head -5)
    fi
    printf '\n%s tier: %ss wall (budget %ss)\n' "$tier" "$wall" "$scaled"
fi

if [ "$tier" = pr ] && [ "${N00B_UPDATE_TIMES:-0}" = 1 ]; then
    cp "$results/cpu" "$baseline_file"
    printf 'recorded %s\n' "$baseline_file"
elif [ "$tier" = pr ] && [ -f "$baseline_file" ]; then
    while read -r name seconds; do
        base=$(awk -v n="$name" '$1 == n { print $2 }' "$baseline_file")
        if [ -z "$base" ]; then
            problems+=("$name has no recorded time in $baseline_file")
        elif [ "$(awk -v t="$seconds" -v b="$base" \
                'BEGIN { print (t > b * 1.25 + 0.5) }')" = 1 ]; then
            problems+=("$name used ${seconds} CPU s, up from ${base} in $baseline_file")
        fi
    done < "$results/cpu"
fi

for problem in "${problems[@]+"${problems[@]}"}"; do
    printf '%s\n' "$problem"
done

printf '\n%d passed, %d failed, %d skipped\n' "$ok" "${#failed[@]}" "$skipped"
if [ "${#failed[@]}" -gt 0 ] || [ "${#problems[@]}" -gt 0 ]; then
    if [ "${#failed[@]}" -gt 0 ]; then
        printf 'Logs are in %s\n' "$results"
    fi
    exit 1
fi
