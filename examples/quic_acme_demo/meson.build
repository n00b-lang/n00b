# One-shot ACME cert acquisition demo (manual DNS-01 provider).
# Used by docs/net/quic/le_staging.md for manual interop verification.

quic_acme_demo = executable('quic_acme_demo',
    ['main.c'],
    include_directories: incdir,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    c_args: exe_c_args,
    link_args: link_args,
)
