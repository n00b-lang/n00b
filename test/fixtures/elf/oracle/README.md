# ELF Oracle Helpers

Optional ELF oracle tests use a subprocess helper selected by
`N00B_ELF_ORACLE_BIN`. The helper protocol is line-oriented:

```text
verdict=valid-target
code=0
detail=ok
```

The first runner compares only `verdict`. `code` and `detail` are diagnostic
until the helper protocol settles.

`N00B_ELF_ORACLE_ROOT` auto-build support is intentionally deferred; use a
prebuilt helper for the first oracle runs.
