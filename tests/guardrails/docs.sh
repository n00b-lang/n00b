fixture docs
printf 'LICENSE\n' > "$work/docs/tests/doc-allowlist"
printf 'MIT\n' > "$work/docs/LICENSE"
mkdir -p "$work/docs/docs"
printf 'fine\n' > "$work/docs/docs/guide.md"
expect pass "docs: docs/ and allowlisted files" check docs docs
printf 'notes\n' > "$work/docs/compiler/NOTES.md"
expect fail "docs: markdown outside docs/" check docs docs
rm "$work/docs/compiler/NOTES.md"
printf 'todo\n' > "$work/docs/TODO"
expect fail "docs: README-like file outside docs/" check docs docs
