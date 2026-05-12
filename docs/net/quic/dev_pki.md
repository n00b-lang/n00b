# Local development PKI

> **Status: working.**  `examples/quic_echo/setup_dev_pki.sh` is in
> tree, idempotent, and produces a working ACME-issued cert from a
> local step-ca instance.  This document describes what it does and
> how to use it.

The QUIC examples (`examples/quic_echo` today;
`examples/quic_throughput` and the Phase 5 deployment helpers later)
all run against **real authenticated certs**, not self-signed.  The
reason is in `~/dd/quic_1.md § 14.4`: self-signed workflows train
developers to ignore trust failures, and the n00b QUIC libraries are
explicitly designed to refuse degraded modes.  We ship a tiny local
PKI built on
[`smallstep`](https://smallstep.com/docs/step-ca/) and run the
examples against that.

## What you get

After `setup_dev_pki.sh`:

- `~/.n00b-dev-pki/` — self-contained data dir (CA material, db,
  config, password files, server cert/key).  Nothing leaks into
  `~/.step/` or other system-wide locations.
- A local `step-ca` instance running in the background on
  `127.0.0.1:8443`.  PID file at `~/.n00b-dev-pki/step-ca.pid`,
  log at `~/.n00b-dev-pki/step-ca.log`.
- The CA root cert added to the **macOS System Keychain** as a
  trusted root.  This is the only system-wide effect; teardown
  removes it.
- A server cert + key for `localhost`, issued via **ACME against the
  local step-ca**:
  - `~/.n00b-dev-pki/server.crt.pem` — PEM x509 chain.
  - `~/.n00b-dev-pki/server.key.pem` — PEM PKCS#8 private key (the
    setup script converts step's default `EC PRIVATE KEY` form into
    PKCS#8 because that's what picotls's minicrypto loader expects).

## Quickstart

```bash
# One-time prereq (no sudo).
brew install step

# Bring everything up.  Will prompt for sudo to install the CA root
# in System Keychain.
./examples/quic_echo/setup_dev_pki.sh

# Run the echo demo against the real cert chain.  Note: the client
# still uses picoquic's null verifier today (the trust_t -> picotls
# verify-callback bridge ships in Phase 3); but the cert itself is
# real, ACME-issued, OS-trusted.
./build_debug/quic_echo server 4433 \
    --cert-pem=$HOME/.n00b-dev-pki/server.crt.pem \
    --key-pem=$HOME/.n00b-dev-pki/server.key.pem &

./build_debug/quic_echo client 127.0.0.1 4433 "hello via real PKI"

# Tear down when done.  Will prompt for sudo to remove the CA root.
./examples/quic_echo/teardown_dev_pki.sh
```

`teardown_dev_pki.sh --purge` also removes `~/.n00b-dev-pki/`
entirely.  Without `--purge`, re-running `setup_dev_pki.sh` is fast:
the script detects the existing CA, just (re-)issues the server cert
if it's near expiry.

## What the setup script actually does

Each step is idempotent.

1. **Create `~/.n00b-dev-pki/`** if missing (mode 700).  Generate
   random password files for the CA + provisioner if not already
   present.
2. **Run `step ca init`** — creates root + intermediate CA certs,
   ca.json config, db, and a default ACME provisioner named `acme`.
   Skipped on subsequent runs.
3. **Add the CA root to System Keychain** via
   `sudo security add-trusted-cert -d -r trustRoot
   -k /Library/Keychains/System.keychain ~/.n00b-dev-pki/step/certs/root_ca.crt`.
   Skipped if a cert with CN `n00b dev` is already present.
4. **Start step-ca in the background** writing PID + log to
   `~/.n00b-dev-pki/`.  Skipped if the PID file points at a running
   process.
5. **Issue (or re-issue) the server cert** via the ACME provisioner.
   Re-issued only if the existing cert is within 7 days of expiring.
   Default ACME duration is 24 hours — short by design.  Convert the
   key from `EC PRIVATE KEY` PEM to PKCS#8 PEM so picotls's
   minicrypto loader accepts it.

## What the teardown script does

1. **Stop step-ca** (TERM, then KILL after 2s grace).
2. **Remove the CA root** from System Keychain via
   `sudo security delete-certificate -c "n00b dev"
   /Library/Keychains/System.keychain`.
3. With `--purge`, **`rm -rf ~/.n00b-dev-pki/`**.

## Linux notes

The setup script today targets macOS (Keychain).  The Linux variant
should:

- Copy `~/.n00b-dev-pki/step/certs/root_ca.crt` to
  `/usr/local/share/ca-certificates/n00b-dev-root.crt` and run
  `sudo update-ca-certificates`.
- Use `secret-tool` to import the server cert + key into libsecret
  for use via the (still pending) `libsecret:` provider in
  `n00b_quic_secret_open`.

This branch lands when the Linux `libsecret:` secret provider does
in Phase 1.5.

## Troubleshooting

### "step-ca failed to start; tail of log:" → "can't find or open the configuration file"

`step ca init --pki ...` was used somewhere.  The `--pki` flag means
"PKI material WITHOUT the CA config file" — the opposite of what we
want.  Drop it.  The current script doesn't do this; the symptom
appears only if you've manually run `step ca init` against
`~/.n00b-dev-pki/step/`.

### ACME finalize: "requested duration of 720h is more than the authorized maximum certificate duration of 24h"

ACME provisioners cap cert lifetime at 24h by default.  Drop any
`--not-after` flag larger than that, or configure the provisioner
to allow longer lifetimes (not recommended for a dev PKI — short
lifetimes are part of the security story).

### picoquic_set_private_key_from_file returns non-zero

picotls's minicrypto loader requires the **PKCS#8 PEM** wrapping
(`-----BEGIN PRIVATE KEY-----`), not the **`EC PRIVATE KEY`** form
that `step ca certificate` produces by default.  The setup script
converts in-place via `openssl pkcs8 -topk8 -nocrypt`.  If you
issued certs out-of-band with step, run that conversion yourself.

### "step not found"

`brew install step` installs both `step` (the CLI) and `step-ca`
(the daemon).  Verify with `step version`.

## Why step-ca and not mkcert / openssl

Two reasons:

1. **ACME is the production cert flow.**  Phase 5's deployment
   playbooks assume an ACME directory the operator points at — Let's
   Encrypt for public deployments, step-ca / cert-manager for
   internal PKI, etc.  Driving the dev workflow through ACME against
   a local step-ca exercises the same code path the production
   server lifecycle uses; mkcert and openssl-direct don't.
2. **step-ca onboarding is "install one binary"** — no deep
   operations knowledge required to bring up a working ACME
   directory locally.

## Threat-model footnote

Adding a CA root to the System Keychain is a privilege escalation
event: that root can vouch for any cert that ends up in the same
system trust path.  The dev PKI's CA root is trusted on **the
developer's machine only** and should be rolled when:

- The script is re-run with `--rotate` (forthcoming flag).
- The developer leaves a project / wipes their machine.

Production deployments do **not** use this dev CA.  The pattern is
purely a developer-machine convenience and is documented as such.

## See also

- `~/dd/quic_1.md § 14.4` — design source.
- `docs/net/quic/vendored.md` — picotls / picoquic backend choice.
- `docs/net/quic/overview.md` — overall module status.
- `examples/quic_echo/setup_dev_pki.sh` — the setup script.
- `examples/quic_echo/teardown_dev_pki.sh` — the teardown script.
