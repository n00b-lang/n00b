repo="$work/budget"
mkdir -p "$repo/compiler" "$repo/tests/goldens"
vcs=
if command -v jj > /dev/null 2>&1; then
    v() { JJ_USER=guardrails JJ_EMAIL=guardrails@example.invalid jj --quiet --no-pager -R "$repo" "$@"; }
    vinit() { jj --quiet git init "$repo" > /dev/null 2>&1; }
    vnew() { v new; }
    vsave() { v describe "$@"; }
    vbase() { v log --no-graph -r @ -T commit_id; }
    vcs=jj
elif command -v git > /dev/null 2>&1; then
    v() { git -C "$repo" -c user.name=guardrails -c user.email=guardrails@example.invalid \
        -c commit.gpgsign=false "$@"; }
    vinit() { v init -q; }
    vnew() { :; }
    vsave() { v add -A; v commit -q "$@"; }
    vbase() { v rev-parse HEAD; }
    vcs=git
else
    echo "skip: change budget (neither jj nor git is installed)"
fi
lines() {
    i=0
    while [ "$i" -lt "${2:-40}" ]; do
        printf 'int %s_%s = %s;\n' "$1" "$i" "$i" >> "$repo/compiler/$1.c"
        i=$((i + 1))
    done
}
goldens() {
    i=0
    while [ "$i" -lt 11 ]; do
        printf 'golden %s\n' "$i" > "$repo/tests/goldens/$1_$i.expected"
        i=$((i + 1))
    done
}
drop_ten() {
    total=$(wc -l < "$repo/compiler/a.c")
    head -n $((total - 10)) "$repo/compiler/a.c" > "$repo/compiler/a.c.kept"
    mv "$repo/compiler/a.c.kept" "$repo/compiler/a.c"
}
verdict() {
    if printf '%s\n' "$report" | grep -q "^$1 .* $3\$"; then
        echo "ok: change budget: $2"
    else
        echo "change budget: $2: expected $1 for '$3'"
        printf '%s\n' "$report"
        status=1
    fi
}
if [ -n "$vcs" ]; then
    vinit
    printf 'int base;\n' > "$repo/compiler/a.c"
    vsave -m base
    base=$(vbase)
    vnew; lines a; vsave -m "Add forty lines"
    vnew; lines b; vsave -m "Add forty lines over budget" -m "Budget: 20"
    vnew; lines c; vsave -m "Add forty lines on budget" -m "Budget: 30"
    vnew; lines d 5; vsave -m "Add five lines with a malformed budget" -m "Budget: lots"
    vnew; drop_ten; vsave -m "Remove ten lines short of budget" -m "Budget: -30"
    vnew; drop_ten; vsave -m "Remove ten lines on budget" -m "Budget: -10"
    vnew; goldens first; vsave -m "Rewrite goldens"
    vnew; goldens second; vsave -m "Rewrite goldens with a reason" \
        -m "Goldens: the planted format changed for every case"
    if [ "$vcs" = jj ]; then range="$base..@"; else range="$base..HEAD"; fi
    report=$(python3 "$tools/check_change_budget.py" "$repo" "$range" "--vcs=$vcs" 2>&1)
    if printf '%s\n' "$report" | grep -q '^Traceback'; then
        echo "change budget: the check crashed"
        printf '%s\n' "$report"
        status=1
    else
        verdict FAIL "40 net lines without a trailer" "Add forty lines"
        verdict FAIL "40 net lines against Budget: 20" "Add forty lines over budget"
        verdict ok "40 net lines against Budget: 30" "Add forty lines on budget"
        verdict FAIL "malformed Budget trailer" "Add five lines with a malformed budget"
        verdict FAIL "removing 10 lines against Budget: -30" "Remove ten lines short of budget"
        verdict ok "removing 10 lines against Budget: -10" "Remove ten lines on budget"
        verdict FAIL "eleven goldens without a Goldens trailer" "Rewrite goldens"
        verdict ok "eleven goldens with a Goldens trailer" "Rewrite goldens with a reason"
    fi
fi
