# QUIC echo demo — exercises the full transport end-to-end (loopback,
# server, and client modes).  See the surrounding `examples/quic_echo/`
# directory for the source plus the dev-PKI setup/teardown scripts.

quic_echo = executable('quic_echo',
    ['main.c'],
    include_directories: incdir,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    c_args: exe_c_args,
    link_args: link_args,
)
