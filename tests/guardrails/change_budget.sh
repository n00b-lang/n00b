repo="$work/budget"
mkdir -p "$repo/compiler" "$repo/tests/goldens"
vcs=
if command -v jj > /dev/null 2>&1; then
    v() { JJ_USER=guardrails JJ_EMAIL=guardrails@example.invalid jj --quiet --no-pager -R "$repo" "$@"; }
    vinit() { jj --quiet git init "$repo" > /dev/null 2>&1; }
    vnew() { v new; }
    vsave() { v describe "$@"; }
    vcs=jj
    range=@
elif command -v git > /dev/null 2>&1; then
    v() { git -C "$repo" -c user.name=guardrails -c user.email=guardrails@example.invalid \
        -c commit.gpgsign=false "$@"; }
    vinit() { v init -q; fresh=1; }
    vnew() { fresh=1; }
    vsave() {
        v add -A
        if [ "$fresh" = 1 ]; then v commit -q "$@"; else v commit -q --amend "$@"; fi
        fresh=0
    }
    vcs=git
    range=HEAD^..HEAD
else
    echo "skip: change budget (neither jj nor git is installed)"
fi
budget() { python3 "$tools/check_change_budget.py" "$repo" "$range" "--vcs=$vcs"; }
if [ -n "$vcs" ]; then
    vinit
    printf 'int base;\n' > "$repo/compiler/a.c"
    vsave -m base
    vnew
    i=0
    while [ "$i" -lt 40 ]; do
        printf 'int value_%s = %s;\n' "$i" "$i" >> "$repo/compiler/a.c"
        i=$((i + 1))
    done
    vsave -m "Add forty lines"
    expect fail "change budget: 40 net lines without a trailer" budget
    vsave -m "Add forty lines" -m "Budget: 20"
    expect fail "change budget: 40 net lines against Budget: 20" budget
    vsave -m "Add forty lines" -m "Budget: 30"
    expect pass "change budget: 40 net lines against Budget: 30" budget
    vnew
    i=0
    while [ "$i" -lt 11 ]; do
        printf 'golden %s\n' "$i" > "$repo/tests/goldens/case_$i.expected"
        i=$((i + 1))
    done
    vsave -m "Rewrite goldens"
    expect fail "change budget: eleven goldens without a Goldens trailer" budget
    vsave -m "Rewrite goldens" -m "Goldens: the planted format changed for every case"
    expect pass "change budget: eleven goldens with a Goldens trailer" budget
fi
