fixture comments
printf 'enforce on\n' > "$work/comments/tests/comment-allowlist"
printf '#!/bin/sh\n# shellcheck disable=SC2086\necho "#not" ${#x} "$(echo a)"\n' > "$work/comments/run.sh"
printf 'int x = 1;\nchar *s = "// not a comment";\n' > "$work/comments/compiler/a.c"
printf 'func f() -> int {\n    return 1\n}\n' > "$work/comments/lib/a.n"
expect pass "comments: directives and strings are not comments" check comments comments
printf 'int y = 2; // planted\n' >> "$work/comments/compiler/a.c"
expect fail "comments: C line comment" check comments comments
printf 'lib/a.n\ncompiler/a.c\n' >> "$work/comments/tests/comment-allowlist"
expect pass "comments: allowlisted test input" check comments comments
fixture comments
printf 'x = 1\n# planted\n' > "$work/comments/lib/a.n"
expect pass "comments: .n files skipped without a compiler" check comments comments
n00b="${BUILD:-$src/build}/n00b"
if "$n00b" fmt --help 2>/dev/null | grep -q -- --strip-comments; then
    printf 'func f() -> int {\n    return 1\n}\n' > "$work/comments/lib/a.n"
    "$n00b" fmt "$work/comments/lib/a.n" > /dev/null 2>&1
    expect pass "comments: .n file without comments" \
        python3 "$tools/check_comments.py" "$work/comments" "--n00b=$n00b"
    printf '# planted\n' >> "$work/comments/lib/a.n"
    expect fail "comments: .n file with a comment" \
        python3 "$tools/check_comments.py" "$work/comments" "--n00b=$n00b"
else
    echo "skip: comments in .n files ($n00b has no fmt --strip-comments yet)"
fi
fixture comments
printf 'def f():\n    """planted"""\n    return 1\n' > "$work/comments/tool.py"
expect fail "comments: Python docstring" check comments comments
fixture comments
printf 'all:\n\techo hi # planted\n' > "$work/comments/Makefile"
expect fail "comments: Makefile recipe comment" check comments comments
fixture comments
printf 'int y; /* planted */\n' > "$work/comments/compiler/a.c"
printf 'enforce off\n' > "$work/comments/tests/comment-allowlist"
expect pass "comments: enforcement off" check comments comments
expect fail "comments: --enforce overrides" python3 "$tools/check_comments.py" "$work/comments" --enforce
