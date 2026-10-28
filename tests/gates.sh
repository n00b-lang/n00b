#!/bin/sh
set -u

src=$1
cd "$src" || exit 1

status=0
releasing=$(grep -l -- '--lto\|--release' tests/*.sh 2>/dev/null \
    | grep -v '^tests/gates\.sh$' | tr '\n' ' ')
awk -F'\t' -v releasing="$releasing" '
    FILENAME == "tests/budget" {
        if ($0 ~ /^release /) { split($0, r, " "); release[r[2]] = 1 }
        else if ($0 ~ /^exempt /) { split($0, x, " "); exempt[x[2]] = x[3] }
        else { split($0, b, " "); budget[b[1] " " b[2]] = b[3] }
        next
    }
    function fail(message) { print FILENAME ": " message > "/dev/stderr"; status = 1 }
    $1 == "" { next }
    NF < 4 || $3 !~ /^[0-9]+(\.[0-9]+)?$/ || $4 == "" {
        fail($1 " must have four tab-separated fields: name, tier, budget and command")
        next
    }
    {
        tags = "," $2 ","
        dev = tags ~ /,dev,/
        pr = tags ~ /,pr,/
        if (dev + pr != 1) { fail($1 " must be tagged with exactly one of dev or pr"); next }
        tier = dev ? "dev" : "pr"
        count[tier]++
        cap = ($1 in exempt) ? exempt[$1] : budget[tier " test"]
        if (cap != "" && $3 + 0 > cap + 0)
            fail(tier " test " $1 " has a " $3 "s budget; the cap is " cap "s")
        if (dev && $4 ~ /\$BUILD\/n00b"/ && $4 !~ /n00b" check/)
            fail("dev test " $1 " runs the n00b compiler for more than `n00b check`")
        uses_release = $4 ~ /--lto|--release/
        n = split(releasing, scripts, " ")
        for (i = 1; i <= n; i++) if (index($4, scripts[i])) uses_release = 1
        if (uses_release && !($1 in release))
            fail($1 " builds with --release or --lto; only the release entries in tests/budget may")
    }
    END {
        for (tier in count) {
            cap = budget[tier " tests"]
            if (cap != "" && count[tier] > cap + 0) {
                print "there are " count[tier] " " tier " tests; the cap in tests/budget is " cap > "/dev/stderr"
                status = 1
            }
        }
        exit status
    }
' tests/budget tests/n00b.tests lantern/tests/lantern.tests writ/tests/writ.tests \
    || status=1

find tests/programs -type f -name '*.n' | awk '
    FILENAME != "-" { refs = refs "\n" $0; next }
    function quote(text) { gsub(/[.+*?()\[\]{}|^$]/, "\\\\&", text); return text }
    {
        path = substr($0, length("tests/programs/") + 1)
        candidate = path
        found = 0
        while (!found && candidate != "") {
            if (match(refs, "(programs/|\"|[ \t]|/)" quote(candidate) "([^A-Za-z0-9_]|$)"))
                found = 1
            else if (index(candidate, "/")) sub(/\/[^\/]*$/, "", candidate)
            else candidate = ""
        }
        if (!found) {
            print "tests/programs/" path ": no test reads this program" > "/dev/stderr"
            status = 1
        }
    }
    END { exit status }
' tests/n00b.tests tests/*.sh - || status=1

for helper in tests/diagnostics/modules/*/; do
    name=${helper%/}
    name=${name##*/}
    set -- tests/diagnostics/*/"$name".n
    [ -f "$1" ] && continue
    echo "$helper: no corpus entry named $name.n reads these modules" >&2
    status=1
done
exit "$status"
