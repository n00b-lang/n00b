# Manifest-driven managed server (Phase 2 deliverable § 13.1).  Loads
# a manifest JSON, runs `--preflight`, then binds the endpoints
# declared therein with cert/key plumbing wired up automatically.

quic_server_managed = executable('quic_server_managed',
    ['main.c'],
    include_directories: incdir,
    link_with: [n00b],
    dependencies: [picoquic_dep],
    c_args: exe_c_args,
    link_args: link_args,
)
