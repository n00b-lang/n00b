# H3 interop fixtures

Docker-compose stubs that bring up real HTTP/3 servers for the
Phase 4 H3 client and server smoke tests.

| Fixture                   | Server         | Pinned image           | Used by                            |
|---------------------------|----------------|------------------------|------------------------------------|
| `test/fixtures/caddy/`        | Caddy 2.9.1    | `caddy:2.9.1`          | Phase 4.3 — H3 client smoke test   |
| `test/fixtures/nginx-quic/`   | nginx 1.29.8   | `nginx:1.29.8`         | Phase 4.4 — H3 server smoke test   |

These fixtures don't compile any n00b code.  They exist so the
upcoming H3 client / server unit tests have a real interop target
to talk to in CI.

## Why two servers?

H3 interop bugs frequently hit one implementation but not another
(QPACK in particular has a long tail of subtle compatibility
issues).  Hitting both Caddy (Go's `quic-go`) and nginx (its own
QUIC stack) catches more regressions than either alone.  The cost
is two more docker images in CI; we accept that.

## Local run

```bash
# Caddy
eval "$(bash test/fixtures/caddy/start.sh)"
curl -k --http3 "$CADDY_BASE_URL/"      # requires curl with QUIC
bash test/fixtures/caddy/stop.sh

# nginx-quic
eval "$(bash test/fixtures/nginx-quic/start.sh)"
curl -k --http3 "$NGINX_QUIC_BASE_URL/"
bash test/fixtures/nginx-quic/stop.sh
```

`start.sh` prints `eval`-able shell vars that the test binary picks
up via the environment:

| Caddy                | nginx-quic              | Meaning                                                    |
|----------------------|-------------------------|------------------------------------------------------------|
| `CADDY_CONTAINER`    | `NGINX_QUIC_CONTAINER`  | Docker container name (uniquely port-suffixed)             |
| `CADDY_PORT`         | `NGINX_QUIC_PORT`       | Ephemeral host port (TCP+UDP both bound)                   |
| `CADDY_BASE_URL`     | `NGINX_QUIC_BASE_URL`   | `https://localhost:<port>`                                 |
| `CADDY_CERT_FP`      | `NGINX_QUIC_CERT_FP`    | SHA-256 of the leaf cert (hex, lower-case, no colons) — pin against this |
| n/a                  | `NGINX_QUIC_CERT_DIR`   | Tmp dir holding the generated self-signed cert + key + rendered nginx.conf |

The fingerprint is captured *after* the server is serving a real
cert (not the placeholder Caddy returns during lazy issuance), so
tests can pin trust without dragging an internal CA into the OS
trust store.

## Local prerequisites

- **Docker / Docker Desktop.**  Both fixtures call `docker compose`.
- **`openssl`.**  Used to generate self-signed certs (nginx) and to
  capture leaf cert fingerprints (both).
- **`envsubst`** (from gettext).  Renders `nginx.conf` with the
  ephemeral port chosen at startup.  Caddy doesn't need it — its
  Caddyfile uses Caddy's own `{$VAR}` substitution at runtime.
- **`python3`.**  Used for the ephemeral-port socket trick.
- **`curl` with HTTP/3 support** *(optional)*.  Needed only if you
  want to hit the fixture from your shell with `curl --http3`.
  macOS's bundled `/usr/bin/curl` (LibreSSL build) does **not**
  support HTTP/3.  `brew install curl` likewise doesn't enable
  QUIC by default.  You need a curl built against
  `ngtcp2`/`nghttp3`.  Workaround: hit the TCP fallback with
  `curl -k $CADDY_BASE_URL/` — the fixtures advertise `alt-svc`
  but also serve over h1/h2 on the same port.  CI uses our own
  H3 client (which is the whole point).

## CI integration

Phase 4 sub-phases 4.3 (H3 client) and 4.4 (H3 server) will add
two interop tests to `meson.build`:

```meson
if get_option('h3_docker_smoke') and run_command('docker', '--version', check: false).returncode() == 0
    test('quic_h3_caddy_smoke',
         test_quic_h3_caddy_smoke,
         suite: 'interop',
         env: ['N00B_TEST_DOCKER=1'],
         timeout: 90)
    test('quic_h3_nginx_smoke',
         test_quic_h3_nginx_smoke,
         suite: 'interop',
         env: ['N00B_TEST_DOCKER=1'],
         timeout: 90)
endif
```

The tests themselves call `start.sh` / `stop.sh` (probably via
`popen`), parse the eval-able output, set up an H3 client / server
against the captured fingerprint, and assert on the canonical
response bodies:

| Path        | Caddy body          | nginx-quic body          |
|-------------|---------------------|--------------------------|
| `GET /`     | `hello-from-caddy`  | `hello-from-nginx\n`     |
| `POST /echo`| (n/a — no /echo)    | `received-by-nginx\n`    |
| anything else | `not-found` (404) | `not-found\n` (404)     |

> nginx-quic note: the official `nginx:mainline` image doesn't
> ship the `echo-nginx-module`, so `/echo` returns a fixed
> `received-by-nginx\n` rather than echoing the request body
> verbatim.  The H3 server smoke test asserts on the fixed body
> after POSTing the request, which is sufficient to prove that
> our client framed and sent an H3 POST that nginx accepted.  If
> a future test needs a real round-trip echo, swap the image for
> a custom one with the echo module compiled in (or use Caddy's
> `respond {http.request.body}`).

## Updating pinned image versions

We pin to a specific patch tag (e.g., `caddy:2.9.1`,
`nginx:1.29.8`) rather than `latest` or `mainline` so a CI run a
year from now reproduces the same wire behavior.  Refresh on these
triggers:

- **Caddy / quic-go ships an H3 fix that affects us.**  Update the
  tag in `test/fixtures/caddy/docker-compose.yml`, run the smoke
  test locally with `N00B_TEST_DOCKER=1 meson test --suite
  interop`, and bump the tag here in this README.
- **nginx ships an H3 fix.**  Same process for
  `test/fixtures/nginx-quic/docker-compose.yml`.
- **CVE in either image.**  Bump immediately even if no behavior
  change — these images run in CI but with no inbound exposure
  outside the test container, so the urgency is moderate, not
  critical.

When you bump:

1. Update the `image:` line in `docker-compose.yml`.
2. Update the table at the top of this README.
3. Re-run both smoke tests to confirm no regression.
4. Note the bump in the commit message (so a future bisect can
   correlate behavior changes to image bumps).

## Concurrency

Both fixtures pick an ephemeral host port via the Python
`socket.getsockname()` trick and uniquely name their container
`n00b-test-<server>-<port>`.  Concurrent test runs on the same
machine therefore can't collide on either port or container name.

## Cleanup

`stop.sh` is idempotent: missing container, missing temp dir, no
docker — all are no-ops.  It refuses to `rm -rf` anything outside
`/tmp/*` or `/var/folders/*` (the macOS temp roots) as a
defensive measure against a stale env-var.

If a test crashes between `start.sh` and `stop.sh`, list and prune
leftovers manually:

```bash
docker ps --filter "name=n00b-test-" --format '{{.Names}}' \
    | xargs -r docker rm -f
```
