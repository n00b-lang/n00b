fixture clones
cat > "$work/clones/compiler/a.c" <<'CODE'
static int
sum_widths(const record_t *records, size_t count, int limit)
{
    int total = 0;
    for (size_t i = 0; i < count; i++) {
        const record_t *record = &records[i];
        if (!record->present || record->width < 0) {
            continue;
        }
        if (record->width > limit) {
            total += limit;
        }
        else {
            total += record->width * 2 + record->padding;
        }
        if (total > limit * 4) {
            return -1;
        }
        if (record->next && record->next->width == record->width) {
            total -= record->padding;
            record = record->next;
        }
    }
    if (total < 0 || count == 0) {
        return 0;
    }
    return total / (int)count + limit % 3;
}
CODE
sed -e 's/sum_widths/add_heights/; s/records/rows/g; s/record/row/g; s/total/height/g' \
    "$work/clones/compiler/a.c" > "$work/clones/compiler/b.c"
expect fail "clones: renamed copy of a function" check clones clones
python3 "$tools/check_clones.py" "$work/clones" --update > /dev/null
expect pass "clones: copy recorded in the baseline" check clones clones
cp "$work/clones/compiler/b.c" "$work/clones/compiler/c.c"
expect fail "clones: third copy beyond the baseline" check clones clones
