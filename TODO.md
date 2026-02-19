# Buffer Migration TODO

Deferred items from the `n00b_buffer_t` migration (old `n00b_buf_t`).

## Pending

- [ ] `buffer_fmt` / `buffer_repr` -- format/repr functions for vtable
- [ ] `buffer_can_coerce_to` / `buffer_coerce_to` -- type coercion
- [ ] `n00b_buffer_vtable` -- register vtable entry
- [ ] `buffer_lit` -- literal parsing from compiler
- [ ] `n00b_buffer_hash` -- hash function for use in dicts
- [ ] `n00b_pmap_first_word` -- GC pointer map
- [ ] Full UTF-8 validation in `n00b_buffer_to_string`
- [ ] Port `buffer_view` / `buffer_item_type` for vtable
- [ ] Replace `n00b_buffer_to_string` placeholder (byte == codepoint) with real UTF-8 counting
