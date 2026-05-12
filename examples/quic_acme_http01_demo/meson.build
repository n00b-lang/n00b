# Phase 5 § 5.7 — n00b-direct-ACME demo with embedded HTTP-01 responder.
# Single-binary smoke against Pebble (or a real LE staging endpoint).

quic_acme_http01_demo = executable('quic_acme_http01_demo',
    ['main.c'],
    include_directories: incdir,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    c_args: exe_c_args,
    link_args: link_args,
)
